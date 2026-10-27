#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/sendfile.h>
#include <pthread.h>
#include "string_ops.h"
#include "fs.h"

#define CRLF "\r\n"
#define SP " "

/*
 * Constant brace initialiser for file-scope string_views.
 * (A compound literal isn't a constant expression in strict C, which is what
 * -Wpedantic was complaining about.) Only usable in declarations.
 * Length excludes the NUL terminator.
 */
#define SV_LIT(s) { .start = (s), .len = sizeof(s) - 1 }

static const string_view WEB_ROOT = SV_LIT("./www/");

/* request line - method sp request-uri sp http-version crlf */
typedef struct {
	string_view method;
	string_view uri;
	string_view version;
} http_req_line;

typedef enum {
	HTTP_RES_OK = 200,
	HTTP_RES_BAD_REQUEST = 400,
	HTTP_RES_NOT_FOUND = 404,
	HTTP_RES_INTERNAL_SERVER_ERR = 500
} http_status;

const char* http_status_to_string(http_status status) {
	switch (status) {
		case HTTP_RES_OK:
			return "OK";
		case HTTP_RES_BAD_REQUEST:
			return "Bad Request";
		case HTTP_RES_INTERNAL_SERVER_ERR:
			return "Internal Server Error";
		case HTTP_RES_NOT_FOUND:
			return "Not Found";
		default:
			return "Unknown";
	}
}

http_req_line http_req_line_init(void) {
	http_req_line line;

	memset(&line, 0, sizeof(line));

	return line;
}

http_status parse_req_line(http_req_line* req_line, const char* buf, size_t len) {
	string_splits components;

	if (!buf || !req_line) {
		return HTTP_RES_INTERNAL_SERVER_ERR;
	}

	components = split_string(buf, len, SP);

	if (components.count != 3) {
		printf("ERROR: invalid request line: expected 3 components, got %zu\n", components.count);
		free_splits(&components);
		return HTTP_RES_BAD_REQUEST;
	}

	req_line->method.start = components.splits[0].start;
	req_line->method.len = components.splits[0].len;

	req_line->uri.start = components.splits[1].start;
	req_line->uri.len = components.splits[1].len;

	req_line->version.start = components.splits[2].start;
	req_line->version.len = components.splits[2].len;

	free_splits(&components);
	return HTTP_RES_OK;
}

string_view http_response_generate(char* buf, size_t buf_len, http_status status, size_t body_len) {
	string_view response;
	int n;

	n = snprintf(buf, buf_len,
			"HTTP/1.0 %d %s" CRLF
			"Content-Type: text/html" CRLF
			"Content-Length: %zu" CRLF
			"Connection: keep-alive" CRLF
			CRLF,
			(int)status, http_status_to_string(status), body_len);

	if (n < 0) {
		n = 0;
	} else if ((size_t)n >= buf_len) {
		n = (int)buf_len - 1; /* truncated */
	}

	response.start = buf;
	response.len = (size_t)n;
	return response;
}

/* send everything, handling partial sends */
static bool send_all(int socket, const char* data, size_t len, int flags) {
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(socket, data + sent, len - sent, flags);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("send()");
			return false;
		}
		if (n == 0) {
			fprintf(stderr, "send() returned 0\n");
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

static const string_view err_404 =
	SV_LIT("<p>Error 404: Not Found</p><p><a href=\"/\">back to home</a></p>");

bool http_send_response(int socket, string_view header, string_view body) {
	if (!send_all(socket, header.start, header.len, MSG_MORE)) {
		return false;
	}
	return send_all(socket, body.start, body.len, 0);
}

static void http_send_not_found(int socket) {
	char buf[256];

	(void)http_send_response(
			socket,
			http_response_generate(buf, sizeof(buf), HTTP_RES_NOT_FOUND, err_404.len),
			err_404);
}

/*
 * Reject URIs that could escape WEB_ROOT: anything containing ".."
 * or an embedded NUL byte.
 */
static bool uri_is_safe(string_view uri) {
	size_t i;

	for (i = 0; i < uri.len; ++i) {
		if (uri.start[i] == '\0') {
			return false;
		}
		if (uri.start[i] == '.' && i + 1 < uri.len && uri.start[i + 1] == '.') {
			return false;
		}
	}
	return true;
}

/*
 * Serve a file located under WEB_ROOT.
 * `filename` is relative to WEB_ROOT, e.g. "index.html".
 */
bool http_serve_file(int socket, string_view filename) {
	char buf[256];
	char filename_buf[PATH_MAX];
	off_t sendfile_offset = 0;
	size_t total;
	size_t sent = 0;
	int in_fd = -1;
	bool return_value = true;
	string_view header;
	fs_metadata file_metadata;

	if (!uri_is_safe(filename)) {
		http_send_not_found(socket);
		return false;
	}

	/* build "<WEB_ROOT><filename>", rejecting anything that won't fit */
	if (WEB_ROOT.len + filename.len >= sizeof(filename_buf)) {
		http_send_not_found(socket);
		return false;
	}

	memset(filename_buf, 0, sizeof(filename_buf));
	memcpy(filename_buf, WEB_ROOT.start, WEB_ROOT.len);
	memcpy(filename_buf + WEB_ROOT.len, filename.start, filename.len);

	/* check the full path, not just the bare filename */
	file_metadata = fs_get_metadata(string_from_cstr(filename_buf));
	if (!file_metadata.exists) {
		http_send_not_found(socket);
		return false;
	}

	/* open BEFORE sending headers so we can still send a proper 404 */
	in_fd = open(filename_buf, O_RDONLY);
	if (in_fd < 0) {
		http_send_not_found(socket);
		return false;
	}

	total = (size_t)file_metadata.size;
	header = http_response_generate(buf, sizeof(buf), HTTP_RES_OK, total);

	if (!send_all(socket, header.start, header.len, MSG_MORE)) {
		return_value = false;
		goto cleanup;
	}

	/* use sendfile(2) to send file without using any userspace memory */
	while (sent < total) {
		ssize_t result = sendfile(socket, in_fd, &sendfile_offset, total - sent);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			printf("ERROR: sendfile() failed for \"%s\": %s\n",
					filename_buf, strerror(errno));
			/* headers already sent, can't send an error page now */
			return_value = false;
			goto cleanup;
		}
		if (result == 0) {
			/* file shrank underneath us */
			return_value = false;
			goto cleanup;
		}
		sent += (size_t)result;
	}

cleanup:
	if (in_fd != -1) {
		close(in_fd);
	}
	return return_value;
}

void* handle_client(void* client_socket_ptr) {
	int client_socket = (int)(intptr_t)client_socket_ptr;
	ssize_t n = 0;
	int ret = 0;
	char buf[1024];

	printf("\n---\n");

	for (;;) {
		string_splits lines;
		http_req_line req_line;
		http_status result;
		string_view route_root;

		memset(buf, 0, sizeof(buf));

		n = read(client_socket, buf, sizeof(buf)-1);
		if (n < 0) {
			perror("read(client)");
			ret = -1;
			break;
		}

		if (n == 0) {
			printf("connection closed gracefully\n");
			break;
		}

		lines = split_string(buf, (size_t)n, CRLF);

		if (lines.count < 1) {
			printf("ERROR: empty request\n");
			free_splits(&lines);
			ret = -1;
			break;
		}

		req_line = http_req_line_init();
		result = parse_req_line(&req_line, lines.splits[0].start, lines.splits[0].len);
		free_splits(&lines);
		if (result != HTTP_RES_OK) {
			printf("ERROR: failed to parse line\n");
			ret = -1;
			break;
		}

		printf("REQUEST: %.*s %.*s\n", (int)req_line.method.len, req_line.method.start,
					       (int)req_line.uri.len, req_line.uri.start);

		route_root = string_from_cstr("/");

		if (string_equal(&req_line.uri, &route_root)) {
			/* path is relative to WEB_ROOT, which http_serve_file prepends */
			if (!http_serve_file(client_socket, string_from_cstr("index.html"))) {
				ret = -1;
				break;
			}
		} else {
			if (!http_serve_file(client_socket, req_line.uri)) {
				ret = -1;
				break;
			}
		}

		break;
	}

	printf("Closing Connection%d\n", ret);
	close(client_socket);

	pthread_exit((void*)(intptr_t)ret);
}

const int PORT = 6969;

int main(void) {
	/* declare */
	int rc = 0;
	struct sockaddr_in bind_addr;
	int tcp_socket = 0;
	int ret = 0;
	int client_socket = 0;
	int enabled = 1;
	fs_metadata web_root_meta;

	/* a client hanging up mid-send shouldn't kill the whole server */
	signal(SIGPIPE, SIG_IGN);

	web_root_meta = fs_get_metadata(WEB_ROOT);
	if (!web_root_meta.exists) {
		/* rwxr-xr-x */
		/* this is safe because it is from a literal which is 0-terminated */
		mkdir(WEB_ROOT.start, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	}

	/* initialise */
	memset(&bind_addr, 0, sizeof(bind_addr));
	tcp_socket = socket(
		AF_INET, /* IPv4 */
		SOCK_STREAM, /* TCP */
		0
	);

	if (tcp_socket < 0) {
		perror("socket");
		return 1;
	}
	printf("socket creation succeeded\n");

	/* we dont care if this fails */
	(void)setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

	bind_addr.sin_port = htons(PORT);
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;

	rc = bind(tcp_socket, (const struct sockaddr*)&bind_addr, sizeof(bind_addr));

	if (rc < 0) {
		perror("bind()");
		ret = 1;
		goto exit;
	}

	printf("bind successful\n");

	rc = listen(tcp_socket, SOMAXCONN);

	if (rc < 0) {
		perror("listen()");
		ret = 1;
		goto exit;
	}

	printf("listening on http://localhost:%d/\n", PORT);

	for (;;) {
		pthread_t thread;

		printf("waiting for connections...\n");
		client_socket = accept(tcp_socket, NULL, NULL);
		if (client_socket < 0) {
			perror("accept()");
			continue;
		}

		printf("got a connection\n");
		rc = pthread_create(&thread, NULL, handle_client, (void*)(intptr_t)client_socket);
		if (rc != 0) {
			/* pthread_create returns the error code, it doesn't set errno */
			fprintf(stderr, "pthread_create(): %s\n", strerror(rc));
			close(client_socket);
			continue;
		}

		/* nobody joins these, so let the system reclaim them when they finish */
		pthread_detach(thread);
	}

exit:
	close(tcp_socket);

	return ret;
}

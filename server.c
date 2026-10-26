#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include "string_ops.h"
#include "fs.h"

#define CRLF "\r\n"
#define SP " "

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

bool http_send_response(int socket, string_view header, string_view body) {
	if (!send_all(socket, header.start, header.len, MSG_MORE)) {
		return false;
	}
	return send_all(socket, body.start, body.len, 0);
}

int handle_client(int client_socket) {
	ssize_t n = 0;
	int ret = 0;
	char buf[1024];
	char header_buf[256]; /* separate from buf so we don't clobber the request */
	string_view hello_body = string_from_cstr("<h1>Hello World!</h1>");
	string_view bye_body = string_from_cstr("<h1>Bye!</h1>");
	string_view err_404 = string_from_cstr("<p>Error 404: Not Found</p><p><a href=\"/\">back to home</a></p>");

	printf("\n---\n");

	for (;;) {
		string_splits lines;
		http_req_line req_line;
		http_status result;
		string_view route_hello;
		string_view route_bye;

		memset(buf, 0, sizeof(buf));

		n = read(client_socket, buf, sizeof(buf)-1);
		if (n < 0) {
			perror("read(client)");
			close(client_socket);
			return -1;
		}

		if (n == 0) {
			printf("connection closed gracefully\n");
			break;
		}

		printf("REQUEST:\n%s", buf);

		lines = split_string(buf, (size_t)n, CRLF);

		if (lines.count < 1) {
			printf("ERROR: empty request\n");
			free_splits(&lines);
			close(client_socket);
			return -1;
		}

		req_line = http_req_line_init();
		result = parse_req_line(&req_line, lines.splits[0].start, lines.splits[0].len);
		free_splits(&lines);
		if (result != HTTP_RES_OK) {
			/* TODO: Return correct error + error page */
			printf("ERROR: failed to parse line\n");
			close(client_socket);
			return -1;
		}

		route_hello = string_from_cstr("/hello");
		route_bye = string_from_cstr("/bye");

		if (string_equal(&req_line.uri, &route_hello)) {
			if (!http_send_response(
					client_socket,
					http_response_generate(header_buf, sizeof(header_buf), HTTP_RES_OK, hello_body.len),
					hello_body)) {
				ret = -1;
			}
		} else if (string_equal(&req_line.uri, &route_bye)) {
			if (!http_send_response(
					client_socket,
					http_response_generate(header_buf, sizeof(header_buf), HTTP_RES_OK, bye_body.len),
					bye_body)) {
				ret = -1;
			}
		} else {
			printf("ERROR: unknown route \"%.*s\"\n", (int)req_line.uri.len, req_line.uri.start);
			(void)http_send_response(
					client_socket,
					http_response_generate(header_buf, sizeof(header_buf), HTTP_RES_NOT_FOUND, err_404.len),
					err_404);
			ret = -1;
		}

		break;
	}
	close(client_socket);
	printf("\n---\n");

	return ret;
}

const int PORT = 6969;

int main(void) {
	/* declare */
	int rc = 0;
	struct sockaddr_in bind_addr;
	int tcp_socket = 0;
	int ret = 0;
	int client_socket = 0;
	int enabled = true;

	const char* web_root = "./www";

	string_view web_root_view;
	web_root_view.start = web_root;
	web_root_view.len = strlen(web_root);

	fs_metadata web_root_meta = fs_get_metadata(web_root_view);
	if (!web_root_meta.exists) {
		mkdir(web_root,
				S_IEXEC | S_IWRITE | S_IREAD | S_IRGRP | S_IXGRP | S_IXOTH | S_IROTH);
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
		printf("waiting for connections...\n");
		client_socket = accept(tcp_socket, NULL, NULL);
		if (client_socket < 0) {
			perror("accept()");
			continue;
		}

		printf("got a connection\n");
		rc = handle_client(client_socket);
		/* ignore errors, dont care for now */
	}

exit:
	close(tcp_socket);

	return ret;
}

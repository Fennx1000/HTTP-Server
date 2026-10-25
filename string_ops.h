#ifndef STRING_OPS_H
#define STRING_OPS_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

/* TODO: This is the same as string_view */
typedef struct {
	const char* start;
	size_t len;
} string_view;

typedef struct {
	string_view* splits;
	size_t count;
	size_t capacity;
} string_splits;

typedef struct {
	const char* data;
	size_t len;
} string;

static inline bool string_equal(const string* l, const string* r) {
	return l->len == r->len && memcmp(l->data, r->data, l->len) == 0;
}

static inline string string_from_cstr(const char* str) {
	string s;
	s.len = strlen(str);
	s.data = str;
	return s;
}

static inline void string_trim_spaces(string* s) {
	/* left trim */
	while (s->len > 0 && *s->data == ' ') {
		s->data += 1;
		s->len -= 1;
	}

	/* right trim */
	while (s->len > 0 && s->data[s->len-1] == ' ') {
		s->len -= 1;
	}
}

static inline void splits_push(string_splits* splits, const char* start, size_t len) {
	if (splits->count == splits->capacity) {
		string_view* temp;
		splits->capacity *= 2;
		temp = (string_view*)realloc(splits->splits, sizeof(string_view) * splits->capacity);
		if (!temp) {
			perror("realloc()");
			abort();
		}
		splits->splits = temp;
	}

	splits->splits[splits->count].start = start;
	splits->splits[splits->count].len = len;
	splits->count += 1;
}

static inline string_splits split_string(const char* str, size_t len, const char* split_by) {
	string_splits result;
	const char* start = str;
	size_t split_by_len = strlen(split_by);
	size_t last_len;
	size_t i;

	result.capacity = 8;
	result.splits = (string_view*)calloc(result.capacity, sizeof(string_view));
	if (!result.splits) {
		perror("calloc()");
		abort();
	}
	result.count = 0;

	for (i = 0; i < len; ++i) {
		if ((i + split_by_len <= len) && memcmp(&str[i], split_by, split_by_len) == 0) {
			splits_push(&result, start, (size_t)(&str[i] - start));

			start = &str[i + split_by_len];
			i += split_by_len - 1;
		}
	}

	/* we might miss the last one if the input string doesn't end with the delimiter */
	last_len = (size_t)(&str[len] - start);
	if (last_len > 0) {
		splits_push(&result, start, last_len);
	}
	return result;
}

static inline void free_splits(string_splits* splits) {
	if (splits) {
		free(splits->splits);
		splits->splits = NULL;
		splits->count = 0;
		splits->capacity = 0;
	}
}

#endif /* STRING_OPS_H */

CC=cc
CFLAGS=-Wall -Wextra -pedantic -std=gnu99 -ggdb -Werror=return-type -flto

server: server.c
	${CC} ${CFLAGS} $^ -o $@

.PHONY: clean

clean:
	rm -f server

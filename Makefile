CC=cc
CFLAGS=-Wall -Wextra -pedantic -std=gnu99 -ggdb -Werror=return-type -03 -flto

server: server.c
	${CC} ${CFLAGS} $^ -o $@

.PHONY: clean

clean:
	rm -f server

CC = gcc
CFLAGS = -Wall -Wextra -g

cserver: cserver.o http_parser.o
	$(CC) $(CFLAGS) -o cserver cserver.o http_parser.o

cserver.o: cserver.c http_parser.h
	$(CC) $(CFLAGS) -c cserver.c

http_parser.o: http_parser.c http_parser.h
	$(CC) $(CFLAGS) -c http_parser.c

clean:
	rm -f cserver

.PHONY: clean
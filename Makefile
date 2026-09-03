CC = gcc
CFLAGS = -Wall -Wextra -g

server: server.c 
	$(CC) $(CFLAGS) -o server server.c 

clean:
	rm -rf server

.PHONY: clean
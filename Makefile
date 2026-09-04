CC = gcc
CFLAGS = -Wall -Wextra -g

server: cserver.c 
	$(CC) $(CFLAGS) -o cserver cserver.c 

clean:
	rm -rf cserver

.PHONY: clean
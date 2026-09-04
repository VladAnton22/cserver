#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BACKLOG 16

int main(void) {
    // Create TCP socket (IPv4, stream = TCP)
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // Allow immediate reuse of the port on restart
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); exit(1);
    }

    // Bind socket to 0.0.0.0:8080
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    // Start listening
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen"); exit(1);
    }

    printf("Listening on http://localhost:%d\n", PORT);

    // A valid minimal HTTP/1.1 response
    const char *response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Hello, world!";

        
        for (;;) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                perror("accept"); continue;
            }

            char buf[4096];
            read(client_fd, buf, sizeof(buf));

            write(client_fd, response, strlen(response));
            close(client_fd);
        }

        close(listen_fd);
        return 0;

}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BACKLOG 16

struct request {
    char method[8];
    char path[256];
    char version[16];
};

int parse_request(const char *buffer, ssize_t buffer_len, struct request *req) {
    int fields_matched = sscanf(buffer, "%7s %255s %15s", req->method, req->path, req->version);
    return fields_matched;
}

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

        // TODO: single read assumes whole request arrived - fix in Part 6
        for (;;) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                perror("accept"); continue;
            }

            char buf[4096];
            ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
            
            // Check data from read
            if (n < 0) {
                perror("read"); 
                close(client_fd);
                continue;
            } 
            else if (n == 0) {
                close(client_fd); continue;
            }

            buf[n] = '\0';  // add null terminator to the end of the message

            struct request parsed_request = {0};
            int fields_matched = parse_request(buf, n, &parsed_request);

            if (fields_matched != 3) {
                fprintf(stderr, "malformed request line, got %d fields\n", fields_matched);
                close(client_fd);
                continue;
            }

            printf("%s %s %s\n", parsed_request.method, parsed_request.path, parsed_request.version);

            write(client_fd, response, strlen(response));
            close(client_fd);
        }

        close(listen_fd);
        return 0;
}
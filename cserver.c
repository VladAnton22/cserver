#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

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

const char *reason_phrase(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        default:
            return "Unknown";
    }
}

void send_response(int client_fd,
                   int status_code,
                   const char *content_type,
                   const char *body,
                   size_t body_len
) {
    char head[1024];
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);

    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    int n = snprintf(
        head,
        sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        reason_phrase(status_code),
        content_type,
        body_len,
        date
    );

    if (n < 0 || (size_t)n >= sizeof(head)) {
        fprintf(stderr, "snprintf failed\n");
        return;
    }

    send(client_fd, head, n, 0);
    send(client_fd, body, body_len, 0);
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

        if (strcmp(parsed_request.path, "/") == 0) {
            char *body = "<h1>cserver</h1>";
            send_response(client_fd, 200, "text/html", body, strlen(body));
        } 
        else {
            char *body = "<h1>404 Not Found</h1>";
            send_response(client_fd, 404, "text/html", body, strlen(body));
        }

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include "http_parser.h"

#define PORT 8080
#define BACKLOG 16
#define WEBROOT "./www"

const char *reason_phrase(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "Unknown";
    }
}

const char *mime_type(const char *path) {
    char *last_slash = strrchr(path, '/');
    char *dot = strrchr(last_slash, '.');

    if (dot == NULL || (last_slash != NULL && dot < last_slash)) {
        return "application/octet-stream";
    }

    // MIME Table
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "text/javascript";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0) return "image/jpg";
    if (strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    return "application/octet-stream";
}

int resolve_path(const char *request_path, char *resolved_out) {
    // Strip query string
    char effective_path[256];
    char *query = strchr(request_path, '?');
    size_t len = query ? (size_t)(query - request_path) : strlen(request_path);
    if (len >= sizeof(effective_path)) {
        return 400;
    }
    memcpy(effective_path, request_path, len);
    effective_path[len] = '\0';

    // Default document
    if (strcmp(effective_path, "/") == 0) {
        strcpy(effective_path, "/index.html");
    }

    // Join with WEBROOT
    char filepath[512];
    int written = snprintf(filepath, sizeof(filepath), "%s%s", WEBROOT, effective_path);
    if (written < 0 || (size_t)written >= sizeof(filepath)) {
        return 400;
    }

    // Resolve + traversal check
    char resolved_root[PATH_MAX];
    if (realpath(WEBROOT, resolved_root) == NULL) {
        perror("realpath webroot");
        return 500;
    }
    if (realpath(filepath, resolved_out) == NULL) {
        return 404;
    }
    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_out, resolved_root, root_len) != 0 ||
        (resolved_out[root_len] != '\0' &&
         resolved_out[root_len] != '/')) {
        return 403;
    }

    return 200;
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

void send_error(int client_fd, int status_code) {
    switch (status_code)
    {
    case 400:
        send_response(client_fd, 400, "text/html", "<h1>400 Bad Request</h1>", 24);
        return;
    case 403:
        send_response(client_fd, 403, "text/html", "<h1>403 Forbidden</h1>", 22);
        return;
    case 404:
        send_response(client_fd, 404, "text/html", "<h1>404 Not Found</h1>", 22);
        return;
    case 500:
        send_response(client_fd, 500, "text/html", "<h1>500 Internal Server Error</h1>", 34);
        return;
    default:
        send_response(client_fd, status_code, "text/html", "<h1>Unknown Error</h1>", 22);
        return;
    }
}

void handle_connection(int client_fd) {
    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    
    // Check data from read
    if (n < 0) {
        perror("read"); 
        close(client_fd);
        return;
    } 
    else if (n == 0) {
        close(client_fd); return;
    }

    buf[n] = '\0';  // add null terminator to the end of the message

    struct request parsed_request = {0};
    int fields_matched = parse_request(buf, n, &parsed_request);

    if (fields_matched != 3) {
        fprintf(stderr, "malformed request line, got %d fields\n", fields_matched);
        close(client_fd);
        return;
    }

    printf("%s %s %s\n", parsed_request.method, parsed_request.path, parsed_request.version);

    // Resolve path
    char resolved[PATH_MAX];
    int status = resolve_path(parsed_request.path, resolved);
    if (status != 200) {
        send_error(client_fd, status);
        close(client_fd);
        return;
    }

    // Open file
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        send_error(client_fd, 404);
        close(client_fd);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        close(client_fd);
        return;
    }
    // Check if it is a regular file
    if (!S_ISREG(st.st_mode)) {
        send_error(client_fd, 404);
        close(fd);
        close(client_fd);
        return;
    }

    // Read file
    char *filebuf = malloc(st.st_size);
    if (filebuf == NULL) {
        perror("malloc");
        close(fd);
        close(client_fd);
        return;
    }

    size_t total_read = 0;

    while(total_read < (size_t)st.st_size) {
        ssize_t bytes = read(fd, filebuf + total_read, st.st_size - total_read);
        if (bytes < 0) {
            perror("read");
            break;
        }
        if (bytes == 0) {
            break;
        }
        total_read += bytes;
    }
    if (total_read != (size_t)st.st_size) {
        fprintf(stderr, "incomplete read of %s\n", resolved);
        free(filebuf);
        close(fd);
        close(client_fd);
        return;
    }

    send_response(client_fd, 200, mime_type(resolved), filebuf, st.st_size);
    free(filebuf);
    close(fd);
    close(client_fd);
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
        handle_connection(client_fd);
    }

    close(listen_fd);
    return 0;
}
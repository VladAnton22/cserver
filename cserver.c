#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <errno.h>
#include <sys/epoll.h>

#include "http_parser.h"

#define PORT 8080
#define BACKLOG 16
#define MAX_EVENTS 64
#define WEBROOT "./www"
#define MAX_FILE_SIZE (8 * 1024 * 1024)

enum state {
    READING_HEADERS, READING_BODY, WRITING
};
struct connection {
    int fd;
    enum state state;
    char read_buf[8192];
    size_t read_len;
    struct request request;
    size_t content_length;
    size_t body_received;
    char *write_buf;
    size_t write_len;
    size_t write_sent;
};

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
        case 413:
            return "Payload Too Large";
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
    if (strcmp(dot, ".jpg") == 0) return "image/jpeg";
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

char *scan_segment(const char *buffer, size_t total) {
    for (size_t i = 0; i + 3 < total; i++) {
        if (buffer[i] == '\r' &&
            buffer[i+1] == '\n' &&
            buffer[i+2] == '\r' &&
            buffer[i+3] == '\n') {

            return (char *)&buffer[i];
            }
    }
    return NULL;
}

int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;
    
    return 0;
}

void accept_new_connections(int epollfd, int listen_fd) {
    for(;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        // Accept connection
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            else {
                perror("accept");
                return;
            }
        }
        if (make_nonblocking(client_fd) < 0) {
            perror("fcntl");
            close(client_fd);
            return;
        }
        // Initialise connection struct to 0
        struct connection *conn = calloc(1, sizeof(struct connection));
        if (conn == NULL) {
            perror("calloc");
            close(client_fd);
            return;
        }
        conn->fd = client_fd;
        conn->state = READING_HEADERS;

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = conn;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl: client_fd");
            close(client_fd);
            free(conn);
            return;
        }
        printf("registered client fd %d\n", client_fd);
    }
}

void close_connection(struct connection *conn) {
    close(conn->fd);
    free(conn->write_buf);
    free(conn);
    return;
}

void begin_writing(struct connection *conn, int epollfd) {
    conn->write_sent = 0;
    conn->state = WRITING;

    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.ptr = conn;
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
        perror("epoll_ctl: mod");
        close_connection(conn);
        return;
    }
}

int queue_response(struct connection *conn, 
                    int status,
                    const char *content_type,
                    const char *body,
                    size_t body_len
) {
    char head[1024];
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);

    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    // Set up headers
    int header_n = snprintf(
        head,
        sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        reason_phrase(status),
        content_type,
        body_len,
        date
    );

    if (header_n < 0 || (size_t)header_n >= sizeof(head)) {
        fprintf(stderr, "snprintf failed\n");
        return -1;
    }

    // Save headers and body to write buffer
    free(conn->write_buf);
    conn->write_buf = malloc(header_n + body_len);
    if (conn->write_buf == NULL) {
        return -1;
    }
    memcpy(conn->write_buf, head, header_n);
    memcpy(conn->write_buf + header_n, body, body_len);
    conn->write_len = header_n + body_len;
    return 0;
}

void queue_error(struct connection *conn, int status, int epollfd) {
    const char *body;

    switch (status)
    {
    case 400:
        body = "<h1>400 Bad Request</h1>";
        break;
    case 403:
        body = "<h1>403 Forbidden</h1>";
        break;
    case 404:
        body = "<h1>404 Not Found</h1>";
        break;
    case 413:
        body = "<h1>413 Payload Too Large</h1>";
        break;
    case 500:
        body = "<h1>500 Internal Server Error</h1>";
        break;
    default:
        body = "<h1>Unknown Error</h1>";
        break;
    }

    int response_status = queue_response(conn, status, "text/html", body, strlen(body));
    if (response_status != 0) {
        fprintf(stderr, "queue_response_failed\n");
        close_connection(conn);
        return;
    }
    begin_writing(conn, epollfd);
    return;
}

void start_response(int epollfd, struct connection *conn) {
    // Resolve path
    char resolved[PATH_MAX];
    int status = resolve_path(conn->request.path, resolved);
    if (status != 200) {
        queue_error(conn, status, epollfd);
        return;
    }

    // Open file
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        queue_error(conn, 404, epollfd);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        queue_error(conn, 500, epollfd);
        return ;
    }
    // Check if it is a regular file
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        queue_error(conn, 404, epollfd);
        return;
    }
    // Check if file size is too big
    if (st.st_size > MAX_FILE_SIZE) {
        close(fd);
        queue_error(conn, 500, epollfd);
        return;
    }

    // Read file
    size_t body_read = 0;
    char *body = malloc((size_t)st.st_size + 1);
    if (body == NULL) {
        close(fd);
        queue_error(conn, 500, epollfd);
        return;
    }

    while(body_read < (size_t)st.st_size) {
        ssize_t bytes = read(fd, body + body_read, st.st_size - body_read);
        if (bytes < 0) {
            perror("read");
            break;
        }
        if (bytes == 0) {
            break;
        }
        body_read += bytes;
    }
    close(fd);
    
    if (body_read != (size_t)st.st_size) {
        free(body);
        queue_error(conn, 500, epollfd);
        return;
    }

    int response_status = queue_response(conn, 200, mime_type(resolved), body, st.st_size);
    free(body);

    if (response_status != 0) {
        queue_error(conn, 500, epollfd);
        return;
    }

    begin_writing(conn, epollfd);
    return;
}

const char *find_header(struct request *request, char *header_name) {
    for (size_t i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, header_name) == 0) {    // HTTP headers are case insensitive
            return request->headers[i].value;
        }
    }
    return NULL;
}

void handle_reading_headers(int epollfd, struct connection *conn) {
    // read into read_buf where we left off
    ssize_t n = read(conn->fd, conn->read_buf + conn->read_len, sizeof(conn->read_buf) - 1 - conn->read_len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    } else if (n < 0) {
        close_connection(conn);
        return;
    }
    else if (n == 0) {
        close_connection(conn);
        return;
    }
    conn->read_len += n;

    char *end = scan_segment(conn->read_buf, conn->read_len);
    conn->read_buf[conn->read_len] = '\0';
    if (end == NULL) {
        if (conn->read_len == sizeof(conn->read_buf) - 1) {
            queue_error(conn, 413, epollfd);
            return;
        }
        return;   // room left - wait for next event
    }
    int parse_status = parse(conn->read_buf, conn->read_len, &conn->request);
    if (parse_status != 200) {
        queue_error(conn, parse_status, epollfd);
        return;
    }
    // Find body length to prepare for next state (READING_BODY)
    const char *content_length = find_header(&conn->request, "Content-Length");
    if (content_length == NULL) {
        start_response(epollfd, conn);  // no body: go to WRITING
        return;
    }
    if (content_length[0] == '-') {
        queue_error(conn, 400, epollfd);
        return;
    }

    // Content-Length is valid
    errno = 0;
    char *endptr;
    unsigned long cl = strtoul(content_length, &endptr, 10);
    if (endptr == content_length) {     // consumed no digits at all
        queue_error(conn, 400, epollfd);
        return;
    }
    if (*endptr != '\0') {   // leftover char after number
        queue_error(conn, 413, epollfd);
        return;
    }
    if (errno == ERANGE) {      // number overflowed
        queue_error(conn, 413, epollfd);
        return;
    }

    conn->content_length = cl;
    size_t body_offset = end + 4 - conn->read_buf;
    if (body_offset + conn->content_length > sizeof(conn->read_buf) - 1) {
        queue_error(conn, 413, epollfd);
        return;
    }

    conn->body_received = conn->read_len - (body_offset);
    if (conn->body_received >= conn->content_length) {
        start_response(epollfd, conn);
    }
    else {
        conn->state = READING_BODY;
    }
}

void handle_reading_body(int epollfd, struct connection *conn) {
    ssize_t n = read(conn->fd, conn->read_buf + conn->read_len, sizeof(conn->read_buf) - 1 - conn->read_len);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    } 
    else if (n < 0) {
        close_connection(conn);
        return;
    }
    else if (n == 0) {
        close_connection(conn);
        return;
    }
    conn->read_len += n;
    conn->body_received += n;

    if (conn->body_received >= conn->content_length) {
        start_response(epollfd, conn);
    }
}

void handle_writing(struct connection *conn) {
    ssize_t n = send(conn->fd, conn->write_buf + conn->write_sent, conn->write_len - conn->write_sent, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    } 
    else if (n < 0) {
        close_connection(conn);
        return;
    }
    else if (n >= 0) {
        conn->write_sent += n;
    }
    if (conn->write_sent == conn->write_len) {
            close_connection(conn);
            return;
        }
    return;
}

void handle_client(int epollfd, struct connection *conn) {
    switch (conn->state) {
        case READING_HEADERS: {
            handle_reading_headers(epollfd, conn);
            break;
        }
        case READING_BODY: {
            handle_reading_body(epollfd, conn);
            break;
        }
        case WRITING: {
            handle_writing(conn);
        }
    }
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
    if (make_nonblocking(listen_fd) < 0) {
        perror("fcntl"); exit(1);
    }

    printf("Listening on http://localhost:%d\n", PORT);

    // Create epoll instance and events struct
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        perror("epoll_create1");
        exit(1);
    }
    struct epoll_event events[MAX_EVENTS];

    // Register the listen socket
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;     // sentinel for listen socket
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl: listen_fd");
        exit(1);
    }
    
    int nfds;
    for (;;) {
        // Block to get ready fds
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait");
            exit(1);
        }

        for (int n = 0; n < nfds; ++n) {
            // Check if listen socket, else -> handle_client
            if (events[n].data.ptr == NULL) {
                accept_new_connections(epollfd, listen_fd);
            }
            else {
                struct connection *conn = events[n].data.ptr;
                handle_client(epollfd, conn);
            }
        }
    }

    close(listen_fd);
    return 0;
}
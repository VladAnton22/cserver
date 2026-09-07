#include <unistd.h>

#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#define MAX_HEADERS 32
#define MAX_HEADER_NAME 64
#define MAX_HEADER_VALUE 1024

struct header {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct request {
    char method[8];
    char path[256];
    char version[16];

    struct header headers[MAX_HEADERS];
    size_t header_count;
};

int parse_request(const char *buffer, ssize_t buffer_len, struct request *req);

#endif
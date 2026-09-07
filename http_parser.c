#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "http_parser.h"

int parse_request(const char *buffer, ssize_t buffer_len, struct request *req) {
    int fields_matched = sscanf(buffer, "%7s %255s %15s", req->method, req->path, req->version);
    return fields_matched;
}


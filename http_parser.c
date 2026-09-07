#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "http_parser.h"

static int parse_request(const char *buffer, struct request *req) {
    int fields_matched = sscanf(buffer, "%7s %255s %15s", req->method, req->path, req->version);
    return (fields_matched == 3) ? 0 : -1;
}

static int parse_headers(const char *headers_start, size_t len, struct request *req) {
    const char *cursor = headers_start;
    const char *end = headers_start + len;
    req->header_count = 0;

    while (cursor < end) {
        // Terminator check
        if (end - cursor >= 2 && cursor[0] == '\r' && cursor[1] == '\n') {
            return 0;
        }

        // Find this line's end
        const char *line_end = strstr(cursor, "\r\n");
        if (line_end == NULL) {
            return -1;
        }

        // Find ':' that splits name from value
        const char *colon = memchr(cursor, ':', line_end - cursor);
        if (colon == NULL) {
            return -1;
        }

        // Extract name and value
        size_t name_len = colon - cursor;

        const char *value_start = colon + 1;
        while (value_start < line_end && (*value_start == ' ' || *value_start == '\t')) {
            value_start++;
        }
        size_t value_len = line_end - value_start;

        if ((req->header_count < MAX_HEADERS) && (name_len < MAX_HEADER_NAME) && (value_len < MAX_HEADER_VALUE)) {
            struct header *h = &req->headers[req->header_count];

            memcpy(h->name, cursor, name_len);
            h->name[name_len] = '\0';

            memcpy(h->value, value_start, value_len);
            h->value[value_len] = '\0';

            req->header_count++;
        } else {
            return -1;
        }
        
        // Advance cursor past "\r\n"
        cursor = line_end + 2;
    }
    return -1;
}

int parse(const char *buffer, size_t buffer_len, struct request *req) {
    int request_result = parse_request(buffer, req);
    if (request_result != 0) {
        return 400;
    }

    // Get the start of headers
    char *crlf = strstr(buffer, "\r\n");
    if (crlf == NULL) {
        return 400;
    }
    size_t request_line_len = crlf - buffer;
    size_t offset = request_line_len + 2;

    int header_result = parse_headers(buffer + offset, buffer_len - offset, req);
    if (header_result != 0) {
        return 400;
    }

    return 200;
}
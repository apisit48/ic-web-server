#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Given a char buffer, returns the parsed request headers.
 */
Request* parse(char *buffer, int size, int socketFd) {

    enum {
        STATE_START = 0, STATE_CR, STATE_CRLF, STATE_CRLFCR, STATE_CRLFCRLF
    };

    int i = 0, state;
    size_t offset = 0;
    char ch;
    char buf[8192];
    memset(buf, 0, 8192);

    state = STATE_START;
    while (state != STATE_CRLFCRLF) {
        char expected = 0;

        if (i == size)
            break;

        ch = buffer[i++];
        
        if (offset >= sizeof(buf) - 1) {
            fprintf(stderr, "Buffer overflow detected\n");
            return NULL;
        }

        buf[offset++] = ch;

        switch (state) {
            case STATE_START:
            case STATE_CRLF:
                expected = '\r';
                break;
            case STATE_CR:
            case STATE_CRLFCR:
                expected = '\n';
                break;
            default:
                state = STATE_START;
                continue;
        }

        if (ch == expected)
            state++;
        else
            state = STATE_START;
    }

    if (state != STATE_CRLFCRLF) {
        fprintf(stderr, "Debug: Malformed request\n");
        return NULL;
    }


    Request *request = (Request *)malloc(sizeof(Request));
    if (!request) {
        perror("Failed to allocate memory for request");
        return NULL;
    }

    memset(request, 0, sizeof(Request));
    request->header_count = 0;

    int initial_header_capacity = 10;
    request->headers = (Request_header *)malloc(initial_header_capacity * sizeof(Request_header));
    if (!request->headers) {
        perror("Failed to allocate memory for request headers");
        free(request);
        return NULL;
    }

    yyrestart(NULL);
    set_parsing_options(buf, i, request);

    if (yyparse() != SUCCESS) {
        fprintf(stderr, "Debug: Failed to parse request\n");
        free(request->headers);
        free(request);
        return NULL;
    }

    return request;
}

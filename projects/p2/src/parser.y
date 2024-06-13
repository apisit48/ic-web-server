/**
 * @file parser.y
 * @brief Grammar for HTTP
 * @author Rajul Bhatnagar (2016)
 */

%{
#include "parse.h"

/* Define YACCDEBUG to enable debug messages for this lex file */
//#define YACCDEBUG
#define YYERROR_VERBOSE
#ifdef YACCDEBUG
#include <stdio.h>
#define YPRINTF(...) printf(__VA_ARGS__)
#else
#define YPRINTF(...)
#endif

void yyerror (const char *s);
void set_parsing_options(char *buf, size_t siz, Request *parsing_request);
extern int yylex();

char *parsing_buf;
int parsing_offset;
size_t parsing_buf_siz;
Request *parsing_request;

%}

%union {
    char str[8192];
    int i;
}

%start request

%token t_crlf
%token t_backslash
%token t_slash
%token t_digit
%token t_dot
%token t_token_char
%token t_lws
%token t_colon
%token t_separators
%token t_sp
%token t_ws
%token t_ctl

%type<str> t_crlf
%type<i> t_backslash
%type<i> t_slash
%type<i> t_digit
%type<i> t_dot
%type<i> t_token_char
%type<str> t_lws
%type<i> t_colon
%type<i> t_separators
%type<i> t_sp
%type<str> t_ws
%type<i> t_ctl

%type<i> allowed_char_for_token
%type<i> allowed_char_for_text
%type<str> ows
%type<str> token
%type<str> text

%%

allowed_char_for_token:
    t_token_char |
    t_digit { $$ = '0' + $1; } |
    t_dot;

token:
    allowed_char_for_token {
        YPRINTF("token: Matched rule 1.\n");
        snprintf($$, 8192, "%c", $1);
    } |
    token allowed_char_for_token {
        YPRINTF("token: Matched rule 2.\n");
        snprintf($$ + strlen($1), 8192 - strlen($1), "%c", $2);
    };

allowed_char_for_text:
    allowed_char_for_token |
    t_separators { $$ = $1; } |
    t_colon { $$ = $1; } |
    t_slash { $$ = $1; };

text: allowed_char_for_text {
    YPRINTF("text: Matched rule 1.\n");
    snprintf($$, 8192, "%c", $1);
} |
text ows allowed_char_for_text {
    YPRINTF("text: Matched rule 2.\n");
    snprintf($$ + strlen($1) + strlen($2), 8192 - strlen($1) - strlen($2), "%c", $3);
};

ows:
    /* Empty */ {
        YPRINTF("OWS: Matched rule 1\n");
        $$[0] = 0;
    } |
    t_sp {
        YPRINTF("OWS: Matched rule 2\n");
        snprintf($$, 8192, "%c", $1);
    } |
    t_ws {
        YPRINTF("OWS: Matched rule 3\n");
        snprintf($$, 8192, "%s", $1);
    };

request_line: token t_sp text t_sp text t_crlf {
    YPRINTF("Debug: Parsing Request Line: Method=%s, URI=%s, Version=%s\n", $1, $3, $5);
    strcpy(parsing_request->http_method, $1);
    strcpy(parsing_request->http_uri, $3);
    strcpy(parsing_request->http_version, $5);
};

single_header: token ows t_colon ows text t_crlf {
    YPRINTF("Debug: Parsing Header: Name=%s, Value=%s\n", $1, $5);
    strcpy(parsing_request->headers[parsing_request->header_count].header_name, $1);
    strcpy(parsing_request->headers[parsing_request->header_count].header_value, $5);
    parsing_request->header_count++;
};

request_header: single_header | request_header single_header;

request: request_line request_header t_crlf {
    YPRINTF("parsing_request: Matched Success.\n");
    return SUCCESS;
};

%%

void set_parsing_options(char *buf, size_t siz, Request *request) {
    parsing_buf = buf;
    parsing_offset = 0;
    parsing_buf_siz = siz;
    parsing_request = request;
}

void yyerror (const char *s) {
    fprintf(stderr, "Debug: Parser Error: %s\n", s);
    fprintf(stderr, "%s\n", s);
}

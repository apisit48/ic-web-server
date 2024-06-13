#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SUCCESS 0

//Header field
typedef struct
{
	char header_name[4096];
	char header_value[4096];
} Request_header;

//HTTP Request Header
typedef struct
{
	char http_version[50];
	char http_method[50];
	char http_uri[4096];
	Request_header *headers;
	int header_count;
	char query_string[4096];  // Add query string field
    char content_length[4096];  // Add content length field
    char content_type[4096];    // Add content type field
    char *body;               // For storing the body of POST requests
    size_t body_length;       // Length of the body
} Request;

Request* parse(char *buffer, int size,int socketFd);

// functions decalred in parser.y
int yyparse();
void set_parsing_options(char *buf, size_t i, Request *request);
// to allow resetting the parser the request failed to properly parse
void yyrestart(FILE *input_file);
void free_request(Request *request); 

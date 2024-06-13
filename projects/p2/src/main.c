#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <ctype.h>
#include <sys/wait.h>
#include <arpa/inet.h> 
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include "thread_pool.h"
#include "parse.h"

#define DEFAULT_PORT 8080
#define MAX_BACKLOG 10
#define BUFFER_SIZE 8192
#define DEFAULT_NUM_THREADS 4
#define DEFAULT_QUEUE_CAPACITY 100
#define DEFAULT_TIMEOUT_DURATION 5000

void signal_handler(int signum);
void start_server(int port, const char *wwwroot, ThreadPool *threadPool);
void handle_connection(int sock, const char *wwwroot, int timeout, const char *cgi_base_path, const char *client_ip, int server_port);
const char* get_content_type(const char *path);
void send_response(int sock, const char *status, const char *content_type, const char *body, size_t body_length);
void enqueue_work(WorkQueue* queue, int socket_fd);
WorkItem dequeue_work(WorkQueue* queue);
void handle_cgi_request(int sock, const char *cgi_script_path, Request *request, const char *client_ip, int server_port);

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    char *wwwroot = NULL;
    int numThreads = DEFAULT_NUM_THREADS;
    int timeout = DEFAULT_TIMEOUT_DURATION; 
    char *cgi_script_path = NULL;

    struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"root", required_argument, 0, 'r'},
        {"numThreads", required_argument, 0, 'n'},
        {"timeout", required_argument, 0, 't'},
        {"cgiHandler", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:r:n:t:c:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                wwwroot = strdup(optarg);
                break;
            case 'n':
                numThreads = atoi(optarg);
                break;
            case 't':
                timeout = atoi(optarg) * 1000; 
                break;
            case 'c':
                cgi_script_path = strdup(optarg);
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    if (!wwwroot) {
        fprintf(stderr, "wwwroot directory is required\n");
        exit(EXIT_FAILURE);
    }
    if (cgi_script_path == NULL) {
        fprintf(stderr, "CGI handler script path is required\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, signal_handler);

    ThreadPool *threadPool = malloc(sizeof(ThreadPool));
    if (!threadPool) {
        perror("Failed to allocate memory for ThreadPool");
        exit(EXIT_FAILURE);
    }

    threadPool->work_queue = malloc(sizeof(WorkQueue));
    if (!threadPool->work_queue) {
        perror("Failed to allocate memory for WorkQueue");
        exit(EXIT_FAILURE);
    }

    init_work_queue(threadPool->work_queue, DEFAULT_QUEUE_CAPACITY);
    init_thread_pool(threadPool, numThreads, threadPool->work_queue, wwwroot, timeout, cgi_script_path);

    start_server(port, wwwroot, threadPool);

    free(wwwroot);
    for (int i = 0; i < numThreads; ++i) {
        pthread_join(threadPool->threads[i], NULL);
    }
    free(threadPool->work_queue->items);
    free(threadPool->work_queue);
    free(threadPool);

    if (cgi_script_path) {
        free(cgi_script_path);
    }

    return 0;
}


void init_work_queue(WorkQueue* queue, int capacity) {
    queue->items = (WorkItem*)malloc(sizeof(WorkItem) * capacity);
    queue->capacity = capacity;
    queue->count = 0;
    queue->front = 0;
    queue->rear = -1;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_var, NULL);
}


void* worker_thread(void* arg) {
    WorkerArgs *workerArgs = (WorkerArgs *)arg;
    WorkQueue *queue = workerArgs->workQueue;
    char *wwwRoot = workerArgs->wwwRoot;
    int timeout = workerArgs->timeout;
    char *cgi_script_path = workerArgs->cgi_script_path;

    while (1) {
        pthread_mutex_lock(&queue->mutex);

        while (queue->count == 0) {
            pthread_cond_wait(&queue->cond_var, &queue->mutex);
        }

        WorkItem item = dequeue_work(queue);
        pthread_mutex_unlock(&queue->mutex);

        struct pollfd fd;
        fd.fd = item.socket_fd;
        fd.events = POLLIN;
        int ret = poll(&fd, 1, timeout);

        if (ret > 0) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            getpeername(item.socket_fd, (struct sockaddr *)&addr, &addr_len);
            int server_port = ntohs(addr.sin_port);

            handle_connection(item.socket_fd, wwwRoot, timeout, cgi_script_path, inet_ntoa(addr.sin_addr), server_port);
        } else if (ret == 0) {
            printf("Connection timed out (socket fd: %d).\n", item.socket_fd);
            send_response(item.socket_fd, "408 Request Timeout", "text/html", "<h1>408 Request Timeout</h1>", 26);
        } else {
            perror("Poll error");
        }

        close(item.socket_fd);
    }

    free(wwwRoot);
    if (cgi_script_path) {
        free(cgi_script_path);
    }
    free(workerArgs);

    return NULL;
}


void init_thread_pool(ThreadPool* pool, int num_threads, WorkQueue* queue, char *wwwRoot, int timeout, char *cgi_script_path) {
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
    pool->thread_count = num_threads;
    pool->work_queue = queue;

    for (int i = 0; i < num_threads; ++i) {
        WorkerArgs *workerArgs = malloc(sizeof(WorkerArgs));
        workerArgs->workQueue = queue;
        workerArgs->wwwRoot = strdup(wwwRoot);
        workerArgs->timeout = timeout;
        workerArgs->cgi_script_path = cgi_script_path ? strdup(cgi_script_path) : NULL;

        pthread_create(&pool->threads[i], NULL, worker_thread, workerArgs);
    }
}



void enqueue_work(WorkQueue* queue, int socket_fd) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->count < queue->capacity) {
        queue->rear = (queue->rear + 1) % queue->capacity;
        queue->items[queue->rear].socket_fd = socket_fd;
        queue->count++;

        pthread_cond_signal(&queue->cond_var);
    }

    pthread_mutex_unlock(&queue->mutex);
}

WorkItem dequeue_work(WorkQueue* queue) {
    WorkItem item = queue->items[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->count--;

    return item;
}

void free_request(Request *request) {
    if (request != NULL) {
        if (request->headers) {
            free(request->headers); 
        }
        if (request->body) {
            free(request->body); 
        }
        free(request);
    }
}


void signal_handler(int signum) {
    printf("\nReceived signal %d. Shutting down...\n", signum);
    exit(signum);
}

void start_server(int port, const char *wwwroot, ThreadPool *threadPool){
    int sockfd, newsockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t clilen;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR on binding");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    listen(sockfd, MAX_BACKLOG);
    printf("Server is listening on port %d...\n", port);

    clilen = sizeof(client_addr);
    while (1) {
        newsockfd = accept(sockfd, (struct sockaddr *)&client_addr, &clilen);
        if (newsockfd < 0) {
            perror("ERROR on accept");
            continue;
        }

        enqueue_work(threadPool->work_queue, newsockfd);
    }

    close(sockfd);
}

void handle_connection(int sock, const char *wwwroot, int timeout, const char *cgi_base_path, const char *client_ip, int server_port) {
    char buffer[BUFFER_SIZE];
    int nbytes;

    nbytes = read(sock, buffer, BUFFER_SIZE - 1);
    if (nbytes < 0) {
        perror("ERROR reading from socket");
        return;
    }

    buffer[nbytes] = '\0';

    Request *request = parse(buffer, nbytes, sock);
    if (request == NULL) {
        send_response(sock, "400 Bad Request", "text/html", "<h1>400 Bad Request</h1>", 25);
        return;
    }

    if (strcmp(request->http_method, "GET") != 0 && strcmp(request->http_method, "HEAD") != 0 && strcmp(request->http_method, "POST") != 0) {
        send_response(sock, "501 Not Implemented", "text/html", "<h1>501 Not Implemented</h1>", 27);
        free_request(request);
        return;
    }

    if (strcmp(request->http_version, "HTTP/1.1") != 0) {
        send_response(sock, "505 HTTP Version Not Supported", "text/html", "<h1>505 HTTP Version Not Supported</h1>", 37);
        free_request(request);
        return;
    }

    if (strncmp(request->http_uri, "/cgi/", 5) == 0) {
        char cgi_script_path[4096];
        snprintf(cgi_script_path, sizeof(cgi_script_path), "%s%s", cgi_base_path, request->http_uri + 5);
        printf("CGI script path: %s\n", cgi_script_path);
        handle_cgi_request(sock, cgi_script_path, request, client_ip, server_port);
    } 
    
    else {
        char filepath[8192];
        snprintf(filepath, sizeof(filepath), "%s%s", wwwroot, request->http_uri);

        FILE *file = fopen(filepath, "rb");
        if (file == NULL) {
            send_response(sock, "404 Not Found", "text/html", "<h1>404 Not Found</h1>", 22);
        } 
        
        else {
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);

            char *file_content = malloc(file_size);
            if (file_content == NULL) {
                send_response(sock, "500 Internal Server Error", "text/html", "<h1>500 Internal Server Error</h1>", 30);
                fclose(file);
            } 
            
            else {
                fread(file_content, 1, file_size, file);
                fclose(file);
                send_response(sock, "200 OK", get_content_type(filepath), file_content, file_size);
                free(file_content);
            }
        }
    }

    free_request(request);
}

void handle_cgi_request(int sock, const char *cgi_script_path, Request *request, const char *client_ip, int server_port) {
    int c2pFds[2]; 
    int p2cFds[2]; 
    char buffer[4096];
    ssize_t nread;

    if (pipe(c2pFds) == -1 || pipe(p2cFds) == -1) {
        perror("pipe");
        send_response(sock, "500 Internal Server Error", "text/html", "<h1>500 Internal Server Error</h1>", 30);
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        send_response(sock, "500 Internal Server Error", "text/html", "<h1>500 Internal Server Error</h1>", 30);
        close(c2pFds[0]);
        close(c2pFds[1]);
        close(p2cFds[0]);
        close(p2cFds[1]);
        return;
    }

    if (pid == 0) { 
        close(c2pFds[0]);
        close(p2cFds[1]);
        if (dup2(p2cFds[0], STDIN_FILENO) == -1 || dup2(c2pFds[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            exit(EXIT_FAILURE);
        }
        close(p2cFds[0]);
        close(c2pFds[1]);

        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("REQUEST_METHOD", request->http_method, 1);
        setenv("QUERY_STRING", request->query_string, 1);
        setenv("CONTENT_LENGTH", request->content_length, 1);
        setenv("CONTENT_TYPE", request->content_type, 1);
        setenv("REMOTE_ADDR", client_ip, 1);
        setenv("REQUEST_URI", request->http_uri, 1);
        char server_port_str[6];
        snprintf(server_port_str, sizeof(server_port_str), "%d", server_port);
        setenv("SERVER_PORT", server_port_str, 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("SERVER_SOFTWARE", "MyHTTPServer/1.0", 1);

        char script_name[8192];
        char *query_string_start = strchr(cgi_script_path, '?');
        if (query_string_start) {
            int script_name_len = query_string_start - cgi_script_path;
            strncpy(script_name, cgi_script_path, script_name_len);
            script_name[script_name_len] = '\0';
            setenv("QUERY_STRING", query_string_start + 1, 1); 
        } else {
            strncpy(script_name, cgi_script_path, sizeof(script_name));
            setenv("QUERY_STRING", "", 1);
        }

        setenv("SCRIPT_NAME", script_name, 1);
        setenv("PATH_INFO", "", 1); 

        for (int i = 0; i < request->header_count; i++) {
            if (strcasecmp(request->headers[i].header_name, "Accept") == 0) {
                setenv("HTTP_ACCEPT", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Referer") == 0) {
                setenv("HTTP_REFERER", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Accept-Encoding") == 0) {
                setenv("HTTP_ACCEPT_ENCODING", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Accept-Language") == 0) {
                setenv("HTTP_ACCEPT_LANGUAGE", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Accept-Charset") == 0) {
                setenv("HTTP_ACCEPT_CHARSET", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Host") == 0) {
                setenv("HTTP_HOST", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Cookie") == 0) {
                setenv("HTTP_COOKIE", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "User-Agent") == 0) {
                setenv("HTTP_USER_AGENT", request->headers[i].header_value, 1);
            } else if (strcasecmp(request->headers[i].header_name, "Connection") == 0) {
                setenv("HTTP_CONNECTION", request->headers[i].header_value, 1);
            }
        }

        char *base_name = strrchr(script_name, '/');
        if (base_name) {
            base_name++; 
        } else {
            base_name = script_name;
        }

        execl(script_name, base_name, NULL);
        perror("execl");
        exit(EXIT_FAILURE);
    } 
    else { 
        close(c2pFds[1]);
        close(p2cFds[0]);

        if (strcmp(request->http_method, "POST") == 0) {
            write(p2cFds[1], request->body, request->body_length);
        }
        close(p2cFds[1]);

        while ((nread = read(c2pFds[0], buffer, sizeof(buffer))) > 0) {
            send(sock, buffer, nread, 0);
        }
        close(c2pFds[0]);

        waitpid(pid, NULL, 0);
    }
}


const char* get_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "text/plain";

    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    else if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    else if (strcmp(dot, ".css") == 0) return "text/css";
    else if (strcmp(dot, ".js") == 0) return "application/javascript";
    else if (strcmp(dot, ".png") == 0) return "image/png";
    else if (strcmp(dot, ".js") == 0) return "text/javascript";
    else if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpg";
    else if (strcmp(dot, ".gif") == 0) return "image/gif";
    else return "text/plain";
}

void send_response(int sock, const char *status, const char *content_type, const char *body, size_t body_length) {
    char header[2048];
    time_t now = time(0);
    struct tm *gmt = gmtime(&now);
    char date[128];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Date: %s\r\n"
        "Server: MyHTTPServer/1.0 (Unix)\r\n"
        "Connection: close\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        status, date, content_type, body_length);

    write(sock, header, header_length);
    if (body && body_length > 0) {
        write(sock, body, body_length);
    }
}

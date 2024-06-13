#include <pthread.h>

typedef struct {
    int socket_fd;
} WorkItem;

typedef struct {
    WorkItem* items;
    int capacity;
    int count;
    int front;
    int rear;
    pthread_mutex_t mutex;
    pthread_cond_t cond_var;
} WorkQueue;

typedef struct {
    pthread_t* threads;
    int thread_count;
    WorkQueue* work_queue;
} ThreadPool;

typedef struct {
    WorkQueue *workQueue;
    char *wwwRoot;
    int timeout;
    char* cgi_script_path;
    int server_port;  
} WorkerArgs;


void init_work_queue(WorkQueue* queue, int capacity);
void init_thread_pool(ThreadPool* pool, int num_threads, WorkQueue* queue, char *wwwRoot, int timeout, char *cgi_script_path);
void* worker_thread(void* arg);

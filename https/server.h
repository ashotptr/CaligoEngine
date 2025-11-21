#define _GNU_SOURCE
#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <jansson.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define PORT 8080
#define RADIO_PORT 9001
#define BUFFER_SIZE 4096
#define NUM_WORKER_THREADS 8
#define MAX_EPOLL_EVENTS 64
#define MAX_ROUTES 32

typedef enum {
    STATE_SSL_HANDSHAKE,
    STATE_READ_REQUEST,
    STATE_PROXYING
} ClientConnState;

typedef struct ClientState {
    int fd;
    SSL* ssl;
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    ClientConnState state;
    struct ClientState* peer;
    
    char* pending_write_data;
    size_t pending_write_len;
    FILE* file_stream;
    int is_cgi;
    int ssl_want_write;
} ClientState;


typedef struct Task {
    ClientState* client;
    struct Task* next;
} Task;

typedef struct {
    Task* head;
    Task* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TaskQueue;

typedef enum {
    ROUTE_STATIC,
    ROUTE_CGI,
    ROUTE_PROXY
} RouteType;

typedef struct {
    char path[256];
    RouteType type;
    char target[256];
    int needs_auth;
} RouteRule;

extern RouteRule g_routes[MAX_ROUTES];
extern int g_route_count;
extern int epoll_fd;

void queue_init(TaskQueue* q);
void queue_push(TaskQueue* q, ClientState* client);
ClientState* queue_pop(TaskQueue* q);
void handle_work(ClientState* client);
int set_nonblock(int fd);
ClientState* create_client_state(int fd);
void load_config_file(const char* filename);
int ssl_send_response(ClientState* client, const char* response, size_t len);
#endif
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

#define PORT 8080
#define RADIO_PORT 9001
#define BUFFER_SIZE 4096
#define NUM_WORKER_THREADS 8
#define MAX_EPOLL_EVENTS 64
#define MAX_ROUTES 32

typedef struct Task {
    int client_socket;
    char* request_buffer;
    struct Task* next;
} Task;

typedef struct {
    Task* head;
    Task* tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TaskQueue;

typedef struct {
    int fd;
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
} ClientState;

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

void load_config_file(const char* filename);

void handle_work(int client_socket, char* request_buffer);

#endif
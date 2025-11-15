#include "server.h"

TaskQueue task_queue;

static char* safe_strndup(const char* src, size_t max_len) {
    size_t len = strnlen(src, max_len);
    char* out = (char*)malloc(len + 1);

    if (!out) {
        return NULL;
    }

    memcpy(out, src, len);
    out[len] = '\0';

    return out;
}

void queue_init(TaskQueue* q) {
    q->head = NULL;
    q->tail = NULL;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void queue_push(TaskQueue* q, int client_socket, char* request_buffer) {
    Task* new_task = (Task*)malloc(sizeof(Task));
    new_task->client_socket = client_socket;
    new_task->request_buffer = request_buffer;
    new_task->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (q->tail) {
        q->tail->next = new_task;
    }
    else {
        q->head = new_task;
    }

    q->tail = new_task;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

Task* queue_pop(TaskQueue* q) {
    pthread_mutex_lock(&q->mutex);

    while (q->head == NULL) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    Task* task = q->head;
    q->head = q->head->next;

    if (q->head == NULL) {
        q->tail = NULL;
    }

    pthread_mutex_unlock(&q->mutex);

    return task;
}

void* worker_thread_function(void* arg) {
    (void)arg;

    while (1) {
        Task* task = queue_pop(&task_queue);
        
        handle_work(task->client_socket, task->request_buffer);
        
        free(task->request_buffer);
        free(task);
    }

    return NULL;
}

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        perror("fcntl F_GETFL");

        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");

        return -1;
    }

    return 0;
}

ClientState* create_client_state(int fd) {
    ClientState* client = (ClientState*)calloc(1, sizeof(ClientState));
    client->fd = fd;

    return client;
}

int main() {
    int server_socket, client_socket;
    struct sockaddr_in6 server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    queue_init(&task_queue);

    pthread_t worker_threads[NUM_WORKER_THREADS];

    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_thread_function, NULL) != 0) {
            perror("Could not create worker thread");

            return 1;
        }

        pthread_detach(worker_threads[i]);
    }

    server_socket = socket(AF_INET6, SOCK_STREAM, 0);

    if (server_socket == -1) { 
        perror("Could not create socket");
        
        return 1; 
    }
    
    int reuse = 1;

    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");

        return 1;
    }

    if (set_nonblock(server_socket) < 0){
        return 1;
    }

    int optval = 0;

    if (setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
        perror("setsockopt IPV6_V6ONLY failed");
    }

    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");

        return 1;
    }

    load_config_file("server.conf");

    if (listen(server_socket, 128) < 0) {
        perror("Listen failed");

        return 1;
    }

    int epoll_fd = epoll_create1(0);

    if (epoll_fd == -1) {
        perror("epoll_create1");

        return 1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
        perror("epoll_ctl: add server_socket");

        return 1;
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];

    printf("Server listening on port %d with %d worker threads (epoll model)\n", PORT, NUM_WORKER_THREADS);

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);

        if (n_events == -1) {
            if (errno == EINTR){
                continue;
            }

            perror("epoll_wait");

            break;
        }

        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_socket) {
                while (1) {
                    client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

                    if (client_socket == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        else { 
                            perror("accept"); 
                            
                            break;
                        }
                    }

                    printf("Main Thread: Connection accepted (fd=%d)\n", client_socket);

                    int keepalive = 1;
                    
                    if (setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
                        perror("setsockopt(SO_KEEPALIVE) failed");
                    }

                    set_nonblock(client_socket);
                    
                    ClientState* client = create_client_state(client_socket);
                    
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.ptr = client;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1) {
                        perror("epoll_ctl: add client_socket");

                        free(client);
                        
                        close(client_socket);
                    }
                }
            }
            else {
                ClientState* client = (ClientState*)events[i].data.ptr;

                while(1) {
                    ssize_t bytes_received = recv(client->fd, client->buffer + client->bytes_read, BUFFER_SIZE - client->bytes_read - 1, 0);
                    
                    if (bytes_received == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        else {
                            perror("recv");

                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
                            
                            close(client->fd);
                            
                            free(client);
                            
                            break;
                        }
                    }
                    if (bytes_received == 0) {
                        printf("Main Thread: Client (fd=%d) disconnected.\n", client->fd);
                        
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
                        
                        close(client->fd);
                        
                        free(client);
                        
                        break;
                    }
                    
                    client->bytes_read += bytes_received;
                    
                    client->buffer[client->bytes_read] = '\0';
                    char* eoh = strstr(client->buffer, "\r\n\r\n");
                    
                    if (eoh) {
                        size_t request_len = (eoh - client->buffer) + 4;
                        char* request_copy = safe_strndup(client->buffer, request_len);

                        if (request_copy == NULL) {
                            fprintf(stderr, "Main Thread: strndup failed. Closing socket %d.\n", client->fd);
                            
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
                            
                            close(client->fd);
                            
                            free(client);
                            
                            break;
                        }
                        
                        printf("Main Thread: Full request received (fd=%d), handing to worker.\n", client->fd);
                        
                        queue_push(&task_queue, client->fd, request_copy);
                        
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
                        
                        free(client);
                        
                        break;
                    }
                    if (client->bytes_read >= BUFFER_SIZE - 1) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
                        
                        close(client->fd);
                        
                        free(client);
                        
                        break;
                    }
                }
            }
        }
    }
    
    close(server_socket);
    close(epoll_fd);
    
    return 0;
}
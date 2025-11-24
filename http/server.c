#include "server.h"

TaskQueue task_queue;
int epoll_fd;

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

void queue_push(TaskQueue* q, ClientState* client) {
    Task* new_task = (Task*)malloc(sizeof(Task));
    new_task->client = client;
    new_task->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (q->tail) {
        q->tail->next = new_task;
    }

    q->tail = new_task;

    if (q->head == NULL) {
        q->head = new_task;
    }

    pthread_mutex_unlock(&q->mutex);

    pthread_cond_signal(&q->cond);
}

ClientState* queue_pop(TaskQueue* q) {
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

    ClientState* client = task->client;

    free(task);

    return client;
}

void* worker_thread_function(void* arg) {
    (void)arg;

    while (1) {
        ClientState* client = queue_pop(&task_queue);

        if (client) {
            handle_work(client);
        }
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
    client->state = STATE_READ_REQUEST;
    client->peer = NULL;
    client->bytes_read = 0;

    return client;
}

pthread_mutex_t cleanup_mutex = PTHREAD_MUTEX_INITIALIZER;

void cleanup_client(ClientState* client) {
    if (!client) {
        return;
    }

    pthread_mutex_lock(&cleanup_mutex);

    if (client->fd == -1) {
        pthread_mutex_unlock(&cleanup_mutex);
        return;
    }

    if (epoll_fd != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    }

    close(client->fd);
    client->fd = -1;

    if (client->peer) {
        ClientState* peer = client->peer;
        
        if (peer->fd != -1) {
            shutdown(peer->fd, SHUT_RDWR);
        }

        peer->peer = NULL;
        client->peer = NULL;
    }

    pthread_mutex_unlock(&cleanup_mutex);
    
    free(client);
}

int main() {
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

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

    epoll_fd = epoll_create1(0);

    if (epoll_fd == -1) {
        perror("epoll_create1");

        return 1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    ev.data.ptr = create_client_state(server_socket);

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
        perror("epoll_ctl: add server_socket");

        return 1;
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];

    printf("Server listening on port %d with %d worker threads\n", PORT, NUM_WORKER_THREADS);

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
            ClientState* client = (ClientState*)events[i].data.ptr;

            if (client->fd == server_socket) {
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
                    
                    ClientState* new_client = create_client_state(client_socket);
                    
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.ptr = new_client;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1) {
                        perror("epoll_ctl: add client_socket");

                        free(new_client);

                        close(client_socket);
                    }
                }
            }
            else if (events[i].events & EPOLLIN) {
                if (client->state == STATE_READ_REQUEST) {
                    ssize_t bytes_received = 0;
                    
                    while (client->bytes_read < BUFFER_SIZE - 1) {
                        bytes_received = recv(client->fd, client->buffer + client->bytes_read, BUFFER_SIZE - client->bytes_read - 1, 0);
                        
                        if (bytes_received == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            
                            perror("recv");

                            cleanup_client(client);

                            break;
                        }
                        if (bytes_received == 0) {
                            cleanup_client(client);

                            break;
                        }

                        client->bytes_read += bytes_received;
                        client->buffer[client->bytes_read] = '\0';

                        if (strstr(client->buffer, "\r\n\r\n")) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);

                            queue_push(&task_queue, client);

                            break;
                        }
                    }
                    if (client->bytes_read >= BUFFER_SIZE - 1) {
                        fprintf(stderr, "Request too large. Closing %d\n", client->fd);

                        cleanup_client(client);
                    }
                } 
                else if (client->state == STATE_PROXYING) {
                    while (1) {
                        char bridge_buffer[BUFFER_SIZE];
                        ssize_t bytes_read = recv(client->fd, bridge_buffer, BUFFER_SIZE, 0);

                        if (bytes_read > 0) {
                            if (client->peer && send(client->peer->fd, bridge_buffer, bytes_read, 0) < 0) {
                                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                                    break; 
                                }
                                
                                cleanup_client(client);
                                break; 
                            }
                        }
                        else if (bytes_read == 0) {
                            cleanup_client(client);

                            break;
                        }
                        else {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                
                                cleanup_client(client);
                            }
                            
                            break;
                        }
                    }
                }
            }
        }
    }
    
    close(server_socket);
    close(epoll_fd);
    
    return 0;
}
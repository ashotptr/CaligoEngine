#include "server.h"
#include <signal.h>

TaskQueue task_queue;
int epoll_fd;
SSL_CTX *ctx;

void init_openssl() {
    const SSL_METHOD *method = TLS_server_method();

    ctx = SSL_CTX_new(method);

    if (!ctx) {
        perror("Unable to create SSL context");

        ERR_print_errors_fp(stderr);

        exit(EXIT_FAILURE);
    }

    long opts = SSL_OP_IGNORE_UNEXPECTED_EOF | SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE;
    
    SSL_CTX_set_options(ctx, opts);

    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)) {
        SSL_CTX_free(ctx);

        ERR_print_errors_fp(stderr);

        fprintf(stderr, "Failed to set the minimum TLS protocol version\n");

        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);

        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);

        exit(EXIT_FAILURE);
    }
}

void cleanup_openssl() {
    SSL_CTX_free(ctx);

    EVP_cleanup();
}

int flush_pending_write(ClientState* client) {
    if (!client->pending_write_data || client->pending_write_len == 0) {
        return 0;
    }

    int sent = SSL_write(client->ssl, client->pending_write_data, client->pending_write_len);

    if (sent > 0) {
        if ((size_t)sent == client->pending_write_len) {
            free(client->pending_write_data);

            client->pending_write_data = NULL;
            client->pending_write_len = 0;

            struct epoll_event ev;
            
            ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
            ev.data.ptr = client;

            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
            
            if (client->peer) {
                ev.events = EPOLLIN | EPOLLET;
                ev.data.ptr = client->peer;

                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->peer->fd, &ev);
            }

            return 0;
        } 
        else {
            size_t remaining = client->pending_write_len - sent;

            memmove(client->pending_write_data, client->pending_write_data + sent, remaining);

            client->pending_write_len = remaining;

            return 1;
        }
    } 
    else {
        int err = SSL_get_error(client->ssl, sent);

        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            struct epoll_event ev;
            
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT;
            ev.data.ptr = client;

            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);

            return 1;
        }

        return -1;
    }
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
    client->ssl = NULL;
    client->state = STATE_READ_REQUEST;
    client->peer = NULL;
    client->bytes_read = 0;

    return client;
}

void cleanup_client(ClientState* client) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);

    if (client->ssl) {
        SSL_shutdown(client->ssl);

        SSL_free(client->ssl);
        
        client->ssl = NULL;
    }
    
    if (client->fd >= 0) {
        close(client->fd);
    }
    
    if (client->peer) {
        ClientState* peer = client->peer;

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, peer->fd, NULL);

        close(peer->fd);

        peer->peer = NULL;

        free(peer);
    }

    free(client);
}

int perform_ssl_handshake(ClientState* client) {
    int ret = SSL_accept(client->ssl);
    
    if (ret == 1) {
        printf("SSL Handshake complete for fd %d\n", client->fd);

        client->state = STATE_READ_REQUEST;
        client->ssl_want_write = 0;

        return 0; 
    }

    int err = SSL_get_error(client->ssl, ret);
    
    if (err == SSL_ERROR_WANT_WRITE) {
        client->ssl_want_write = 1;

        return 1; 
    }
    
    client->ssl_want_write = 0;

    if (err == SSL_ERROR_WANT_READ) {
        return 1; 
    }
    
    ERR_print_errors_fp(stderr);
    
    return -1;
}

void rearm_client(int epoll_fd, ClientState* client) {
    struct epoll_event ev;
    ev.data.ptr = client;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    
    if (client->ssl_want_write) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev) == -1) {
        perror("rearm_client: epoll_ctl");

        cleanup_client(client);
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    
    init_openssl();

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

                    SSL *ssl = SSL_new(ctx);

                    SSL_set_fd(ssl, client_socket);

                    ClientState* new_client = create_client_state(client_socket);

                    new_client->ssl = ssl;
                    new_client->state = STATE_SSL_HANDSHAKE;

                    int shake_ret = perform_ssl_handshake(new_client);

                    if (shake_ret < 0) {
                        cleanup_client(new_client);
                        
                        continue;
                    }
                    
                    ev.events = EPOLLIN | EPOLLET| EPOLLONESHOT;
                    
                    if (new_client->ssl_want_write) {
                        ev.events |= EPOLLOUT;
                    }

                    ev.data.ptr = new_client;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1) {
                        perror("epoll_ctl: add client_socket");

                        cleanup_client(new_client);
                    }
                }
            }
            else if (events[i].events & EPOLLIN) {
                if (client->state == STATE_SSL_HANDSHAKE) {
                    int ret = perform_ssl_handshake(client);
                    
                    if (ret < 0) {
                        cleanup_client(client);
                    }
                    else if (ret == 1) {
                        rearm_client(epoll_fd, client);
                    }
                    else {
                        rearm_client(epoll_fd, client);
                    }
                }
                else if (client->state == STATE_READ_REQUEST) {
                    ssize_t bytes_received = 0;
                    
                    while (client->bytes_read < BUFFER_SIZE - 1) {
                        bytes_received = SSL_read(client->ssl, client->buffer + client->bytes_read, BUFFER_SIZE - client->bytes_read - 1);

                        if (bytes_received <= 0) {
                            int err = SSL_get_error(client->ssl, bytes_received);

                            if (err == SSL_ERROR_WANT_WRITE) {
                                client->ssl_want_write = 1;
                            }
                            else {
                                client->ssl_want_write = 0;
                            }

                            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                                rearm_client(epoll_fd, client);

                                break; 
                            }
                            
                            cleanup_client(client);

                            break;
                        }
                        
                        client->ssl_want_write = 0;

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
                    char bridge_buffer[BUFFER_SIZE];
                    ssize_t bytes_read;

                    if (client->ssl) {
                        bytes_read = SSL_read(client->ssl, bridge_buffer, sizeof(bridge_buffer));
                        
                        if (bytes_read > 0) {
                            client->ssl_want_write = 0;
                            
                            if (send(client->peer->fd, bridge_buffer, bytes_read, MSG_NOSIGNAL) < 0) {
                                if (errno != EWOULDBLOCK && errno != EAGAIN) cleanup_client(client);
                            }
                            
                            rearm_client(epoll_fd, client);
                        }
                        else {
                            int err = SSL_get_error(client->ssl, bytes_read);
                            
                            if (err == SSL_ERROR_WANT_WRITE) {
                                client->ssl_want_write = 1;
                            }
                            else {
                                client->ssl_want_write = 0;
                            }

                            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                                rearm_client(epoll_fd, client);
                            }
                            else {
                                cleanup_client(client);
                            }
                        }
                    }
                    else {
                        if (client->peer->pending_write_len > 0) {
                            break; 
                        }

                        bytes_read = recv(client->fd, bridge_buffer, sizeof(bridge_buffer), 0);

                        if (bytes_read > 0) {
                            int sent = SSL_write(client->peer->ssl, bridge_buffer, bytes_read);
                            
                            if (sent <= 0) {
                                int err = SSL_get_error(client->peer->ssl, sent);

                                if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
                                    cleanup_client(client);

                                    break;
                                }

                                sent = 0;
                            }

                            if ((size_t)sent < (size_t)bytes_read) {
                                ClientState* browser = client->peer;
                                size_t diff = bytes_read - sent;

                                browser->pending_write_data = malloc(diff);

                                memcpy(browser->pending_write_data, bridge_buffer + sent, diff);
                                
                                browser->pending_write_len = diff;

                                struct epoll_event ev;                                
                                ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT;
                                ev.data.ptr = browser;
                                
                                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, browser->fd, &ev);

                                ev.events = 0;
                                ev.data.ptr = client;

                                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
                            }
                        }
                        else if (bytes_read == 0) {
                            cleanup_client(client);
                        }
                    }
                }
            }
            else if (events[i].events & EPOLLOUT) {
                ClientState* client = (ClientState*)events[i].data.ptr;
    
                int flush_ret = flush_pending_write(client); 
                
                if (flush_ret < 0) {
                    cleanup_client(client);

                    continue;
                }
                
                if (flush_ret == 0 && client->file_stream != NULL) {
                    char file_buffer[BUFFER_SIZE];

                    size_t bytes_read = fread(file_buffer, 1, BUFFER_SIZE, client->file_stream);

                    if (bytes_read > 0) {
                        int ret = ssl_send_response(client, file_buffer, bytes_read);

                        if (ret < 0) {
                            cleanup_client(client);
                        }
                    }
                    else {
                        if (client->is_cgi) {
                            pclose(client->file_stream);

                            client->is_cgi = 0;
                        }
                        else {
                            fclose(client->file_stream);
                        }
                        
                        client->file_stream = NULL;
                    }
                }
            }
        }
    }
    
    close(server_socket);
    close(epoll_fd);
    
    return 0;
}
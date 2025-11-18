#include "server.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#define STREAM_BITRATE 128000
#define BYTES_PER_SEC (STREAM_BITRATE / 8)
#define RADIO_PLAYLIST "public_html/mixtape/radio_playlist_cbr.mp3"
#define BROADCAST_BUFFER_SLOTS 64
#define NUM_SENDER_THREADS 12

typedef struct RadioClient {
    int fd;
    int read_index;
    struct RadioClient* next;
} RadioClient;

typedef struct {
    int thread_id;
    RadioClient* client_list_head;
    pthread_mutex_t list_mutex;
} SenderContext;

static char g_broadcast_buffer[BROADCAST_BUFFER_SLOTS][BUFFER_SIZE];
static size_t g_buffer_chunk_size[BROADCAST_BUFFER_SLOTS];
static volatile int g_write_index = 0;
static pthread_mutex_t g_buffer_mutex = PTHREAD_MUTEX_INITIALIZER; 
static pthread_cond_t g_new_data_cond = PTHREAD_COND_INITIALIZER;
static SenderContext g_senders[NUM_SENDER_THREADS];

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    
    if (flags == -1){
        return -1;
    }
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void sync_stream_timing(size_t total_bytes_streamed, struct timespec* start_time) {
    struct timespec current_time;

    clock_gettime(CLOCK_MONOTONIC, &current_time);

    double expected_duration = (double)total_bytes_streamed / BYTES_PER_SEC;

    double actual_elapsed = (current_time.tv_sec - start_time->tv_sec) + (current_time.tv_nsec - start_time->tv_nsec) / 1e9;

    if (actual_elapsed < expected_duration) {
        double sleep_needed = expected_duration - actual_elapsed;

        struct timespec req;
        req.tv_sec = (time_t)sleep_needed;
        req.tv_nsec = (long)((sleep_needed - req.tv_sec) * 1e9);

        nanosleep(&req, NULL);
    }
}

void* broadcast_thread_function(void* arg) {
    (void)arg;
    char local_buffer[BUFFER_SIZE];
    FILE* file = fopen(RADIO_PLAYLIST, "rb");

    if (file == NULL) {
        perror("FATAL: Could not open radio playlist");

        exit(1);
    }
    
    printf("[Radio Broadcaster] Starting stream (Bitrate: %d bps)\n", STREAM_BITRATE);

    struct timespec start_time;
    size_t total_bytes_in_loop = 0;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (1) {
        size_t bytes_read = fread(local_buffer, 1, BUFFER_SIZE, file);

        if (bytes_read == 0) {
            if (feof(file)) {
                printf("[Radio Broadcaster] Playlist loop. Resetting clock.\n");

                fclose(file);

                file = fopen(RADIO_PLAYLIST, "rb");

                total_bytes_in_loop = 0;

                clock_gettime(CLOCK_MONOTONIC, &start_time);

                if (file == NULL) { 
                    perror("FATAL: Could not reopen playlist");
                    
                    exit(1); 
                }

                continue;
            }

            perror("RADIO ERROR: fread");

            sleep(1);

            continue;
        }

        pthread_mutex_lock(&g_buffer_mutex);

        g_write_index = (g_write_index + 1) % BROADCAST_BUFFER_SLOTS;

        memcpy(g_broadcast_buffer[g_write_index], local_buffer, bytes_read);

        g_buffer_chunk_size[g_write_index] = bytes_read;

        pthread_mutex_unlock(&g_buffer_mutex);

        pthread_cond_broadcast(&g_new_data_cond);

        total_bytes_in_loop += bytes_read;

        sync_stream_timing(total_bytes_in_loop, &start_time);
    }

    fclose(file);

    return NULL;
}

void* sender_worker_thread(void* arg) {
    SenderContext* ctx = (SenderContext*)arg;
    char chunk_header[32];
    
    printf("[Radio Sender #%d] Worker thread live.\n", ctx->thread_id);
    
    int last_processed_idx = -1;
    while (1) {
        pthread_mutex_lock(&g_buffer_mutex);

        while (last_processed_idx == g_write_index) {
            pthread_cond_wait(&g_new_data_cond, &g_buffer_mutex);
        }
        
        int current_write_idx = g_write_index;
        last_processed_idx = current_write_idx;
        size_t current_chunk_size = g_buffer_chunk_size[current_write_idx];
        
        pthread_mutex_unlock(&g_buffer_mutex);

        pthread_mutex_lock(&ctx->list_mutex);
        
        RadioClient* curr = ctx->client_list_head;
        RadioClient* prev = NULL;

        while (curr != NULL) {
            snprintf(chunk_header, sizeof(chunk_header), "%lx\r\n", current_chunk_size);
            
            int success = 1;
            
            if (send(curr->fd, chunk_header, strlen(chunk_header), MSG_NOSIGNAL) < 0) {
                 success = 0;
            }
            else if (send(curr->fd, g_broadcast_buffer[current_write_idx], current_chunk_size, MSG_NOSIGNAL) < 0) {
                success = 0;
            }
            else if (send(curr->fd, "\r\n", 2, MSG_NOSIGNAL) < 0) {
                success = 0;
            }

            if (!success) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    printf("[Radio Sender #%d] Listener disconnected (fd=%d).\n", ctx->thread_id, curr->fd);
                    
                    close(curr->fd);
                    
                    if (prev == NULL) { 
                        ctx->client_list_head = curr->next;
                    }
                    else { 
                        prev->next = curr->next; 
                    }
                    
                    RadioClient* to_free = curr;
                    curr = curr->next;

                    free(to_free);
                    
                    continue; 
                }
            }
            else {
                curr->read_index = current_write_idx;
            }

            prev = curr;
            curr = curr->next;
        }

        pthread_mutex_unlock(&ctx->list_mutex);
    }

    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int server_socket, epoll_fd;
    struct sockaddr_in server_addr;

    pthread_t sender_tids[NUM_SENDER_THREADS];

    for (int i = 0; i < NUM_SENDER_THREADS; i++) {
        g_senders[i].thread_id = i;
        g_senders[i].client_list_head = NULL;

        pthread_mutex_init(&g_senders[i].list_mutex, NULL);
        
        if (pthread_create(&sender_tids[i], NULL, sender_worker_thread, &g_senders[i]) != 0) {
            perror("FATAL: Failed to create sender thread");

            return 1;
        }
            
        pthread_detach(sender_tids[i]);
    }

    pthread_t broadcast_tid;

    if (pthread_create(&broadcast_tid, NULL, broadcast_thread_function, NULL) != 0) {
        perror("FATAL: Failed to create broadcast thread");

        return 1;
    }
    
    pthread_detach(broadcast_tid);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket == -1) {
        perror("radio_server: socket");

        return 1;
    }

    int reuse = 1;
    
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("radio_server: setsockopt");

        return 1;
    }

    set_nonblock(server_socket);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(RADIO_PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("radio_server: bind");

        return 1;
    }

    if (listen(server_socket, 128) < 0) {
        perror("radio_server: listen");

        return 1;
    }
    
    epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EPOLL_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev);

    printf("[Radio Server] Live on port %d... (%d Threads)\n", RADIO_PORT, NUM_SENDER_THREADS);

    int current_thread_idx = 0;

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_socket) {
                while(1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
                    
                    if (client_fd < 0) {
                        break;
                    }

                    set_nonblock(client_fd);
                    
                    char dump[1024];

                    recv(client_fd, dump, sizeof(dump), 0); 

                    const char* header = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: audio/mpeg\r\n"
                                         "Transfer-Encoding: chunked\r\n"
                                         "Connection: keep-alive\r\n\r\n";

                    send(client_fd, header, strlen(header), 0);
                    
                    SenderContext* target_thread = &g_senders[current_thread_idx];
                    
                    RadioClient* new_client = malloc(sizeof(RadioClient));
                    new_client->fd = client_fd;
                    new_client->read_index = g_write_index;

                    pthread_mutex_lock(&target_thread->list_mutex);

                    new_client->next = target_thread->client_list_head;
                    target_thread->client_list_head = new_client;

                    pthread_mutex_unlock(&target_thread->list_mutex);
                    
                    printf("[Radio Main] Assigned Listener (fd=%d) to Thread %d\n", client_fd, current_thread_idx);

                    current_thread_idx = (current_thread_idx + 1) % NUM_SENDER_THREADS;
                }
            }
        }
    }

    close(server_socket);
    
    return 0;
}
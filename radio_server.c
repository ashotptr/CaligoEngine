#include "server.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

#define STREAM_BITRATE 128000 
#define BYTES_PER_SEC (STREAM_BITRATE / 8)
#define RADIO_PLAYLIST "public_html/mixtape/radio_playlist_cbr.mp3"
#define BROADCAST_BUFFER_SLOTS 16 //maybe make 32

static char g_broadcast_buffer[BROADCAST_BUFFER_SLOTS][BUFFER_SIZE];
static size_t g_buffer_chunk_size[BROADCAST_BUFFER_SLOTS];
static volatile int g_write_index = 0;
static pthread_mutex_t g_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_new_data_cond = PTHREAD_COND_INITIALIZER;

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

void* client_thread_function(void* arg) {
    int client_socket = (int)(intptr_t)arg;
    char chunk_header[32];
    char local_buffer[BUFFER_SIZE];
    size_t chunk_size;
    int my_read_index;

    char request_siphon[BUFFER_SIZE];

    if (recv(client_socket, request_siphon, BUFFER_SIZE - 1, 0) < 0) {
        perror("radio_server: client recv");

        close(client_socket);
        
        return NULL;
    }

    const char* header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: audio/mpeg\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "Connection: keep-alive\r\n\r\n";

    if (send(client_socket, header, strlen(header), 0) < 0) {
        close(client_socket);
        
        return NULL;
    }

    pthread_mutex_lock(&g_buffer_mutex);
    
    my_read_index = g_write_index;
    
    pthread_mutex_unlock(&g_buffer_mutex);

    printf("[Radio Client] New listener joined, starting at index %d\n", my_read_index);

    while (1) {
        pthread_mutex_lock(&g_buffer_mutex);

        while (my_read_index == g_write_index) {
            pthread_cond_wait(&g_new_data_cond, &g_buffer_mutex);
        }

        my_read_index = (my_read_index + 1) % BROADCAST_BUFFER_SLOTS;

        chunk_size = g_buffer_chunk_size[my_read_index];
        
        memcpy(local_buffer, g_broadcast_buffer[my_read_index], chunk_size);
        
        pthread_mutex_unlock(&g_buffer_mutex);

        snprintf(chunk_header, sizeof(chunk_header), "%lx\r\n", chunk_size);

        if (send(client_socket, chunk_header, strlen(chunk_header), 0) < 0) {
            break;
        }

        if (send(client_socket, local_buffer, chunk_size, 0) < 0) {
            break;
        }

        if (send(client_socket, "\r\n", 2, 0) < 0) {
            break;
        }
    }

    printf("[Radio Client] Listener disconnected.\n");

    close(client_socket);
    
    return NULL;
}

int main(void) {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    pthread_t broadcast_tid;

    signal(SIGPIPE, SIG_IGN); 

    if (pthread_create(&broadcast_tid, NULL, broadcast_thread_function, NULL) != 0) {
        perror("Failed to create broadcast thread");
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

    printf("[Radio Server] Live on port %d...\n", RADIO_PORT);

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("radio_server: accept");

            continue;
        }

        pthread_t client_tid;

        if (pthread_create(&client_tid, NULL, client_thread_function, (void*)(intptr_t)client_socket) != 0) {
            perror("radio_server: pthread_create");

            close(client_socket);
        }

        pthread_detach(client_tid);
    }

    close(server_socket);

    return 0;
}
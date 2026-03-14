#include "server.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/un.h>
#include <dirent.h>
#include <ctype.h>
#include <jansson.h>

#define RADIO_SOCKET_PATH "/tmp/caligo_radio.sock"
#define STREAM_BITRATE 128000
#define BYTES_PER_SEC (STREAM_BITRATE / 8)
#define MUSIC_DIR "public_html/music"
#define PLAYLIST_JSON_PATH "public_html/music/playlist.json"
#define BROADCAST_BUFFER_SLOTS 64
#define NUM_SENDER_THREADS 12
#define MAX_CLIENT_BACKLOG (1024 * 1024 * 2)

static char g_cur_file[512] = "";
static char g_cur_artist[128] = "Unknown";
static char g_cur_album[128] = "Unknown";
static char g_cur_title[128] = "Waiting for stream...";
static int g_cur_is_req = 0;

typedef struct QueueItem {
    char filepath[512];
    char artist[128];
    char album[128];
    char title[128];
    int is_temp; 
    struct QueueItem* next;
} QueueItem;

typedef struct RadioClient {
    int fd;
    int read_index;    
    char* backlog;
    size_t backlog_len;
    struct RadioClient* next;
} RadioClient;

typedef struct {
    int thread_id;
    RadioClient* client_list_head;
    pthread_mutex_t list_mutex;
} SenderContext;

typedef struct EventSub {
    int fd;
    struct EventSub* next;
} EventSub;

static EventSub* g_event_subs = NULL;
static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char filepath[512];
    char artist[128];
    char album[128];
    char title[128];
} PlaylistEntry;

static PlaylistEntry* g_playlist = NULL;
static int g_playlist_count = 0;
static int g_playlist_index = 0;

void shuffle_playlist() {
    if (g_playlist_count <= 1) {
        return;
    }

    srand(time(NULL));
    
    for (int i = g_playlist_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        PlaylistEntry temp = g_playlist[i];
        g_playlist[i] = g_playlist[j];
        g_playlist[j] = temp;
    }

    printf("[Radio] Playlist shuffled (%d tracks).\n", g_playlist_count);
}

void load_playlist_json() {
    if (g_playlist) { 
        free(g_playlist);

        g_playlist = NULL; 
    }

    g_playlist_count = 0;
    json_error_t error;
    json_t *root = json_load_file(PLAYLIST_JSON_PATH, 0, &error);

    if (!root) {
        printf("[Radio] Error: Failed to load JSON playlist on line %d: %s\n", error.line, error.text);

        return;
    }

    if (!json_is_array(root)) {
        printf("[Radio] Error: Root of JSON must be an array.\n");

        json_decref(root);
        
        return;
    }

    g_playlist_count = json_array_size(root);

    if (g_playlist_count == 0) {
        json_decref(root);

        return;
    }

    g_playlist = malloc(sizeof(PlaylistEntry) * g_playlist_count);
    
    size_t index;
    json_t *value;

    json_array_foreach(root, index, value) {
        strcpy(g_playlist[index].artist, "Unknown Artist");
        strcpy(g_playlist[index].album,  "Unknown Album");
        strcpy(g_playlist[index].title,  "Unknown Title");

        json_t *j_file = json_object_get(value, "file");

        if (json_is_string(j_file)) {
            const char *fname = json_string_value(j_file);

            snprintf(g_playlist[index].filepath, 512, "%s/%s", MUSIC_DIR, fname);
        }
        else {
            printf("[Radio] Warning: Item %ld missing 'file' field.\n", index);
            
            g_playlist[index].filepath[0] = '\0'; 
        }

        json_t *j_artist = json_object_get(value, "artist");

        if (json_is_string(j_artist)) {
            strncpy(g_playlist[index].artist, json_string_value(j_artist), 127);
        }
        else {
            printf("[Radio] Warning: Item %ld missing 'artist' field.\n", index);
            
            g_playlist[index].artist[0] = '\0'; 
        }

        json_t *j_album = json_object_get(value, "album");

        if (json_is_string(j_album)) {
            strncpy(g_playlist[index].album, json_string_value(j_album), 127);
        } 
        else {
            printf("[Radio] Warning: Item %ld missing 'album' field.\n", index);
            
            g_playlist[index].album[0] = '\0'; 
        }

        json_t *j_title = json_object_get(value, "title");

        if (json_is_string(j_title)) {
            strncpy(g_playlist[index].title, json_string_value(j_title), 127);
        }
        else {
            printf("[Radio] Warning: Item %ld missing 'title' field.\n", index);
            
            g_playlist[index].title[0] = '\0';
        }
    }

    json_decref(root);

    printf("[Radio] Loaded %d tracks from JSON.\n", g_playlist_count);

    shuffle_playlist();
}

void broadcast_status_change(const char* current_file, const char* artist, const char* album, const char* title, int is_request) {
    pthread_mutex_lock(&g_event_mutex);

    strncpy(g_cur_file, current_file, sizeof(g_cur_file) - 1);
    strncpy(g_cur_artist, artist, sizeof(g_cur_artist) - 1);
    strncpy(g_cur_album, album, sizeof(g_cur_album) - 1);
    strncpy(g_cur_title, title, sizeof(g_cur_title) - 1);
    g_cur_is_req = is_request;

    EventSub* curr = g_event_subs;
    EventSub* prev = NULL;
    char msg[2048];
    
    snprintf(msg, sizeof(msg),
        "data: {"
        "\"playing\": \"%s\", "
        "\"artist\": \"%s\", "
        "\"album\": \"%s\", "
        "\"title\": \"%s\", "
        "\"bitrate\": 128, "
        "\"is_request\": %s"
        "}\n\n",
        current_file, artist, album, title, is_request ? "true" : "false");
    
    while (curr) {
        if (send(curr->fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                prev = curr;
                curr = curr->next;

                continue; 
            }

            close(curr->fd);

            EventSub* to_free = curr;

            if (prev) {
                prev->next = curr->next;
            }
            else {
                g_event_subs = curr->next;
            }

            curr = curr->next;

            free(to_free);
        } 
        else {
            prev = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&g_event_mutex);
}

static QueueItem* g_queue_head = NULL;
static QueueItem* g_queue_tail = NULL;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
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
    FILE* file = NULL;
    char current_filepath[512] = {0};
    char current_artist[128] = {0};
    char current_album[128] = {0};
    char current_title[128] = {0};    
    int is_temp_file = 0;
    int is_playing_queue = 0;
    int is_interrupted = 0;
    long default_resume_cursor = 0;
    int default_resume_index = 0;

    printf("[Radio Broadcaster] Starting stream (Bitrate: %d bps)\n", STREAM_BITRATE);
    
    load_playlist_json();

    struct timespec start_time;
    size_t total_bytes_in_loop = 0;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (1) {
        if (file != NULL && is_playing_queue == 0) {
            pthread_mutex_lock(&g_queue_mutex);

            int queue_has_items = (g_queue_head != NULL);

            pthread_mutex_unlock(&g_queue_mutex);

            if (queue_has_items) {
                printf("[Radio] Interrupting background playlist for Live Queue!\n");
                
                if (file != NULL) {
                    default_resume_cursor = ftell(file);
                    default_resume_index = (g_playlist_index == 0) ? g_playlist_count - 1 : g_playlist_index - 1;
                    is_interrupted = 1;
                }
                
                fclose(file);

                file = NULL;

                total_bytes_in_loop = 0;

                clock_gettime(CLOCK_MONOTONIC, &start_time);
            }
        }

        if (file == NULL) {
            pthread_mutex_lock(&g_queue_mutex);

            if (g_queue_head) {
                QueueItem* item = g_queue_head;
                g_queue_head = item->next;

                if (!g_queue_head) {
                    g_queue_tail = NULL;
                }

                strncpy(current_filepath, item->filepath, sizeof(current_filepath) - 1);
                
                strncpy(current_artist, item->artist[0] ? item->artist : "Listener Request", 127);
                strncpy(current_album, item->album[0] ? item->album : "Live Queue", 127);
                strncpy(current_title, item->title[0] ? item->title : "Unknown Title", 127);
                
                is_temp_file = item->is_temp; 
                is_playing_queue = 1;
                
                free(item);

                printf("[Radio] Switching to Queue: %s (%s - %s)\n", current_filepath, current_artist, current_title);

                file = fopen(current_filepath, "rb");

                if (file) {
                    broadcast_status_change(current_filepath, current_artist, current_album, current_title, 1);
                }
                else {
                    is_playing_queue = 0; 
                }
            }
            
            pthread_mutex_unlock(&g_queue_mutex);

            if (file == NULL) {
                if (is_temp_file && current_filepath[0]) {
                    unlink(current_filepath);
                    
                    is_temp_file = 0;
                }
                
                is_playing_queue = 0;

                if (g_playlist_count == 0) {
                    printf("[Radio] Error: Playlist empty. Retrying in 5s...\n");
                    
                    sleep(5);
                    
                    load_playlist_json();
                    
                    continue;
                }

                int track_index_to_play;
                
                if (is_interrupted) {
                    track_index_to_play = default_resume_index;
                    
                    printf("[Radio] Resuming interrupted track index: %d\n", track_index_to_play);
                }
                else {                    
                    track_index_to_play = g_playlist_index;
                    
                    g_playlist_index++;
                    
                    if (g_playlist_index >= g_playlist_count) {
                        g_playlist_index = 0;
                        
                        shuffle_playlist();
                    }
                }

                PlaylistEntry* entry = &g_playlist[track_index_to_play];
                
                strncpy(current_filepath, entry->filepath, sizeof(current_filepath) - 1);
                strncpy(current_artist, entry->artist, sizeof(current_artist) - 1);
                strncpy(current_album, entry->album, sizeof(current_album) - 1);
                strncpy(current_title, entry->title, sizeof(current_title) - 1);

                file = fopen(current_filepath, "rb");
                
                if (!file) {
                    printf("[Radio] Error opening: %s. Skipping.\n", current_filepath);
                    
                    is_interrupted = 0;

                    continue;
                }
                
                if (is_interrupted && default_resume_cursor > 0) {
                    fseek(file, default_resume_cursor, SEEK_SET);

                    is_interrupted = 0;

                    default_resume_cursor = 0;
                }

                broadcast_status_change(current_filepath, current_artist, current_album, current_title, 0);
            }
        }

        size_t bytes_read = fread(local_buffer, 1, BUFFER_SIZE, file);

        if (bytes_read == 0) {
            if (feof(file)) {
                fclose(file);

                file = NULL;

                total_bytes_in_loop = 0;

                clock_gettime(CLOCK_MONOTONIC, &start_time);

                continue; 
            }
            
            perror("radio_server: fread");

            sleep(1);

            continue;
        }

        pthread_mutex_lock(&g_buffer_mutex);

        int next_index = (g_write_index + 1) % BROADCAST_BUFFER_SLOTS;
        
        memcpy(g_broadcast_buffer[next_index], local_buffer, bytes_read);
        g_buffer_chunk_size[next_index] = bytes_read;
        
        g_write_index = next_index;
        
        pthread_mutex_unlock(&g_buffer_mutex);
        
        pthread_cond_broadcast(&g_new_data_cond);

        total_bytes_in_loop += bytes_read;
        
        sync_stream_timing(total_bytes_in_loop, &start_time);
    }

    fclose(file); 

    return NULL;
}

void remove_event_subscriber(int fd) {
    pthread_mutex_lock(&g_event_mutex);

    EventSub* curr = g_event_subs;
    EventSub* prev = NULL;

    while (curr) {
        if (curr->fd == fd) {
            if (prev) {
                prev->next = curr->next;
            } 
            else {
                g_event_subs = curr->next;
            }

            free(curr);

            printf("[Radio] Cleaned up SSE subscriber FD=%d\n", fd);

            break;
        }

        prev = curr;
        curr = curr->next;
    }

    pthread_mutex_unlock(&g_event_mutex);
}

void* sender_worker_thread(void* arg) {
    SenderContext* ctx = (SenderContext*)arg;
    
    char scratch_buffer[BUFFER_SIZE + 64]; 

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
        
        char* data_ptr = g_broadcast_buffer[current_write_idx];

        pthread_mutex_unlock(&g_buffer_mutex);
        
        if (current_chunk_size == 0) {
            pthread_mutex_lock(&ctx->list_mutex);

            RadioClient* c = ctx->client_list_head;

            while(c) { 
                c->read_index = current_write_idx; 
                c = c->next; 
            }

            pthread_mutex_unlock(&ctx->list_mutex);

            continue;
        }
        
        int header_len = snprintf(scratch_buffer, sizeof(scratch_buffer), "%lx\r\n", current_chunk_size);
        
        if (header_len < 0 || (size_t)header_len >= sizeof(scratch_buffer)) {
            continue;
        }

        memcpy(scratch_buffer + header_len, data_ptr, current_chunk_size);
        
        memcpy(scratch_buffer + header_len + current_chunk_size, "\r\n", 2);

        size_t total_packet_len = header_len + current_chunk_size + 2;

        pthread_mutex_lock(&ctx->list_mutex);
        
        RadioClient* curr = ctx->client_list_head;
        RadioClient* prev = NULL;

        while (curr != NULL) {
            int disconnect = 0;
            
            if (curr->backlog_len > 0) {
                if (curr->backlog_len + total_packet_len > MAX_CLIENT_BACKLOG) {
                    printf("[Radio Sender #%d] Client dropped (Backlog Full: %zu bytes)\n", ctx->thread_id, curr->backlog_len);
                    
                    disconnect = 1;
                } 
                else {
                    char* new_ptr = realloc(curr->backlog, curr->backlog_len + total_packet_len);
                    
                    if (!new_ptr) {
                        disconnect = 1;
                    }
                    else {
                        curr->backlog = new_ptr;
                        
                        memcpy(curr->backlog + curr->backlog_len, scratch_buffer, total_packet_len);
                        
                        curr->backlog_len += total_packet_len;
                    }
                }
            } 
            else {
                ssize_t sent = send(curr->fd, scratch_buffer, total_packet_len, MSG_NOSIGNAL);
                
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        sent = 0;
                    }
                    else {
                        disconnect = 1;
                    }
                }
                
                if (!disconnect && (size_t)sent < total_packet_len) {
                    size_t remaining = total_packet_len - sent;
                    curr->backlog = malloc(remaining);

                    if (curr->backlog) {
                        memcpy(curr->backlog, scratch_buffer + sent, remaining);
                        
                        curr->backlog_len = remaining;
                    }
                    else {
                        disconnect = 1;
                    }
                }
            }

            if (!disconnect && curr->backlog_len > 0) {
                ssize_t sent = send(curr->fd, curr->backlog, curr->backlog_len, MSG_NOSIGNAL);
                
                if (sent < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK){
                        disconnect = 1;
                    }
                }
                else if (sent > 0) {
                    if ((size_t)sent == curr->backlog_len) {
                        free(curr->backlog);

                        curr->backlog = NULL;
                        curr->backlog_len = 0;
                    } 
                    else {
                        size_t remaining = curr->backlog_len - sent;
                        
                        memmove(curr->backlog, curr->backlog + sent, remaining);
                        
                        char* new_ptr = realloc(curr->backlog, remaining);
                        curr->backlog = (new_ptr) ? new_ptr : curr->backlog;
                        curr->backlog_len = remaining;
                    }
                }
            }
            
            if (disconnect) {
                close(curr->fd);

                if (curr->backlog) {
                    free(curr->backlog);
                }
                
                RadioClient* to_free = curr;

                if (prev == NULL) {
                    ctx->client_list_head = curr->next;
                    curr = curr->next;
                } 
                else {
                    prev->next = curr->next;
                    curr = curr->next;
                }

                free(to_free);

                continue; 
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

void* command_listener_thread(void* arg) {
    (void)arg;
    int sock_fd, client_fd;
    struct sockaddr_un server_addr;
    char buffer[4096];

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sock_fd < 0) {
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sun_family = AF_UNIX;

    strncpy(server_addr.sun_path, RADIO_SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    unlink(RADIO_SOCKET_PATH);

    if (bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        return NULL;
    }

    if (listen(sock_fd, 5) < 0) {
        return NULL;
    }
    
    chmod(RADIO_SOCKET_PATH, 0666);

    printf("[Radio Cmd] Listening on %s\n", RADIO_SOCKET_PATH);

    while (1) {
        client_fd = accept(sock_fd, NULL, NULL);

        if (client_fd < 0) {
            continue;
        }

        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

        if (n > 0) {
            buffer[n] = '\0';
            
            if (strncmp(buffer, "QUEUE_JSON ", 11) == 0) {
                char* json_str = buffer + 11;
                json_error_t error;
                json_t *root = json_loads(json_str, 0, &error);

                if (root) {
                    const char *file_val = json_string_value(json_object_get(root, "file"));
                    const char *artist_val = json_string_value(json_object_get(root, "artist"));
                    const char *album_val = json_string_value(json_object_get(root, "album"));
                    const char *title_val = json_string_value(json_object_get(root, "title"));
                    
                    json_t *j_temp = json_object_get(root, "temp");
                    int is_temp_val = (json_is_boolean(j_temp) && json_is_false(j_temp)) ? 0 : 1;

                    if (file_val) {
                        pthread_mutex_lock(&g_queue_mutex);

                        QueueItem* item = malloc(sizeof(QueueItem));
                        
                        strncpy(item->filepath, file_val, sizeof(item->filepath) - 1);
                        strncpy(item->artist, artist_val ? artist_val : "Unknown Artist", 127);
                        strncpy(item->album, album_val ? album_val : "Unknown Album", 127);
                        strncpy(item->title, title_val ? title_val : "Unknown Title", 127);
                        
                        item->is_temp = is_temp_val;
                        item->next = NULL;
                        
                        if (g_queue_tail) {
                            g_queue_tail->next = item;
                            g_queue_tail = item;
                        }
                        else {
                            g_queue_head = item;
                            g_queue_tail = item;
                        }

                        printf("[Radio Cmd] Enqueued JSON: %s\n", item->title);

                        pthread_mutex_unlock(&g_queue_mutex);
                    }
                    else {
                        printf("[Radio Cmd] JSON Error: Missing 'file' field.\n");
                    }

                    json_decref(root);
                }
                else {
                    printf("[Radio Cmd] JSON Parse Error: %s\n", error.text);
                }
            }
        }
        
        close(client_fd);
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

        if (pthread_mutex_init(&g_senders[i].list_mutex, NULL) != 0) {
            perror("radio_server: pthread_mutex_init");

            return 1;
        }
        
        if (pthread_create(&sender_tids[i], NULL, sender_worker_thread, &g_senders[i]) != 0) {
            perror("radio_server: pthread_create");

            return 1;
        }

        pthread_detach(sender_tids[i]);
    }
    
    pthread_t cmd_tid;

    if (pthread_create(&cmd_tid, NULL, command_listener_thread, NULL) != 0) {
        perror("radio_server: pthread_create");

        return 1;
    }

    pthread_detach(cmd_tid);

    pthread_t broadcast_tid;

    if (pthread_create(&broadcast_tid, NULL, broadcast_thread_function, NULL) != 0) {
        perror("radio_server: pthread_create");

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
    
    if (set_nonblock(server_socket) < 0) {
        perror("radio_server: set_nonblock");

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

    epoll_fd = epoll_create1(0);

    if (epoll_fd == -1) {
        perror("radio_server: epoll_create1");

        return 1;
    }

    struct epoll_event ev, events[MAX_EPOLL_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
        perror("radio_server: epoll_ctl");

        return 1;
    }

    printf("[Radio Server] Live on port %d... (%d Threads)\n", RADIO_PORT, NUM_SENDER_THREADS);

    int current_thread_idx = 0;

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);

        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            
            perror("radio_server: epoll_wait");

            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_socket) {
                while(1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        else {
                            perror("radio_server: accept");

                            break;
                        }
                    }
                    
                    set_nonblock(client_fd);
                    
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: add client");

                        close(client_fd);
                    }
                }
            }
            else {
                int client_fd = events[i].data.fd;
                char dump[1024];

                ssize_t n = recv(client_fd, dump, sizeof(dump) - 1, 0);

                if (n <= 0) {
                     if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                         continue; 
                     }
                     
                     remove_event_subscriber(client_fd);

                     close(client_fd);
                     
                     epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);

                     continue;
                }

                dump[n] = '\0';
                
                //printf("[Radio] Request Buffer: %s\n", dump);


                if (strstr(dump, "Accept: text/event-stream") || strstr(dump, "/events")) {
                    remove_event_subscriber(client_fd);

                    const char* sse_header = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Connection: keep-alive\r\n"
                        "Access-Control-Allow-Origin: *\r\n\r\n";
                    
                    send(client_fd, sse_header, strlen(sse_header), MSG_NOSIGNAL);
                    
                    pthread_mutex_lock(&g_event_mutex);

                    if (g_cur_title[0] != '\0') {
                        char welcome_msg[2048];

                        snprintf(welcome_msg, sizeof(welcome_msg),
                            "data: {"
                            "\"playing\": \"%s\", "
                            "\"artist\": \"%s\", "
                            "\"album\": \"%s\", "
                            "\"title\": \"%s\", "
                            "\"bitrate\": 128, "
                            "\"is_request\": %s"
                            "}\n\n",
                            g_cur_file, g_cur_artist, g_cur_album, g_cur_title, g_cur_is_req ? "true" : "false");
                            
                        send(client_fd, welcome_msg, strlen(welcome_msg), MSG_NOSIGNAL);
                    }

                    pthread_mutex_unlock(&g_event_mutex);

                    EventSub* sub = malloc(sizeof(EventSub));
                    sub->fd = client_fd;
                    
                    pthread_mutex_lock(&g_event_mutex);

                    sub->next = g_event_subs;
                    g_event_subs = sub;

                    pthread_mutex_unlock(&g_event_mutex);
                    
                    printf("[Radio] New SSE Listener connected (fd=%d)\n", client_fd);

                    continue;
                }

                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);

                const char* header = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: audio/mpeg\r\n"
                                     "Transfer-Encoding: chunked\r\n"
                                     "Connection: keep-alive\r\n"
                                     "Cache-Control: no-cache, no-store\r\n"
                                     "Access-Control-Allow-Origin: *\r\n\r\n";
                
                if (send(client_fd, header, strlen(header), MSG_NOSIGNAL) < 0) {
                    close(client_fd);

                    continue;
                }

                SenderContext* target_thread = &g_senders[current_thread_idx];
                
                RadioClient* new_client = malloc(sizeof(RadioClient));
                
                if (new_client == NULL) {
                    perror("radio_server: malloc");

                    close(client_fd);

                    continue;
                }

                new_client->fd = client_fd;
                new_client->read_index = g_write_index;
                new_client->backlog = NULL;
                new_client->backlog_len = 0;
                pthread_mutex_lock(&target_thread->list_mutex);

                new_client->next = target_thread->client_list_head;
                target_thread->client_list_head = new_client;

                pthread_mutex_unlock(&target_thread->list_mutex);
                
                printf("[Radio Main] Assigned Listener (fd=%d) to Thread %d\n", client_fd, current_thread_idx);

                current_thread_idx = (current_thread_idx + 1) % NUM_SENDER_THREADS;
            }
        }
    }

    close(server_socket);
    close(epoll_fd);

    return 0;
}
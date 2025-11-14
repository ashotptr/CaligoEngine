#include "server.h"
#include <sys/stat.h>
#include <signal.h>

static time_t g_radio_start_time;

#define RADIO_PLAYLIST "public_html/mixtape/radio_playlist.mp3"
#define RADIO_BITRATE 16384

void init_radio_time(void) {
    g_radio_start_time = time(NULL);

    printf("[Radio Server] Broadcast clock started.\n");
}

void handle_radio_stream(int client_socket) {
    FILE* file = fopen(RADIO_PLAYLIST, "rb");

    if (file == NULL) {
        perror("RADIO ERROR: Could not open radio playlist");

        close(client_socket);

        return;
    }

    struct stat file_stat;

    if (fstat(fileno(file), &file_stat) < 0) {
        perror("RADIO ERROR: fstat");

        fclose(file);

        close(client_socket);

        return;
    }
    
    long file_size = file_stat.st_size;

    if (file_size == 0) {
        fprintf(stderr, "RADIO ERROR: Playlist file is empty.\n");

        fclose(file);

        close(client_socket);

        return;
    }

    long total_duration_sec = file_size / RADIO_BITRATE;
    
    if (total_duration_sec == 0) {
        total_duration_sec = 1;
    }

    time_t now = time(NULL);
    long time_elapsed = now - g_radio_start_time;
    long current_time_in_song = time_elapsed % total_duration_sec;
    long start_byte = current_time_in_song * RADIO_BITRATE;

    fseek(file, start_byte, SEEK_SET);

    char header[256];

    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: audio/mpeg\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Connection: keep-alive\r\n\r\n");

    send(client_socket, header, strlen(header), 0);
    
    char file_buffer[BUFFER_SIZE];
    char chunk_header[32];
    
    while (1) {
        size_t bytes_read = fread(file_buffer, 1, BUFFER_SIZE, file);

        if (bytes_read == 0) {
            fseek(file, 0, SEEK_SET);

            continue;
        }

        snprintf(chunk_header, sizeof(chunk_header), "%lx\r\n", bytes_read);
        
        if (send(client_socket, chunk_header, strlen(chunk_header), 0) < 0) {
            break;
        }

        if (send(client_socket, file_buffer, bytes_read, 0) < 0) {
            break;
        }
        
        if (send(client_socket, "\r\n", 2, 0) < 0) {
            break;
        }
    }

    printf("[Radio Server] Client disconnected.\n");
    
    fclose(file);
}

int main(void) {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    signal(SIGCHLD, SIG_IGN);

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

    init_radio_time();

    printf("[Radio Server] Live on port %d...\n", RADIO_PORT);

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_socket < 0) {
            perror("radio_server: accept");

            continue;
        }

        pid_t pid = fork();

        if (pid < 0) {
            perror("radio_server: fork");

            close(client_socket);
        }
        else if (pid == 0) {
            close(server_socket);
            
            printf("[Radio Server] New client connected.\n");
            
            char buffer[BUFFER_SIZE];

            recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

            handle_radio_stream(client_socket);
            
            close(client_socket);

            exit(0);
        }
        else {
            close(client_socket);
        }
    }

    close(server_socket);
    
    return 0;
}
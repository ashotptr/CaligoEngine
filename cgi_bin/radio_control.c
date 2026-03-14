#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jansson.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define RADIO_SOCKET_PATH "/tmp/caligo_radio.sock"

void send_json(int success, const char* msg) {
    char safe_msg[4096];
    char body[8192];
    int i = 0;

    for (const char* p = msg; *p && i < 4095; p++) {
        if (*p == '"') { 
            safe_msg[i++] = '\\'; safe_msg[i++] = '"'; 
        }
        else if (*p == '\n') { 
            safe_msg[i++] = ' '; 
        }
        else if (*p != '\r') { 
            safe_msg[i++] = *p; 
        }
    }

    safe_msg[i] = '\0';

    snprintf(body, sizeof(body), "{\"success\": %s, \"message\": \"%s\"}", 
             success ? "true" : "false", safe_msg);

    printf("HTTP/1.1 200 OK\r\n");
    printf("Content-Type: application/json\r\n");
    printf("Content-Length: %ld\r\n", strlen(body));
    printf("Connection: close\r\n\r\n");

    printf("%s", body);
    fflush(stdout);
}

int is_file_valid(const char* path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        return (st.st_size > 1024);
    }

    return 0;
}

int main() {
    srand(time(NULL));
    
    char *len_str = getenv("CONTENT_LENGTH");

    if (!len_str) { 
        send_json(0, "No content");

        return 0; 
    }

    int len = atoi(len_str);
    char *body = malloc(len + 1);

    fread(body, 1, len, stdin);

    body[len] = '\0';

    json_error_t error;
    json_t *root = json_loads(body, 0, &error);

    free(body);

    if (!root) { 
        send_json(0, "Invalid JSON");

        return 0;
    }
    
    const char *path = json_string_value(json_object_get(root, "path"));    
    const char *artist = json_string_value(json_object_get(root, "artist"));
    const char *album  = json_string_value(json_object_get(root, "album"));
    const char *title  = json_string_value(json_object_get(root, "title"));

    if (!path) { 
        send_json(0, "No path provided");

        return 0; 
    }

    char cwd[1024];

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        cwd[0] = '\0'; 
    }

    char source_path[4096];
    char temp_path[4096];

    snprintf(source_path, sizeof(source_path), "public_html/mixtape/%s", path);
    snprintf(temp_path, sizeof(temp_path), "/tmp/radio_%ld_%d.mp3", (long)time(NULL), rand());

    if (access(source_path, F_OK) != 0) {
        char err_msg[4192];

        snprintf(err_msg, sizeof(err_msg), "Input file not found at: %s", source_path);

        send_json(0, err_msg);

        json_decref(root);

        return 0;
    }

    char cmd[10240];

    snprintf(cmd, sizeof(cmd), 
        "/usr/bin/ffmpeg -y -i \"%s\" "
        "-vn "
        "-acodec libmp3lame "
        "-ar 44100 "
        "-ac 2 "
        "-b:a 128k "
        "-minrate 128k "
        "-maxrate 128k "
        "-bufsize 128k "
        "-f mp3 \"%s\" "
        "2> /tmp/ffmpeg_debug.log",
        source_path, temp_path);
    
    system(cmd);

    if (!is_file_valid(temp_path)) {
        char log_content[512] = "Transcoding failed (No output file). Check logs.";
        FILE *f = fopen("/tmp/ffmpeg_debug.log", "r");

        if (f) {
            fseek(f, 0, SEEK_END);

            long fsize = ftell(f);
            long seek_pos = (fsize > 200) ? fsize - 200 : 0;

            fseek(f, seek_pos, SEEK_SET);

            fread(log_content, 1, 200, f);

            log_content[200] = 0;

            fclose(f);
        }

        send_json(0, log_content);

        return 0;
    }
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, RADIO_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        json_t *srv_cmd = json_object();
        json_object_set_new(srv_cmd, "file", json_string(temp_path));
        json_object_set_new(srv_cmd, "temp", json_true());        
        json_object_set_new(srv_cmd, "artist", json_string(artist ? artist : ""));
        json_object_set_new(srv_cmd, "album",  json_string(album  ? album  : ""));
        json_object_set_new(srv_cmd, "title",  json_string(title  ? title  : ""));

        char *json_payload = json_dumps(srv_cmd, 0);
        
        char msg[8192];
        
        snprintf(msg, sizeof(msg), "QUEUE_JSON %s", json_payload);

        write(sock_fd, msg, strlen(msg));
        
        free(json_payload);

        json_decref(srv_cmd);
        
        close(sock_fd);

        char body[5120]; 
        
        snprintf(body, sizeof(body), 
            "{\"success\": true, \"message\": \"Queued successfully\", \"track_id\": \"%s\"}", 
            temp_path);

        printf("HTTP/1.1 200 OK\r\n");
        printf("Content-Type: application/json\r\n");
        printf("Content-Length: %ld\r\n", strlen(body));
        printf("Connection: close\r\n\r\n");
        printf("%s", body);
        
        fflush(stdout);
    }
    else {
        unlink(temp_path);

        send_json(0, "Radio server offline");
    }
    
    json_decref(root);

    return 0;
}
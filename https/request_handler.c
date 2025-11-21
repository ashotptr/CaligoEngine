#include "server.h"
#include <sys/stat.h>
#include <limits.h>

RouteRule g_routes[MAX_ROUTES];
int g_route_count = 0;

static unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length) {
    static char b64_table[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4,
        5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
        0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
        40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
    };

    if (input_length % 4 != 0) {
        return NULL;
    }

    *output_length = input_length / 4 * 3;

    if (data[input_length - 1] == '=') {
        (*output_length)--;
    }

    if (data[input_length - 2] == '=') {
        (*output_length)--;
    }

    unsigned char* decoded_data = malloc(*output_length + 1);

    if (decoded_data == NULL) {
        return NULL;
    }

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = data[i] == '=' ? (i++, 0) : b64_table[(unsigned char)data[i++]];
        uint32_t sextet_b = data[i] == '=' ? (i++, 0) : b64_table[(unsigned char)data[i++]];
        uint32_t sextet_c = data[i] == '=' ? (i++, 0) : b64_table[(unsigned char)data[i++]];
        uint32_t sextet_d = data[i] == '=' ? (i++, 0) : b64_table[(unsigned char)data[i++]];
        uint32_t triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;

        if (j < *output_length) {
            decoded_data[j++] = (triple >> 16) & 0xFF;
        }

        if (j < *output_length) {
            decoded_data[j++] = (triple >> 8) & 0xFF;
        }

        if (j < *output_length) {
            decoded_data[j++] = triple & 0xFF;
        }
    }

    decoded_data[*output_length] = '\0';

    return decoded_data;
}

int ssl_send_response(ClientState* client, const char* response, size_t len) {
    if (!client->ssl) {
        return -1;
    }

    if (client->pending_write_len > 0) {
        client->pending_write_data = realloc(client->pending_write_data, client->pending_write_len + len);

        memcpy(client->pending_write_data + client->pending_write_len, response, len);

        client->pending_write_len += len;

        return 1;
    }

    int sent = SSL_write(client->ssl, response, len);

    if (sent > 0) {
        if ((size_t)sent < len) {
            size_t remaining = len - sent;
            
            client->pending_write_data = malloc(remaining);
            
            memcpy(client->pending_write_data, response + sent, remaining);
            
            client->pending_write_len = remaining;
            
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.ptr = client;

            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
            
            return 1;
        }

        return 0;
    }
    else {
        int err = SSL_get_error(client->ssl, sent);

        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {            
            client->pending_write_data = malloc(len);

            memcpy(client->pending_write_data, response, len);

            client->pending_write_len = len;

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.ptr = client;

            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
            
            return 1;
        }
        
        return -1;
    }
}

static char* find_header_value(char* request, const char* header_name) {
    char* header_line = strcasestr(request, header_name);

    if (!header_line) {
        return NULL;
    }

    char* value_start = strchr(header_line, ':');
    
    if (!value_start) {
        return NULL;
    }

    value_start++;

    while (*value_start == ' ') {
        value_start++;
    }

    char* value_end = strstr(value_start, "\r\n");

    if (!value_end) {
        return NULL;
    }

    *value_end = '\0';
    char* value = strdup(value_start);
    *value_end = '\r';

    return value;
}

static void send_401_unauthorized(int client_socket, ClientState* client) {
    const char* response = "HTTP/1.1 401 Unauthorized\r\n"
                           "WWW-Authenticate: Basic realm=\"My Protected Server\"\r\n"
                           "Content-Length: 0\r\n\r\n";

    ssl_send_response(client, response, strlen(response));
}

static void send_404_not_found(int client_socket, ClientState* client) {
    char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

    ssl_send_response(client, response, strlen(response));
}

static void send_502_bad_gateway(int client_socket, ClientState* client) {
    char response[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";

    ssl_send_response(client, response, strlen(response));
}

static const char* get_content_type(const char* path) {
    if (strstr(path, ".html")) {
        return "text/html";
    }

    if (strstr(path, ".css")) {
        return "text/css";
    }

    if (strstr(path, ".js")) {
        return "application/javascript";
    }

    if (strstr(path, ".mp3")) {
        return "audio/mpeg";
    }

    if (strstr(path, ".jpg")) {
        return "image/jpeg";
    }

    if (strstr(path, ".png")) {
        return "image/png";
    }

    return "application/octet-stream";
}

static void serve_static_file(int client_socket, char* request_buffer, const char* path_prefix, const char* requested_path, ClientState* client) {
    char full_path[512];
    char resolved_path[PATH_MAX];
    char resolved_prefix[PATH_MAX];

    snprintf(full_path, sizeof(full_path), "%s%s", path_prefix, requested_path);

    if (realpath(full_path, resolved_path) == NULL) {
        send_404_not_found(client_socket, client);

        return;
    }

    if (realpath(path_prefix, resolved_prefix) == NULL) {
        perror("FATAL: realpath failed for path_prefix");

        send_502_bad_gateway(client_socket, client);
        
        return;
    }

    if (strncmp(resolved_path, resolved_prefix, strlen(resolved_prefix)) != 0) {
        send_404_not_found(client_socket, client);

        return;
    }
    
    const char* content_type = get_content_type(resolved_path);
    FILE* file = fopen(resolved_path, "rb");

    if (file == NULL) {
        send_404_not_found(client_socket, client);

        return;
    }

    struct stat file_stat;
    if (fstat(fileno(file), &file_stat) < 0) {
        perror("fstat");

        fclose(file);

        send_404_not_found(client_socket, client);

        return;
    }

    long file_size = file_stat.st_size;
    char header[512];
    long start_byte = 0;
    long end_byte = file_size - 1;
    int status_code = 200;
    char* range_header = find_header_value(request_buffer, "Range");

    if (range_header) {
        status_code = 206;
        
        if (sscanf(range_header, "bytes=%ld-%ld", &start_byte, &end_byte) == 2) {
            
        } 
        else if (sscanf(range_header, "bytes=%ld-", &start_byte) == 1) {
            end_byte = file_size - 1;
        }

        fseek(file, start_byte, SEEK_SET);
        free(range_header);
    }

    long content_length = (end_byte - start_byte) + 1;

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Accept-Ranges: bytes\r\n"
             "Content-Range: bytes %ld-%ld/%ld\r\n"
             "Connection: keep-alive\r\n\r\n",
             status_code, (status_code == 200 ? "OK" : "Partial Content"),
             content_type,
             content_length,
             start_byte, end_byte, file_size);

    if (ssl_send_response(client, header, strlen(header)) < 0) {
        fclose(file);

        return; 
    }

    char file_buffer[BUFFER_SIZE];
    size_t bytes_read;
    int transfer_complete = 1;

    if (client->pending_write_len == 0) {
        while (!feof(file)) {
            bytes_read = fread(file_buffer, 1, BUFFER_SIZE, file);
            
            if (bytes_read == 0) {
                break;
            }

            int ret = ssl_send_response(client, file_buffer, bytes_read);
            
            if (ret < 0) {
                fclose(file);
                
                return;
            }
            
            if (ret == 1) {
                client->file_stream = file;
                transfer_complete = 0;

                break;
            }
        }
    }
    else {
        client->file_stream = file;
        transfer_complete = 0;
    }

    if (transfer_complete) {
        fclose(file);

        client->file_stream = NULL;
    }
}

static void handle_cgi_request(int client_socket, char* request_buffer, const char* path_prefix, const char* requested_path, ClientState* client) {
    char full_path[512];

    snprintf(full_path, sizeof(full_path), "./%s%s", path_prefix, requested_path + strlen(path_prefix));

    struct stat st;

    if (stat(full_path, &st) < 0 || !(st.st_mode & S_IXUSR)) {
        send_404_not_found(client_socket, client);

        return;
    }
    
    char* query_string = "";
    char* auth_header = find_header_value(request_buffer, "Authorization");

    char* query_start = strchr(request_buffer, '?');

    if (query_start) {
        char* query_end = strchr(query_start, ' ');

        if (query_end) {
            *query_end = '\0'; 
            query_string = query_start + 1;
        }
    }

    setenv("QUERY_STRING", query_string, 1);

    if (auth_header) {
        setenv("HTTP_AUTHORIZATION", auth_header, 1);

        free(auth_header);
    }

    FILE* pipe = popen(full_path, "r");

    if (!pipe) {
        perror("popen failed");

        char response[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";

        ssl_send_response(client, response, strlen(response));

        return;
    }

    char buffer[1024];
    size_t bytes_read;
    client->is_cgi = 0;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        if (client->pending_write_len > 4 * 1024 * 1024) {
            client->file_stream = pipe;
            client->is_cgi = 1;

            return;
        }

        if (ssl_send_response(client, buffer, bytes_read) < 0) {
            pclose(pipe);

            return;
        }
    }

    if (client->file_stream == NULL) {
        pclose(pipe);
    }
}

static int check_authentication(int client_socket, char* request_buffer, ClientState* client) {
    char* auth_header = find_header_value(request_buffer, "Authorization");
    int authorized = 0;

    if (auth_header) {
        if (strncmp(auth_header, "Basic ", 6) == 0) {
            char* b64_token = auth_header + 6;
            size_t decoded_len;
            unsigned char* decoded = base64_decode(b64_token, strlen(b64_token), &decoded_len);
            
            if (decoded) {
                if (strcmp((char*)decoded, "admin:password123") == 0) {
                    authorized = 1;
                }

                free(decoded);
            }
        }

        free(auth_header);
    }

    if (!authorized) {
        printf("Worker Thread: Auth failed. Sending 401.\n");

        send_401_unauthorized(client_socket, client);
    }

    return authorized;
}

static void handle_proxy_request_async(ClientState* client) {
    int upstream_socket;
    struct sockaddr_in upstream_addr;

    upstream_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (upstream_socket < 0) {
        perror("proxy: socket");

        send_502_bad_gateway(client->fd, client);
        
        return;
    }

    memset(&upstream_addr, 0, sizeof(upstream_addr));

    upstream_addr.sin_family = AF_INET;
    upstream_addr.sin_port = htons(RADIO_PORT);
    upstream_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    set_nonblock(upstream_socket);

    if (connect(upstream_socket, (struct sockaddr*)&upstream_addr, sizeof(upstream_addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("proxy: connect");

            send_502_bad_gateway(client->fd, client);
            
            client->bytes_read = 0;

            bzero(client->buffer, BUFFER_SIZE); //memset

            client->state = STATE_READ_REQUEST;

            close(upstream_socket);

            return;
        }
    }

    if (send(upstream_socket, client->buffer, client->bytes_read, 0) < 0) {
        perror("proxy: send");

        send_502_bad_gateway(client->fd, client);
        
        client->bytes_read = 0;

        bzero(client->buffer, BUFFER_SIZE);

        client->state = STATE_READ_REQUEST;

        close(upstream_socket);
        
        client->peer = NULL;
        
        return;
    }
    
    ClientState* upstream_state = create_client_state(upstream_socket);

    client->state = STATE_PROXYING;
    client->peer = upstream_state;
    upstream_state->state = STATE_PROXYING;
    upstream_state->peer = client;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = upstream_state;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, upstream_socket, &ev) == -1) {
        perror("epoll_ctl: add upstream_socket");
        
        free(upstream_state);

        client->state = STATE_READ_REQUEST;
        client->peer = NULL;
        client->bytes_read = 0;

        bzero(client->buffer, BUFFER_SIZE);

        close(upstream_socket);
    }
}

void load_config_file(const char* filename) {
    printf("Loading config file: %s\n", filename);

    FILE* file = fopen(filename, "r");

    if (file == NULL) {
        perror("FATAL: Could not open server.conf");

        exit(1);
    }

    char line[512];
    g_route_count = 0;

    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        char type_str[32], path[256], target[256];

        if (sscanf(line, "%s %s %s", type_str, path, target) == 3) {
            if (g_route_count >= MAX_ROUTES) {
                fprintf(stderr, "FATAL: Exceeded MAX_ROUTES\n");

                break;
            }

            RouteRule* rule = &g_routes[g_route_count];

            strncpy(rule->path, path, sizeof(rule->path) - 1);
            strncpy(rule->target, target, sizeof(rule->target) - 1);

            rule->needs_auth = 0;

            if (strcmp(type_str, "STATIC") == 0) {
                rule->type = ROUTE_STATIC;
            }
            else if (strcmp(type_str, "CGI") == 0) {
                rule->type = ROUTE_CGI;
            }
            else if (strcmp(type_str, "PROXY") == 0) {
                rule->type = ROUTE_PROXY;
            }
            else {
                continue; 
            }

            printf("Config: Loaded route %s -> %s (%s)\n", rule->path, rule->target, type_str);

            g_route_count++;
        }
        else if (sscanf(line, "%s %s", type_str, path) == 2) {
            if (strcmp(type_str, "AUTH") == 0) {
                for (int i = 0; i < g_route_count; i++) {
                    if (strcmp(g_routes[i].path, path) == 0) {
                        g_routes[i].needs_auth = 1;

                        printf("Config: Added AUTH to route %s\n", path);
                    }
                }
            }
        }
    }

    fclose(file);
    
    printf("Loaded %d rules:\n", g_route_count);
    for (int i = 0; i < g_route_count; i++) {
        printf("  Rule %d: %s -> %s (Auth: %d)\n", i, 
            g_routes[i].path, g_routes[i].target, g_routes[i].needs_auth);
    }
}

void handle_work(ClientState* client) {
    char* request_buffer = client->buffer;
    int client_socket = client->fd;

    if (request_buffer == NULL) {
        return;
    }
    
    char *path_start = strchr(request_buffer, ' ');

    if (!path_start) { 
        send_404_not_found(client_socket, client);

        return; 
    }

    path_start++;

    char *path_end = strchr(path_start, ' ');

    if (!path_end) { 
        send_404_not_found(client_socket, client);
        
        return; 
    }

    char *query_pos = strchr(path_start, '?');
    
    if (query_pos != NULL && query_pos < path_end) {
        path_end = query_pos; 
    }

    char requested_path[256];
    size_t path_len = path_end - path_start;

    if (path_len > sizeof(requested_path) - 1) { 
        send_404_not_found(client_socket, client);
        
        return;
    }

    strncpy(requested_path, path_start, path_len);

    requested_path[path_len] = '\0';
    
    if (strcmp(requested_path, "/") == 0) {
        strncpy(requested_path, "/index.html", sizeof(requested_path) - 1);
    }

    if (strstr(requested_path, "..") != NULL) {
        send_404_not_found(client_socket, client);

        return;
    }

    RouteRule* best_rule = NULL;
    int best_match_len = -1;

    for (int i = 0; i < g_route_count; i++) {
        int rule_len = strlen(g_routes[i].path);

        if (strncmp(requested_path, g_routes[i].path, rule_len) == 0) {
            if (rule_len > best_match_len) {
                best_match_len = rule_len;
                best_rule = &g_routes[i];
            }
        }
    }

    if (best_rule == NULL) {
        printf("Worker Thread: 404 Not Found (No route rule for: %s)\n", requested_path);

        send_404_not_found(client_socket, client);
    }
    else {
        if (best_rule->needs_auth) {
            if (!check_authentication(client_socket, request_buffer, client)) {
                return;
            }
        }

        if (best_rule->type == ROUTE_STATIC) {
            printf("Worker Thread: Routing to STATIC: %s\n", best_rule->target);
            
            serve_static_file(client_socket, request_buffer, best_rule->target, requested_path, client);
        }
        else if (best_rule->type == ROUTE_CGI) {
            printf("Worker Thread: Routing to CGI: %s\n", best_rule->target);
 
            handle_cgi_request(client_socket, request_buffer, best_rule->target, requested_path, client);        
        }
        else if (best_rule->type == ROUTE_PROXY) {
            printf("Worker Thread: Routing to PROXY: %s\n", best_rule->target);

            handle_proxy_request_async(client);

            return;
        }
    }
    
    struct epoll_event ev;
    ev.data.ptr = client;

    if (client->pending_write_len > 0 || client->file_stream != NULL || client->ssl_want_write) {
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET| EPOLLONESHOT;
    }
    else {
        client->state = STATE_READ_REQUEST;
        client->bytes_read = 0;

        bzero(client->buffer, BUFFER_SIZE);

        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->fd, &ev) == -1) {
        if (client->ssl) {
            SSL_free(client->ssl);
        }

        close(client->fd);
        
        free(client);
    }
}
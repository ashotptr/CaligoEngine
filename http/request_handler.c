#include "server.h"
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

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

static void send_401_unauthorized(int client_socket) {
    const char* response = "HTTP/1.1 401 Unauthorized\r\n"
                           "WWW-Authenticate: Basic realm=\"My Protected Server\"\r\n"
                           "Content-Length: 0\r\n\r\n";

    send(client_socket, response, strlen(response), 0);
}

static void send_404_not_found(int client_socket) {
    char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

    send(client_socket, response, strlen(response), 0);
}

static void send_502_bad_gateway(int client_socket) {
    char response[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";

    send(client_socket, response, strlen(response), 0);
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

static void serve_static_file(int client_socket, char* request_buffer, const char* path_prefix, const char* requested_path) {
    char full_path[512];
    char resolved_path[PATH_MAX];
    char resolved_prefix[PATH_MAX];

    snprintf(full_path, sizeof(full_path), "%s%s", path_prefix, requested_path);

    if (realpath(full_path, resolved_path) == NULL) {
        send_404_not_found(client_socket);

        return;
    }

    if (realpath(path_prefix, resolved_prefix) == NULL) {
        perror("FATAL: realpath failed for path_prefix");

        send_502_bad_gateway(client_socket);
        
        return;
    }

    if (strncmp(resolved_path, resolved_prefix, strlen(resolved_prefix)) != 0) {
        send_404_not_found(client_socket);

        return;
    }
    
    const char* content_type = get_content_type(resolved_path);
    FILE* file = fopen(resolved_path, "rb");

    if (file == NULL) {
        send_404_not_found(client_socket);

        return;
    }

    struct stat file_stat;
    if (fstat(fileno(file), &file_stat) < 0) {
        perror("fstat");

        fclose(file);

        send_404_not_found(client_socket);

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
             "Connection: close\r\n\r\n",//idk Connection: keep-alive
             status_code, (status_code == 200 ? "OK" : "Partial Content"),
             content_type,
             content_length,
             start_byte, end_byte, file_size);

    send(client_socket, header, strlen(header), 0);

    char file_buffer[BUFFER_SIZE];
    size_t bytes_to_send = content_length;

    while (bytes_to_send > 0) {
        size_t bytes_to_read = (bytes_to_send > BUFFER_SIZE) ? BUFFER_SIZE : bytes_to_send;
        size_t bytes_read = fread(file_buffer, 1, bytes_to_read, file);

        if (bytes_read <= 0) {
            break; 
        }

        if (send(client_socket, file_buffer, bytes_read, 0) < 0) {
            break; 
        }

        bytes_to_send -= bytes_read;
    }

    fclose(file);
}

static void handle_cgi_request(int client_socket, char* request_buffer, const char* path_prefix, const char* requested_path) {
    char full_path[512];
    
    snprintf(full_path, sizeof(full_path), "./%s%s", path_prefix, requested_path + strlen(path_prefix));

    struct stat st;
    
    if (stat(full_path, &st) < 0 || !(st.st_mode & S_IXUSR)) {
        send_404_not_found(client_socket); 
        
        return;
    }

    char *method = "GET";
    int content_length = 0;

    if (strncmp(request_buffer, "POST", 4) == 0) {
        method = "POST";

        char *cl_ptr = strcasestr(request_buffer, "Content-Length:");

        if (cl_ptr) {
            content_length = atoi(cl_ptr + 15);
        }
    }

    int input_pipe[2], output_pipe[2];
    
    if (pipe(input_pipe) < 0 || pipe(output_pipe) < 0) {
        perror("pipe");
        
        char response[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";

        send(client_socket, response, strlen(response), 0);

        return; 
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        
        char response[] = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        
        send(client_socket, response, strlen(response), 0);
        
        return;
    }
    if (pid == 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);

        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        
        setenv("REQUEST_METHOD", method, 1);

        char len_str[32];
        
        sprintf(len_str, "%d", content_length);
        
        setenv("CONTENT_LENGTH", len_str, 1);

        char* query_string = "";

        char* query_start = strchr(request_buffer, '?');

        if (query_start) {
            char* query_end = strchr(query_start, ' ');

            if (query_end) { 
                *query_end = '\0'; 
                query_string = query_start + 1; 
            }
        }

        setenv("QUERY_STRING", query_string, 1);

        char* auth_header = find_header_value(request_buffer, "Authorization");
        
        if (auth_header) {
            setenv("HTTP_AUTHORIZATION", auth_header, 1);

            free(auth_header);
        }

        execl(full_path, full_path, NULL); 
        exit(1);
    }
    else {
        close(input_pipe[0]);
        close(output_pipe[1]);

        if (strcmp(method, "POST") == 0 && content_length > 0) {
            printf("DEBUG: POST request. Expecting %d bytes.\n", content_length);

            char *body_start = strstr(request_buffer, "\r\n\r\n");
            int bytes_written = 0;

            if (body_start) {
                body_start += 4;
                
                int header_len = body_start - request_buffer;
                int total_buffered = strlen(request_buffer); 
                int body_in_buffer = total_buffered - header_len;

                printf("DEBUG: Found %d bytes in initial buffer.\n", body_in_buffer);

                if (body_in_buffer > 0) {
                    if (body_in_buffer > content_length) body_in_buffer = content_length;
                    
                    printf("DEBUG: Writing initial chunk: '%.*s'\n", body_in_buffer, body_start);
                    
                    write(input_pipe[1], body_start, body_in_buffer);

                    bytes_written += body_in_buffer;
                }
            }

            if (bytes_written < content_length) {
                int remaining = content_length - bytes_written;

                printf("DEBUG: Reading remaining %d bytes using epoll...\n", remaining);

                int local_epoll_fd = epoll_create1(0);
                if (local_epoll_fd == -1) {
                    perror("epoll_create1");
                }
                else {
                    struct epoll_event ev, events[1];
                    ev.events = EPOLLIN;
                    ev.data.fd = client_socket;

                    if (epoll_ctl(local_epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1) {
                        perror("epoll_ctl");
                    }

                    char buf[4096];
                    while (remaining > 0) {
                        int nfds = epoll_wait(local_epoll_fd, events, 1, 2000);

                        if (nfds == -1) {
                            perror("epoll_wait");

                            break;
                        }
                        else if (nfds == 0) {
                            printf("DEBUG: Timeout waiting for POST data\n");
                            
                            break;
                        }

                        ssize_t n = recv(client_socket, buf, (remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining), 0);                        

                        if (n > 0) {
                            printf("DEBUG: Read %ld bytes from socket\n", n);
                            
                            write(input_pipe[1], buf, n);
                            
                            remaining -= n;
                        }
                        else if (n == 0) {
                            printf("DEBUG: Client closed connection\n");
                            
                            break;
                        }
                        else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }

                            perror("recv");
                            
                            break;
                        }
                    }

                    close(local_epoll_fd);
                }
            }
            else {
                printf("DEBUG: Entire body was in first packet.\n");
            }
        }
        
        close(input_pipe[1]);

        char buffer[1024];
        ssize_t bytes_read;
        
        while ((bytes_read = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
            if (send(client_socket, buffer, bytes_read, 0) < 0) {
                break; 
            }
        }

        close(output_pipe[0]);

        waitpid(pid, NULL, 0);
    }
}

static int check_authentication(int client_socket, char* request_buffer) {
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

        send_401_unauthorized(client_socket);
    }

    return authorized;
}

static void handle_proxy_request_async(ClientState* client) {
    int upstream_socket;
    struct sockaddr_in upstream_addr;

    upstream_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (upstream_socket < 0) {
        perror("proxy: socket");

        send_502_bad_gateway(client->fd);
        
        return;
    }

    memset(&upstream_addr, 0, sizeof(upstream_addr));

    upstream_addr.sin_family = AF_INET;
    upstream_addr.sin_port = htons(RADIO_PORT);
    upstream_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(upstream_socket, (struct sockaddr*)&upstream_addr, sizeof(upstream_addr)) < 0) {
        perror("proxy: connect");

        send_502_bad_gateway(client->fd);
        
        client->bytes_read = 0;

        bzero(client->buffer, BUFFER_SIZE); //memset

        client->state = STATE_READ_REQUEST;

        close(upstream_socket);

        return;
    }

    if (send(upstream_socket, client->buffer, client->bytes_read, 0) < 0) {
        perror("proxy: send");

        send_502_bad_gateway(client->fd);
        
        client->bytes_read = 0;

        bzero(client->buffer, BUFFER_SIZE);

        client->state = STATE_READ_REQUEST;

        close(upstream_socket);

        return;
    }
    
    set_nonblock(upstream_socket);
    
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
        close(client_socket);
        
        return;
    }
    
    char *path_start = strchr(request_buffer, ' ');

    //idk
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags & ~O_NONBLOCK);
    //idk

    if (!path_start) { 
        send_404_not_found(client_socket);
        
        close(client_socket);
        
        return; 
    }

    char method[16] = {0};
    size_t method_len = path_start - request_buffer;
    
    if (method_len < sizeof(method)) {
        strncpy(method, request_buffer, method_len);
    }

    path_start++;
    
    char *path_end = strchr(path_start, ' ');
    
    if (!path_end) { 
        send_404_not_found(client_socket); 
        
        close(client_socket);
        
        return; 
    }

    char *query_pos = strchr(path_start, '?');
    
    if (query_pos != NULL && query_pos < path_end) {
        path_end = query_pos; 
    }

    char requested_path[256];
    size_t path_len = path_end - path_start;
    
    if (path_len > sizeof(requested_path) - 1) { 
        send_404_not_found(client_socket); 
        
        close(client_socket); 
        
        return;
    }

    strncpy(requested_path, path_start, path_len);
    
    requested_path[path_len] = '\0';
    
    if (strcmp(requested_path, "/") == 0) {
        strncpy(requested_path, "/index.html", sizeof(requested_path) - 1);
    }

    if (strstr(requested_path, "..") != NULL) {
        send_404_not_found(client_socket);
        
        close(client_socket);
        
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
        
        send_404_not_found(client_socket);
    }
    else {
        if (best_rule->needs_auth) {
            if (!check_authentication(client_socket, request_buffer)) {
                close(client_socket);
                
                return;
            }
        }

        if (best_rule->type == ROUTE_STATIC) {
            printf("Worker Thread: Routing to STATIC: %s\n", best_rule->target);
            
            serve_static_file(client_socket, request_buffer, best_rule->target, requested_path);
        }
        else if (best_rule->type == ROUTE_CGI) {
            printf("Worker Thread: Routing to CGI: %s\n", best_rule->target);
 
            handle_cgi_request(client_socket, request_buffer, best_rule->target, requested_path);        
        }
        else if (best_rule->type == ROUTE_PROXY) {
            printf("Worker Thread: Routing to PROXY: %s\n", best_rule->target);
                
            handle_proxy_request_async(client);
            
            set_nonblock(client->fd);

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.ptr = client;
            
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->fd, &ev) == -1) {
                perror("Worker Thread: Failed to re-add proxy client to epoll");

                close(client->fd);

                return;
            }
            
            return; 
        }
    }
    
    client->bytes_read = 0;
    client->state = STATE_READ_REQUEST;

    bzero(client->buffer, BUFFER_SIZE);

    set_nonblock(client->fd);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = client;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->fd, &ev) == -1) {
        perror("Worker Thread: Failed to re-arm client in epoll");

        close(client->fd);
    }
}
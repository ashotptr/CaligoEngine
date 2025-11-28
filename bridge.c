#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#define TARGET_IP "127.0.0.1"
#define MAX_EVENTS 1024 
#define BUFFER_SIZE 8192

int g_target_port = 5222;

typedef enum { 
    STATE_HANDSHAKE, 
    STATE_ESTABLISHED 
} State;

struct Session {
    int client_fd;
    int target_fd;
    State state;
};

struct Session *sessions[MAX_EVENTS];

#define ROL(val, n) (((val) << (n)) | ((val) >> (32 - (n))))

void sha1(const unsigned char *data, size_t len, unsigned char *hash) {
    unsigned int h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    unsigned int a, b, c, d, e, w[80], i, j;
    unsigned char buf[64];
    
    size_t num_blocks = len / 64;

    for (i = 0; i < num_blocks; i++) {
        const unsigned char *block = data + i * 64;

        for (j = 0; j < 16; j++){
            w[j] = (block[j*4] << 24) | (block[j*4+1] << 16) | (block[j*4+2] << 8) | block[j*4+3];
        }

        for (j = 16; j < 80; j++){
            w[j] = ROL(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);
        }

        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

        for (j = 0; j < 80; j++) {
            unsigned int f, k;
            if (j < 20) { 
                f = (b & c) | ((~b) & d); 
                k = 0x5A827999; 
            }
            else if (j < 40) { 
                f = b ^ c ^ d; k = 0x6ED9EBA1; 
            }
            else if (j < 60) {
                f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; 
            }
            else { 
                f = b ^ c ^ d; k = 0xCA62C1D6; 
            }

            unsigned int temp = ROL(a, 5) + f + e + k + w[j];

            e = d; 
            d = c; 
            c = ROL(b, 30); 
            b = a; 
            a = temp;
        }

        h[0] += a; 
        h[1] += b; 
        h[2] += c; 
        h[3] += d; 
        h[4] += e;
    }
    
    size_t idx = 0;
    size_t remaining = len % 64;

    for (j = 0; j < remaining; j++) {
        buf[idx++] = data[num_blocks * 64 + j];
    }
    
    buf[idx++] = 0x80;
    
    if (idx > 56) {
        while (idx < 64) {
            buf[idx++] = 0;
        }
        
        for (j = 0; j < 16; j++) {
            w[j] = (buf[j*4] << 24) | (buf[j*4+1] << 16) | (buf[j*4+2] << 8) | buf[j*4+3];
        }

        for (j = 16; j < 80; j++) {
            w[j] = ROL(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);
        }

        a = h[0]; 
        b = h[1]; 
        c = h[2]; 
        d = h[3]; 
        e = h[4];

        for (j = 0; j < 80; j++) {
            unsigned int f, k;
            if (j < 20) { 
                f = (b & c) | ((~b) & d); 
                k = 0x5A827999; 
            }
            else if (j < 40) { 
                f = b ^ c ^ d; 
                k = 0x6ED9EBA1; 
            }
            else if (j < 60) { 
                f = (b & c) | (b & d) | (c & d); 
                k = 0x8F1BBCDC; 
            }
            else { 
                f = b ^ c ^ d; 
                k = 0xCA62C1D6; 
            }

            unsigned int temp = ROL(a, 5) + f + e + k + w[j];

            e = d; 
            d = c; 
            c = ROL(b, 30); 
            b = a; 
            a = temp;
        }

        h[0] += a; 
        h[1] += b; 
        h[2] += c; 
        h[3] += d; 
        h[4] += e;

        idx = 0;
    }

    while (idx < 56) {
        buf[idx++] = 0;
    }

    unsigned long long bit_len = len * 8;

    for (j = 0; j < 8; j++) {
        buf[63 - j] = (bit_len >> (j * 8)) & 0xFF;
    }

    for (j = 0; j < 16; j++) {
        w[j] = (buf[j*4] << 24) | (buf[j*4+1] << 16) | (buf[j*4+2] << 8) | buf[j*4+3];
    }

    for (j = 16; j < 80; j++) {
        w[j] = ROL(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);
    }

    a = h[0]; 
    b = h[1]; 
    c = h[2]; 
    d = h[3]; 
    e = h[4];

    for (j = 0; j < 80; j++) {
        unsigned int f, k;

        if (j < 20) { 
            f = (b & c) | ((~b) & d); 
            k = 0x5A827999; 
        }
        else if (j < 40) { 
            f = b ^ c ^ d; 
            k = 0x6ED9EBA1; 
        }
        else if (j < 60) { 
            f = (b & c) | (b & d) | (c & d); 
            k = 0x8F1BBCDC; 
        }
        else { 
            f = b ^ c ^ d; 
            k = 0xCA62C1D6; 
        }

        unsigned int temp = ROL(a, 5) + f + e + k + w[j];

        e = d; 
        d = c; 
        c = ROL(b, 30); 
        b = a; 
        a = temp;
    }

    h[0] += a; 
    h[1] += b; 
    h[2] += c; 
    h[3] += d; 
    h[4] += e;

    for (i = 0; i < 5; i++) {
        hash[i*4] = (h[i] >> 24) & 0xFF;
        hash[i*4+1] = (h[i] >> 16) & 0xFF;
        hash[i*4+2] = (h[i] >> 8) & 0xFF;
        hash[i*4+3] = h[i] & 0xFF;
    }
}

void base64_encode(const unsigned char *data, size_t input_length, char *encoded_data) {
    static char encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                    'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                    '4', '5', '6', '7', '8', '9', '+', '/'};
    size_t i, j;
    int mod_table[] = {0, 2, 1};

    for (i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
    }

    for (i = 0; i < (size_t)mod_table[input_length % 3]; i++){
        encoded_data[j - 1 - i] = '=';
    }

    encoded_data[j] = '\0';
}

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

int handle_handshake(struct Session *s, char *buffer) {
    printf("[Bridge] >>> HANDSHAKE REQUEST <<<\n%s\n", buffer);

    char *key_start = strcasestr(buffer, "Sec-WebSocket-Key: ");

    if (!key_start) {
        printf("[Bridge] Error: Missing Sec-WebSocket-Key\n");

        return -1;
    }

    key_start += 19;
    
    char *key_end = strstr(key_start, "\r\n");
    
    if (!key_end) {
        return -1;
    }

    char key[64];
    size_t key_len = key_end - key_start;

    if (key_len >= sizeof(key)) {
        key_len = sizeof(key) - 1;
    }

    strncpy(key, key_start, key_len);

    key[key_len] = '\0';

    char protocol[64] = {0};
    char *proto_start = strcasestr(buffer, "Sec-WebSocket-Protocol: ");

    if (proto_start) {
        proto_start += 24;

        char *proto_end = strstr(proto_start, "\r\n");
        
        if (proto_end) {
            size_t proto_len = proto_end - proto_start;

            if (proto_len >= sizeof(protocol)) {
                proto_len = sizeof(protocol) - 1;
            }

            strncpy(protocol, proto_start, proto_len);

            protocol[proto_len] = '\0';
            
            char *end = protocol + strlen(protocol) - 1;

            while(end > protocol && isspace((unsigned char)*end)) {
                end--;
            }

            end[1] = '\0';
        }
    }

    char accept_str[256];

    snprintf(accept_str, sizeof(accept_str), "%s%s", key, WS_GUID);

    unsigned char hash[20];

    sha1((unsigned char *)accept_str, strlen(accept_str), hash);

    char base64_hash[64];

    base64_encode(hash, 20, base64_hash);

    char response[512];
    int offset = 0;
    
    offset += sprintf(response + offset, "HTTP/1.1 101 Switching Protocols\r\n");
    offset += sprintf(response + offset, "Upgrade: websocket\r\n");
    offset += sprintf(response + offset, "Connection: Upgrade\r\n");
    offset += sprintf(response + offset, "Sec-WebSocket-Accept: %s\r\n", base64_hash);
    
    if (strlen(protocol) > 0) {
        char *comma = strchr(protocol, ',');

        if (comma) {
            *comma = '\0';
        }
        
        offset += sprintf(response + offset, "Sec-WebSocket-Protocol: %s\r\n", protocol);
    }
    
    offset += sprintf(response + offset, "\r\n");

    printf("[Bridge] <<< SENDING HANDSHAKE RESPONSE <<<\n%s\n", response);

    send(s->client_fd, response, strlen(response), 0);

    s->state = STATE_ESTABLISHED;
    
    int target_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(g_target_port);

    inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);

    if (connect(target_fd, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
        perror("[Bridge] Failed to connect to XMPP target");

        return -1;
    }

    int flags = fcntl(target_fd, F_GETFL, 0);

    fcntl(target_fd, F_SETFL, flags | O_NONBLOCK);

    s->target_fd = target_fd;

    printf("[Bridge] Connected to XMPP Target on FD %d\n", target_fd);
    
    return 0;
}

void forward_client_to_target(struct Session *s) {
    unsigned char buf[BUFFER_SIZE];
    int n = recv(s->client_fd, buf, sizeof(buf), 0);
    
    if (n <= 0) { 
        printf("[Bridge] Client disconnected (FD %d)\n", s->client_fd);

        close(s->client_fd); 

        if (s->target_fd != -1) {
            close(s->target_fd); 
        }

        return; 
    }

    int processed = 0;

    while (processed < n) {
        unsigned char *frame = buf + processed;
        int remaining = n - processed;

        if (remaining < 2) {
            break;
        }

        int payload_len = frame[1] & 0x7F;
        int has_mask = frame[1] & 0x80;
        int header_len = 2;
        
        if (payload_len == 126) {
            header_len = 4;
        }
        else if (payload_len == 127) {
            header_len = 10;
        }

        if (remaining < header_len) {
            break;
        }

        size_t full_payload_len = payload_len;

        if (payload_len == 126) {
            full_payload_len = (frame[2] << 8) | frame[3];
        }
        else if (payload_len == 127) {
            full_payload_len = 0;

            for(int k=0; k<8; k++) {
                full_payload_len = (full_payload_len << 8) | frame[2+k];
            }
        }

        int mask_len = has_mask ? 4 : 0;
        size_t total_frame_size = header_len + mask_len + full_payload_len;

        if ((size_t)remaining < total_frame_size) {
            printf("[Bridge] Partial frame (Need %zu, Got %d). Dropping.\n", total_frame_size, remaining);
            
            break; 
        }

        unsigned char *masks = frame + header_len;
        unsigned char *payload = frame + header_len + mask_len;

        if (has_mask) {
            for (size_t i = 0; i < full_payload_len; i++) {
                payload[i] ^= masks[i % 4];
            }
        }

        printf("[Bridge] C->S XML: %.*s\n", (int)full_payload_len, payload);

        send(s->target_fd, payload, full_payload_len, 0);

        processed += total_frame_size;
    }
}

void forward_target_to_client(struct Session *s) {
    unsigned char buf[BUFFER_SIZE];
    int n = recv(s->target_fd, buf, sizeof(buf) - 10, 0);

    if (n <= 0) { 
        printf("[Bridge] Target disconnected (FD %d)\n", s->target_fd);

        close(s->client_fd);

        close(s->target_fd); 
        
        return; 
    }

    printf("[Bridge] S->C XML: %.*s\n", n, buf);

    unsigned char frame[BUFFER_SIZE];
    frame[0] = 0x81;
    int header_len = 2;

    if (n <= 125) {
        frame[1] = n;
    }
    else if (n <= 65535) {
        frame[1] = 126;
        frame[2] = (n >> 8) & 0xFF;
        frame[3] = n & 0xFF;
        header_len = 4;
    }
    else {
        frame[1] = 127;
        header_len = 10; 
    }

    memcpy(frame + header_len, buf, n);

    send(s->client_fd, frame, header_len + n, 0);
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    if (argc > 1) {
        g_target_port = atoi(argv[2]);
    }

    int port = (argc > 1) ? atoi(argv[1]) : 8082;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    int opt = 1;

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));

    listen(listen_fd, 10);
    
    printf("[Bridge] Listening on %d -> Forwarding to localhost:%d\n", port, g_target_port);

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                int client_fd = accept(listen_fd, NULL, NULL);

                if (client_fd < 0) { 
                    perror("accept");

                    continue;
                }
                
                int flags = fcntl(client_fd, F_GETFL, 0);

                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                struct Session *s = malloc(sizeof(struct Session));
                s->client_fd = client_fd;
                s->target_fd = -1;
                s->state = STATE_HANDSHAKE;
                sessions[client_fd] = s;

                ev.events = EPOLLIN;
                ev.data.fd = client_fd;

                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                
                printf("[Bridge] New client connected: %d\n", client_fd);
            }
            else {
                int fd = events[i].data.fd;
                struct Session *s = sessions[fd];
                
                if (!s) {
                   for(int k=0; k<MAX_EVENTS; k++) {
                       if(sessions[k] && sessions[k]->target_fd == fd) {
                           s = sessions[k];

                           break;
                       }
                   }
                }

                if (!s) {
                    continue;
                }

                if (fd == s->client_fd) {
                    if (s->state == STATE_HANDSHAKE) {
                        char temp_buf[BUFFER_SIZE];
                        int n = recv(fd, temp_buf, sizeof(temp_buf) - 1, MSG_PEEK); 
                        
                        if (n <= 0) {
                            close(fd);

                            free(s); 

                            sessions[fd] = NULL;

                            continue;
                        }
                        
                        temp_buf[n] = 0;

                        if (strstr(temp_buf, "\r\n\r\n")) {
                            n = recv(fd, temp_buf, sizeof(temp_buf) - 1, 0);
                            temp_buf[n] = 0;
                            
                            if (handle_handshake(s, temp_buf) == 0) {
                                ev.events = EPOLLIN | EPOLLET;
                                ev.data.fd = s->target_fd;

                                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s->target_fd, &ev);
                                
                                sessions[s->target_fd] = s; 
                            }
                            else {
                                printf("[Bridge] Handshake failed (Missing Key?)\n");

                                close(fd); 
                                
                                free(s); 
                                
                                sessions[fd] = NULL;
                            }
                        }
                        else {
                            continue; 
                        }
                    }
                    else {
                        forward_client_to_target(s);
                    }
                }
                else if (fd == s->target_fd) {
                    forward_target_to_client(s);
                }
            }
        }
    }
    
    return 0;
}
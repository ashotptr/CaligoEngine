#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_EVENTS 64
#define BUFFER_SIZE 4096
#define PORT 5222
#define ROOM_CONFIG_FILE "chat_rooms.txt"
#define MAX_ROOMS 64
#define MAX_ROOM_NAME 64

char known_rooms[MAX_ROOMS][MAX_ROOM_NAME];
int known_room_count = 0;

typedef struct {
    int fd;
    char jid[64];
    char current_room[64];
    int authenticated;
} Client;

Client* clients[MAX_EVENTS];

int add_room_to_memory(const char* room_name) {
    for (int i = 0; i < known_room_count; i++) {
        if (strcmp(known_rooms[i], room_name) == 0) {
            return 0;
        }
    }

    if (known_room_count < MAX_ROOMS) {
        strncpy(known_rooms[known_room_count], room_name, MAX_ROOM_NAME - 1);
        
        known_room_count++;
        
        return 1;
    }

    return 0;
}

void save_room_to_disk(const char* room_name) {
    FILE *f = fopen(ROOM_CONFIG_FILE, "a");

    if (f) {
        fprintf(f, "%s\n", room_name);

        fclose(f);

        printf("[Storage] Persisted room: %s\n", room_name);
    }
}

void load_persistent_rooms() {
    FILE *f = fopen(ROOM_CONFIG_FILE, "r");

    if (!f) {
        return; 
    }

    char line[128];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;

        if (strlen(line) > 0) {
            add_room_to_memory(line);

            printf("[Storage] Loaded room: %s\n", line);
        }
    }

    fclose(f);
}

int get_users_in_room(const char* room_name) {
    int count = 0;

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->authenticated) {
            if (strcmp(clients[i]->current_room, room_name) == 0) {
                count++;
            }
        }
    }

    return count;
}

void broadcast_room_list(int target_fd) {
    char buffer[BUFFER_SIZE];

    strcpy(buffer, "<roomlist>");

    for (int i = 0; i < known_room_count; i++) {
        char item[128];

        int count = get_users_in_room(known_rooms[i]);

        sprintf(item, "<room name='%s' count='%d' />", known_rooms[i], count);

        strcat(buffer, item);
    }

    strcat(buffer, "</roomlist>");

    if (target_fd == -1) {
        for (int i = 0; i < MAX_EVENTS; i++) {
            if (clients[i] && clients[i]->authenticated) {
                send(clients[i]->fd, buffer, strlen(buffer), 0);
            }
        }
    }
    else {
        send(target_fd, buffer, strlen(buffer), 0);
    }
}

void set_nonblocking(int sockfd) {
    int opts = fcntl(sockfd, F_GETFL);

    if (opts < 0) { 
        perror("fcntl(F_GETFL)"); 
        
        exit(1); 
    }

    opts = (opts | O_NONBLOCK);

    if (fcntl(sockfd, F_SETFL, opts) < 0) { 
        perror("fcntl(F_SETFL)"); 
        
        exit(1); 
    }
}

void extract_attribute(const char* xml, const char* attr, char* dest) {
    char search[32];

    snprintf(search, sizeof(search), "%s='", attr);
    
    char* start = strstr(xml, search);

    if (!start) {
        snprintf(search, sizeof(search), "%s=\"", attr);

        start = strstr(xml, search);
    }
    
    if (start) {
        start += strlen(search);

        char* end = strpbrk(start, "'\"");

        if (end) {
            int len = end - start;

            if(len >= 63) {
                len = 63;
            }

            strncpy(dest, start, len);

            dest[len] = '\0';
        }
    }
}

void extract_body(const char* xml, char* dest) {
    char* start = strstr(xml, "<body>");
    
    if (start) {
        start += 6; 
        
        char* end = strstr(start, "</body>");
        
        if (end) {
            int len = end - start;
            
            strncpy(dest, start, len);
            
            dest[len] = '\0';
        }
    }
}

void handle_client_message(Client* sender, char* buffer, int len) {
    printf("[XMPP] Received from %d: %s\n", sender->fd, buffer);

    if (strstr(buffer, "<stream:stream")) {
        char response[] = 
            "<?xml version='1.0'?>"
            "<stream:stream from='caligo-radio' id='12345' version='1.0' "
            "xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>";

        send(sender->fd, response, strlen(response), 0);
        
        sprintf(sender->jid, "user%d@radio", sender->fd);

        sender->authenticated = 1;

        printf("[XMPP] Client %d assigned JID: %s\n", sender->fd, sender->jid);
        
        char features[] = "<stream:features><bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/></stream:features>";
        
        send(sender->fd, features, strlen(features), 0);
        
        broadcast_room_list(sender->fd);
    }

    char *presence_tag = strstr(buffer, "<presence");

    if (presence_tag) {
        char to_room[64] = {0};

        extract_attribute(presence_tag, "to", to_room); 
        
        char *slash = strchr(to_room, '/');

        if (slash) {
            *slash = '\0';
        }
        
        if (strlen(to_room) == 0 || strcmp(to_room, "null") == 0) {
            memset(sender->current_room, 0, 64);

            printf("[XMPP] User %s moved to Lobby\n", sender->jid);
        }
        else {
            strncpy(sender->current_room, to_room, sizeof(sender->current_room) - 1);

            printf("[XMPP] User %s JOINED room '%s'\n", sender->jid, sender->current_room);

            int exists = 0;

            for(int k=0; k<known_room_count; k++) {
                if(strcmp(known_rooms[k], to_room) == 0) {
                    exists = 1;
                }
            }
            
            if (!exists) {
                if(add_room_to_memory(to_room)) {
                    save_room_to_disk(to_room);

                    printf("[Room] Created new persistent room: %s\n", to_room);
                }
            }
        }

        broadcast_room_list(-1);
    }

    if (strstr(buffer, "<message")) {
        char to_jid[64] = {0};

        extract_attribute(buffer, "to", to_jid);
        
        char body[BUFFER_SIZE] = {0};

        extract_body(buffer, body);

        int is_room_message = 0;
        
        if (strstr(to_jid, "user") == NULL) {
            is_room_message = 1;
        }

        if (is_room_message) {
            printf("[XMPP] Broadcasting to Room '%s'\n", to_jid);
            
            for (int i = 0; i < MAX_EVENTS; i++) {
                if (clients[i] && clients[i]->authenticated) {
                    if (clients[i]->fd == sender->fd) {
                        continue;
                    }

                    if (strcmp(clients[i]->current_room, to_jid) == 0) {
                        char broadcast_msg[BUFFER_SIZE + 512];
                        
                        snprintf(broadcast_msg, sizeof(broadcast_msg), 
                                 "<message type='chat' from='%s' to='%s'><body>%s: %s</body></message>", 
                                 to_jid, clients[i]->jid, sender->jid, body);
                        
                        send(clients[i]->fd, broadcast_msg, strlen(broadcast_msg), 0);
                    }
                }
            }

            return;
        }
        
        if (strlen(to_jid) > 0) {
            int found = 0;

            for (int i = 0; i < MAX_EVENTS; i++) {
                if (clients[i] && strcmp(clients[i]->jid, to_jid) == 0) {
                    send(clients[i]->fd, buffer, len, 0);

                    found = 1;
                    
                    printf("[XMPP] Routed Private Message from %s to %s\n", sender->jid, to_jid);
                    
                    break;
                }
            }

            if (!found) {
                printf("[XMPP] User %s not found\n", to_jid);
            }
        } 
    }
}

void broadcast_user_count() {
    int count = 0;
    
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->authenticated) {
            count++;
        }
    }

    printf("[XMPP] Current Online Count: %d\n", count);

    char count_msg[256];

    snprintf(count_msg, sizeof(count_msg), 
             "<message type='info' from='server@radio' to='all'><body>USER_COUNT:%d</body></message>", 
             count);

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->authenticated) {
            send(clients[i]->fd, count_msg, strlen(count_msg), 0);
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    int listener, epoll_fd;
    struct sockaddr_in addr;
    struct epoll_event ev, events[MAX_EVENTS];

    for(int i=0; i<MAX_EVENTS; i++) {
        clients[i] = NULL;
    }
    
    load_persistent_rooms();
    
    if ((listener = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
     
        exit(1);
    }
    
    int opt = 1;
    
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
    
        exit(1);
    }

    listen(listener, 10);
    
    set_nonblocking(listener);

    epoll_fd = epoll_create1(0);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listener;
    
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &ev);

    printf("Caligo XMPP Server started on port %d\n", PORT);
    
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listener) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listener, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd < 0) {
                    continue;
                }
                
                set_nonblocking(client_fd);

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;

                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

                Client* new_c = malloc(sizeof(Client));
                new_c->fd = client_fd;
                new_c->authenticated = 0;

                memset(new_c->jid, 0, 64); 
                memset(new_c->current_room, 0, 64);

                clients[client_fd % MAX_EVENTS] = new_c;

                printf("[Conn] New connection: FD %d\n", client_fd);
            }
            else {
                int fd = events[i].data.fd;
                char buf[BUFFER_SIZE];
                int n = recv(fd, buf, sizeof(buf) - 1, 0);

                if (n <= 0) {
                    close(fd);

                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    
                    if (clients[fd % MAX_EVENTS]) {
                        free(clients[fd % MAX_EVENTS]);

                        clients[fd % MAX_EVENTS] = NULL;

                        broadcast_room_list(-1);
                    }
                    
                    printf("[Conn] Closed FD %d\n", fd);
                }
                else {
                    buf[n] = '\0';

                    if (strchr(buf, '<') && strchr(buf, '>')) {
                        if (clients[fd % MAX_EVENTS]) {
                            handle_client_message(clients[fd % MAX_EVENTS], buf, n);
                        }
                    }
                    else {
                        printf("[XMPP] Ignored %d bytes of non-XML data from FD %d. Disconnecting.\n", n, fd);

                        close(fd);

                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);

                        if (clients[fd % MAX_EVENTS]) {
                            free(clients[fd % MAX_EVENTS]);

                            clients[fd % MAX_EVENTS] = NULL;
                            
                            broadcast_room_list(-1);
                        }
                    }
                }
            }
        }
    }

    close(listener);
    
    return 0;
}
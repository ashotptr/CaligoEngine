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

typedef enum {
    STATE_CONNECTED,
    STATE_STREAM_OPEN,
    STATE_AUTHENTICATED,
    STATE_NEGOTIATING,
    STATE_BOUND
} ConnState;

typedef struct {
    int fd;
    char jid[128];
    char current_room[64];
    char nickname[64];
    ConnState state;
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
        if (clients[i] && clients[i]->state == STATE_BOUND) {
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
            if (clients[i] && clients[i]->state == STATE_BOUND) {
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

void handle_muc_join(Client* c, char* room_name, char* nickname) {
    strncpy(c->current_room, room_name, sizeof(c->current_room)-1);
    strncpy(c->nickname, nickname, sizeof(c->nickname)-1);
    
    add_room_to_memory(room_name);

    printf("[MUC] User %d joined '%s' as '%s'\n", c->fd, room_name, nickname);

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->state == STATE_BOUND && strcmp(clients[i]->current_room, room_name) == 0) {   
            char pres[512];

            snprintf(pres, sizeof(pres), 
                "<presence from='%s@conference.radio/%s' to='%s'>"
                "<x xmlns='http://jabber.org/protocol/muc#user'>"
                "<item affiliation='member' role='participant'/>"
                "</x></presence>", 
                room_name, c->nickname, clients[i]->jid);
            
            send(clients[i]->fd, pres, strlen(pres), 0);
        }
    }

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->state == STATE_BOUND && strcmp(clients[i]->current_room, room_name) == 0) {
            if (clients[i]->fd == c->fd) {
                continue;
            }

            char pres[512];

            snprintf(pres, sizeof(pres), 
                "<presence from='%s@conference.radio/%s' to='%s'>"
                "<x xmlns='http://jabber.org/protocol/muc#user'>"
                "<item affiliation='member' role='participant'/>"
                "</x></presence>", 
                room_name, clients[i]->nickname, c->jid);
            
            send(c->fd, pres, strlen(pres), 0);
        }
    }

    char self_pres[512];

    snprintf(self_pres, sizeof(self_pres), 
        "<presence from='%s@conference.radio/%s' to='%s'>"
        "<x xmlns='http://jabber.org/protocol/muc#user'>"
        "<item affiliation='member' role='participant'/>"
        "<status code='110'/>"
        "</x></presence>", 
        room_name, c->nickname, c->jid);

    send(c->fd, self_pres, strlen(self_pres), 0);
}

void handle_disco_info(int fd, char* id, char* to_addr) {
    char response[1024];
    
    snprintf(response, sizeof(response),
        "<iq type='result' id='%s' from='%s' to='user%d@radio/xmpp'>"
        "<query xmlns='http://jabber.org/protocol/disco#info'>"
        "<identity category='conference' type='text' name='Caligo Chat Service'/>"
        "<feature var='http://jabber.org/protocol/muc'/>"
        "<feature var='http://jabber.org/protocol/disco#info'/>"
        "<feature var='http://jabber.org/protocol/disco#items'/>"
        "</query></iq>", 
        id, to_addr, fd);
        
    send(fd, response, strlen(response), 0);
}

void handle_disco_items(int fd, char* id, char* to_addr) {
    char response[4096];
    char items[3072] = "";
    
    for (int i = 0; i < known_room_count; i++) {
        char item[256];

        snprintf(item, sizeof(item), 
            "<item jid='%s@conference.radio' name='%s'/>", 
            known_rooms[i], known_rooms[i]);

        strcat(items, item);
    }

    snprintf(response, sizeof(response),
        "<iq type='result' id='%s' from='%s' to='user%d@radio/xmpp'>"
        "<query xmlns='http://jabber.org/protocol/disco#items'>"
        "%s"
        "</query></iq>", 
        id, to_addr, fd, items);
        
    send(fd, response, strlen(response), 0);
}

void extract_id(const char* xml, char* dest) {
    extract_attribute(xml, "id", dest);

    if (strlen(dest) == 0) {
        strcpy(dest, "unknown");
    }
}

void send_stream_header_sasl(int fd) {
    char response[] = 
        "<?xml version='1.0'?>"
        "<stream:stream from='caligo-radio' id='auth-stream' version='1.0' "
        "xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>"
        "<stream:features>"
        "<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
        "<mechanism>PLAIN</mechanism>"
        "</mechanisms>"
        "</stream:features>";

    send(fd, response, strlen(response), 0);
}

void send_stream_header_bind(int fd) {
    char response[] = 
        "<?xml version='1.0'?>"
        "<stream:stream from='caligo-radio' id='bind-stream' version='1.0' "
        "xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>"
        "<stream:features>"
        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
        "<session xmlns='urn:ietf:params:xml:ns:xmpp-session'/>"
        "</stream:features>";

    send(fd, response, strlen(response), 0);
}

void handle_bind_request(Client* c, char* buffer) {
    char id[64];

    extract_id(buffer, id);

    sprintf(c->jid, "user%d@radio/xmpp", c->fd);

    char response[512];

    snprintf(response, sizeof(response), 
        "<iq type='result' id='%s'>"
        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
        "<jid>%s</jid>"
        "</bind>"
        "</iq>", id, c->jid);
    
    send(c->fd, response, strlen(response), 0);
    
    c->state = STATE_BOUND;
    
    printf("[XMPP] Client %d Bound to JID: %s\n", c->fd, c->jid);
}

void handle_client_message(Client* sender, char* buffer, int len) {
    printf("[XMPP] Received from %d: %s\n", sender->fd, buffer);

    if (sender->state == STATE_CONNECTED) {
        if (strstr(buffer, "<stream:stream") || strstr(buffer, "<?xml")) {
            send_stream_header_sasl(sender->fd);
            
            sender->state = STATE_STREAM_OPEN;
            
            return;
        }
    }

    if (sender->state == STATE_STREAM_OPEN) {
        if (strstr(buffer, "<auth")) {
            char success[] = "<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>";
            
            send(sender->fd, success, strlen(success), 0);
            
            sender->state = STATE_AUTHENTICATED;
            
            return;
        }
    }

    if (sender->state == STATE_AUTHENTICATED) {
        if (strstr(buffer, "<stream:stream")) {
            send_stream_header_bind(sender->fd);
            
            sender->state = STATE_NEGOTIATING;
            
            return;
        }
    }

    if (sender->state == STATE_NEGOTIATING) {
        if (strstr(buffer, "<bind")) {
            handle_bind_request(sender, buffer);

            broadcast_room_list(sender->fd);
            
            return;
        }
    }

    if (strstr(buffer, "<session")) {
        char id[64];

        extract_id(buffer, id);
        
        char response[256];
        
        snprintf(response, sizeof(response), "<iq type='result' id='%s'/>", id);
        
        send(sender->fd, response, strlen(response), 0);
        
        return;
    }

    if (sender->state != STATE_BOUND) {
        return; 
    }
    
    if (strstr(buffer, "<iq") && strstr(buffer, "disco#")) {
        char id[64];

        extract_id(buffer, id);
        
        char to_addr[128] = "conference.radio";

        extract_attribute(buffer, "to", to_addr);

        if (strstr(buffer, "disco#info")) {
            handle_disco_info(sender->fd, id, to_addr);
        }
        else if (strstr(buffer, "disco#items")) {
            handle_disco_items(sender->fd, id, to_addr);
        }

        return;
    }
    
    if (strstr(buffer, "<presence")) {
        char to_raw[128] = {0};

        extract_attribute(buffer, "to", to_raw);

        char* at_sym = strchr(to_raw, '@');
        char* slash_sym = strchr(to_raw, '/');

        if (at_sym && slash_sym) {
            *at_sym = '\0';
            char* room_name = to_raw;
            char* nickname = slash_sym + 1;

            handle_muc_join(sender, room_name, nickname);
        } 
        else {
            char* slash = strchr(to_raw, '/');

            if(slash) {
                *slash = '\0';
            }
            
            handle_muc_join(sender, to_raw, "WebGuest");
        }

        broadcast_room_list(-1);

        return;
    }

    if (strstr(buffer, "<message")) {
        char to_raw[128] = {0};

        extract_attribute(buffer, "to", to_raw);

        char body[BUFFER_SIZE] = {0};

        extract_body(buffer, body);
        
        int is_room = (strstr(to_raw, "@conference") != NULL || strchr(to_raw, '@') == NULL);
        
        if (is_room) {
            char room_target[64];

            if (strstr(to_raw, "@conference")) {
                char* at = strchr(to_raw, '@');
                int len = at - to_raw;
                
                strncpy(room_target, to_raw, len);
                
                room_target[len] = '\0';
            }
            else {
                strcpy(room_target, to_raw);
            }

            printf("[MSG] From %s to Room %s: %s\n", sender->nickname, room_target, body);

            for (int i = 0; i < MAX_EVENTS; i++) {
                if (clients[i] && clients[i]->state == STATE_BOUND && 
                    strcmp(clients[i]->current_room, room_target) == 0) {
                    
                    if (clients[i]->fd == sender->fd) {
                        continue;
                    }

                    char msg[BUFFER_SIZE + 512];

                    snprintf(msg, sizeof(msg), 
                        "<message type='groupchat' from='%s@conference.radio/%s' to='%s'>"
                        "<body>%s</body></message>", 
                        room_target, sender->nickname, clients[i]->jid, body);
                    
                    send(clients[i]->fd, msg, strlen(msg), 0);
                }
            }
        }
        else {
             if (strlen(to_raw) > 0) {
                for (int i = 0; i < MAX_EVENTS; i++) {
                    if (clients[i] && strcmp(clients[i]->jid, to_raw) == 0) {
                        send(clients[i]->fd, buffer, len, 0);

                        break;
                    }
                }
            }
        }
    }
}

void broadcast_user_count() {
    int count = 0;

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->state == STATE_BOUND) {
            count++;
        }
    }

    printf("[XMPP] Current Online Count: %d\n", count);

    char count_msg[256];

    snprintf(count_msg, sizeof(count_msg), 
             "<message type='info' from='server@radio' to='all'><body>USER_COUNT:%d</body></message>", 
             count);

    for (int i = 0; i < MAX_EVENTS; i++) {
        if (clients[i] && clients[i]->state == STATE_BOUND) {
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
                new_c->state = STATE_CONNECTED;

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
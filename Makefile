CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

LIBS_COMMON = -lpthread -ljansson

LIBS_SSL = -lssl -lcrypto

all: server_http server_https cgi_bin/mixtape_app radio_server xmppd bridge cgi_bin/playlist_manager cgi_bin/auth_app cgi_bin/request_song cgi_bin/get_chat_rooms

HTTP_OBJS = http/server.o http/request_handler.o
HTTPS_OBJS = https/server.o https/request_handler.o

server_http: $(HTTP_OBJS)
	$(CC) $(CFLAGS) -o server_http $(HTTP_OBJS) $(LIBS_COMMON)

http/%.o: http/%.c
	$(CC) $(CFLAGS) -Ihttp -c $< -o $@

server_https: $(HTTPS_OBJS)
	$(CC) $(CFLAGS) -o server_https $(HTTPS_OBJS) $(LIBS_COMMON) $(LIBS_SSL)

https/%.o: https/%.c
	$(CC) $(CFLAGS) -Ihttps -c $< -o $@

radio_server: radio_server.c https/server.h
	$(CC) $(CFLAGS) -Ihttps -o radio_server radio_server.c $(LIBS_COMMON)
	
cgi_bin/mixtape_app: cgi_bin/mixtape_app.c
	$(CC) $(CFLAGS) -o cgi_bin/mixtape_app cgi_bin/mixtape_app.c $(LIBS_COMMON)

cgi_bin/playlist_manager: cgi_bin/playlist_manager.c
	$(CC) $(CFLAGS) -o cgi_bin/playlist_manager cgi_bin/playlist_manager.c $(LIBS_COMMON)

cgi_bin/auth_app: cgi_bin/auth_app.c
	$(CC) $(CFLAGS) -o cgi_bin/auth_app cgi_bin/auth_app.c $(LIBS_COMMON)

cgi_bin/request_song: cgi_bin/request_song.c
	$(CC) $(CFLAGS) -o cgi_bin/request_song cgi_bin/request_song.c

xmppd: xmppd.c
	$(CC) $(CFLAGS) -o xmppd xmppd.c

bridge: bridge.c
	$(CC) $(CFLAGS) -o bridge bridge.c

cgi_bin/get_chat_rooms: cgi_bin/get_chat_rooms.c
	$(CC) $(CFLAGS) -o cgi_bin/get_chat_rooms cgi_bin/get_chat_rooms.c $(LIBS_COMMON)
	
clean:
	rm -f server_http server_https xmppd bridge radio_server cgi_bin/mixtape_app cgi_bin/playlist_manager cgi_bin/auth_app cgi_bin/request_song cgi_bin/get_chat_rooms *.o
	rm -f http/*.o https/*.o
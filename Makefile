CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

LIBS_COMMON = -lpthread -ljansson

LIBS_SSL = -lssl -lcrypto

all: server_http server_https cgi_bin/mixtape_app radio_server cgi_bin/playlist_manager cgi_bin/auth_app

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

cgi_bin/mixtape_app: cgi_bin/mixtape_app.c
	$(CC) $(CFLAGS) -o cgi_bin/mixtape_app cgi_bin/mixtape_app.c $(LIBS_COMMON)

cgi_bin/playlist_manager: cgi_bin/playlist_manager.c
	$(CC) $(CFLAGS) -o cgi_bin/playlist_manager cgi_bin/playlist_manager.c $(LIBS_COMMON)

cgi_bin/auth_app: cgi_bin/auth_app.c
	$(CC) $(CFLAGS) -o cgi_bin/auth_app cgi_bin/auth_app.c $(LIBS_COMMON)
	
radio_server: radio_server.c https/server.h
	$(CC) $(CFLAGS) -Ihttps -o radio_server radio_server.c $(LIBS_COMMON)

clean:
	rm -f server_http server_https radio_server cgi_bin/mixtape_app cgi_bin/playlist_manager cgi_bin/auth_app
	rm -f http/*.o https/*.o
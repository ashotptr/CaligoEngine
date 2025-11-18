CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99
LIBS = -lpthread -ljansson

all: server cgi_bin/mixtape_app radio_server

SERVER_OBJS = server.o request_handler.o
server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o server $(SERVER_OBJS) $(LIBS)

server.o: server.c server.h
	$(CC) $(CFLAGS) -c server.c

request_handler.o: request_handler.c server.h
	$(CC) $(CFLAGS) -c request_handler.c

cgi_bin/mixtape_app: cgi_bin/mixtape_app.c
	$(CC) $(CFLAGS) -o cgi_bin/mixtape_app cgi_bin/mixtape_app.c $(LIBS)

radio_server: radio_server.c server.h
	$(CC) $(CFLAGS) -o radio_server radio_server.c $(LIBS)

clean:
	rm -f server radio_server cgi_bin/mixtape_app $(SERVER_OBJS) radio_server.o cgi_bin/mixtape_app.o
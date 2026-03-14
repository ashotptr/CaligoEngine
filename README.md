# **Caligo Engine**
This project is a web application system. Instead of using standard server software this project uses **custom-built servers** to handle networking, media streaming, and real-time communication.

The system is designed as a cluster of distinct specialized servers that work together.

### `server.conf`
This file contains configurations for the HTTP server:

```
STATIC / public_html/
CGI /cgi_bin/ cgi_bin/
PROXY /radio/ http://127.0.0.1:9001
PROXY /chat/ http://127.0.0.1:8082

```

Also, we can secure a path by using `AUTH`:

```
AUTH /radio

```

### `xmppd.c`
This file is a custom implementation of a multi-user chat XMPP server.

* This server currently works with a custom XMPP client.
* This is to be improved to the MUC XEP standard.

### `radio_server.c`
This is a non-blocking HTTP streaming server that broadcasts a single MP3 playlist to many concurrent listeners in real-time.

* **Broadcaster Thread:** 
Reads raw MP3 data from the disk into a shared **ring buffer**. It controls the timing (sleeping for specific nanoseconds) to ensure the audio plays at the correct bitrate (128kbps) and that all listeners get the same starting byte of data. Every time it writes a new chunk of audio, it signals all worker threads to wake up.
* **Main Thread:** 
Listens on the socket using `epoll` and distributes new listeners evenly across the 12 worker threads. The server never freezes while waiting for clients. It handles `SIGPIPE` and socket errors, if a listener disconnects, they are removed from the list without crashing the server.
* **Worker Threads:** 
12 sender worker threads spend most of their time sleeping. They only wake up when the broadcaster thread signals that a new audio chunk is ready. Once awake, they send the new data to every client in their list and go back to sleep.
* It uses **HTTP chunked transfer encoding** to stream data without knowing the file size beforehand.
* Data is read from disk once into a global buffer, and sender threads read from the shared memory.

### `bridge.c`
This is a server that acts as a translator between a web client (like a browser running a chat app) and an XMPP server.

* Web browsers speak **WebSocket**.
* XMPP servers do not understand WebSocket frames.
* This bridge sits in the middle to translate between the two protocols in real-time.

### `index.html`
Client for testing the servers.

### `files inside cgi_bin`
These files are for testing the HTTP server. They are parts for a custom website.

### `server.c`
Server architecture is designed to handle high concurrency by combining event-driven I/O using `epoll` with a thread pool. It buffers incoming requests and offloads the decision-making to a worker thread.

* **Main Thread:** Uses `epoll` and handles lightweight tasks like accepting connections, reading raw bytes into buffers, and forwarding data between peers. All sockets are set to `O_NONBLOCK`, ensuring the loop never freezes waiting for a slow client.
* **Task Queue:** A linked list protected by mutexes. When the main thread determines a client has sent a full request, it temporarily removes that client from `epoll` and pushes them into this queue.
* **Worker Pool:** There is a pool of worker threads that sleep until a task arrives and then perform the task.

### `request_handler.c`
This file defines what the HTTP server actually does when it receives a request. It takes a raw HTTP request, parses it, matches it against a configuration, and executes one of four actions:

1. **Serve Static Files (`STATIC`):** Reads files from disk (HTML, CSS, JS, MP3) and sends them to the client.
2. **Execute CGI (`CGI`):** Creates external processes to handle dynamic content via pipes.
3. **Reverse Proxy (`PROXY`):** Asynchronously connects to upstream servers and bridges the traffic.

It also detects **AUTH** flags to protect specific routes.

### `files inside https`
This is an HTTPS server implementation using the OpenSSL library. It works but lacks some of the features that are currently implemented in the HTTP server and is not tested enough.

> **Note:** Might be unstable.

---

## Usage
### Build the project

```
make all

```

### Run the components
**For Radio Server:**

```
./radio_server

```

**For XMPP Chat:**

```
./xmppd

```

**For WebSocket Bridge:**

```
./bridge

```

**For HTTP Server:**

```
./server_http

```


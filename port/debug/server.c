#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "debug.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Winsock and BSD sockets differ only in these: how a socket is named, closed, made non-blocking and kept from child
// processes, how a full buffer is reported, and how a peer that went away is kept from raising SIGPIPE.
#ifdef _WIN32
typedef SOCKET Socket;
#define NO_SOCKET INVALID_SOCKET
#define SEND_FLAGS 0
static void close_socket(Socket socket) { closesocket(socket); }
static int socket_error(void) { return WSAGetLastError(); }
static int would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }

static void set_blocking(Socket socket, int blocking) {
    u_long mode = !blocking;
    ioctlsocket(socket, FIONBIO, &mode);
}

static void keep_private(Socket socket) { SetHandleInformation((HANDLE)socket, HANDLE_FLAG_INHERIT, 0); }

static int start_sockets(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
#else
typedef int Socket;
#define NO_SOCKET (-1)
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
static void close_socket(Socket socket) { close(socket); }
static int socket_error(void) { return errno; }
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

static void set_blocking(Socket socket, int blocking) {
    int flags = fcntl(socket, F_GETFL);
    fcntl(socket, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}

static void keep_private(Socket socket) {
    fcntl(socket, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int no_signal = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof no_signal);
#endif
}

static int start_sockets(void) { return 1; }
#endif

enum { CLIENT_LIMIT = 32, REQUEST_LIMIT = 8192 };

typedef struct {
    Socket fd;
    int waiting;
    size_t length;
    char request[REQUEST_LIMIT];
} Client;

static Socket listener = NO_SOCKET;
static Client clients[CLIENT_LIMIT];
static int listening_port;

void debug_server_start(int port) {
    listening_port = port;
    for (int i = 0; i < CLIENT_LIMIT; ++i) clients[i].fd = NO_SOCKET;
    if (!start_sockets()) return;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == NO_SOCKET) return;
#ifndef _WIN32
    // Windows' SO_REUSEADDR would let a second listener take the port; there a closed one frees it at once anyway
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 16)) {
        fprintf(stderr, "debug: cannot listen on 127.0.0.1:%d (error %d)\n", port, socket_error());
        close_socket(listener);
        listener = NO_SOCKET;
        return;
    }
    set_blocking(listener, 0);
    keep_private(listener);
    fprintf(stderr, "debug: commands on http://127.0.0.1:%d/help\n", port);
}

static void drop(int index) {
    if (clients[index].fd != NO_SOCKET) close_socket(clients[index].fd);
    clients[index].fd = NO_SOCKET;
    clients[index].waiting = 0;
    clients[index].length = 0;
}

void debug_server_stop(void) {
    for (int i = 0; i < CLIENT_LIMIT; ++i) drop(i);
    if (listener != NO_SOCKET) close_socket(listener);
    listener = NO_SOCKET;
}

void debug_server_resume(void) {
    if (listener == NO_SOCKET) debug_server_start(listening_port);
}

void debug_server_send(int client, int status, const char *content_type, const void *data, size_t length) {
    if (client < 0 || client >= CLIENT_LIMIT || clients[client].fd == NO_SOCKET) return;
    Socket fd = clients[client].fd;
    set_blocking(fd, 1);
    char header[256];
    int size = snprintf(header, sizeof header,
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                        "Access-Control-Allow-Origin: *\r\n\r\n",
                        status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request", content_type, length);
    const char *parts[2] = {header, (const char *)data};
    size_t sizes[2] = {(size_t)size, length};
    for (int part = 0; part < 2; ++part) {
        size_t sent = 0;
        while (sent < sizes[part]) {
            size_t left = sizes[part] - sent;
            int wrote = (int)send(fd, parts[part] + sent, (int)(left > 1 << 30 ? 1 << 30 : left), SEND_FLAGS);
            if (wrote <= 0) break;
            sent += (size_t)wrote;
        }
    }
    drop(client);
}

static void handle(int index) {
    Client *client = &clients[index];
    char *line_end = strstr(client->request, "\r\n");
    if (!line_end) return;
    *line_end = 0;
    char method[8] = {0}, target[REQUEST_LIMIT] = {0};
    if (sscanf(client->request, "%7s %8191s", method, target) != 2 || target[0] != '/') {
        debug_server_send(index, 400, "application/json", "{\"error\": \"bad request\"}", 24);
        return;
    }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    const char *name = target + 1;
    if (!*name) name = "help";
    DebugArgs args;
    debug_parse_query(query ? query : "", &args);
    client->waiting = 1;
    debug_dispatch(name, &args, index);
}

void debug_server_poll(void) {
    if (listener == NO_SOCKET) return;
    for (;;) {
        Socket fd = accept(listener, NULL, NULL);
        if (fd == NO_SOCKET) break;
        keep_private(fd);
        int slot = -1;
        for (int i = 0; i < CLIENT_LIMIT && slot < 0; ++i)
            if (clients[i].fd == NO_SOCKET) slot = i;
        if (slot < 0) {
            close_socket(fd);
            continue;
        }
        set_blocking(fd, 0);
        clients[slot].fd = fd;
        clients[slot].length = 0;
        clients[slot].waiting = 0;
    }
    for (int i = 0; i < CLIENT_LIMIT; ++i) {
        Client *client = &clients[i];
        if (client->fd == NO_SOCKET || client->waiting) continue;
        int got = (int)recv(client->fd, client->request + client->length, (int)(REQUEST_LIMIT - 1 - client->length), 0);
        if (got == 0 || (got < 0 && !would_block())) {
            drop(i);
            continue;
        }
        if (got < 0) continue;
        client->length += (size_t)got;
        client->request[client->length] = 0;
        if (strstr(client->request, "\r\n\r\n") || client->length >= REQUEST_LIMIT - 1) handle(i);
    }
}

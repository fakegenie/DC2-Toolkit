#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "debug.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

enum { CLIENT_LIMIT = 32, REQUEST_LIMIT = 8192 };

typedef struct {
    int fd;
    int waiting;
    size_t length;
    char request[REQUEST_LIMIT];
} Client;

static int listener = -1;
static Client clients[CLIENT_LIMIT];

void debug_server_start(int port) {
    for (int i = 0; i < CLIENT_LIMIT; ++i) clients[i].fd = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return;
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 16)) {
        fprintf(stderr, "debug: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        close(listener);
        listener = -1;
        return;
    }
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    fprintf(stderr, "debug: commands on http://127.0.0.1:%d/help\n", port);
}

static void drop(int index) {
    if (clients[index].fd >= 0) close(clients[index].fd);
    clients[index].fd = -1;
    clients[index].waiting = 0;
    clients[index].length = 0;
}

void debug_server_send(int client, int status, const char *content_type, const void *data, size_t length) {
    if (client < 0 || client >= CLIENT_LIMIT || clients[client].fd < 0) return;
    int fd = clients[client].fd;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
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
            ssize_t wrote = send(fd, parts[part] + sent, sizes[part] - sent, SEND_FLAGS);
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
    if (listener < 0) return;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) break;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int slot = -1;
        for (int i = 0; i < CLIENT_LIMIT && slot < 0; ++i)
            if (clients[i].fd < 0) slot = i;
        if (slot < 0) {
            close(fd);
            continue;
        }
        int no_signal = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof no_signal);
#else
        (void)no_signal;
#endif
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        clients[slot].fd = fd;
        clients[slot].length = 0;
        clients[slot].waiting = 0;
    }
    for (int i = 0; i < CLIENT_LIMIT; ++i) {
        Client *client = &clients[i];
        if (client->fd < 0 || client->waiting) continue;
        ssize_t got = recv(client->fd, client->request + client->length, REQUEST_LIMIT - 1 - client->length, 0);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            drop(i);
            continue;
        }
        if (got < 0) continue;
        client->length += (size_t)got;
        client->request[client->length] = 0;
        if (strstr(client->request, "\r\n\r\n") || client->length >= REQUEST_LIMIT - 1) handle(i);
    }
}

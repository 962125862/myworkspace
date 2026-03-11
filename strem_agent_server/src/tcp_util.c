#define _POSIX_C_SOURCE 200809L

#include "tcp_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int tcp_listen(const char* host, uint16_t port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!host || host[0] == '\0') host = "0.0.0.0";
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog > 0 ? backlog : 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tcp_accept(int listen_fd) {
    return accept(listen_fd, NULL, NULL);
}

int recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}


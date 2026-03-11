#define _POSIX_C_SOURCE 200809L

#include "mlctl_cmd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int udp_send_to(const char* ip, uint16_t port, const uint8_t* data, size_t len) {
    if (!ip || !data || len == 0) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    ssize_t n = sendto(fd, data, len, 0, (struct sockaddr*)&a, sizeof(a));
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}


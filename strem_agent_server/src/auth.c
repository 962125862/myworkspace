#define _POSIX_C_SOURCE 200809L

#include "auth.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int set_recv_timeout_ms(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int agent_auth_read_and_check(int fd, const char* token, int timeout_ms) {
    if (!token || token[0] == '\0') {
        return 0; /* auth disabled */
    }
    if (timeout_ms <= 0) timeout_ms = 3000;
    (void)set_recv_timeout_ms(fd, timeout_ms);

    char buf[512];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';

    /* Expect: AUTH <token>\n */
    const char* p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "AUTH ", 5) != 0) {
        return -1;
    }
    p += 5;

    char got[256];
    int gi = 0;
    while (*p && *p != '\n' && *p != '\r' && gi < (int)sizeof(got) - 1) {
        got[gi++] = *p++;
    }
    got[gi] = '\0';

    if (strcmp(got, token) != 0) {
        return -1;
    }
    return 0;
}


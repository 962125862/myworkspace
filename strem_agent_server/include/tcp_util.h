#ifndef AGENT_TCP_UTIL_H
#define AGENT_TCP_UTIL_H

#include <stddef.h>
#include <stdint.h>

int tcp_listen(const char* host, uint16_t port, int backlog);
int tcp_accept(int listen_fd);
int recv_exact(int fd, uint8_t* buf, size_t n);

#endif


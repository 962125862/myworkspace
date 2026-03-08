#ifndef CONTROL_SOCKET_H
#define CONTROL_SOCKET_H

#include <stdint.h>

#define ML_CTRL_MAGIC   0x4d4c4354u   /* "MLCT" */
#define ML_CTRL_VERSION 1u

#define ML_CTRL_CMD_MOUSE_ABS    1u
#define ML_CTRL_CMD_MOUSE_REL    2u
#define ML_CTRL_CMD_MOUSE_BUTTON 3u
#define ML_CTRL_CMD_MOUSE_CLICK  4u
#define ML_CTRL_CMD_MOUSE_SCROLL   5u
#define ML_CTRL_CMD_MOUSE_HSCROLL  6u


typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    uint64_t seq;
} MlControlCmd;

typedef struct {
    int fd;
    uint16_t port;
} ControlSocket;

int control_socket_open(ControlSocket* s, const char* bind_ip, uint16_t port);
void control_socket_close(ControlSocket* s);
int control_socket_process_all(ControlSocket* s, int reference_width, int reference_height);

#endif

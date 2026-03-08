#include "control_socket.h"

#include <Limelight.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ML_CTRL_MAX_PACKET 2048

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static short clamp_short_signed(int v) {
    if (v < SHRT_MIN) return SHRT_MIN;
    if (v > SHRT_MAX) return SHRT_MAX;
    return (short)v;
}

static int dispatch_cmd(const MlControlCmd* cmd,
                        const unsigned char* payload,
                        size_t payload_len,
                        int reference_width,
                        int reference_height) {
    if (!cmd) {
        return -1;
    }

    switch (cmd->type) {
        case ML_CTRL_CMD_MOUSE_ABS: {
            int ref_w = cmd->c > 0 ? cmd->c : reference_width;
            int ref_h = cmd->d > 0 ? cmd->d : reference_height;

            if (ref_w <= 0 || ref_h <= 0) {
                fprintf(stderr, "[ctrl] invalid reference size: %d x %d\n", ref_w, ref_h);
                return -1;
            }

            int x = clamp_int(cmd->a, 0, ref_w - 1);
            int y = clamp_int(cmd->b, 0, ref_h - 1);

            return LiSendMousePositionEvent(
                (short)x,
                (short)y,
                (short)ref_w,
                (short)ref_h
            );
        }

        case ML_CTRL_CMD_MOUSE_REL:
            return LiSendMouseMoveEvent(
                clamp_short_signed(cmd->a),
                clamp_short_signed(cmd->b)
            );

        case ML_CTRL_CMD_MOUSE_BUTTON:
            /* a=action, b=button */
            return LiSendMouseButtonEvent((char)cmd->a, cmd->b);

        case ML_CTRL_CMD_MOUSE_CLICK: {
            /* a=button */
            int rc = LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, cmd->a);
            if (rc != 0) {
                return rc;
            }
            return LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, cmd->a);
        }

        case ML_CTRL_CMD_MOUSE_SCROLL:
            /* a=clicks */
            return LiSendScrollEvent((signed char)cmd->a);

        case ML_CTRL_CMD_MOUSE_HSCROLL:
            /* a=clicks */
            return LiSendHScrollEvent((signed char)cmd->a);

        case ML_CTRL_CMD_KEYBOARD:
            /* a=keyCode, b=keyAction, c=modifiers */
            return LiSendKeyboardEvent((short)cmd->a, (char)cmd->b, (char)cmd->c);

        case ML_CTRL_CMD_KEY_PRESS: {
            /* a=keyCode, b=modifiers */
            int rc = LiSendKeyboardEvent((short)cmd->a, KEY_ACTION_DOWN, (char)cmd->b);
            if (rc != 0) {
                return rc;
            }
            return LiSendKeyboardEvent((short)cmd->a, KEY_ACTION_UP, (char)cmd->b);
        }

        case ML_CTRL_CMD_TEXT:
            if ((int32_t)payload_len != cmd->a) {
                fprintf(stderr, "[ctrl] text payload length mismatch: hdr=%d pkt=%zu\n",
                        cmd->a, payload_len);
                return -1;
            }
            if (payload_len == 0) {
                return 0;
            }
            return LiSendUtf8TextEvent((const char*)payload, (unsigned int)payload_len);

        default:
            fprintf(stderr, "[ctrl] unknown cmd type=%u\n", cmd->type);
            return -1;
    }
}

int control_socket_open(ControlSocket* s, const char* bind_ip, uint16_t port) {
    if (!s) {
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->fd = -1;

    if (port == 0) {
        return 0;
    }

    if (!bind_ip || bind_ip[0] == '\0') {
        bind_ip = "127.0.0.1";
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[ctrl] socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "[ctrl] fcntl(O_NONBLOCK) failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[ctrl] invalid bind ip: %s\n", bind_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ctrl] bind(%s:%u) failed: %s\n",
                bind_ip, (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }

    s->fd = fd;
    s->port = port;

    fprintf(stderr, "[ctrl] listening on %s:%u\n", bind_ip, (unsigned)port);
    return 0;
}

void control_socket_close(ControlSocket* s) {
    if (!s) {
        return;
    }

    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }

    s->port = 0;
}

int control_socket_process_all(ControlSocket* s, int reference_width, int reference_height) {
    if (!s || s->fd < 0) {
        return 0;
    }

    int processed = 0;

    for (;;) {
        unsigned char packet[ML_CTRL_MAX_PACKET];
        ssize_t n = recvfrom(s->fd, packet, sizeof(packet), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            fprintf(stderr, "[ctrl] recvfrom() failed: %s\n", strerror(errno));
            break;
        }

        if ((size_t)n < sizeof(MlControlCmd)) {
            fprintf(stderr, "[ctrl] ignoring short packet: %zd\n", n);
            continue;
        }

        MlControlCmd cmd;
        memcpy(&cmd, packet, sizeof(cmd));

        if (cmd.magic != ML_CTRL_MAGIC) {
            fprintf(stderr, "[ctrl] ignoring packet with bad magic: 0x%x\n", cmd.magic);
            continue;
        }

        if (cmd.version != ML_CTRL_VERSION) {
            fprintf(stderr, "[ctrl] ignoring packet with bad version: %u\n", cmd.version);
            continue;
        }

        const unsigned char* payload = packet + sizeof(MlControlCmd);
        size_t payload_len = (size_t)n - sizeof(MlControlCmd);

        int rc = dispatch_cmd(&cmd, payload, payload_len, reference_width, reference_height);
        if (rc != 0) {
            fprintf(stderr, "[ctrl] command seq=%llu type=%u failed: %d\n",
                    (unsigned long long)cmd.seq, cmd.type, rc);
        }

        processed++;
    }

    return processed;
}

/**
 * @file h264_tap.c
 * @brief H.264 tap server (agent): publish AnnexB bytestream over TCP
 *
 * This file is based on stream_server/src/h264_tap.c, with an extra optional
 * AUTH <token> line on video connections.
 */

#define _GNU_SOURCE

#include "h264_tap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "tlv_protocol.h" /* MAX_STREAMS */

/* token for AUTH on video connections */
static char g_video_token[256] = {0};

void h264_tap_set_token(const char* token) {
    if (!token) {
        g_video_token[0] = '\0';
        return;
    }
    snprintf(g_video_token, sizeof(g_video_token), "%s", token);
}

static int read_and_check_auth_line(int fd) {
    if (g_video_token[0] == '\0') return 0;
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';
    const char* p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "AUTH ", 5) != 0) return -1;
    p += 5;
    char got[256];
    int gi = 0;
    while (*p && *p != '\n' && *p != '\r' && gi < (int)sizeof(got) - 1) {
        got[gi++] = *p++;
    }
    got[gi] = '\0';
    return (strcmp(got, g_video_token) == 0) ? 0 : -1;
}

/* Simplified support for few subscribers (agent usage) */
#define TAP_MAX_CLIENTS 16

typedef struct {
    int fd;
    uint16_t stream_id;
    bool active;

    /* send state: keep one pending buffer, drop old data if client stalls */
    uint8_t* out_buf;
    size_t out_cap;
    size_t out_len;
    size_t out_off;
    uint64_t out_start_ns;

    bool need_idr;
    bool pending_idr; /* the pending out_buf contains an IDR used for recovery */
    uint8_t* sps;
    size_t sps_len;
    uint8_t* pps;
    size_t pps_len;
} TapClient;

static struct {
    int listen_fd;
    bool running;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    TapClient clients[TAP_MAX_CLIENTS];
} g_tap;

static int g_stall_ms = 200;
static int g_drop_to_idr = 1;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void client_close(TapClient* c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->out_buf);
    free(c->sps);
    free(c->pps);
    c->fd = -1;
    c->active = false;
    c->stream_id = 0;
    c->out_buf = NULL;
    c->out_cap = c->out_len = c->out_off = 0;
    c->out_start_ns = 0;
    c->need_idr = false;
    c->pending_idr = false;
    c->sps = c->pps = NULL;
    c->sps_len = c->pps_len = 0;
}

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Read a subscribe line (best effort). Supports: "SUB 3\n" or "3\n" */
/* Best-effort optional subscribe line.
 * IMPORTANT: do not consume bytes if the peer doesn't actually send an ASCII line,
 * otherwise we'd eat the beginning of H.264 bytestream and break decoding (e.g. ffmpeg/ffplay).
 */
static uint16_t read_subscribe_stream_id(int fd) {
    uint16_t sid = 1;

    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_PEEK);
    if (n <= 0) {
        return sid;
    }
    buf[n] = '\0';

    /* If it doesn't look like ASCII command, assume it's H264 and don't consume. */
    unsigned char c0 = (unsigned char)buf[0];
    if (!((c0 >= '0' && c0 <= '9') || c0 == 'S')) {
        return sid;
    }

    /* Need a newline to treat as a line-based command. */
    char* nl = strchr(buf, '\n');
    if (!nl) {
        return sid;
    }

    int parsed = 0;
    int tmp = 1;
    if (sscanf(buf, "SUB %d", &tmp) == 1 || sscanf(buf, "%d", &tmp) == 1) {
        if (tmp < 1) tmp = 1;
        if (tmp > MAX_STREAMS) tmp = MAX_STREAMS;
        sid = (uint16_t)tmp;
        parsed = 1;
    }
    if (!parsed) {
        return 1;
    }

    /* Consume the line (including newline). */
    size_t line_len = (size_t)(nl - buf) + 1;
    (void)recv(fd, buf, line_len, 0);
    return sid;
}

static bool has_start_code(const uint8_t* p, int n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

/* Scan AnnexB bytestream for NAL type */
static bool annexb_contains_nal_type(const uint8_t* p, int n, int nal_type) {
    if (!p || n < 5) return false;
    int i = 0;
    while (i + 4 < n) {
        int sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            sc = 3;
        } else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            sc = 4;
        }
        if (sc) {
            int hdr_idx = i + sc;
            if (hdr_idx < n) {
                int t = p[hdr_idx] & 0x1F;
                if (t == nal_type) return true;
            }
            i = hdr_idx + 1;
            continue;
        }
        i++;
    }
    return false;
}

static void maybe_cache_param_sets(TapClient* c, const uint8_t* data, int size) {
    if (!c || !data || size <= 0) return;
    if (!has_start_code(data, size)) return;

    /* Get first NAL type */
    int nal_type = -1;
    if (size >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal_type = data[4] & 0x1F;
    } else if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        nal_type = data[3] & 0x1F;
    }
    if (nal_type != 7 && nal_type != 8) return;

    uint8_t** dst = (nal_type == 7) ? &c->sps : &c->pps;
    size_t* dst_len = (nal_type == 7) ? &c->sps_len : &c->pps_len;

    uint8_t* tmp = realloc(*dst, (size_t)size);
    if (!tmp) return;
    memcpy(tmp, data, (size_t)size);
    *dst = tmp;
    *dst_len = (size_t)size;
}

static void ensure_out_buf(TapClient* c, size_t need) {
    if (!c) return;
    if (need <= c->out_cap) return;
    size_t cap = c->out_cap ? c->out_cap : (256 * 1024);
    while (cap < need) cap *= 2;
    uint8_t* nb = realloc(c->out_buf, cap);
    if (!nb) return;
    c->out_buf = nb;
    c->out_cap = cap;
}

/* Try flush current out_buf. Return true if cleared. */
static bool flush_out_buf(TapClient* c) {
    if (!c || !c->active) return true;
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out_buf + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n == 0) {
            client_close(c);
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        client_close(c);
        return true;
    }

    c->out_len = 0;
    c->out_off = 0;
    c->out_start_ns = 0;

    /* If we just finished sending a recovery IDR, exit recovery mode.
     * Without this, we'd keep dropping non-IDR frames until the next IDR.
     */
    if (c->pending_idr) {
        c->pending_idr = false;
        c->need_idr = false;
    }
    return true;
}

void h264_tap_publish(uint16_t stream_id, const uint8_t* data, int size) {
    if (!g_tap.running || !data || size <= 0) return;

    static const uint8_t sc4[4] = {0, 0, 0, 1};
    bool need_sc = !has_start_code(data, size);
    bool is_idr = annexb_contains_nal_type(data, size, 5);

    pthread_mutex_lock(&g_tap.lock);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        TapClient* c = &g_tap.clients[i];
        if (!c->active) continue;
        if (c->stream_id != stream_id) continue;

        /* cache SPS/PPS (enhancement) */
        maybe_cache_param_sets(c, data, size);

        /* If previous send stalled too long, drop pending and wait for IDR */
        if (c->out_len > c->out_off && c->out_start_ns) {
            const uint64_t now = monotonic_ns();
            const uint64_t stall_ns = now - c->out_start_ns;
            if (stall_ns > (uint64_t)g_stall_ms * 1000ull * 1000ull) {
                c->out_len = 0;
                c->out_off = 0;
                c->out_start_ns = 0;
                if (g_drop_to_idr) {
                    c->need_idr = true;
                }
            }
        }

        /* Flush old pending; if still pending, drop current */
        if (c->out_len > c->out_off) {
            if (!flush_out_buf(c)) {
                continue;
            }
        }

        /* recovery: drop until IDR */
        if (c->need_idr && !is_idr) {
            continue;
        }

        /* Assemble: optional SPS/PPS on recovery + current AU */
        size_t total = 0;
        if (c->need_idr) {
            if (c->sps && c->sps_len) total += c->sps_len;
            if (c->pps && c->pps_len) total += c->pps_len;
        }
        total += (need_sc ? sizeof(sc4) : 0) + (size_t)size;

        ensure_out_buf(c, total);
        if (!c->out_buf || c->out_cap < total) {
            continue;
        }

        size_t off = 0;
        if (c->need_idr) {
            if (c->sps && c->sps_len) {
                memcpy(c->out_buf + off, c->sps, c->sps_len);
                off += c->sps_len;
            }
            if (c->pps && c->pps_len) {
                memcpy(c->out_buf + off, c->pps, c->pps_len);
                off += c->pps_len;
            }
        }
        if (need_sc) {
            memcpy(c->out_buf + off, sc4, sizeof(sc4));
            off += sizeof(sc4);
        }
        memcpy(c->out_buf + off, data, (size_t)size);
        off += (size_t)size;

        c->out_len = off;
        c->out_off = 0;
        c->out_start_ns = monotonic_ns();

        if (c->need_idr && is_idr) {
            c->pending_idr = true;
        }

        /* Try immediate flush, non-blocking */
        (void)flush_out_buf(c);
        /* need_idr may be cleared inside flush_out_buf() when pending_idr finishes */
    }
    pthread_mutex_unlock(&g_tap.lock);
}

static void* accept_loop(void* arg) {
    (void)arg;
    while (g_tap.running) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(g_tap.listen_fd, (struct sockaddr*)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            if (!g_tap.running) break;
            usleep(10000);
            continue;
        }

        set_nonblocking(fd);

        if (read_and_check_auth_line(fd) != 0) {
            close(fd);
            continue;
        }

        uint16_t sid = read_subscribe_stream_id(fd);

        pthread_mutex_lock(&g_tap.lock);
        int placed = 0;
        for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
            if (!g_tap.clients[i].active) {
                TapClient* c = &g_tap.clients[i];
                c->fd = fd;
                c->stream_id = sid;
                c->active = true;
                c->need_idr = true;
                placed = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_tap.lock);

        if (!placed) {
            close(fd);
        }
    }
    return NULL;
}

int h264_tap_start(const char* bind_ip, uint16_t port) {
    if (port == 0) return 0;

    memset(&g_tap, 0, sizeof(g_tap));
    g_tap.listen_fd = -1;
    pthread_mutex_init(&g_tap.lock, NULL);

    const char* stall_env = getenv("H264_TAP_STALL_MS");
    if (stall_env) {
        int v = atoi(stall_env);
        if (v >= 10 && v <= 5000) g_stall_ms = v;
    }
    const char* drop_env = getenv("H264_TAP_DROP_IDR");
    if (drop_env) {
        g_drop_to_idr = atoi(drop_env) > 0;
    }

    if (!bind_ip || bind_ip[0] == '\0') {
        bind_ip = "127.0.0.1";
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    set_nonblocking(fd);
    g_tap.listen_fd = fd;
    g_tap.running = true;

    if (pthread_create(&g_tap.accept_thread, NULL, accept_loop, NULL) != 0) {
        close(fd);
        g_tap.listen_fd = -1;
        g_tap.running = false;
        return -1;
    }

    fprintf(stderr, "[agent_h264_tap] listening on %s:%u\n", bind_ip, (unsigned)port);
    return 0;
}

void h264_tap_stop(void) {
    if (!g_tap.running) return;
    g_tap.running = false;
    if (g_tap.listen_fd >= 0) {
        close(g_tap.listen_fd);
        g_tap.listen_fd = -1;
    }
    pthread_join(g_tap.accept_thread, NULL);

    pthread_mutex_lock(&g_tap.lock);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        if (g_tap.clients[i].active) {
            client_close(&g_tap.clients[i]);
        }
    }
    pthread_mutex_unlock(&g_tap.lock);

    pthread_mutex_destroy(&g_tap.lock);
}

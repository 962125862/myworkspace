/* Copied (lightly) from stream_server/src/h264_tap.c.
 * Purpose: TCP bridge for raw AnnexB H.264.
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

#include "tlv_protocol.h" /* for MAX_STREAMS */

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

#define TAP_MAX_CLIENTS 16

typedef struct {
    int fd;
    uint16_t stream_id;
    bool active;

    uint8_t* out_buf;
    size_t out_cap;
    size_t out_len;
    size_t out_off;
    uint64_t out_start_ns;

    bool need_idr;
    uint8_t* sps;
    size_t sps_len;
    uint8_t* pps;
    size_t pps_len;
} TapClient;

static struct {
    int listen_fd;
    bool running;
    pthread_t accept_thread;
    pthread_t send_thread;
    pthread_mutex_t lock;
    TapClient clients[TAP_MAX_CLIENTS];
} g_tap;

static int g_stall_ms = 200;
static int g_drop_to_idr = 1;

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

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
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static uint16_t read_subscribe_stream_id(int fd) {
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 1;
    buf[n] = '\0';
    int sid = 1;
    if (sscanf(buf, "SUB %d", &sid) == 1 || sscanf(buf, "%d", &sid) == 1) {
        if (sid < 1) sid = 1;
        if (sid > MAX_STREAMS) sid = MAX_STREAMS;
        return (uint16_t)sid;
    }
    return 1;
}

static bool has_start_code(const uint8_t* p, int n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

static bool annexb_contains_nal_type(const uint8_t* p, int n, int nal_type) {
    if (!p || n < 5) return false;
    int i = 0;
    while (i + 4 < n) {
        int sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) sc = 3;
        else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        if (sc) {
            int hdr_idx = i + sc;
            if (hdr_idx < n) {
                int t = p[hdr_idx] & 0x1F;
                if (t == nal_type) return true;
            }
            i = hdr_idx + 1;
        } else {
            i++;
        }
    }
    return false;
}

static void maybe_cache_param_sets(TapClient* c, const uint8_t* data, int size) {
    if (!c || !data || size <= 0) return;
    if (!has_start_code(data, size)) return;

    /* Only cache if this payload looks like a single NAL (lightweight rule):
     * - has a start code at beginning
     * - does NOT contain another start code later
     */
    for (int i = 4; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) return;
    }

    int nal_type = -1;
    if (size >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal_type = data[4] & 0x1F;
    } else if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        nal_type = data[3] & 0x1F;
    }
    if (nal_type != 7 && nal_type != 8) return;

    uint8_t** dst = (nal_type == 7) ? &c->sps : &c->pps;
    size_t* dst_len = (nal_type == 7) ? &c->sps_len : &c->pps_len;
    uint8_t* nb = (uint8_t*)realloc(*dst, (size_t)size);
    if (!nb) return;
    memcpy(nb, data, (size_t)size);
    *dst = nb;
    *dst_len = (size_t)size;
}

static int client_queue_frame(TapClient* c, const uint8_t* data, int size) {
    if (!c || !c->active) return -1;

    maybe_cache_param_sets(c, data, size);

    /* If congested, optionally drop until IDR */
    const bool is_idr = annexb_contains_nal_type(data, size, 5);
    if (g_drop_to_idr && c->need_idr && !is_idr) {
        return 0;
    }

    /* Ensure start code */
    static const uint8_t sc4[4] = {0, 0, 0, 1};
    int need_sc = has_start_code(data, size) ? 0 : 1;

    /* When recovering (need_idr), prepend cached SPS/PPS if available.
     * This makes new clients decodable even if SPS/PPS were sent earlier.
     */
    size_t total = 0;
    if (c->need_idr && is_idr) {
        if (c->sps && c->sps_len) total += c->sps_len;
        if (c->pps && c->pps_len) total += c->pps_len;
    }
    total += (size_t)size + (need_sc ? 4u : 0u);
    if (total > 5 * 1024 * 1024) {
        return -1;
    }

    if (c->out_cap < total) {
        uint8_t* nb = (uint8_t*)realloc(c->out_buf, total);
        if (!nb) return -1;
        c->out_buf = nb;
        c->out_cap = total;
    }
    c->out_len = total;
    c->out_off = 0;
    c->out_start_ns = monotonic_ns();

    uint8_t* p = c->out_buf;
    size_t off = 0;
    if (c->need_idr && is_idr) {
        if (c->sps && c->sps_len) {
            memcpy(p + off, c->sps, c->sps_len);
            off += c->sps_len;
        }
        if (c->pps && c->pps_len) {
            memcpy(p + off, c->pps, c->pps_len);
            off += c->pps_len;
        }
    }
    if (need_sc) {
        memcpy(p + off, sc4, 4);
        off += 4;
    }
    memcpy(p + off, data, (size_t)size);
    off += (size_t)size;
    c->out_len = off;

    if (is_idr) {
        c->need_idr = false;
    }
    return 0;
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

        /* If token enabled, expect AUTH line first. */
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
                memset(c, 0, sizeof(*c));
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

static void* send_loop(void* arg) {
    (void)arg;
    while (g_tap.running) {
        pthread_mutex_lock(&g_tap.lock);
        for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
            TapClient* c = &g_tap.clients[i];
            if (!c->active || c->fd < 0) continue;
            if (c->out_off >= c->out_len) continue;

            /* stall detection */
            uint64_t now = monotonic_ns();
            uint64_t elapsed_ms = (now - c->out_start_ns) / 1000000ull;
            if (elapsed_ms > (uint64_t)g_stall_ms) {
                c->out_len = c->out_off = 0;
                c->need_idr = true;
                continue;
            }

            ssize_t n = send(c->fd, c->out_buf + c->out_off, c->out_len - c->out_off, 0);
            if (n > 0) {
                c->out_off += (size_t)n;
                if (c->out_off >= c->out_len) {
                    c->out_len = c->out_off = 0;
                }
            } else if (n == 0) {
                client_close(c);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                client_close(c);
            }
        }
        pthread_mutex_unlock(&g_tap.lock);
        usleep(1000);
    }
    return NULL;
}

int h264_tap_start(const char* bind_ip, uint16_t port) {
    if (port == 0) return -1;
    if (!bind_ip || bind_ip[0] == '\0') bind_ip = "0.0.0.0";

    memset(&g_tap, 0, sizeof(g_tap));
    g_tap.listen_fd = -1;
    pthread_mutex_init(&g_tap.lock, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
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
    if (listen(fd, 32) < 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    g_tap.listen_fd = fd;
    g_tap.running = true;

    const char* stall_env = getenv("H264_TAP_STALL_MS");
    if (stall_env) {
        int v = atoi(stall_env);
        if (v >= 10 && v <= 5000) g_stall_ms = v;
    }
    const char* drop_env = getenv("H264_TAP_DROP_IDR");
    if (drop_env) {
        g_drop_to_idr = atoi(drop_env) > 0;
    }

    if (pthread_create(&g_tap.accept_thread, NULL, accept_loop, NULL) != 0) {
        close(fd);
        g_tap.running = false;
        return -1;
    }
    if (pthread_create(&g_tap.send_thread, NULL, send_loop, NULL) != 0) {
        g_tap.running = false;
        return -1;
    }

    fprintf(stderr, "[agent_h264_tap] listening on %s:%u\n", bind_ip, (unsigned)port);
    return 0;
}

void h264_tap_stop(void) {
    if (!g_tap.running) return;
    g_tap.running = false;
    if (g_tap.listen_fd >= 0) close(g_tap.listen_fd);
    pthread_join(g_tap.accept_thread, NULL);
    pthread_join(g_tap.send_thread, NULL);

    pthread_mutex_lock(&g_tap.lock);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        if (g_tap.clients[i].active) {
            client_close(&g_tap.clients[i]);
        }
    }
    pthread_mutex_unlock(&g_tap.lock);
    pthread_mutex_destroy(&g_tap.lock);
}

void h264_tap_publish(uint16_t stream_id, const uint8_t* data, int size) {
    if (!g_tap.running || !data || size <= 0) return;
    pthread_mutex_lock(&g_tap.lock);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        TapClient* c = &g_tap.clients[i];
        if (!c->active) continue;
        if (c->stream_id != stream_id) continue;
        (void)client_queue_frame(c, data, size);
    }
    pthread_mutex_unlock(&g_tap.lock);
}

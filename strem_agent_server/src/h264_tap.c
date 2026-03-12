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
#include "mlctl_cmd.h"

/* agent_server -> ml_worker udp control (for request IDR) */
extern int udp_send_to(const char* ip, uint16_t port, const uint8_t* data, size_t len);

/* token for AUTH on video connections */
static char g_video_token[256] = {0};

static inline uint64_t monotonic_ns(void);

static char g_worker_ctrl_ip[64] = {0};
static uint16_t g_worker_ctrl_port = 0;

void h264_tap_set_worker_ctrl(const char* ip, uint16_t port) {
    if (!ip || ip[0] == '\0' || port == 0) {
        g_worker_ctrl_ip[0] = '\0';
        g_worker_ctrl_port = 0;
        return;
    }
    snprintf(g_worker_ctrl_ip, sizeof(g_worker_ctrl_ip), "%s", ip);
    g_worker_ctrl_port = port;
}

static void request_idr_best_effort(void) {
    if (g_worker_ctrl_ip[0] == '\0' || g_worker_ctrl_port == 0) {
        return;
    }
    /* Send MlControlCmd to ml_worker UDP control socket.
     * Use type=10 (ML_CTRL_CMD_REQ_IDR) which ml_worker handles by LiRequestIdrFrame().
     */
    MlControlCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = ML_CTRL_MAGIC;
    cmd.version = (uint16_t)ML_CTRL_VERSION;
    cmd.type = (uint16_t)10; /* ML_CTRL_CMD_REQ_IDR */
    cmd.seq = (uint64_t)monotonic_ns();
    (void)udp_send_to(g_worker_ctrl_ip, g_worker_ctrl_port, (const uint8_t*)&cmd, sizeof(cmd));
}

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void h264_tap_set_token(const char* token) {
    if (!token) {
        g_video_token[0] = '\0';
        return;
    }
    snprintf(g_video_token, sizeof(g_video_token), "%s", token);
}

static int parse_sub_line(const char* line, uint16_t* out_sid) {
    if (!line || !out_sid) return -1;
    int sid = -1;
    if (sscanf(line, "SUB %d", &sid) == 1 || sscanf(line, "%d", &sid) == 1) {
        if (sid < 1) sid = 1;
        if (sid > MAX_STREAMS) sid = MAX_STREAMS;
        *out_sid = (uint16_t)sid;
        return 0;
    }
    return -1;
}

/* Handshake: require SUB line; if token enabled, require AUTH line first.
 * This function is robust to AUTH and SUB being sent in the same TCP packet.
 */
static int do_handshake(int fd, uint16_t* out_sid) {
    if (!out_sid) return -1;

    const int timeout_ms = 3000;
    const uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;

    bool need_auth = (g_video_token[0] != '\0');
    bool authed = !need_auth;
    bool got_sub = false;
    uint16_t sid = 1;

    char buf[1024];
    size_t len = 0;

    while (monotonic_ns() < deadline) {
        ssize_t n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n > 0) {
            len += (size_t)n;
            buf[len] = '\0';

            /* parse complete lines */
            char* start = buf;
            while (1) {
                char* nl = strchr(start, '\n');
                if (!nl) break;
                *nl = '\0';
                /* trim CR */
                size_t L = strlen(start);
                while (L > 0 && (start[L - 1] == '\r' || start[L - 1] == ' ' || start[L - 1] == '\t')) {
                    start[--L] = '\0';
                }

                if (!authed) {
                    if (strncmp(start, "AUTH ", 5) != 0) {
                        return -1;
                    }
                    const char* tok = start + 5;
                    if (strcmp(tok, g_video_token) != 0) {
                        return -1;
                    }
                    authed = true;
                } else if (!got_sub) {
                    if (parse_sub_line(start, &sid) != 0) {
                        return -1;
                    }
                    got_sub = true;
                    break;
                }

                start = nl + 1;
            }

            if (got_sub) {
                *out_sid = sid;
                return 0;
            }

            /* keep remaining partial line */
            if (start != buf) {
                size_t remain = strlen(start);
                memmove(buf, start, remain);
                len = remain;
                buf[len] = '\0';
            }
            continue;
        }

        if (n == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* non-blocking socket */
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }

    return -1;
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

/* forward decls */
static void ensure_out_buf(TapClient* c, size_t need);
static bool flush_out_buf(TapClient* c);
static bool has_start_code(const uint8_t* p, int n);

static struct {
    int listen_fd;
    bool running;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    TapClient clients[TAP_MAX_CLIENTS];

    /* Latest parameter sets seen for each stream (AnnexB NAL including start code).
     * Used to bootstrap late-joining clients.
     */
    uint8_t* sps[MAX_STREAMS + 1];
    size_t sps_len[MAX_STREAMS + 1];
    uint8_t* pps[MAX_STREAMS + 1];
    size_t pps_len[MAX_STREAMS + 1];
} g_tap;

static int g_stall_ms = 200;
static int g_drop_to_idr = 1;

/* Cache SPS/PPS NALs for late joiners.
 * Best-effort: only caches packets where the *first* NAL is SPS/PPS.
 */
static void update_global_param_sets(uint16_t stream_id, const uint8_t* data, int size) {
    if (!data || size <= 0) return;
    if (stream_id < 1 || stream_id > MAX_STREAMS) return;

    if (!has_start_code(data, size)) {
        return;
    }
    int nal_type = -1;
    if (size >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal_type = data[4] & 0x1F;
    } else if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        nal_type = data[3] & 0x1F;
    }
    if (nal_type != 7 && nal_type != 8) {
        return;
    }

    uint8_t** dst = (nal_type == 7) ? &g_tap.sps[stream_id] : &g_tap.pps[stream_id];
    size_t* dst_len = (nal_type == 7) ? &g_tap.sps_len[stream_id] : &g_tap.pps_len[stream_id];

    uint8_t* tmp = realloc(*dst, (size_t)size);
    if (!tmp) return;
    memcpy(tmp, data, (size_t)size);
    *dst = tmp;
    *dst_len = (size_t)size;
}

static void copy_global_param_sets_to_client(TapClient* c, uint16_t stream_id) {
    if (!c) return;
    if (stream_id < 1 || stream_id > MAX_STREAMS) return;

    if (g_tap.sps[stream_id] && g_tap.sps_len[stream_id]) {
        uint8_t* tmp = realloc(c->sps, g_tap.sps_len[stream_id]);
        if (tmp) {
            memcpy(tmp, g_tap.sps[stream_id], g_tap.sps_len[stream_id]);
            c->sps = tmp;
            c->sps_len = g_tap.sps_len[stream_id];
        }
    }
    if (g_tap.pps[stream_id] && g_tap.pps_len[stream_id]) {
        uint8_t* tmp = realloc(c->pps, g_tap.pps_len[stream_id]);
        if (tmp) {
            memcpy(tmp, g_tap.pps[stream_id], g_tap.pps_len[stream_id]);
            c->pps = tmp;
            c->pps_len = g_tap.pps_len[stream_id];
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void client_close(TapClient* c);

/* If no video is being published, we may never call send(), so TCP close (FIN/RST)
 * from a client can remain undetected and the slot stays occupied. Prune clients
 * by peeking recv() in non-blocking mode.
 * Caller must hold g_tap.lock.
 */
static void prune_dead_clients_locked(void) {
    char ch;
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        TapClient* c = &g_tap.clients[i];
        if (!c->active || c->fd < 0) continue;

        ssize_t n = recv(c->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) {
            /* Unexpected inbound data. Ignore (we don't expect any after SUB). */
            continue;
        }
        if (n == 0) {
            /* Peer performed an orderly shutdown. */
            client_close(c);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue; /* no data, still alive */
        }
        /* Treat other errors as dead. */
        client_close(c);
    }
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

/* Read a subscribe line (SUB is mandatory in agent).
 * Client must send: "SUB <stream_id>\n" (optionally preceded by AUTH line).
 */
/* SUB is mandatory in agent: client must send SUB <stream_id>\n */
static uint16_t read_subscribe_stream_id(int fd) {
    uint16_t sid = 1;
    if (do_handshake(fd, &sid) != 0) {
        return 0;
    }
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

    bool is_idr = annexb_contains_nal_type(data, size, 5);
    static const uint8_t sc4[4] = {0, 0, 0, 1};
    bool need_sc = !has_start_code(data, size);

    pthread_mutex_lock(&g_tap.lock);
    /* Update global SPS/PPS cache under lock (shared with accept loop). */
    update_global_param_sets(stream_id, data, size);
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

        uint16_t sid = read_subscribe_stream_id(fd);
        if (sid == 0) {
            close(fd);
            continue;
        }

        pthread_mutex_lock(&g_tap.lock);
        prune_dead_clients_locked();
        int placed = 0;
        TapClient* placed_c = NULL;
        for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
            if (!g_tap.clients[i].active) {
                TapClient* c = &g_tap.clients[i];
                c->fd = fd;
                c->stream_id = sid;
                c->active = true;
                /* Late join: wait for an IDR frame for clean decoder start.
                 * We will also actively request an IDR from the upstream host.
                 */
                c->need_idr = true;
                placed = 1;
                placed_c = c;

                /* Bootstrap param sets for late joiners (best-effort). */
                copy_global_param_sets_to_client(c, sid);
                fprintf(stderr, "[agent_h264_tap] client subscribed stream_id=%u\n", (unsigned)sid);
                break;
            }
        }

        if (!placed) {
            /* Retry after pruning (in case dead clients were occupying slots). */
            prune_dead_clients_locked();
            for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
                if (!g_tap.clients[i].active) {
                    TapClient* c = &g_tap.clients[i];
                    c->fd = fd;
                    c->stream_id = sid;
                    c->active = true;
                    c->need_idr = true;
                    placed = 1;
                    placed_c = c;

                    copy_global_param_sets_to_client(c, sid);
                    fprintf(stderr, "[agent_h264_tap] client subscribed stream_id=%u\n", (unsigned)sid);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_tap.lock);

        /* Ask upstream to send an IDR soon (do it outside the lock). */
        if (placed && placed_c) {
            request_idr_best_effort();
        }

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

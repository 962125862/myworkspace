/**
 * @file h264_tap.c
 * @brief H.264 tap server (debug/bridge): publish AnnexB bytestream over TCP
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

static inline uint64_t monotonic_ns(void);

/* 简化实现：最多支持少量订阅者（调试用途）。 */
#define TAP_MAX_CLIENTS 8

typedef struct {
    int fd;
    uint16_t stream_id;
    bool active;

    /* 发送状态：为避免非阻塞 send 的 partial write 导致码流损坏，
     * 每个客户端维护一个“待发送缓冲区”(最多一帧/AU)。
     * 若发送跟不上，则丢弃后续帧，直到待发送缓冲区清空。
     */
    uint8_t* out_buf;
    size_t out_cap;
    size_t out_len;
    size_t out_off;
    uint64_t out_start_ns;

    /* 拥塞后恢复：丢弃直到下一个 IDR，再尝试发送（可选缓存 SPS/PPS） */
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

static void client_close(TapClient* c);

/* If no stream data is being published, we may never call send(), so TCP close (FIN/RST)
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
            /* Ignore unexpected inbound data (SUB is read once at accept). */
            continue;
        }
        if (n == 0) {
            client_close(c);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
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
    c->sps = c->pps = NULL;
    c->sps_len = c->pps_len = 0;
}

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 读取一行订阅命令（非阻塞，尽力而为）。支持："SUB 3\n" 或 "3\n" */
static uint16_t read_subscribe_stream_id(int fd) {
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return 1; /* 默认 */
    }
    buf[n] = '\0';
    int sid = 1;
    if (sscanf(buf, "SUB %d", &sid) == 1 || sscanf(buf, "%d", &sid) == 1) {
        if (sid < 1) sid = 1;
        if (sid > 20) sid = 20;
        return (uint16_t)sid;
    }
    return 1;
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
            perror("[h264_tap] accept");
            usleep(10000);
            continue;
        }

        /* 客户端 fd 设为非阻塞：如果发送端跟不上，就丢弃/断开，不阻塞主线程 */
        set_nonblocking(fd);

        uint16_t sid = read_subscribe_stream_id(fd);

        pthread_mutex_lock(&g_tap.lock);
        prune_dead_clients_locked();
        int placed = 0;
        for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
            if (!g_tap.clients[i].active) {
                g_tap.clients[i].fd = fd;
                g_tap.clients[i].stream_id = sid;
                g_tap.clients[i].active = true;
                placed = 1;
                break;
            }
        }

        if (!placed) {
            prune_dead_clients_locked();
            for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
                if (!g_tap.clients[i].active) {
                    g_tap.clients[i].fd = fd;
                    g_tap.clients[i].stream_id = sid;
                    g_tap.clients[i].active = true;
                    placed = 1;
                    break;
                }
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

    /* runtime config */
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
        perror("[h264_tap] socket");
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
        fprintf(stderr, "[h264_tap] invalid bind ip: %s\n", bind_ip);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        perror("[h264_tap] bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("[h264_tap] listen");
        close(fd);
        return -1;
    }

    set_nonblocking(fd);
    g_tap.listen_fd = fd;
    g_tap.running = true;

    if (pthread_create(&g_tap.accept_thread, NULL, accept_loop, NULL) != 0) {
        perror("[h264_tap] pthread_create");
        close(fd);
        g_tap.listen_fd = -1;
        g_tap.running = false;
        return -1;
    }

    fprintf(stderr, "[h264_tap] listening on %s:%u\n", bind_ip, (unsigned)port);
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

static bool has_start_code(const uint8_t* p, int n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

/* 扫描 AnnexB bytestream，判断是否包含 nal_type。
 * 注意：这不是完整解析，只做 start code + header 检测，足够用于 IDR/SPS/PPS 识别。
 */
static bool annexb_contains_nal_type(const uint8_t* p, int n, int nal_type) {
    if (!p || n < 5) return false;
    int i = 0;
    while (i + 4 < n) {
        int sc = 0;
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
            sc = 3;
        } else if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) {
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

    /* 如果 payload 本身就是单个 SPS/PPS NAL，则缓存；
     * 若是多 NAL 聚合包，这里不做精细切分（保持轻量），只做“包含就跳过缓存”。
     * 你接受连接初期黑屏，因此缓存属于增强项，不影响主功能。
     */
    if (!has_start_code(data, size)) return;

    /* 取第一个 NAL 类型 */
    int nal_type = -1;
    if (size >= 5 && data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1) nal_type = data[4] & 0x1F;
    else if (size >= 4 && data[0]==0 && data[1]==0 && data[2]==1) nal_type = data[3] & 0x1F;
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

/* 尝试 flush 当前 out_buf。返回 true 表示已清空。 */
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
    return true;
}

void h264_tap_publish(uint16_t stream_id, const uint8_t* data, int size) {
    if (!g_tap.running || !data || size <= 0) return;

    /* 允许 payload 不带 start code：补 0x00000001 让下游按 AnnexB 解 */
    static const uint8_t sc4[4] = {0, 0, 0, 1};
    bool need_sc = !has_start_code(data, size);

    /* 识别 IDR（用于拥塞恢复时丢弃到 IDR） */
    bool is_idr = annexb_contains_nal_type(data, size, 5);

    pthread_mutex_lock(&g_tap.lock);
    for (int i = 0; i < TAP_MAX_CLIENTS; i++) {
        TapClient* c = &g_tap.clients[i];
        if (!c->active) continue;
        if (c->stream_id != stream_id) continue;

        /* 缓存 SPS/PPS（增强项） */
        maybe_cache_param_sets(c, data, size);

        /* 若上次发送卡住太久，直接丢弃未发完的旧数据并等待 IDR 再恢复（避免延迟累积）。 */
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

        /* 先尽力 flush 之前没发完的数据；没发完则丢弃当前帧（不断开连接） */
        if (c->out_len > c->out_off) {
            if (!flush_out_buf(c)) {
                continue;
            }
        }

        /* 拥塞恢复阶段：丢弃直到 IDR */
        if (c->need_idr && !is_idr) {
            continue;
        }

        /* 组装本次要发的数据：可选 SPS/PPS + 当前 AU */
        size_t total = 0;
        if (c->need_idr) {
            /* 恢复时先发 SPS/PPS（如果有） */
            if (c->sps && c->sps_len) total += c->sps_len;
            if (c->pps && c->pps_len) total += c->pps_len;
        }
        total += (need_sc ? sizeof(sc4) : 0) + (size_t)size;

        ensure_out_buf(c, total);
        if (!c->out_buf || c->out_cap < total) {
            /* OOM: 直接跳过 */
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

        /* 尝试立即发送，不阻塞；若没发完留到下次 publish 再 flush。 */
        (void)flush_out_buf(c);

        if (c->need_idr && is_idr) {
            /* 成功发出一个 IDR 后，退出恢复状态 */
            if (c->out_len == 0) {
                c->need_idr = false;
            }
        }
    }
    pthread_mutex_unlock(&g_tap.lock);
}

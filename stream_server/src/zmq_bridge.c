/**
 * @file zmq_bridge.c
 * @brief stream_server 内置 ZMQ bridge（可选编译）。
 *
 * 默认不启用（需要编译时定义 HAVE_ZMQ 并链接 libzmq）。
 *
 * 设计原则：
 * - 只提供“取最新帧”的能力（GET_LATEST_NV12）。
 * - 复用现有 Python client 协议（multipart frames）。
 * - 不引入 SHM request_seq/publish_seq 语义，避免多客户端竞争。
 * - 每次请求会把 latest NV12 深拷贝成紧凑包（Y w*h + UV w*h/2），然后发送。
 *
 * 进一步优化方向（后续可做）：
 * - 预先维护每路紧凑 NV12 缓冲，解码时更新；请求时零拷贝发送（或单拷贝）。
 */

/* for clock_gettime/CLOCK_MONOTONIC under -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "zmq_bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_ZMQ

#include <zmq.h>

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_frame(void* sock, const void* data, size_t len, int more) {
    zmq_msg_t msg;
    zmq_msg_init_size(&msg, len);
    if (len > 0 && data) {
        memcpy(zmq_msg_data(&msg), data, len);
    }
    int flags = more ? ZMQ_SNDMORE : 0;
    int rc = zmq_msg_send(&msg, sock, flags);
    zmq_msg_close(&msg);
    return (rc >= 0) ? 0 : -1;
}

static int recv_frame(void* sock, zmq_msg_t* out) {
    zmq_msg_init(out);
    int rc = zmq_msg_recv(out, sock, 0);
    if (rc < 0) {
        zmq_msg_close(out);
        return -1;
    }
    return 0;
}

static int msg_is_more(void* sock) {
    int more = 0;
    size_t sz = sizeof(more);
    zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &sz);
    return more;
}

static int parse_stream_id_from_json(const char* json, size_t len) {
    /* 极简解析：找 "stream_id" 后面的数字。
     * 只支持正整数；解析失败则返回 1。
     */
    if (!json || len == 0) return 1;
    const char* p = json;
    const char* end = json + len;
    const char* key = "\"stream_id\"";
    const size_t klen = strlen(key);

    while (p + klen < end) {
        if (memcmp(p, key, klen) == 0) {
            p += klen;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
            int v = 0;
            int any = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                any = 1;
                v = v * 10 + (*p - '0');
                p++;
            }
            if (any && v > 0) return v;
            return 1;
        }
        p++;
    }
    return 1;
}

static int pack_compact_nv12(const DecodedFrame* f,
                             uint8_t** out_y, size_t* out_y_sz,
                             uint8_t** out_uv, size_t* out_uv_sz) {
    if (!f || !out_y || !out_uv || !out_y_sz || !out_uv_sz) return -1;
    if (f->format != DECODE_FMT_NV12) return -1;
    int w = f->width;
    int h = f->height;
    if (w <= 0 || h <= 0) return -1;

    size_t y_sz = (size_t)w * (size_t)h;
    size_t uv_sz = (size_t)w * (size_t)h / 2;

    uint8_t* y = (uint8_t*)malloc(y_sz);
    uint8_t* uv = (uint8_t*)malloc(uv_sz);
    if (!y || !uv) {
        free(y);
        free(uv);
        return -1;
    }

    /* copy Y: h lines, each w bytes from data[0] with linesize[0] */
    const uint8_t* src_y = f->data[0];
    const uint8_t* src_uv = f->data[1];
    int ls_y = f->linesize[0];
    int ls_uv = f->linesize[1];
    if (!src_y || !src_uv || ls_y <= 0 || ls_uv <= 0) {
        free(y);
        free(uv);
        return -1;
    }

    for (int row = 0; row < h; row++) {
        memcpy(y + (size_t)row * (size_t)w, src_y + (size_t)row * (size_t)ls_y, (size_t)w);
    }
    for (int row = 0; row < h / 2; row++) {
        memcpy(uv + (size_t)row * (size_t)w, src_uv + (size_t)row * (size_t)ls_uv, (size_t)w);
    }

    *out_y = y;
    *out_uv = uv;
    *out_y_sz = y_sz;
    *out_uv_sz = uv_sz;
    return 0;
}

typedef struct {
    StreamManager* mgr;
    char bind_addr[128];
    volatile int* running;
} ZmqBridgeArg;

static void* zmq_bridge_thread(void* p) {
    ZmqBridgeArg* arg = (ZmqBridgeArg*)p;

    void* ctx = zmq_ctx_new();
    if (!ctx) {
        fprintf(stderr, "[ZMQ] zmq_ctx_new failed\n");
        free(arg);
        return NULL;
    }

    void* router = zmq_socket(ctx, ZMQ_ROUTER);
    if (!router) {
        fprintf(stderr, "[ZMQ] zmq_socket(ROUTER) failed\n");
        zmq_ctx_term(ctx);
        free(arg);
        return NULL;
    }

    /* 避免慢 client 堆积太多大帧 */
    int sndhwm = 2;
    zmq_setsockopt(router, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
    int rcvhwm = 100;
    zmq_setsockopt(router, ZMQ_RCVHWM, &rcvhwm, sizeof(rcvhwm));

    if (zmq_bind(router, arg->bind_addr) != 0) {
        fprintf(stderr, "[ZMQ] bind failed: %s (%s)\n", arg->bind_addr, zmq_strerror(errno));
        zmq_close(router);
        zmq_ctx_term(ctx);
        free(arg);
        return NULL;
    }

    fprintf(stdout, "[ZMQ] bridge enabled (ROUTER) bind=%s\n", arg->bind_addr);

    while (!arg->running || *arg->running) {
        /* recv multipart: [identity][optional delim][cmd][json] */
        zmq_msg_t f0;
        if (recv_frame(router, &f0) < 0) {
            /* interrupted */
            if (errno == EINTR) continue;
            /* small sleep not needed; zmq blocks */
            continue;
        }
        if (!msg_is_more(router)) {
            zmq_msg_close(&f0);
            continue;
        }

        zmq_msg_t f1;
        if (recv_frame(router, &f1) < 0) {
            zmq_msg_close(&f0);
            continue;
        }

        int have_delim = 0;
        zmq_msg_t cmd_msg;
        zmq_msg_t json_msg;
        memset(&cmd_msg, 0, sizeof(cmd_msg));
        memset(&json_msg, 0, sizeof(json_msg));

        /* f1 might be delimiter (REQ compatibility) or cmd */
        size_t f1_len = zmq_msg_size(&f1);
        if (f1_len == 0) {
            have_delim = 1;
            zmq_msg_close(&f1);
            if (recv_frame(router, &cmd_msg) < 0) {
                zmq_msg_close(&f0);
                continue;
            }
        } else {
            cmd_msg = f1; /* move */
        }

        if (msg_is_more(router)) {
            if (recv_frame(router, &json_msg) < 0) {
                zmq_msg_close(&f0);
                zmq_msg_close(&cmd_msg);
                continue;
            }
        } else {
            zmq_msg_init_size(&json_msg, 2);
            memcpy(zmq_msg_data(&json_msg), "{}", 2);
        }

        const char* cmd = (const char*)zmq_msg_data(&cmd_msg);
        size_t cmd_len = zmq_msg_size(&cmd_msg);
        const char* json = (const char*)zmq_msg_data(&json_msg);
        size_t json_len = zmq_msg_size(&json_msg);

        /* build reply: [identity][optional delim][status][meta_json][y][uv] */
        /* identity */
        (void)send_frame(router, zmq_msg_data(&f0), zmq_msg_size(&f0), 1);
        if (have_delim) {
            (void)send_frame(router, "", 0, 1);
        }

        int stream_id = parse_stream_id_from_json(json, json_len);
        StreamContext* stream = stream_manager_get(arg->mgr, (uint16_t)stream_id);

        if (cmd_len == 4 && memcmp(cmd, "PING", 4) == 0) {
            (void)send_frame(router, "OK", 2, 1);
            (void)send_frame(router, "{}", 2, 0);
        } else if ((cmd_len == strlen("GET_LATEST_NV12") && memcmp(cmd, "GET_LATEST_NV12", cmd_len) == 0) ||
                   (cmd_len == strlen("GET_SHM_NV12") && memcmp(cmd, "GET_SHM_NV12", cmd_len) == 0)) {
            if (!stream) {
                (void)send_frame(router, "ERR", 3, 0);
            } else {
                pthread_mutex_lock(&stream->lock);
                DecodedFrame* f = stream->last_frame;
                if (!f || f->format != DECODE_FMT_NV12) {
                    pthread_mutex_unlock(&stream->lock);
                    (void)send_frame(router, "ERR", 3, 0);
                } else {
                    /* pack compact */
                    uint8_t* y = NULL;
                    uint8_t* uv = NULL;
                    size_t y_sz = 0, uv_sz = 0;
                    int key = f->key_frame ? 1 : 0;
                    int w = f->width;
                    int h = f->height;
                    int64_t pts = f->pts;
                    uint64_t mono = monotonic_ns();
                    if (pack_compact_nv12(f, &y, &y_sz, &uv, &uv_sz) != 0) {
                        pthread_mutex_unlock(&stream->lock);
                        (void)send_frame(router, "ERR", 3, 0);
                    } else {
                        pthread_mutex_unlock(&stream->lock);

                        char meta[256];
                        int n = snprintf(meta, sizeof(meta),
                                         "{\"stream_id\":%d,\"width\":%d,\"height\":%d,\"pts\":%lld,\"mono_ns\":%llu,\"key_frame\":%d}",
                                         stream_id, w, h, (long long)pts, (unsigned long long)mono, key);
                        if (n < 0) n = 0;
                        if ((size_t)n >= sizeof(meta)) n = (int)sizeof(meta) - 1;

                        (void)send_frame(router, "OK", 2, 1);
                        (void)send_frame(router, meta, (size_t)n, 1);
                        (void)send_frame(router, y, y_sz, 1);
                        (void)send_frame(router, uv, uv_sz, 0);
                        free(y);
                        free(uv);
                    }
                }
            }
        } else {
            (void)send_frame(router, "ERR", 3, 0);
        }

        zmq_msg_close(&f0);
        zmq_msg_close(&cmd_msg);
        zmq_msg_close(&json_msg);
    }

    zmq_close(router);
    zmq_ctx_term(ctx);
    free(arg);
    return NULL;
}

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, volatile int* running_flag) {
    if (!mgr || !bind_addr) return -1;
    ZmqBridgeArg* arg = (ZmqBridgeArg*)calloc(1, sizeof(*arg));
    if (!arg) return -1;
    arg->mgr = mgr;
    arg->running = running_flag;
    strncpy(arg->bind_addr, bind_addr, sizeof(arg->bind_addr) - 1);

    pthread_t th;
    if (pthread_create(&th, NULL, zmq_bridge_thread, arg) != 0) {
        free(arg);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

#else /* HAVE_ZMQ */

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, volatile int* running_flag) {
    (void)mgr;
    (void)bind_addr;
    (void)running_flag;
    fprintf(stderr, "[ZMQ] bridge not built (HAVE_ZMQ=0)\n");
    return -1;
}

#endif /* HAVE_ZMQ */

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

/* ==================== cached compact NV12 per stream (zero-copy on send) ====================
 *
 * 目标：
 * - 解码线程每产出新帧时，打包成“紧凑 NV12”并缓存。
 * - ZMQ 请求到来时，直接把缓存的 Y/UV buffer 以 zmq_msg_init_data() 方式发送（零拷贝）。
 *
 * 实现要点：
 * - 每个 stream_id 缓存一个 ZmqFrame* 指针（原子交换）。
 * - ZmqFrame 自带原子引用计数：
 *     - cache 持有 1 个引用
 *     - 每个请求发送 y/uv 两个 message，各持有 1 个引用
 *   当引用计数归零时释放该帧缓存。
 */

typedef struct ZmqFrame {
    _Atomic int refcnt;
    uint16_t stream_id;
    int width;
    int height;
    int64_t pts;
    int key_frame;
    uint64_t mono_ns;

    uint8_t* y;
    uint8_t* uv;
    size_t y_sz;
    size_t uv_sz;
} ZmqFrame;

static _Atomic int g_enabled = 0; /* set to 1 after bind success */
static _Atomic uint8_t g_want_stream[MAX_STREAMS + 1]; /* set to 1 after first request */
static pthread_once_t g_latest_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_latest_mu[MAX_STREAMS + 1];
static ZmqFrame* g_latest[MAX_STREAMS + 1];   /* 1..MAX_STREAMS */

static void latest_init_once(void) {
    for (int i = 0; i <= MAX_STREAMS; i++) {
        pthread_mutex_init(&g_latest_mu[i], NULL);
        g_latest[i] = NULL;
        __atomic_store_n(&g_want_stream[i], 0, __ATOMIC_RELAXED);
    }
}

static void frame_free(ZmqFrame* f) {
    if (!f) return;
    free(f->y);
    free(f->uv);
    free(f);
}

static void frame_addref(ZmqFrame* f, int n) {
    if (!f) return;
    __atomic_add_fetch(&f->refcnt, n, __ATOMIC_RELAXED);
}

static void frame_release(ZmqFrame* f) {
    if (!f) return;
    int v = __atomic_sub_fetch(&f->refcnt, 1, __ATOMIC_ACQ_REL);
    if (v == 0) {
        frame_free(f);
    }
}

static void zmq_frame_release_cb(void* data, void* hint) {
    (void)data;
    frame_release((ZmqFrame*)hint);
}

void zmq_bridge_shutdown(void) {
    pthread_once(&g_latest_once, latest_init_once);
    for (int sid = 1; sid <= MAX_STREAMS; sid++) {
        pthread_mutex_lock(&g_latest_mu[sid]);
        ZmqFrame* old = g_latest[sid];
        g_latest[sid] = NULL;
        pthread_mutex_unlock(&g_latest_mu[sid]);
        if (old) frame_release(old); /* drop cache hold */
        __atomic_store_n(&g_want_stream[sid], 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_enabled, 0, __ATOMIC_RELAXED);
}

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

static int send_msg_and_close(void* sock, zmq_msg_t* msg, int more) {
    int flags = more ? ZMQ_SNDMORE : 0;
    int rc = zmq_msg_send(msg, sock, flags);
    zmq_msg_close(msg);
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

static int parse_timeout_ms_from_json(const char* json, size_t len, int default_ms) {
    if (!json || len == 0) return default_ms;
    const char* p = json;
    const char* end = json + len;
    const char* key = "\"timeout_ms\"";
    const size_t klen = strlen(key);

    while (p + klen < end) {
        if (memcmp(p, key, klen) == 0) {
            p += klen;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
            int sign = 1;
            if (p < end && *p == '-') {
                sign = -1;
                p++;
            }
            int v = 0;
            int any = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                any = 1;
                v = v * 10 + (*p - '0');
                p++;
            }
            if (any) return v * sign;
            return default_ms;
        }
        p++;
    }
    return default_ms;
}

static ZmqFrame* cache_get_hold(uint16_t stream_id) {
    if (stream_id < 1 || stream_id > MAX_STREAMS) return NULL;
    pthread_once(&g_latest_once, latest_init_once);
    pthread_mutex_lock(&g_latest_mu[stream_id]);
    ZmqFrame* f = g_latest[stream_id];
    if (f) frame_addref(f, 1);
    pthread_mutex_unlock(&g_latest_mu[stream_id]);
    return f;
}

static void cache_put(uint16_t stream_id, ZmqFrame* nf) {
    if (!nf) return;
    if (stream_id < 1 || stream_id > MAX_STREAMS) {
        frame_release(nf);
        return;
    }
    pthread_once(&g_latest_once, latest_init_once);
    pthread_mutex_lock(&g_latest_mu[stream_id]);
    ZmqFrame* old = g_latest[stream_id];
    g_latest[stream_id] = nf;
    pthread_mutex_unlock(&g_latest_mu[stream_id]);
    if (old) frame_release(old);
}

static ZmqFrame* build_frame_from_last_frame(uint16_t stream_id, StreamContext* stream) {
    if (!stream) return NULL;
    pthread_mutex_lock(&stream->lock);
    DecodedFrame* lf = stream->last_frame;
    if (!lf || lf->format != DECODE_FMT_NV12) {
        pthread_mutex_unlock(&stream->lock);
        return NULL;
    }

    int w = lf->width;
    int h = lf->height;
    if (w <= 0 || h <= 0) {
        pthread_mutex_unlock(&stream->lock);
        return NULL;
    }

    size_t y_sz = (size_t)w * (size_t)h;
    size_t uv_sz = (size_t)w * (size_t)h / 2;

    ZmqFrame* nf = (ZmqFrame*)calloc(1, sizeof(*nf));
    if (!nf) {
        pthread_mutex_unlock(&stream->lock);
        return NULL;
    }
    nf->y = (uint8_t*)malloc(y_sz);
    nf->uv = (uint8_t*)malloc(uv_sz);
    if (!nf->y || !nf->uv) {
        pthread_mutex_unlock(&stream->lock);
        frame_free(nf);
        return NULL;
    }

    const uint8_t* src_y = lf->data[0];
    const uint8_t* src_uv = lf->data[1];
    int ls_y = lf->linesize[0];
    int ls_uv = lf->linesize[1];
    if (!src_y || !src_uv || ls_y <= 0 || ls_uv <= 0) {
        pthread_mutex_unlock(&stream->lock);
        frame_free(nf);
        return NULL;
    }
    for (int row = 0; row < h; row++) {
        memcpy(nf->y + (size_t)row * (size_t)w, src_y + (size_t)row * (size_t)ls_y, (size_t)w);
    }
    for (int row = 0; row < h / 2; row++) {
        memcpy(nf->uv + (size_t)row * (size_t)w, src_uv + (size_t)row * (size_t)ls_uv, (size_t)w);
    }

    nf->stream_id = stream_id;
    nf->width = w;
    nf->height = h;
    nf->pts = lf->pts;
    nf->key_frame = lf->key_frame ? 1 : 0;
    nf->mono_ns = monotonic_ns();
    nf->y_sz = y_sz;
    nf->uv_sz = uv_sz;
    __atomic_store_n(&nf->refcnt, 1, __ATOMIC_RELAXED); /* cache hold */

    pthread_mutex_unlock(&stream->lock);
    return nf;
}

__attribute__((unused))
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

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_RELAXED)) return;
    if (!frame) return;
    if (stream_id < 1 || stream_id > MAX_STREAMS) return;
    if (!__atomic_load_n(&g_want_stream[stream_id], __ATOMIC_RELAXED)) return;
    if (frame->format != DECODE_FMT_NV12) return;

    pthread_once(&g_latest_once, latest_init_once);

    int w = frame->width;
    int h = frame->height;
    if (w <= 0 || h <= 0) return;

    size_t y_sz = (size_t)w * (size_t)h;
    size_t uv_sz = (size_t)w * (size_t)h / 2;
    ZmqFrame* nf = (ZmqFrame*)calloc(1, sizeof(*nf));
    if (!nf) return;
    nf->y = (uint8_t*)malloc(y_sz);
    nf->uv = (uint8_t*)malloc(uv_sz);
    if (!nf->y || !nf->uv) {
        frame_free(nf);
        return;
    }

    const uint8_t* src_y = frame->data[0];
    const uint8_t* src_uv = frame->data[1];
    int ls_y = frame->linesize[0];
    int ls_uv = frame->linesize[1];
    if (!src_y || !src_uv || ls_y <= 0 || ls_uv <= 0) {
        frame_free(nf);
        return;
    }

    for (int row = 0; row < h; row++) {
        memcpy(nf->y + (size_t)row * (size_t)w, src_y + (size_t)row * (size_t)ls_y, (size_t)w);
    }
    for (int row = 0; row < h / 2; row++) {
        memcpy(nf->uv + (size_t)row * (size_t)w, src_uv + (size_t)row * (size_t)ls_uv, (size_t)w);
    }

    nf->stream_id = stream_id;
    nf->width = w;
    nf->height = h;
    nf->pts = frame->pts;
    nf->key_frame = frame->key_frame ? 1 : 0;
    nf->mono_ns = monotonic_ns();
    nf->y_sz = y_sz;
    nf->uv_sz = uv_sz;
    __atomic_store_n(&nf->refcnt, 1, __ATOMIC_RELAXED); /* cache hold */

    pthread_mutex_lock(&g_latest_mu[stream_id]);
    ZmqFrame* old = g_latest[stream_id];
    g_latest[stream_id] = nf;
    pthread_mutex_unlock(&g_latest_mu[stream_id]);
    if (old) frame_release(old);
}

typedef struct {
    StreamManager* mgr;
    char bind_addr[128];
    volatile int* running;

    /* startup sync: set by thread after bind attempt */
    pthread_mutex_t start_mu;
    pthread_cond_t start_cv;
    int start_done;
    int start_ok;
} ZmqBridgeArg;

static void* zmq_bridge_thread(void* p) {
    ZmqBridgeArg* arg = (ZmqBridgeArg*)p;

    void* ctx = zmq_ctx_new();
    if (!ctx) {
        fprintf(stderr, "[ZMQ] zmq_ctx_new failed\n");
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
        return NULL;
    }

    void* router = zmq_socket(ctx, ZMQ_ROUTER);
    if (!router) {
        fprintf(stderr, "[ZMQ] zmq_socket(ROUTER) failed\n");
        zmq_ctx_term(ctx);
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
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
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
        return NULL;
    }

    /* notify start ok */
    pthread_mutex_lock(&arg->start_mu);
    arg->start_ok = 1;
    arg->start_done = 1;
    pthread_cond_signal(&arg->start_cv);
    pthread_mutex_unlock(&arg->start_mu);

    fprintf(stdout, "[ZMQ] bridge enabled (ROUTER) bind=%s\n", arg->bind_addr);
    __atomic_store_n(&g_enabled, 1, __ATOMIC_RELAXED);

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
        int timeout_ms = parse_timeout_ms_from_json(json, json_len, 1000);
        StreamContext* stream = stream_manager_get(arg->mgr, (uint16_t)stream_id);

        if (cmd_len == 4 && memcmp(cmd, "PING", 4) == 0) {
            (void)send_frame(router, "OK", 2, 1);
            (void)send_frame(router, "{}", 2, 0);
        } else if ((cmd_len == strlen("GET_LATEST_NV12") && memcmp(cmd, "GET_LATEST_NV12", cmd_len) == 0) ||
                   (cmd_len == strlen("GET_SHM_NV12") && memcmp(cmd, "GET_SHM_NV12", cmd_len) == 0)) {
            if (!stream) {
                (void)send_frame(router, "ERR", 3, 1);
                (void)send_frame(router, "bad stream_id", strlen("bad stream_id"), 0);
            } else {
                /* Mark this stream as requested so decode thread starts caching it. */
                if (stream_id >= 1 && stream_id <= MAX_STREAMS) {
                    __atomic_store_n(&g_want_stream[stream_id], 1, __ATOMIC_RELAXED);
                }

                ZmqFrame* f = cache_get_hold((uint16_t)stream_id);
                if (!f) {
                    /* If cache is empty, build once from current last_frame so first request can succeed. */
                    ZmqFrame* nf = build_frame_from_last_frame((uint16_t)stream_id, stream);
                    if (nf) {
                        cache_put((uint16_t)stream_id, nf);
                    }
                    f = cache_get_hold((uint16_t)stream_id);
                }

                if (!f && timeout_ms != 0) {
                    const uint64_t deadline = monotonic_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
                    while (timeout_ms < 0 || monotonic_ns() < deadline) {
                        /* small sleep to avoid busy loop */
                        struct timespec ts;
                        ts.tv_sec = 0;
                        ts.tv_nsec = 1000000; /* 1ms */
                        nanosleep(&ts, NULL);

                        f = cache_get_hold((uint16_t)stream_id);
                        if (f) break;

                        ZmqFrame* nf = build_frame_from_last_frame((uint16_t)stream_id, stream);
                        if (nf) {
                            cache_put((uint16_t)stream_id, nf);
                        }
                        f = cache_get_hold((uint16_t)stream_id);
                        if (f) break;
                    }
                }

                if (!f) {
                    (void)send_frame(router, "ERR", 3, 1);
                    (void)send_frame(router, "no frame yet", strlen("no frame yet"), 0);
                } else {
                    /* Hold refs for y+uv messages */
                    frame_addref(f, 2);

                    char meta[256];
                    int meta_n = snprintf(meta, sizeof(meta),
                                          "{\"stream_id\":%d,\"width\":%d,\"height\":%d,\"pts\":%lld,\"mono_ns\":%llu,\"key_frame\":%d}",
                                          (int)f->stream_id, f->width, f->height,
                                          (long long)f->pts, (unsigned long long)f->mono_ns, f->key_frame);
                    if (meta_n < 0) meta_n = 0;
                    if ((size_t)meta_n >= sizeof(meta)) meta_n = (int)sizeof(meta) - 1;

                    zmq_msg_t ymsg;
                    zmq_msg_t uvmsg;
                    if (zmq_msg_init_data(&ymsg, f->y, f->y_sz, zmq_frame_release_cb, f) != 0 ||
                        zmq_msg_init_data(&uvmsg, f->uv, f->uv_sz, zmq_frame_release_cb, f) != 0) {
                        /* init_data failed; drop send refs */
                        frame_release(f);
                        frame_release(f);
                        (void)send_frame(router, "ERR", 3, 1);
                        (void)send_frame(router, "zmq msg init failed", strlen("zmq msg init failed"), 0);
                    } else {
                        (void)send_frame(router, "OK", 2, 1);
                        (void)send_frame(router, meta, (size_t)meta_n, 1);
                        (void)send_msg_and_close(router, &ymsg, 1);
                        (void)send_msg_and_close(router, &uvmsg, 0);
                    }

                    /* drop request hold */
                    frame_release(f);
                }
            }
        } else {
            (void)send_frame(router, "ERR", 3, 1);
            (void)send_frame(router, "unknown cmd", strlen("unknown cmd"), 0);
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

    pthread_mutex_init(&arg->start_mu, NULL);
    pthread_cond_init(&arg->start_cv, NULL);
    arg->start_done = 0;
    arg->start_ok = 0;

    pthread_t th;
    if (pthread_create(&th, NULL, zmq_bridge_thread, arg) != 0) {
        pthread_cond_destroy(&arg->start_cv);
        pthread_mutex_destroy(&arg->start_mu);
        free(arg);
        return -1;
    }

    /* wait for bind result */
    pthread_mutex_lock(&arg->start_mu);
    while (!arg->start_done) {
        pthread_cond_wait(&arg->start_cv, &arg->start_mu);
    }
    int ok = arg->start_ok;
    pthread_mutex_unlock(&arg->start_mu);

    /* start sync no longer used after this point */
    pthread_cond_destroy(&arg->start_cv);
    pthread_mutex_destroy(&arg->start_mu);

    if (!ok) {
        /* thread will exit quickly on failure; join to avoid leaks */
        pthread_join(th, NULL);
        free(arg);
        return -1;
    }

    pthread_detach(th);
    return 0;
}

#else /* HAVE_ZMQ */

/* Build without libzmq: provide no-op symbols so core stream can link. */

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, volatile int* running_flag) {
    (void)mgr;
    (void)bind_addr;
    (void)running_flag;
    fprintf(stderr, "[ZMQ] bridge not built (HAVE_ZMQ=0)\n");
    return -1;
}

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    (void)stream_id;
    (void)frame;
}

void zmq_bridge_shutdown(void) {
}

#endif /* HAVE_ZMQ */

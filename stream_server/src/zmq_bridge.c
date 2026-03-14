/**
 * @file zmq_bridge.c
 * @brief stream_server 内置 ZMQ bridge（请求时从 last_frame 转 BGR24）。
 */

#define _POSIX_C_SOURCE 200809L

#include "zmq_bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(HAVE_ZMQ) && defined(HAVE_LIBYUV)

#include <zmq.h>
#include <libyuv/convert_argb.h>

#define STREAM_COLORSPACE_REC_601 0u
#define STREAM_COLORSPACE_REC_709 1u
#define STREAM_COLOR_RANGE_LIMITED 0u
#define STREAM_COLOR_RANGE_FULL 1u

typedef struct {
    uint16_t stream_id;
    int width;
    int height;
    int stride;
    int64_t pts;
    int key_frame;
    uint64_t mono_ns;
    uint32_t color_space;
    uint32_t color_range;
    uint8_t* bgr;
    size_t bgr_sz;
} ZmqBgrFrame;

typedef struct {
    StreamManager* mgr;
    char bind_addr[128];
    volatile int* running;

    pthread_mutex_t start_mu;
    pthread_cond_t start_cv;
    int start_done;
    int start_ok;
} ZmqBridgeArg;

static const struct YuvConstants* bgr_yuv_constants_from_info(const StreamInfo* info) {
    uint32_t color_space = info ? info->color_space : STREAM_COLORSPACE_REC_709;
    uint32_t color_range = info ? info->color_range : STREAM_COLOR_RANGE_LIMITED;

    if (color_space == STREAM_COLORSPACE_REC_601) {
        return (color_range == STREAM_COLOR_RANGE_FULL)
             ? &kYvuJPEGConstants
             : &kYvuI601Constants;
    }

    return (color_range == STREAM_COLOR_RANGE_FULL)
         ? &kYvuF709Constants
         : &kYvuH709Constants;
}

static const char* color_space_name(uint32_t color_space) {
    switch (color_space) {
        case STREAM_COLORSPACE_REC_601: return "bt601";
        case STREAM_COLORSPACE_REC_709: return "bt709";
        default: return "unknown";
    }
}

static const char* color_range_name(uint32_t color_range) {
    switch (color_range) {
        case STREAM_COLOR_RANGE_LIMITED: return "limited";
        case STREAM_COLOR_RANGE_FULL: return "full";
        default: return "unknown";
    }
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

static int recv_frame(void* sock, zmq_msg_t* out) {
    zmq_msg_init(out);
    if (zmq_msg_recv(out, sock, 0) < 0) {
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

static void free_bgr_frame(ZmqBgrFrame* f) {
    if (!f) return;
    free(f->bgr);
    free(f);
}

static int convert_last_frame_to_bgr(StreamContext* stream, ZmqBgrFrame** out) {
    if (!stream || !out) return -1;

    *out = NULL;

    pthread_mutex_lock(&stream->lock);
    DecodedFrame* lf = stream->last_frame;
    if (!lf || lf->width <= 0 || lf->height <= 0) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }

    ZmqBgrFrame* frame = (ZmqBgrFrame*)calloc(1, sizeof(*frame));
    if (!frame) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }

    frame->stream_id = stream->stream_id;
    frame->width = lf->width;
    frame->height = lf->height;
    frame->stride = lf->width * 3;
    frame->pts = lf->pts;
    frame->key_frame = lf->key_frame ? 1 : 0;
    frame->mono_ns = monotonic_ns();
    frame->color_space = stream->info.color_space;
    frame->color_range = stream->info.color_range;
    frame->bgr_sz = (size_t)frame->stride * (size_t)frame->height;
    frame->bgr = (uint8_t*)malloc(frame->bgr_sz);
    if (!frame->bgr) {
        pthread_mutex_unlock(&stream->lock);
        free(frame);
        return -1;
    }

    const struct YuvConstants* yuv_constants = bgr_yuv_constants_from_info(&stream->info);
    int ret = -1;
    switch (lf->format) {
        case DECODE_FMT_NV12:
            ret = NV12ToRGB24Matrix(
                lf->data[0], lf->linesize[0],
                lf->data[1], lf->linesize[1],
                frame->bgr, frame->stride,
                yuv_constants,
                frame->width, frame->height);
            break;

        case DECODE_FMT_YUV420P:
            ret = I420ToRGB24Matrix(
                lf->data[0], lf->linesize[0],
                lf->data[1], lf->linesize[1],
                lf->data[2], lf->linesize[2],
                frame->bgr, frame->stride,
                yuv_constants,
                frame->width, frame->height);
            break;

        case DECODE_FMT_YUV444P:
            ret = I444ToRGB24Matrix(
                lf->data[0], lf->linesize[0],
                lf->data[1], lf->linesize[1],
                lf->data[2], lf->linesize[2],
                frame->bgr, frame->stride,
                yuv_constants,
                frame->width, frame->height);
            break;

        default:
            ret = -1;
            break;
    }

    pthread_mutex_unlock(&stream->lock);

    if (ret != 0) {
        free_bgr_frame(frame);
        return -1;
    }

    static int color_log_count = 0;
    if (color_log_count++ < 4) {
        printf("[ZMQ] stream %u BGR conversion using %s/%s (fmt=%d)\n",
               stream->stream_id,
               color_space_name(stream->info.color_space),
               color_range_name(stream->info.color_range),
               lf->format);
    }

    *out = frame;
    return 0;
}

static void* zmq_bridge_thread(void* p) {
    ZmqBridgeArg* arg = (ZmqBridgeArg*)p;

    void* ctx = zmq_ctx_new();
    if (!ctx) {
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
        return NULL;
    }

    void* router = zmq_socket(ctx, ZMQ_ROUTER);
    if (!router) {
        zmq_ctx_term(ctx);
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
        return NULL;
    }

    int sndhwm = 2;
    int rcvhwm = 100;
    zmq_setsockopt(router, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
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

    pthread_mutex_lock(&arg->start_mu);
    arg->start_ok = 1;
    arg->start_done = 1;
    pthread_cond_signal(&arg->start_cv);
    pthread_mutex_unlock(&arg->start_mu);

    fprintf(stdout, "[ZMQ] bridge enabled (ROUTER) bind=%s, output=bgr24\n", arg->bind_addr);

    while (!arg->running || *arg->running) {
        zmq_msg_t f0;
        if (recv_frame(router, &f0) < 0) {
            if (errno == EINTR) continue;
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

        if (zmq_msg_size(&f1) == 0) {
            have_delim = 1;
            zmq_msg_close(&f1);
            if (recv_frame(router, &cmd_msg) < 0) {
                zmq_msg_close(&f0);
                continue;
            }
        } else {
            cmd_msg = f1;
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

        (void)send_frame(router, zmq_msg_data(&f0), zmq_msg_size(&f0), 1);
        if (have_delim) {
            (void)send_frame(router, "", 0, 1);
        }

        if (cmd_len == 4 && memcmp(cmd, "PING", 4) == 0) {
            (void)send_frame(router, "OK", 2, 1);
            (void)send_frame(router, "{}", 2, 0);
        } else if (cmd_len == strlen("GET_LATEST_BGR") &&
                   memcmp(cmd, "GET_LATEST_BGR", cmd_len) == 0) {
            int stream_id = parse_stream_id_from_json(json, json_len);
            int timeout_ms = parse_timeout_ms_from_json(json, json_len, 1000);
            StreamContext* stream = stream_manager_get(arg->mgr, (uint16_t)stream_id);
            if (!stream) {
                (void)send_frame(router, "ERR", 3, 1);
                (void)send_frame(router, "bad stream_id", strlen("bad stream_id"), 0);
            } else {
                ZmqBgrFrame* frame = NULL;
                if (convert_last_frame_to_bgr(stream, &frame) != 0 && timeout_ms != 0) {
                    const uint64_t deadline =
                        monotonic_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
                    while (timeout_ms < 0 || monotonic_ns() < deadline) {
                        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                        nanosleep(&ts, NULL);
                        if (convert_last_frame_to_bgr(stream, &frame) == 0) {
                            break;
                        }
                    }
                }

                if (!frame) {
                    (void)send_frame(router, "ERR", 3, 1);
                    (void)send_frame(router, "no frame yet", strlen("no frame yet"), 0);
                } else {
                    char meta[256];
                    int meta_n = snprintf(
                        meta, sizeof(meta),
                        "{\"stream_id\":%d,\"width\":%d,\"height\":%d,\"stride\":%d,"
                        "\"pts\":%lld,\"mono_ns\":%llu,\"key_frame\":%d,\"pixfmt\":\"bgr24\","
                        "\"color_space\":\"%s#%u\",\"color_range\":\"%s#%u\"}",
                        (int)frame->stream_id, frame->width, frame->height, frame->stride,
                        (long long)frame->pts, (unsigned long long)frame->mono_ns, frame->key_frame,
                        color_space_name(frame->color_space), frame->color_space,
                        color_range_name(frame->color_range), frame->color_range);
                    if (meta_n < 0) meta_n = 0;
                    if ((size_t)meta_n >= sizeof(meta)) meta_n = (int)sizeof(meta) - 1;

                    (void)send_frame(router, "OK", 2, 1);
                    (void)send_frame(router, meta, (size_t)meta_n, 1);
                    (void)send_frame(router, frame->bgr, frame->bgr_sz, 0);
                    free_bgr_frame(frame);
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

    pthread_t th;
    if (pthread_create(&th, NULL, zmq_bridge_thread, arg) != 0) {
        pthread_cond_destroy(&arg->start_cv);
        pthread_mutex_destroy(&arg->start_mu);
        free(arg);
        return -1;
    }

    pthread_mutex_lock(&arg->start_mu);
    while (!arg->start_done) {
        pthread_cond_wait(&arg->start_cv, &arg->start_mu);
    }
    int ok = arg->start_ok;
    pthread_mutex_unlock(&arg->start_mu);

    pthread_cond_destroy(&arg->start_cv);
    pthread_mutex_destroy(&arg->start_mu);

    if (!ok) {
        pthread_join(th, NULL);
        free(arg);
        return -1;
    }

    pthread_detach(th);
    return 0;
}

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    (void)stream_id;
    (void)frame;
}

void zmq_bridge_shutdown(void) {
}

#else

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, volatile int* running_flag) {
    (void)mgr;
    (void)bind_addr;
    (void)running_flag;
    fprintf(stderr, "[ZMQ] bridge not built (need libzmq + libyuv)\n");
    return -1;
}

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    (void)stream_id;
    (void)frame;
}

void zmq_bridge_shutdown(void) {
}

#endif

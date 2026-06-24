/**
 * @file zmq_bridge.c
 * @brief stream_server 内置 ZMQ bridge（请求时从 last_frame 转 BGR24）。
 */

#define _POSIX_C_SOURCE 200809L

#include "zmq_bridge.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_ZMQ

#include <zmq.h>

#define STREAM_COLORSPACE_REC_601 0u
#define STREAM_COLORSPACE_REC_709 1u
#define STREAM_COLOR_RANGE_LIMITED 0u
#define STREAM_COLOR_RANGE_FULL 1u

typedef struct {
    atomic_uint refcount;
    uint16_t stream_id;
    int width;
    int height;
    int stride;
    int source_width;
    int source_height;
    int roi_x;
    int roi_y;
    int64_t pts;
    int key_frame;
    uint64_t mono_ns;
    uint32_t color_space;
    uint32_t color_range;
    DecodeFormat source_format;
    uint8_t* bgr;
    size_t bgr_sz;
} ZmqBgrFrame;

typedef struct {
    int requested;
    int x;
    int y;
    int w;
    int h;
} ZmqRoiRequest;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int source_width;
    int source_height;
    int applied;
} ZmqResolvedRoi;

#define ZMQ_BRIDGE_WORKER_COUNT 4
#define ZMQ_BRIDGE_QUEUE_CAP 128
#define ZMQ_BRIDGE_RESULT_CAP 128
#define ZMQ_BRIDGE_DEFAULT_CLIENT_TIMEOUT_MS 1000
#define ZMQ_BRIDGE_DEFAULT_SERVER_WAIT_MS 30
#define ZMQ_BRIDGE_MAX_SERVER_WAIT_MS 50
#define ZMQ_BRIDGE_DEADLINE_MARGIN_MS 100
#define ZMQ_BRIDGE_MAX_SEND_DEADLINE_MS 800

typedef struct ZmqBridgeRequest {
    uint8_t* identity;
    size_t identity_len;
    int have_delim;
    char* cmd;
    size_t cmd_len;
    char* json;
    size_t json_len;
    char request_id[65];
    int stream_id;
    int client_timeout_ms;
    int server_wait_ms;
    uint64_t send_deadline_ns;
    ZmqRoiRequest roi_req;
    int roi_parse;
} ZmqBridgeRequest;

typedef struct ZmqBridgeResponse {
    uint8_t* identity;
    size_t identity_len;
    int have_delim;
    int ok;
    char* meta;
    size_t meta_len;
    char* payload;
    size_t payload_len;
    ZmqBgrFrame* frame;
    uint64_t send_deadline_ns;
} ZmqBridgeResponse;

typedef struct {
    ZmqBridgeRequest* items[ZMQ_BRIDGE_QUEUE_CAP];
    size_t head;
    size_t tail;
    size_t count;
    int stopped;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} ZmqRequestQueue;

typedef struct {
    ZmqBridgeResponse* items[ZMQ_BRIDGE_RESULT_CAP];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mu;
} ZmqResponseQueue;

typedef struct {
    StreamManager* mgr;
    char bind_addr[256];
    char ipc_bind_addr[256];
    volatile int* running;
    void* zmq_ctx;
    ZmqRequestQueue requests;
    ZmqResponseQueue responses;
    pthread_t workers[ZMQ_BRIDGE_WORKER_COUNT];
    int worker_count;
    int workers_started;

    pthread_mutex_t start_mu;
    pthread_cond_t start_cv;
    int start_done;
    int start_ok;
} ZmqBridgeArg;

static pthread_mutex_t g_zmq_bridge_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_zmq_bridge_thread;
static ZmqBridgeArg* g_zmq_bridge_arg = NULL;
static int g_zmq_bridge_started = 0;

static void free_bgr_frame(ZmqBgrFrame* f);
static void free_bridge_request(ZmqBridgeRequest* req);
static void free_bridge_response(ZmqBridgeResponse* resp);

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

static int has_bind_addr(const char* addr) {
    return addr && addr[0] != '\0';
}

static const char* ipc_path_from_bind_addr(const char* addr) {
    if (!has_bind_addr(addr) || strncmp(addr, "ipc://", 6) != 0) {
        return NULL;
    }
    return addr + 6;
}

static void cleanup_ipc_bind_addr(const char* addr) {
    const char* path = ipc_path_from_bind_addr(addr);
    if (!path || !*path) {
        return;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "[ZMQ] ipc cleanup failed: %s (%s)\n", path, strerror(errno));
    }
}

static int bind_router_endpoint(void* router, const char* addr) {
    if (!has_bind_addr(addr)) {
        return 0;
    }

    cleanup_ipc_bind_addr(addr);
    if (zmq_bind(router, addr) == 0) {
        return 0;
    }

    fprintf(stderr, "[ZMQ] bind failed: %s (%s)\n", addr, zmq_strerror(errno));
    cleanup_ipc_bind_addr(addr);
    return -1;
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

static int send_bgr_frame_owned(void* sock, ZmqBgrFrame* frame) {
    if (!sock || !frame || !frame->bgr) {
        free_bgr_frame(frame);
        return -1;
    }

    const int rc = send_frame(sock, frame->bgr, frame->bgr_sz, 0);
    free_bgr_frame(frame);
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

static int is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char* find_json_key(const char* json, const char* end, const char* key) {
    const size_t klen = strlen(key);
    if (!json || !end || !key || klen == 0) {
        return NULL;
    }
    for (const char* p = json; p + klen <= end; p++) {
        if (memcmp(p, key, klen) == 0) {
            return p;
        }
    }
    return NULL;
}

static const char* skip_json_ws(const char* p, const char* end) {
    while (p < end && is_json_ws(*p)) {
        p++;
    }
    return p;
}

static int parse_json_int_key(const char* json, size_t len, const char* key, int* out) {
    if (!json || len == 0 || !key || !out) {
        return 0;
    }

    const char* end = json + len;
    const char* p = find_json_key(json, end, key);
    if (!p) {
        return 0;
    }

    p += strlen(key);
    p = skip_json_ws(p, end);
    if (p >= end || *p != ':') {
        return 0;
    }
    p++;
    p = skip_json_ws(p, end);

    int sign = 1;
    if (p < end && *p == '-') {
        sign = -1;
        p++;
    }

    int any = 0;
    long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        any = 1;
        v = v * 10 + (*p - '0');
        if (v > (long long)INT_MAX + 1ll) {
            return 0;
        }
        p++;
    }
    if (!any) {
        return 0;
    }

    long long signed_v = sign > 0 ? v : -v;
    if (signed_v < INT_MIN || signed_v > INT_MAX) {
        return 0;
    }
    *out = (int)signed_v;
    return 1;
}

static int parse_json_string_key(const char* json, size_t len, const char* key,
                                 char* out, size_t out_sz) {
    if (!json || len == 0 || !key || !out || out_sz == 0) {
        return 0;
    }

    out[0] = '\0';
    const char* end = json + len;
    const char* p = find_json_key(json, end, key);
    if (!p) {
        return 0;
    }

    p += strlen(key);
    p = skip_json_ws(p, end);
    if (p >= end || *p != ':') {
        return 0;
    }
    p++;
    p = skip_json_ws(p, end);
    if (p >= end || *p != '"') {
        return 0;
    }
    p++;

    size_t n = 0;
    int escaped = 0;
    while (p < end) {
        char c = *p++;
        if (escaped) {
            escaped = 0;
        } else if (c == '\\') {
            escaped = 1;
            continue;
        } else if (c == '"') {
            out[n] = '\0';
            return 1;
        }
        if (n + 1 < out_sz) {
            out[n++] = c;
        }
    }

    out[n] = '\0';
    return n > 0;
}

static int parse_stream_id_from_json(const char* json, size_t len) {
    int v = 0;
    if (parse_json_int_key(json, len, "\"stream_id\"", &v) && v > 0) {
        return v;
    }
    return 1;
}

static int parse_timeout_ms_from_json(const char* json, size_t len, int default_ms) {
    int v = 0;
    if (parse_json_int_key(json, len, "\"timeout_ms\"", &v)) {
        return v;
    }
    return default_ms;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int parse_client_timeout_ms_from_json(const char* json, size_t len) {
    int v = 0;
    if (parse_json_int_key(json, len, "\"client_timeout_ms\"", &v) && v > 0) {
        return v;
    }
    return parse_timeout_ms_from_json(json, len, ZMQ_BRIDGE_DEFAULT_CLIENT_TIMEOUT_MS);
}

static int parse_server_wait_ms_from_json(const char* json, size_t len) {
    int v = 0;
    if (parse_json_int_key(json, len, "\"server_wait_ms\"", &v)) {
        return clamp_int(v, 0, ZMQ_BRIDGE_MAX_SERVER_WAIT_MS);
    }
    return clamp_int(parse_timeout_ms_from_json(json, len, ZMQ_BRIDGE_DEFAULT_SERVER_WAIT_MS),
                     0, ZMQ_BRIDGE_MAX_SERVER_WAIT_MS);
}

static int parse_roi_from_json(const char* json, size_t len, ZmqRoiRequest* roi) {
    if (!roi) {
        return -1;
    }
    memset(roi, 0, sizeof(*roi));

    if (!json || len == 0) {
        return 0;
    }

    const char* end = json + len;
    const char* p = find_json_key(json, end, "\"roi\"");
    if (!p) {
        return 0;
    }

    p += strlen("\"roi\"");
    p = skip_json_ws(p, end);
    if (p >= end || *p != ':') {
        return -1;
    }
    p++;
    p = skip_json_ws(p, end);

    if (p + 4 <= end && memcmp(p, "null", 4) == 0) {
        return 0;
    }
    if (p >= end || *p != '{') {
        return -1;
    }

    const char* obj_start = p + 1;
    const char* obj_end = NULL;
    int depth = 1;
    int in_string = 0;
    int escaped = 0;
    for (const char* q = p + 1; q < end; q++) {
        char c = *q;
        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }

        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                obj_end = q;
                break;
            }
        }
    }
    if (!obj_end || obj_end < obj_start) {
        return -1;
    }

    const size_t obj_len = (size_t)(obj_end - obj_start);
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (!parse_json_int_key(obj_start, obj_len, "\"x\"", &x) ||
        !parse_json_int_key(obj_start, obj_len, "\"y\"", &y)) {
        return -1;
    }
    if (!parse_json_int_key(obj_start, obj_len, "\"w\"", &w) &&
        !parse_json_int_key(obj_start, obj_len, "\"width\"", &w)) {
        return -1;
    }
    if (!parse_json_int_key(obj_start, obj_len, "\"h\"", &h) &&
        !parse_json_int_key(obj_start, obj_len, "\"height\"", &h)) {
        return -1;
    }

    roi->requested = 1;
    roi->x = x;
    roi->y = y;
    roi->w = w;
    roi->h = h;
    return 1;
}

static void free_bgr_frame(ZmqBgrFrame* f) {
    if (!f) return;
    if (atomic_fetch_sub_explicit(&f->refcount, 1u, memory_order_acq_rel) != 1u) {
        return;
    }
    free(f->bgr);
    free(f);
}

static int convert_last_frame_to_bgr(StreamContext* stream, ZmqBridgeArg* arg, ZmqBgrFrame** out) {
    if (!stream || !out) return -1;
    (void)arg;

    *out = NULL;

    DecodedFrame* snapshot = NULL;
    DecodedFrame converted = {0};
    StreamInfo info_snapshot;
    uint16_t stream_id = 0;
    DecodeFormat source_format = DECODE_FMT_NONE;

    pthread_mutex_lock(&stream->lock);
    DecodedFrame* lf = stream->last_frame;
    if (!lf || lf->width <= 0 || lf->height <= 0) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }
    snapshot = decoder_ref_frame(lf);
    if (!snapshot) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }
    info_snapshot = stream->info;
    stream_id = stream->stream_id;
    source_format = snapshot->format;

    /*
     * Keep conversion serialized per stream. A burst can otherwise make several
     * workers download/convert references to the same VAAPI frame concurrently.
     */
    if (decoder_convert_format_with_info(NULL, snapshot, &info_snapshot,
                                         &converted, DECODE_FMT_BGR24) != 0) {
        decoder_free_frame(snapshot);
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }
    decoder_free_frame(snapshot);
    pthread_mutex_unlock(&stream->lock);

    ZmqBgrFrame* frame = (ZmqBgrFrame*)calloc(1, sizeof(*frame));
    if (!frame) {
        free(converted.data[0]);
        return -1;
    }

    frame->stream_id = stream_id;
    frame->width = converted.width;
    frame->height = converted.height;
    frame->stride = converted.linesize[0];
    frame->source_width = converted.width;
    frame->source_height = converted.height;
    frame->roi_x = 0;
    frame->roi_y = 0;
    frame->pts = converted.pts;
    frame->key_frame = converted.key_frame ? 1 : 0;
    frame->mono_ns = monotonic_ns();
    frame->color_space = info_snapshot.color_space;
    frame->color_range = info_snapshot.color_range;
    frame->source_format = source_format;
    frame->bgr_sz = (size_t)converted.linesize[0] * (size_t)converted.height;
    frame->bgr = converted.data[0];
    atomic_init(&frame->refcount, 1u);
    converted.data[0] = NULL;
    if (!frame->bgr) {
        free(frame);
        return -1;
    }

    static atomic_int color_log_count = 0;
    if (atomic_fetch_add_explicit(&color_log_count, 1, memory_order_relaxed) < 4) {
        printf("[ZMQ] stream %u BGR conversion using %s/%s via unified decoder path\n",
               stream_id,
               color_space_name(info_snapshot.color_space),
               color_range_name(info_snapshot.color_range));
    }

    *out = frame;
    return 0;
}

static int resolve_bgr_roi(const ZmqBgrFrame* frame, const ZmqRoiRequest* req,
                           ZmqResolvedRoi* roi) {
    if (!frame || !roi || frame->width <= 0 || frame->height <= 0 ||
        frame->stride < frame->width * 3) {
        return -1;
    }

    roi->source_width = frame->width;
    roi->source_height = frame->height;

    if (!req || !req->requested) {
        roi->x = 0;
        roi->y = 0;
        roi->w = frame->width;
        roi->h = frame->height;
        roi->applied = 0;
        return 0;
    }

    if (req->w <= 0 || req->h <= 0) {
        return -1;
    }

    int64_t x = req->x;
    int64_t y = req->y;
    int64_t right = x + req->w;
    int64_t bottom = y + req->h;

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (right > frame->width) {
        right = frame->width;
    }
    if (bottom > frame->height) {
        bottom = frame->height;
    }

    if (right <= x || bottom <= y) {
        return -1;
    }

    roi->x = (int)x;
    roi->y = (int)y;
    roi->w = (int)(right - x);
    roi->h = (int)(bottom - y);
    roi->applied = !(roi->x == 0 && roi->y == 0 &&
                     roi->w == frame->width && roi->h == frame->height);
    return 0;
}

static int crop_bgr_frame(const ZmqBgrFrame* src, const ZmqResolvedRoi* roi,
                          ZmqBgrFrame** out) {
    if (!src || !roi || !out || !src->bgr || roi->w <= 0 || roi->h <= 0 ||
        src->stride < src->width * 3) {
        return -1;
    }

    *out = NULL;
    const int dst_stride = roi->w * 3;
    const size_t bgr_sz = (size_t)dst_stride * (size_t)roi->h;
    uint8_t* bgr = (uint8_t*)malloc(bgr_sz);
    if (!bgr) {
        return -1;
    }

    for (int row = 0; row < roi->h; row++) {
        const uint8_t* src_row = src->bgr +
            (size_t)(roi->y + row) * (size_t)src->stride +
            (size_t)roi->x * 3u;
        uint8_t* dst_row = bgr + (size_t)row * (size_t)dst_stride;
        memcpy(dst_row, src_row, (size_t)dst_stride);
    }

    ZmqBgrFrame* dst = (ZmqBgrFrame*)calloc(1, sizeof(*dst));
    if (!dst) {
        free(bgr);
        return -1;
    }

    atomic_init(&dst->refcount, 1u);
    dst->stream_id = src->stream_id;
    dst->width = roi->w;
    dst->height = roi->h;
    dst->stride = dst_stride;
    dst->source_width = roi->source_width;
    dst->source_height = roi->source_height;
    dst->roi_x = roi->x;
    dst->roi_y = roi->y;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    dst->mono_ns = src->mono_ns;
    dst->color_space = src->color_space;
    dst->color_range = src->color_range;
    dst->source_format = src->source_format;
    dst->bgr = bgr;
    dst->bgr_sz = bgr_sz;
    *out = dst;
    return 0;
}

static char* copy_bytes_as_string(const void* data, size_t len) {
    char* out = (char*)malloc(len + 1u);
    if (!out) {
        return NULL;
    }
    if (len > 0 && data) {
        memcpy(out, data, len);
    }
    out[len] = '\0';
    return out;
}

static uint8_t* copy_bytes(const void* data, size_t len) {
    if (len == 0) {
        return NULL;
    }
    uint8_t* out = (uint8_t*)malloc(len);
    if (!out) {
        return NULL;
    }
    memcpy(out, data, len);
    return out;
}

static int copy_msg_bytes(zmq_msg_t* msg, uint8_t** out, size_t* out_len) {
    if (!msg || !out || !out_len) {
        return -1;
    }
    *out_len = zmq_msg_size(msg);
    *out = copy_bytes(zmq_msg_data(msg), *out_len);
    if (*out_len > 0 && !*out) {
        return -1;
    }
    return 0;
}

static void request_queue_init(ZmqRequestQueue* q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static void response_queue_init(ZmqResponseQueue* q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
}

static int request_queue_push(ZmqRequestQueue* q, ZmqBridgeRequest* req) {
    int ok = 0;
    pthread_mutex_lock(&q->mu);
    if (!q->stopped && q->count < ZMQ_BRIDGE_QUEUE_CAP) {
        q->items[q->tail] = req;
        q->tail = (q->tail + 1u) % ZMQ_BRIDGE_QUEUE_CAP;
        q->count++;
        pthread_cond_signal(&q->cv);
        ok = 1;
    }
    pthread_mutex_unlock(&q->mu);
    return ok ? 0 : -1;
}

static ZmqBridgeRequest* request_queue_pop(ZmqRequestQueue* q) {
    pthread_mutex_lock(&q->mu);
    while (!q->stopped && q->count == 0) {
        pthread_cond_wait(&q->cv, &q->mu);
    }
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return NULL;
    }
    ZmqBridgeRequest* req = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1u) % ZMQ_BRIDGE_QUEUE_CAP;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return req;
}

static void request_queue_stop(ZmqRequestQueue* q) {
    pthread_mutex_lock(&q->mu);
    q->stopped = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static int response_queue_push(ZmqResponseQueue* q, ZmqBridgeResponse* resp) {
    int ok = 0;
    pthread_mutex_lock(&q->mu);
    if (q->count < ZMQ_BRIDGE_RESULT_CAP) {
        q->items[q->tail] = resp;
        q->tail = (q->tail + 1u) % ZMQ_BRIDGE_RESULT_CAP;
        q->count++;
        ok = 1;
    }
    pthread_mutex_unlock(&q->mu);
    return ok ? 0 : -1;
}

static ZmqBridgeResponse* response_queue_pop(ZmqResponseQueue* q) {
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return NULL;
    }
    ZmqBridgeResponse* resp = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1u) % ZMQ_BRIDGE_RESULT_CAP;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return resp;
}

static void free_bridge_request(ZmqBridgeRequest* req) {
    if (!req) {
        return;
    }
    free(req->identity);
    free(req->cmd);
    free(req->json);
    free(req);
}

static void free_bridge_response(ZmqBridgeResponse* resp) {
    if (!resp) {
        return;
    }
    free(resp->identity);
    free(resp->meta);
    free(resp->payload);
    if (resp->frame) {
        free_bgr_frame(resp->frame);
    }
    free(resp);
}

static void request_queue_destroy(ZmqRequestQueue* q) {
    request_queue_stop(q);
    ZmqBridgeRequest* req = NULL;
    while ((req = request_queue_pop(q)) != NULL) {
        free_bridge_request(req);
    }
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
}

static void response_queue_destroy(ZmqResponseQueue* q) {
    ZmqBridgeResponse* resp = NULL;
    while ((resp = response_queue_pop(q)) != NULL) {
        free_bridge_response(resp);
    }
    pthread_mutex_destroy(&q->mu);
}

static uint64_t send_deadline_from_timeout(uint64_t recv_ns, int client_timeout_ms) {
    int deadline_ms = client_timeout_ms - ZMQ_BRIDGE_DEADLINE_MARGIN_MS;
    if (deadline_ms <= 0) {
        deadline_ms = client_timeout_ms;
    }
    if (deadline_ms <= 0) {
        deadline_ms = 1;
    }
    if (deadline_ms > ZMQ_BRIDGE_MAX_SEND_DEADLINE_MS) {
        deadline_ms = ZMQ_BRIDGE_MAX_SEND_DEADLINE_MS;
    }
    return recv_ns + (uint64_t)deadline_ms * 1000000ull;
}

static ZmqBridgeRequest* build_bridge_request(zmq_msg_t* identity_msg, int have_delim,
                                              zmq_msg_t* cmd_msg, zmq_msg_t* json_msg) {
    ZmqBridgeRequest* req = (ZmqBridgeRequest*)calloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }
    if (copy_msg_bytes(identity_msg, &req->identity, &req->identity_len) != 0) {
        free_bridge_request(req);
        return NULL;
    }
    req->have_delim = have_delim;
    req->cmd_len = zmq_msg_size(cmd_msg);
    req->cmd = copy_bytes_as_string(zmq_msg_data(cmd_msg), req->cmd_len);
    req->json_len = zmq_msg_size(json_msg);
    req->json = copy_bytes_as_string(zmq_msg_data(json_msg), req->json_len);
    if (!req->cmd || !req->json) {
        free_bridge_request(req);
        return NULL;
    }

    req->stream_id = parse_stream_id_from_json(req->json, req->json_len);
    req->client_timeout_ms = parse_client_timeout_ms_from_json(req->json, req->json_len);
    req->server_wait_ms = parse_server_wait_ms_from_json(req->json, req->json_len);
    (void)parse_json_string_key(req->json, req->json_len, "\"request_id\"",
                                req->request_id, sizeof(req->request_id));
    req->roi_parse = parse_roi_from_json(req->json, req->json_len, &req->roi_req);
    req->send_deadline_ns = send_deadline_from_timeout(monotonic_ns(), req->client_timeout_ms);
    return req;
}

static ZmqBridgeResponse* bridge_response_take_request(ZmqBridgeRequest* req) {
    ZmqBridgeResponse* resp = (ZmqBridgeResponse*)calloc(1, sizeof(*resp));
    if (!resp) {
        return NULL;
    }
    resp->identity = req->identity;
    resp->identity_len = req->identity_len;
    resp->have_delim = req->have_delim;
    resp->send_deadline_ns = req->send_deadline_ns;
    req->identity = NULL;
    req->identity_len = 0;
    return resp;
}

static int response_set_text_payload(ZmqBridgeResponse* resp, const char* text) {
    const size_t len = text ? strlen(text) : 0;
    resp->payload = copy_bytes_as_string(text ? text : "", len);
    if (!resp->payload) {
        return -1;
    }
    resp->payload_len = len;
    return 0;
}

static ZmqBridgeResponse* make_error_response(ZmqBridgeRequest* req, const char* error) {
    ZmqBridgeResponse* resp = bridge_response_take_request(req);
    if (!resp) {
        return NULL;
    }
    resp->ok = 0;
    char meta[512];
    int meta_n = snprintf(meta, sizeof(meta),
                          "{\"request_id\":\"%s\",\"stream_id\":%d,\"error\":\"%s\"}",
                          req->request_id, req->stream_id, error ? error : "error");
    if (meta_n < 0) meta_n = 0;
    if ((size_t)meta_n >= sizeof(meta)) meta_n = (int)sizeof(meta) - 1;
    resp->meta = copy_bytes_as_string(meta, (size_t)meta_n);
    resp->meta_len = (size_t)meta_n;
    if (!resp->meta || response_set_text_payload(resp, error ? error : "error") != 0) {
        free_bridge_response(resp);
        return NULL;
    }
    return resp;
}

static ZmqBridgeResponse* make_ping_response(ZmqBridgeRequest* req) {
    ZmqBridgeResponse* resp = bridge_response_take_request(req);
    if (!resp) {
        return NULL;
    }
    resp->ok = 1;
    resp->meta = copy_bytes_as_string("{}", 2);
    resp->meta_len = 2;
    if (!resp->meta || response_set_text_payload(resp, "{}") != 0) {
        free_bridge_response(resp);
        return NULL;
    }
    return resp;
}

static ZmqBridgeResponse* make_bgr_response(ZmqBridgeRequest* req, ZmqBgrFrame* reply_frame,
                                            const ZmqResolvedRoi* roi) {
    ZmqBridgeResponse* resp = bridge_response_take_request(req);
    if (!resp) {
        free_bgr_frame(reply_frame);
        return NULL;
    }
    resp->ok = 1;
    resp->frame = reply_frame;

    char meta[768];
    int meta_n = snprintf(
        meta, sizeof(meta),
        "{\"request_id\":\"%s\",\"stream_id\":%d,\"width\":%d,\"height\":%d,\"stride\":%d,"
        "\"source_width\":%d,\"source_height\":%d,"
        "\"roi_x\":%d,\"roi_y\":%d,\"roi_width\":%d,\"roi_height\":%d,"
        "\"roi_applied\":%s,"
        "\"pts\":%lld,\"mono_ns\":%llu,\"key_frame\":%d,\"pixfmt\":\"bgr24\","
        "\"color_space\":\"%s#%u\",\"color_range\":\"%s#%u\"}",
        req->request_id,
        (int)reply_frame->stream_id,
        reply_frame->width, reply_frame->height, reply_frame->stride,
        reply_frame->source_width, reply_frame->source_height,
        reply_frame->roi_x, reply_frame->roi_y,
        reply_frame->width, reply_frame->height,
        (roi && roi->applied) ? "true" : "false",
        (long long)reply_frame->pts,
        (unsigned long long)reply_frame->mono_ns,
        reply_frame->key_frame,
        color_space_name(reply_frame->color_space), reply_frame->color_space,
        color_range_name(reply_frame->color_range), reply_frame->color_range);
    if (meta_n < 0) meta_n = 0;
    if ((size_t)meta_n >= sizeof(meta)) meta_n = (int)sizeof(meta) - 1;
    resp->meta = copy_bytes_as_string(meta, (size_t)meta_n);
    resp->meta_len = (size_t)meta_n;
    if (!resp->meta) {
        free_bridge_response(resp);
        return NULL;
    }
    return resp;
}

static ZmqBridgeResponse* handle_get_latest_bgr_request(ZmqBridgeArg* arg, ZmqBridgeRequest* req) {
    if (req->roi_parse < 0) {
        return make_error_response(req, "bad roi");
    }

    StreamContext* stream = stream_manager_get(arg->mgr, (uint16_t)req->stream_id);
    if (!stream) {
        return make_error_response(req, "bad stream_id");
    }

    ZmqBgrFrame* frame = NULL;
    if (convert_last_frame_to_bgr(stream, NULL, &frame) != 0 && req->server_wait_ms > 0) {
        const uint64_t deadline = monotonic_ns() + (uint64_t)req->server_wait_ms * 1000000ull;
        while (monotonic_ns() < deadline) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
            nanosleep(&ts, NULL);
            if (convert_last_frame_to_bgr(stream, NULL, &frame) == 0) {
                break;
            }
        }
    }

    if (!frame) {
        return make_error_response(req, "no frame yet");
    }

    ZmqResolvedRoi roi;
    ZmqBgrFrame* reply_frame = frame;
    if (resolve_bgr_roi(frame, &req->roi_req, &roi) != 0) {
        free_bgr_frame(frame);
        return make_error_response(req, "bad roi");
    }

    if (roi.applied) {
        ZmqBgrFrame* cropped = NULL;
        if (crop_bgr_frame(frame, &roi, &cropped) != 0) {
            free_bgr_frame(frame);
            return make_error_response(req, "roi crop failed");
        }
        free_bgr_frame(frame);
        reply_frame = cropped;
    }

    return make_bgr_response(req, reply_frame, &roi);
}

static void* zmq_bridge_worker_thread(void* p) {
    ZmqBridgeArg* arg = (ZmqBridgeArg*)p;
    for (;;) {
        ZmqBridgeRequest* req = request_queue_pop(&arg->requests);
        if (!req) {
            break;
        }

        ZmqBridgeResponse* resp = handle_get_latest_bgr_request(arg, req);
        free_bridge_request(req);
        if (!resp) {
            continue;
        }
        if (monotonic_ns() > resp->send_deadline_ns ||
            response_queue_push(&arg->responses, resp) != 0) {
            free_bridge_response(resp);
        }
    }
    return NULL;
}

static int start_bridge_workers(ZmqBridgeArg* arg) {
    arg->worker_count = ZMQ_BRIDGE_WORKER_COUNT;
    for (int i = 0; i < arg->worker_count; i++) {
        if (pthread_create(&arg->workers[i], NULL, zmq_bridge_worker_thread, arg) != 0) {
            request_queue_stop(&arg->requests);
            for (int j = 0; j < i; j++) {
                pthread_join(arg->workers[j], NULL);
            }
            arg->worker_count = 0;
            arg->workers_started = 0;
            return -1;
        }
    }
    arg->workers_started = 1;
    return 0;
}

static void stop_bridge_workers(ZmqBridgeArg* arg) {
    request_queue_stop(&arg->requests);
    if (!arg->workers_started) {
        return;
    }
    for (int i = 0; i < arg->worker_count; i++) {
        pthread_join(arg->workers[i], NULL);
    }
    arg->workers_started = 0;
}

static int send_bridge_response(void* router, ZmqBridgeResponse* resp) {
    if (!router || !resp || !resp->identity || !resp->meta) {
        return -1;
    }

    if (send_frame(router, resp->identity, resp->identity_len, 1) != 0) {
        return -1;
    }
    if (resp->have_delim && send_frame(router, "", 0, 1) != 0) {
        return -1;
    }

    const char* status = resp->ok ? "OK" : "ERR";
    if (send_frame(router, status, strlen(status), 1) != 0 ||
        send_frame(router, resp->meta, resp->meta_len, 1) != 0) {
        return -1;
    }

    if (resp->ok && resp->frame) {
        int rc = send_bgr_frame_owned(router, resp->frame);
        resp->frame = NULL;
        return rc;
    }

    return send_frame(router, resp->payload ? resp->payload : "",
                      resp->payload ? resp->payload_len : 0, 0);
}

static void drain_bridge_responses(void* router, ZmqBridgeArg* arg) {
    ZmqBridgeResponse* resp = NULL;
    while ((resp = response_queue_pop(&arg->responses)) != NULL) {
        if (monotonic_ns() <= resp->send_deadline_ns) {
            (void)send_bridge_response(router, resp);
        }
        free_bridge_response(resp);
    }
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
    arg->zmq_ctx = ctx;

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

    int sndhwm = 1;
    int rcvhwm = 1;
    int sndtimeo = 0;
    zmq_setsockopt(router, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
    zmq_setsockopt(router, ZMQ_RCVHWM, &rcvhwm, sizeof(rcvhwm));
    zmq_setsockopt(router, ZMQ_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));

    if (bind_router_endpoint(router, arg->bind_addr) != 0 ||
        bind_router_endpoint(router, arg->ipc_bind_addr) != 0) {
        zmq_close(router);
        zmq_ctx_term(ctx);
        cleanup_ipc_bind_addr(arg->bind_addr);
        cleanup_ipc_bind_addr(arg->ipc_bind_addr);
        pthread_mutex_lock(&arg->start_mu);
        arg->start_ok = 0;
        arg->start_done = 1;
        pthread_cond_signal(&arg->start_cv);
        pthread_mutex_unlock(&arg->start_mu);
        return NULL;
    }

    if (start_bridge_workers(arg) != 0) {
        zmq_close(router);
        zmq_ctx_term(ctx);
        cleanup_ipc_bind_addr(arg->bind_addr);
        cleanup_ipc_bind_addr(arg->ipc_bind_addr);
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

    if (has_bind_addr(arg->bind_addr) && has_bind_addr(arg->ipc_bind_addr)) {
        fprintf(stdout,
                "[ZMQ] bridge enabled (ROUTER) tcp=%s ipc=%s, output=bgr24, workers=%d\n",
                arg->bind_addr, arg->ipc_bind_addr, arg->worker_count);
    } else if (has_bind_addr(arg->bind_addr)) {
        fprintf(stdout, "[ZMQ] bridge enabled (ROUTER) bind=%s, output=bgr24, workers=%d\n",
                arg->bind_addr, arg->worker_count);
    } else {
        fprintf(stdout, "[ZMQ] bridge enabled (ROUTER) bind=%s, output=bgr24, workers=%d\n",
                arg->ipc_bind_addr, arg->worker_count);
    }

    while (!arg->running || *arg->running) {
        drain_bridge_responses(router, arg);

        zmq_pollitem_t items[] = {
            { .socket = router, .fd = 0, .events = ZMQ_POLLIN, .revents = 0 },
        };
        int poll_rc = zmq_poll(items, 1, 10);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_rc == 0 || !(items[0].revents & ZMQ_POLLIN)) {
            continue;
        }

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
        ZmqBridgeRequest* req = build_bridge_request(&f0, have_delim, &cmd_msg, &json_msg);
        ZmqBridgeResponse* direct_resp = NULL;

        if (cmd_len == 4 && memcmp(cmd, "PING", 4) == 0) {
            if (req) {
                direct_resp = make_ping_response(req);
            }
        } else if (cmd_len == strlen("GET_LATEST_BGR") &&
                   memcmp(cmd, "GET_LATEST_BGR", cmd_len) == 0) {
            if (req && request_queue_push(&arg->requests, req) == 0) {
                req = NULL;
            } else if (req) {
                direct_resp = make_error_response(req, "busy");
            }
        } else if (req) {
            direct_resp = make_error_response(req, "unknown cmd");
        }

        if (direct_resp) {
            if (monotonic_ns() <= direct_resp->send_deadline_ns) {
                (void)send_bridge_response(router, direct_resp);
            }
            free_bridge_response(direct_resp);
        }
        if (req) {
            free_bridge_request(req);
        }

        if (!req && !direct_resp && cmd_len != strlen("GET_LATEST_BGR")) {
            /* build_bridge_request failed; nothing reliable can be returned. */
        }

        zmq_msg_close(&f0);
        zmq_msg_close(&cmd_msg);
        zmq_msg_close(&json_msg);
    }

    drain_bridge_responses(router, arg);
    stop_bridge_workers(arg);
    drain_bridge_responses(router, arg);
    zmq_close(router);
    zmq_ctx_term(ctx);
    cleanup_ipc_bind_addr(arg->bind_addr);
    cleanup_ipc_bind_addr(arg->ipc_bind_addr);
    request_queue_destroy(&arg->requests);
    response_queue_destroy(&arg->responses);
    free(arg);
    return NULL;
}

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, const char* ipc_bind_addr,
                     volatile int* running_flag) {
    if (!mgr || (!has_bind_addr(bind_addr) && !has_bind_addr(ipc_bind_addr))) return -1;

    pthread_mutex_lock(&g_zmq_bridge_mu);
    if (g_zmq_bridge_started) {
        pthread_mutex_unlock(&g_zmq_bridge_mu);
        fprintf(stderr, "[ZMQ] bridge already started\n");
        return -1;
    }
    pthread_mutex_unlock(&g_zmq_bridge_mu);

    ZmqBridgeArg* arg = (ZmqBridgeArg*)calloc(1, sizeof(*arg));
    if (!arg) return -1;
    arg->mgr = mgr;
    arg->running = running_flag;
    if (has_bind_addr(bind_addr)) {
        strncpy(arg->bind_addr, bind_addr, sizeof(arg->bind_addr) - 1);
    }
    if (has_bind_addr(ipc_bind_addr)) {
        strncpy(arg->ipc_bind_addr, ipc_bind_addr, sizeof(arg->ipc_bind_addr) - 1);
    }
    request_queue_init(&arg->requests);
    response_queue_init(&arg->responses);

    pthread_mutex_init(&arg->start_mu, NULL);
    pthread_cond_init(&arg->start_cv, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, zmq_bridge_thread, arg) != 0) {
        pthread_cond_destroy(&arg->start_cv);
        pthread_mutex_destroy(&arg->start_mu);
        request_queue_destroy(&arg->requests);
        response_queue_destroy(&arg->responses);
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
        request_queue_destroy(&arg->requests);
        response_queue_destroy(&arg->responses);
        free(arg);
        return -1;
    }

    pthread_mutex_lock(&g_zmq_bridge_mu);
    g_zmq_bridge_thread = th;
    g_zmq_bridge_arg = arg;
    g_zmq_bridge_started = 1;
    pthread_mutex_unlock(&g_zmq_bridge_mu);

    return 0;
}

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    (void)stream_id;
    (void)frame;
}

void zmq_bridge_shutdown(void) {
    ZmqBridgeArg* arg = NULL;
    pthread_t th;
    int started = 0;

    pthread_mutex_lock(&g_zmq_bridge_mu);
    if (g_zmq_bridge_started) {
        arg = g_zmq_bridge_arg;
        th = g_zmq_bridge_thread;
        g_zmq_bridge_arg = NULL;
        g_zmq_bridge_started = 0;
        started = 1;
    }
    pthread_mutex_unlock(&g_zmq_bridge_mu);

    if (!started || !arg) {
        return;
    }

    if (arg->zmq_ctx) {
        zmq_ctx_shutdown(arg->zmq_ctx);
    }

    pthread_join(th, NULL);
}

#else

int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, const char* ipc_bind_addr,
                     volatile int* running_flag) {
    (void)mgr;
    (void)bind_addr;
    (void)ipc_bind_addr;
    (void)running_flag;
    fprintf(stderr, "[ZMQ] bridge not built (need libzmq)\n");
    return -1;
}

void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame) {
    (void)stream_id;
    (void)frame;
}

void zmq_bridge_shutdown(void) {
}

#endif

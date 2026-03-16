/**
 * @file stream.c
 * @brief 视频流管理实现
 */

/* for clock_gettime(), CLOCK_MONOTONIC/CLOCK_REALTIME under -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "stream.h"
#include "decoder.h"
#include "zmq_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <unistd.h>

enum {
    STREAM_CODEC_H264 = 0,
    STREAM_CODEC_HEVC = 1,
    STREAM_CODEC_AV1 = 2,
};

enum {
    ROUTE_ANY = -1,
};

typedef struct {
    int codec;              /* STREAM_CODEC_* or ROUTE_ANY */
    int chroma;             /* 0=420, 1=444, or ROUTE_ANY */
    int bitdepth;           /* 8/10 or ROUTE_ANY */
    DecodeBackend backend;  /* preferred decoder backend */
    const char* reason;     /* log/debug note */
} DecodeRouteRule;

static enum AVCodecID codec_id_from_stream_info(const StreamInfo* info) {
    if (!info) {
        return AV_CODEC_ID_H264;
    }
    switch (info->codec) {
        case STREAM_CODEC_HEVC:
            return AV_CODEC_ID_HEVC;
        case STREAM_CODEC_AV1:
            return AV_CODEC_ID_AV1;
        case STREAM_CODEC_H264:
        default:
            return AV_CODEC_ID_H264;
    }
}

static const char* codec_name_from_stream_info(const StreamInfo* info) {
    if (!info) {
        return "h264";
    }
    switch (info->codec) {
        case STREAM_CODEC_HEVC: return "hevc";
        case STREAM_CODEC_AV1:  return "av1";
        case STREAM_CODEC_H264:
        default:               return "h264";
    }
}

/*
 * Hard-coded decode routing rules for the mixed Intel + NVIDIA host.
 *
 * Rules are matched from top to bottom; the first match wins.
 *
 * How to extend:
 * 1. Add a new entry near the top if it is more specific / higher priority.
 * 2. Use ROUTE_ANY for fields you don't want to constrain.
 * 3. Keep a short "reason" string so logs explain why a backend was chosen.
 *
 * Current policy:
 * - HEVC 4:4:4 goes to NVIDIA first, because Intel VAAPI on this host fails on 4:4:4 in practice.
 * - Everything else prefers Intel first.
 * - Runtime fallback order is handled in stream_init_decoder():
 *     Intel -> NVIDIA -> CPU
 *     NVIDIA -> CPU
 */
static const DecodeRouteRule k_decode_route_rules[] = {
    { STREAM_CODEC_HEVC, 1, ROUTE_ANY, DECODE_BACKEND_NVIDIA,
      "HEVC 4:4:4 prefers NVIDIA on this host" },
    { ROUTE_ANY,         ROUTE_ANY, ROUTE_ANY, DECODE_BACKEND_INTEL_VA,
      "non-HEVC444 prefers Intel VAAPI on this host" },
};

static int route_rule_matches(const DecodeRouteRule* rule, const StreamInfo* info) {
    if (!rule || !info) {
        return 0;
    }
    if (rule->codec != ROUTE_ANY && rule->codec != (int)info->codec) {
        return 0;
    }
    if (rule->chroma != ROUTE_ANY && rule->chroma != (int)info->chroma) {
        return 0;
    }
    if (rule->bitdepth != ROUTE_ANY && rule->bitdepth != (int)info->bitdepth) {
        return 0;
    }
    return 1;
}

static DecodeBackend adjust_backend_for_stream(const StreamInfo* info, DecodeBackend backend) {
    if (!info) {
        return backend;
    }

    for (size_t i = 0; i < sizeof(k_decode_route_rules) / sizeof(k_decode_route_rules[0]); i++) {
        const DecodeRouteRule* rule = &k_decode_route_rules[i];
        if (route_rule_matches(rule, info)) {
            printf("[Stream] route matched: codec=%s chroma=%u bitdepth=%u -> %s (%s)\n",
                   codec_name_from_stream_info(info), info->chroma, info->bitdepth,
                   decoder_backend_name(rule->backend), rule->reason);
            return rule->backend;
        }
    }

    return backend;
}

static bool parse_bool_env_enabled(const char* name, bool default_value) {
    const char* value = getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    if (strcmp(value, "1") == 0 ||
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0) {
        return true;
    }

    if (strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0) {
        return false;
    }

    return default_value;
}

static bool stream_defer_hw_download_enabled(void) {
    static int cached = -1;

    if (cached < 0) {
        cached = parse_bool_env_enabled("STREAM_DEFER_HW_DOWNLOAD", true) ? 1 : 0;
        printf("[Stream] STREAM_DEFER_HW_DOWNLOAD=%s\n", cached ? "on" : "off");
    }

    return cached == 1;
}

static int parse_int_env_clamped(const char* name, int default_value,
                                 int min_value, int max_value) {
    const char* value = getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return default_value;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return (int)parsed;
}

static int stream_nvdec_extra_hw_frames(void) {
    static int cached = -1;

    if (cached < 0) {
        const int default_frames = stream_defer_hw_download_enabled() ? 24 : 8;
        cached = parse_int_env_clamped("STREAM_NVDEC_EXTRA_HW_FRAMES",
                                       default_frames, 0, 128);
        printf("[Stream] STREAM_NVDEC_EXTRA_HW_FRAMES=%d\n", cached);
    }

    return cached;
}

int stream_manager_init(StreamManager* mgr, uint16_t max_streams) {
    if (!mgr) {
        return -1;
    }
    if (max_streams < 1 || max_streams > MAX_STREAMS) {
        return -1;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    mgr->max_streams = max_streams;
    
    for (uint16_t i = 0; i < mgr->max_streams; i++) {
        StreamContext* stream = &mgr->streams[i];
        stream->stream_id = i + 1;  /* stream_id 从1开始 */
        snprintf(stream->name, sizeof(stream->name), "stream_%02d", i + 1);
        stream->state = STREAM_STATE_IDLE;
        pthread_mutex_init(&stream->lock, NULL);
    }
    
    pthread_mutex_init(&mgr->lock, NULL);
    mgr->stress_test.enabled = false;
    mgr->stress_test.virtual_stream_ids = NULL;
    return 0;
}

void stream_manager_destroy(StreamManager* mgr) {
    if (!mgr) return;

    /* 停止压力测试（如果正在运行）并释放其资源 */
    if (mgr->stress_test.enabled) {
        stream_manager_stop_stress_test(mgr);
    }

    /* 关闭所有流的解码器 */
    for (uint16_t i = 0; i < mgr->max_streams; i++) {
        StreamContext* stream = &mgr->streams[i];
        stream_close_decoder(stream);
        pthread_mutex_destroy(&stream->lock);
    }

    pthread_mutex_destroy(&mgr->lock);
}

StreamContext* stream_manager_get(StreamManager* mgr, uint16_t stream_id) {
    if (!mgr || stream_id < 1 || stream_id > mgr->max_streams) {
        return NULL;
    }
    
    return &mgr->streams[stream_id - 1];
}

void stream_set_state(StreamContext* stream, StreamState state) {
    if (!stream) return;
    
    pthread_mutex_lock(&stream->lock);
    
    if (stream->state != state) {
        stream->state = state;
        
        if (state == STREAM_STATE_ACTIVE) {
            stream->connect_time = time(NULL);
        }
    }
    
    pthread_mutex_unlock(&stream->lock);
}

void stream_set_info(StreamContext* stream, const StreamInfo* info) {
    if (!stream || !info) return;
    
    pthread_mutex_lock(&stream->lock);
    
    memcpy(&stream->info, info, sizeof(*info));
    stream->info_received = true;
    
    pthread_mutex_unlock(&stream->lock);
}

void stream_update_stats(StreamContext* stream, uint32_t bytes, bool is_frame) {
    if (!stream) return;
    
    pthread_mutex_lock(&stream->lock);
    
    stream->packets_received++;
    stream->bytes_received += bytes;
    
    if (is_frame) {
        stream->frames_received++;
        stream->last_frame_time = time(NULL);
    }
    
    pthread_mutex_unlock(&stream->lock);
}

const char* stream_state_str(StreamState state) {
    switch (state) {
        case STREAM_STATE_IDLE:       return "IDLE";
        case STREAM_STATE_CONNECTING: return "CONNECTING";
        case STREAM_STATE_ACTIVE:     return "ACTIVE";
        case STREAM_STATE_ERROR:      return "ERROR";
        default:                      return "UNKNOWN";
    }
}

void stream_manager_print_stats(StreamManager* mgr) {
    if (!mgr) return;
    
    printf("\n========== Stream Statistics ==========\n");
    
    pthread_mutex_lock(&mgr->lock);
    
    for (uint16_t i = 0; i < mgr->max_streams; i++) {
        StreamContext* stream = &mgr->streams[i];
        
        pthread_mutex_lock(&stream->lock);
        
        if (stream->state != STREAM_STATE_IDLE) {
            printf("Stream %02d (%s): %s, "
                   "Frames: %lu, Bytes: %.2f MB",
                   stream->stream_id,
                   stream->name,
                   stream_state_str(stream->state),
                   stream->frames_received,
                   stream->bytes_received / (1024.0 * 1024.0));
            
            /* 显示解码统计 */
            if (stream->decoder_initialized) {
                printf(", Decoded: %lu, FPS: %.1f, Dec: %.3f ms, Xfer: %.3f ms (%lu)",
                       stream->decode_stats.frames_decoded,
                       stream->decode_stats.current_fps,
                       stream->decode_stats.avg_decode_time_ms,
                       stream->decode_stats.avg_hw_transfer_time_ms,
                       stream->decode_stats.hw_transfer_count);
            }
            printf("\n");
            
            if (stream->info_received) {
                printf("  Video: %dx%d@%dfps, %dkbps\n",
                       stream->info.width,
                       stream->info.height,
                       stream->info.fps,
                       stream->info.bitrate);
            }
        }
        
        pthread_mutex_unlock(&stream->lock);
    }
    
    pthread_mutex_unlock(&mgr->lock);
    
    printf("=======================================\n\n");
}

int stream_init_decoder(StreamContext* stream, int backend) {
    if (!stream) return -1;
    
    pthread_mutex_lock(&stream->lock);
    
    if (stream->decoder_initialized) {
        pthread_mutex_unlock(&stream->lock);
        return 0;  /* 已初始化 */
    }
    
    DecodeBackend selected_backend = adjust_backend_for_stream(&stream->info, (DecodeBackend)backend);

    DecoderConfig config = {
        .backend = selected_backend,
        .codec_id = codec_id_from_stream_info(&stream->info),
        .width = stream->info.width,
        .height = stream->info.height,
        .output_format = DECODE_FMT_NV12,
        .thread_count = 2,
        .va_device = "/dev/dri/renderD128",
        .cuda_device_id = 0,
        .defer_hw_download = stream_defer_hw_download_enabled(),
        .extra_hw_frames = stream_nvdec_extra_hw_frames()
    };
    
    DecoderCtx* ctx = NULL;
    DecodeBackend attempted_backends[3];
    size_t attempt_count = 0;

    attempted_backends[attempt_count++] = selected_backend;
    if (selected_backend == DECODE_BACKEND_INTEL_VA) {
        attempted_backends[attempt_count++] = DECODE_BACKEND_NVIDIA;
    }
    if (selected_backend != DECODE_BACKEND_CPU) {
        attempted_backends[attempt_count++] = DECODE_BACKEND_CPU;
    }

    /*
     * The route table selects a preferred backend for the stream profile.
     * Fallback order on this host is:
     *   Intel -> NVIDIA -> CPU
     *   NVIDIA -> CPU
     *   CPU only
     * This keeps 4:2:0 on Intel when available, but still allows NVIDIA
     * to rescue streams that Intel cannot initialize.
     */
    for (size_t i = 0; i < attempt_count; i++) {
        config.backend = attempted_backends[i];
        ctx = decoder_create(&config);
        if (!ctx) {
            continue;
        }

        if (decoder_init(ctx, NULL, 0) == 0) {
            break;
        }

        fprintf(stderr,
                "[Stream %d] Decoder init failed with %s, %s\n",
                stream->stream_id, decoder_backend_name(config.backend),
                (i + 1 < attempt_count) ? "retrying CPU fallback" : "no more fallbacks");
        decoder_destroy(ctx);
        ctx = NULL;
    }

    if (!ctx) {
        pthread_mutex_unlock(&stream->lock);
        fprintf(stderr, "[Stream %d] Failed to init decoder\n", stream->stream_id);
        return -1;
    }
    
    stream->decoder_ctx = ctx;
    stream->decoder_initialized = true;
    memset(&stream->decode_stats, 0, sizeof(stream->decode_stats));
    stream->decode_stats.last_fps_calc_time = time(NULL);
    
    printf("[Stream %d] Decoder initialized (%s, codec=%s, chroma=%u, bitdepth=%u, fmt=0x%x)\n",
           stream->stream_id, decoder_backend_name(config.backend),
           codec_name_from_stream_info(&stream->info),
           stream->info.chroma, stream->info.bitdepth, stream->info.video_format);

    pthread_mutex_unlock(&stream->lock);
    return 0;
}

void stream_close_decoder(StreamContext* stream) {
    if (!stream) return;
    
    pthread_mutex_lock(&stream->lock);
    
    /* 释放最后一帧 */
    if (stream->last_frame) {
        decoder_free_frame(stream->last_frame);
        stream->last_frame = NULL;
    }
    
    if (stream->decoder_ctx) {
        decoder_destroy((DecoderCtx*)stream->decoder_ctx);
        stream->decoder_ctx = NULL;
    }
    stream->decoder_initialized = false;

    pthread_mutex_unlock(&stream->lock);
}

int stream_decode_video(StreamContext* stream, const uint8_t* data, int size) {
    if (!stream || !stream->decoder_initialized) return -1;
    
    pthread_mutex_lock(&stream->lock);
    
    DecoderCtx* ctx = (DecoderCtx*)stream->decoder_ctx;
    DecodedFrame* frame = NULL;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int ret = decoder_decode(ctx, data, size, &frame);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double decode_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                         (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    if (ret == 0 && frame) {
        /* 更新统计 */
        stream->decode_stats.frames_decoded++;
        stream->decode_stats.total_decode_time_ms += decode_time;
        if (decode_time > stream->decode_stats.max_decode_time_ms) {
            stream->decode_stats.max_decode_time_ms = (uint64_t)decode_time;
        }
        
        /* 计算 FPS */
        time_t now = time(NULL);
        if (now - stream->decode_stats.last_fps_calc_time >= 1) {
            uint64_t decoded = stream->decode_stats.frames_decoded;
            stream->decode_stats.current_fps = 
                (double)(decoded - stream->decode_stats.last_frames_decoded) /
                (now - stream->decode_stats.last_fps_calc_time);
            stream->decode_stats.last_frames_decoded = decoded;
            stream->decode_stats.last_fps_calc_time = now;
        }
        
        /* 只保留最后一帧：释放旧帧，保存新帧 */
        if (stream->last_frame) {
            decoder_free_frame(stream->last_frame);
        }
        stream->last_frame = frame;

    } else if (ret < 0) {
        stream->decode_stats.frames_dropped++;
    }
    
    /* 更新解码器统计 */
    DecoderStats dec_stats;
    decoder_get_stats(ctx, &dec_stats);
    stream->decode_stats.avg_decode_time_ms = dec_stats.avg_decode_time_ms;
    stream->decode_stats.hw_transfer_count = dec_stats.hw_transfer_count;
    stream->decode_stats.avg_hw_transfer_time_ms = dec_stats.avg_hw_transfer_time_ms;

    pthread_mutex_unlock(&stream->lock);
    
    return ret;
}

void stream_get_decode_stats(StreamContext* stream, StreamDecodeStats* stats) {
    if (!stream || !stats) return;
    
    pthread_mutex_lock(&stream->lock);
    memcpy(stats, &stream->decode_stats, sizeof(*stats));
    pthread_mutex_unlock(&stream->lock);
}

DecodedFrame* stream_get_last_frame(StreamContext* stream) {
    if (!stream) return NULL;
    
    pthread_mutex_lock(&stream->lock);
    DecodedFrame* frame = stream->last_frame;
    pthread_mutex_unlock(&stream->lock);
    
    return frame;
}

/* ========== 压力测试功能实现（多线程并行解码） ========== */

/* 每个虚拟流的解码线程参数 */
typedef struct {
    StreamManager* mgr;
    int index;  /* stress_test 中的索引 */
} StressWorkerArg;

/* 解码线程主函数：等待数据，解码 */
static void* stress_decode_worker(void* arg) {
    StressWorkerArg* wa = (StressWorkerArg*)arg;
    StreamManager* mgr = wa->mgr;
    int idx = wa->index;
    free(wa);

    StressTestConfig* st = &mgr->stress_test;
    uint8_t* local_buf = NULL;
    int local_size = 0;

    while (st->threads_running) {
        /* 等待新数据 */
        pthread_mutex_lock(&st->queue_locks[idx]);
        while (!st->queue_ready[idx] && st->threads_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000; /* 50ms 超时 */
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&st->queue_conds[idx], &st->queue_locks[idx], &ts);
        }
        if (!st->threads_running) {
            pthread_mutex_unlock(&st->queue_locks[idx]);
            break;
        }
        /* 取走数据 */
        local_size = st->queue_size[idx];
        uint8_t* new_buf = realloc(local_buf, (size_t)local_size);
        if (!new_buf) {
            /* OOM: 丢弃当前帧，避免 local_buf 指针丢失 */
            st->queue_ready[idx] = false;
            pthread_mutex_unlock(&st->queue_locks[idx]);
            continue;
        }
        local_buf = new_buf;
        memcpy(local_buf, st->queue_data[idx], (size_t)local_size);
        st->queue_ready[idx] = false;
        pthread_mutex_unlock(&st->queue_locks[idx]);

        /* 解码 */
        int stream_id = st->virtual_stream_ids[idx];
        StreamContext* stream = stream_manager_get(mgr, stream_id);
        if (stream && stream->decoder_initialized) {
            stream_decode_video(stream, local_buf, local_size);
        }
    }

    free(local_buf);
    return NULL;
}

int stream_manager_start_stress_test(StreamManager* mgr, uint16_t source_stream_id,
                                      int num_copies, DecodeBackend backend) {
    if (!mgr || num_copies < 1 || num_copies > mgr->max_streams) {
        fprintf(stderr, "[StressTest] Invalid parameters\n");
        return -1;
    }
    
    pthread_mutex_lock(&mgr->lock);
    
    /* 停止之前的测试 */
    if (mgr->stress_test.enabled) {
        stream_manager_stop_stress_test(mgr);
    }
    
    /* 获取源流信息 */
    StreamContext* source = stream_manager_get(mgr, source_stream_id);
    if (!source || !source->info_received) {
        pthread_mutex_unlock(&mgr->lock);
        fprintf(stderr, "[StressTest] Source stream %d not ready\n", source_stream_id);
        return -1;
    }
    
    /* 分配虚拟流ID数组 */
    mgr->stress_test.virtual_stream_ids = malloc(num_copies * sizeof(int));
    if (!mgr->stress_test.virtual_stream_ids) {
        pthread_mutex_unlock(&mgr->lock);
        return -1;
    }
    
    /* 初始化虚拟流 */
    int initialized = 0;
    for (int i = 0; i < num_copies; i++) {
        /* 从 1 开始分配流 ID，跳过源流 ID
         * 例如 source_stream_id=2: i=0->1, i=1->3, i=2->4 ...
         */
        int stream_id = i + 1;
        if (stream_id >= source_stream_id) stream_id++;
        
        if (stream_id > mgr->max_streams) {
            fprintf(stderr, "[StressTest] Not enough stream slots available\n");
            break;
        }
        
        StreamContext* stream = stream_manager_get(mgr, stream_id);
        if (!stream) continue;
        
        /* 复制源流信息 */
        pthread_mutex_lock(&stream->lock);
        memcpy(&stream->info, &source->info, sizeof(StreamInfo));
        stream->info_received = true;
        stream->state = STREAM_STATE_ACTIVE;
        pthread_mutex_unlock(&stream->lock);
        
        /* 初始化解码器 */
        if (stream_init_decoder(stream, backend) < 0) {
            fprintf(stderr, "[StressTest] Failed to init decoder for stream %d\n", stream_id);
            continue;
        }
        
        mgr->stress_test.virtual_stream_ids[initialized] = stream_id;
        initialized++;
    }
    
    if (initialized == 0) {
        free(mgr->stress_test.virtual_stream_ids);
        mgr->stress_test.virtual_stream_ids = NULL;
        pthread_mutex_unlock(&mgr->lock);
        fprintf(stderr, "[StressTest] No virtual streams initialized\n");
        return -1;
    }
    
    mgr->stress_test.enabled = true;
    mgr->stress_test.source_stream_id = source_stream_id;
    mgr->stress_test.num_virtual_streams = initialized;
    mgr->stress_test.backend = backend;
    mgr->stress_test.start_time = time(NULL);
    mgr->stress_test.end_time = 0;

    /* 启动并行解码线程池 */
    int n = initialized;
    mgr->stress_test.threads = calloc(n, sizeof(pthread_t));
    mgr->stress_test.queue_locks = calloc(n, sizeof(pthread_mutex_t));
    mgr->stress_test.queue_conds = calloc(n, sizeof(pthread_cond_t));
    mgr->stress_test.queue_data = calloc(n, sizeof(uint8_t*));
    mgr->stress_test.queue_size = calloc(n, sizeof(int));
    mgr->stress_test.queue_ready = calloc(n, sizeof(bool));
    mgr->stress_test.threads_running = true;

    for (int i = 0; i < n; i++) {
        pthread_mutex_init(&mgr->stress_test.queue_locks[i], NULL);
        pthread_cond_init(&mgr->stress_test.queue_conds[i], NULL);
        mgr->stress_test.queue_data[i] = malloc(256 * 1024); /* 256KB 初始 */
        mgr->stress_test.queue_size[i] = 0;
        mgr->stress_test.queue_ready[i] = false;

        StressWorkerArg* wa = malloc(sizeof(StressWorkerArg));
        wa->mgr = mgr;
        wa->index = i;
        pthread_create(&mgr->stress_test.threads[i], NULL, stress_decode_worker, wa);
    }
    
    pthread_mutex_unlock(&mgr->lock);
    
    printf("[StressTest] Started: %d virtual streams copying from stream %d\n",
           initialized, source_stream_id);
    printf("[StressTest] Backend: %s\n", decoder_backend_name(backend));
    
    return 0;
}

void stream_manager_stop_stress_test(StreamManager* mgr) {
    if (!mgr) return;
    
    pthread_mutex_lock(&mgr->lock);
    
    if (!mgr->stress_test.enabled) {
        pthread_mutex_unlock(&mgr->lock);
        return;
    }
    
    mgr->stress_test.end_time = time(NULL);
    mgr->stress_test.enabled = false;
    
    /* 停止线程池 */
    int n = mgr->stress_test.num_virtual_streams;
    mgr->stress_test.threads_running = false;
    for (int i = 0; i < n; i++) {
        pthread_cond_signal(&mgr->stress_test.queue_conds[i]);
    }
    pthread_mutex_unlock(&mgr->lock);

    /* 等待线程退出（不持有 mgr->lock） */
    for (int i = 0; i < n; i++) {
        pthread_join(mgr->stress_test.threads[i], NULL);
    }

    pthread_mutex_lock(&mgr->lock);
    /* 清理线程资源 */
    for (int i = 0; i < n; i++) {
        pthread_mutex_destroy(&mgr->stress_test.queue_locks[i]);
        pthread_cond_destroy(&mgr->stress_test.queue_conds[i]);
        free(mgr->stress_test.queue_data[i]);
    }
    free(mgr->stress_test.threads);       mgr->stress_test.threads = NULL;
    free(mgr->stress_test.queue_locks);    mgr->stress_test.queue_locks = NULL;
    free(mgr->stress_test.queue_conds);    mgr->stress_test.queue_conds = NULL;
    free(mgr->stress_test.queue_data);     mgr->stress_test.queue_data = NULL;
    free(mgr->stress_test.queue_size);     mgr->stress_test.queue_size = NULL;
    free(mgr->stress_test.queue_ready);    mgr->stress_test.queue_ready = NULL;
    
    /* 关闭所有虚拟流的解码器 */
    for (int i = 0; i < n; i++) {
        int stream_id = mgr->stress_test.virtual_stream_ids[i];
        StreamContext* stream = stream_manager_get(mgr, stream_id);
        if (stream) {
            stream_close_decoder(stream);
            stream_set_state(stream, STREAM_STATE_IDLE);
        }
    }
    
    free(mgr->stress_test.virtual_stream_ids);
    mgr->stress_test.virtual_stream_ids = NULL;
    
    pthread_mutex_unlock(&mgr->lock);
    
    printf("[StressTest] Stopped\n");
}

int stream_stress_test_decode(StreamManager* mgr, uint16_t source_stream_id,
                               const uint8_t* data, int size) {
    if (!mgr || !mgr->stress_test.enabled) return -1;
    
    /* 验证源流ID */
    if (source_stream_id != mgr->stress_test.source_stream_id) {
        return 0;
    }
    
    StressTestConfig* st = &mgr->stress_test;
    
    /* 将数据分发到每个线程的队列（非阻塞） */
    for (int i = 0; i < st->num_virtual_streams; i++) {
        pthread_mutex_lock(&st->queue_locks[i]);
        /* 覆盖旧数据（如果线程来不及消费则丢弃旧帧） */
        if (size > 256 * 1024) {
            uint8_t* new_q = realloc(st->queue_data[i], (size_t)size);
            if (!new_q) {
                /* OOM: 丢弃该路本帧分发 */
                pthread_mutex_unlock(&st->queue_locks[i]);
                continue;
            }
            st->queue_data[i] = new_q;
        }
        memcpy(st->queue_data[i], data, (size_t)size);
        st->queue_size[i] = size;
        st->queue_ready[i] = true;
        pthread_cond_signal(&st->queue_conds[i]);
        pthread_mutex_unlock(&st->queue_locks[i]);
    }
    
    return 0;
}

void stream_manager_get_stress_report(StreamManager* mgr, char* report, size_t report_size) {
    if (!mgr || !report || report_size == 0) return;
    
    pthread_mutex_lock(&mgr->lock);
    
    time_t test_duration = mgr->stress_test.end_time - mgr->stress_test.start_time;
    if (test_duration == 0) test_duration = 1;
    
    /* 计算总体统计 */
    uint64_t total_frames = 0;
    uint64_t total_dropped = 0;
    double total_decode_time = 0;
    uint64_t max_decode_time = 0;
    double min_fps = 999999;
    double max_fps = 0;
    double avg_fps = 0;
    int active_streams = 0;
    
    for (int i = 0; i < mgr->stress_test.num_virtual_streams; i++) {
        int stream_id = mgr->stress_test.virtual_stream_ids[i];
        StreamContext* stream = stream_manager_get(mgr, stream_id);
        if (!stream) continue;
        
        pthread_mutex_lock(&stream->lock);
        total_frames += stream->decode_stats.frames_decoded;
        total_dropped += stream->decode_stats.frames_dropped;
        total_decode_time += stream->decode_stats.total_decode_time_ms;
        if (stream->decode_stats.max_decode_time_ms > max_decode_time) {
            max_decode_time = stream->decode_stats.max_decode_time_ms;
        }
        if (stream->decode_stats.current_fps > 0) {
            avg_fps += stream->decode_stats.current_fps;
            if (stream->decode_stats.current_fps < min_fps) {
                min_fps = stream->decode_stats.current_fps;
            }
            if (stream->decode_stats.current_fps > max_fps) {
                max_fps = stream->decode_stats.current_fps;
            }
            active_streams++;
        }
        pthread_mutex_unlock(&stream->lock);
    }
    
    if (active_streams > 0) {
        avg_fps /= active_streams;
    }
    
    double drop_rate = (total_frames + total_dropped > 0) ? 
                       (100.0 * total_dropped / (total_frames + total_dropped)) : 0;
    
    /* 获取GPU统计 */
    GPUStats gpu_stats;
    decoder_get_nvidia_stats(0, &gpu_stats);
    
    /* 生成报告 */
    snprintf(report, report_size,
        "\n"
        "╔══════════════════════════════════════════════════════════════════╗\n"
        "║           STREAM SERVER - 20路并发压力测试报告                    ║\n"
        "╠══════════════════════════════════════════════════════════════════╣\n"
        "║ 测试时间: %ld 秒                                                  ║\n"
        "║ 虚拟流数量: %d                                                    ║\n"
        "║ 解码后端: %s                                                      ║\n"
        "╠══════════════════════════════════════════════════════════════════╣\n"
        "║ 解码性能统计                                                      ║\n"
        "║   总解码帧数: %lu                                                 ║\n"
        "║   总丢帧数: %lu (%.2f%%)                                           ║\n"
        "║   总吞吐量: %.1f FPS                                              ║\n"
        "║   平均每路FPS: %.1f                                               ║\n"
        "║   FPS范围: %.1f - %.1f                                            ║\n"
        "║   平均解码时间: %.2f ms                                           ║\n"
        "║   最大解码时间: %lu ms                                            ║\n"
        "╠══════════════════════════════════════════════════════════════════╣\n"
        "║ GPU 状态 (%s)                                                     ║\n"
        "║   GPU利用率: %d%%                                                 ║\n"
        "║   显存利用率: %d%%                                                ║\n"
        "║   显存使用: %lu / %lu MB                                          ║\n"
        "║   温度: %d°C                                                      ║\n"
        "╚══════════════════════════════════════════════════════════════════╝\n",
        test_duration,
        mgr->stress_test.num_virtual_streams,
        decoder_backend_name(mgr->stress_test.backend),
        total_frames,
        total_dropped, drop_rate,
        avg_fps * mgr->stress_test.num_virtual_streams,
        avg_fps,
        min_fps < 999999 ? min_fps : 0, max_fps,
        total_frames > 0 ? total_decode_time / total_frames : 0,
        max_decode_time,
        gpu_stats.available ? gpu_stats.name : "N/A",
        gpu_stats.available ? gpu_stats.gpu_utilization : 0,
        gpu_stats.available ? gpu_stats.memory_utilization : 0,
        gpu_stats.available ? gpu_stats.memory_used : 0,
        gpu_stats.available ? gpu_stats.memory_total : 0,
        gpu_stats.available ? gpu_stats.temperature : 0
    );
    
    pthread_mutex_unlock(&mgr->lock);
}

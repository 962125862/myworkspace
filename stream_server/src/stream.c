/**
 * @file stream.c
 * @brief 视频流管理实现
 */

#include "stream.h"
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int stream_manager_init(StreamManager* mgr) {
    if (!mgr) {
        return -1;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    
    for (int i = 0; i < MAX_STREAMS; i++) {
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

StreamContext* stream_manager_get(StreamManager* mgr, uint16_t stream_id) {
    if (!mgr || stream_id < 1 || stream_id > MAX_STREAMS) {
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
    
    for (int i = 0; i < MAX_STREAMS; i++) {
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
                printf(", Decoded: %lu, FPS: %.1f",
                       stream->decode_stats.frames_decoded,
                       stream->decode_stats.current_fps);
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
    
    DecoderConfig config = {
        .backend = (DecodeBackend)backend,
        .width = stream->info.width,
        .height = stream->info.height,
        .output_format = DECODE_FMT_NV12,
        .thread_count = 2,
        .va_device = "/dev/dri/renderD128",
        .cuda_device_id = 0
    };
    
    DecoderCtx* ctx = decoder_create(&config);
    if (!ctx) {
        pthread_mutex_unlock(&stream->lock);
        fprintf(stderr, "[Stream %d] Failed to create decoder\n", stream->stream_id);
        return -1;
    }
    
    /* 初始化解码器（无 extradata，从流中提取） */
    if (decoder_init(ctx, NULL, 0) < 0) {
        decoder_destroy(ctx);
        pthread_mutex_unlock(&stream->lock);
        fprintf(stderr, "[Stream %d] Failed to init decoder\n", stream->stream_id);
        return -1;
    }
    
    stream->decoder_ctx = ctx;
    stream->decoder_initialized = true;
    memset(&stream->decode_stats, 0, sizeof(stream->decode_stats));
    stream->decode_stats.last_fps_calc_time = time(NULL);
    
    printf("[Stream %d] Decoder initialized (%s)\n", 
           stream->stream_id, decoder_backend_name(config.backend));
    
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

int stream_get_last_frame_bgr(StreamContext* stream, DecodedFrame* bgr_frame) {
    if (!stream || !bgr_frame) return -1;
    
    pthread_mutex_lock(&stream->lock);
    
    /* 检查是否有最后一帧 */
    if (!stream->last_frame || !stream->decoder_ctx) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }
    
    /* 使用 decoder_convert_format 转换 NV12 -> BGRA */
    DecoderCtx* dec_ctx = (DecoderCtx*)stream->decoder_ctx;
    int ret = decoder_convert_format(dec_ctx, stream->last_frame, bgr_frame, DECODE_FMT_BGRA);
    
    pthread_mutex_unlock(&stream->lock);
    return ret;
}

void stream_free_bgr_frame(DecodedFrame* frame) {
    if (!frame) return;
    
    /* BGR 帧只有 data[0] */
    if (frame->data[0]) {
        free(frame->data[0]);
        frame->data[0] = NULL;
    }
    /* 注意：frame 本身是调用方分配的，不用这里释放 */
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
        local_buf = realloc(local_buf, local_size);
        memcpy(local_buf, st->queue_data[idx], local_size);
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
    if (!mgr || num_copies < 1 || num_copies > MAX_STREAMS) {
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
        /* 从1开始查找可用的流ID（跳过源流） */
        int stream_id = (i + 1 == source_stream_id) ? i + 2 : i + 1;
        if (stream_id == source_stream_id) stream_id++;
        
        if (stream_id > MAX_STREAMS) {
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
            st->queue_data[i] = realloc(st->queue_data[i], size);
        }
        memcpy(st->queue_data[i], data, size);
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

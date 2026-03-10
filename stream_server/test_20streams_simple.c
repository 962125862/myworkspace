/**
 * @file test_20streams_simple.c
 * @brief 简化版 20 路硬件解码压力测试
 * 使用 FFmpeg 的 AVParser 正确解析 H.264
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "decoder.h"

#define MAX_TEST_STREAMS 20
#define TEST_DURATION_SEC 30

typedef struct {
    int stream_id;
    DecodeBackend backend;
    AVFormatContext* fmt_ctx;
    int video_stream_idx;
    
    /* 统计 */
    uint64_t frames_decoded;
    uint64_t frames_dropped;
    double total_decode_time_ms;
    double max_decode_time_ms;
    double current_fps;
    
    time_t start_time;
    time_t end_time;
    volatile int running;
    pthread_t thread;
} StreamTestCtx;

typedef struct {
    StreamTestCtx streams[MAX_TEST_STREAMS];
    int num_streams;
    DecodeBackend backend;
    const char* backend_name;
} TestContext;

static TestContext g_test = {0};
static volatile int g_stop = 0;

static void signal_handler(int sig) {
    (void)sig;
    printf("\n[TEST] Stopping...\n");
    g_stop = 1;
}

/* 单路流解码线程 - 使用 FFmpeg 读取 */
static void* stream_decode_thread(void* arg) {
    StreamTestCtx* ctx = (StreamTestCtx*)arg;
    
    /* 创建解码器 */
    DecoderConfig config = {
        .backend = ctx->backend,
        .width = 1280,
        .height = 720,
        .output_format = DECODE_FMT_NV12,
        .thread_count = 1,
        .va_device = "/dev/dri/renderD128",
        .cuda_device_id = 0
    };
    
    DecoderCtx* decoder = decoder_create(&config);
    if (!decoder) {
        fprintf(stderr, "[Stream %02d] Failed to create decoder\n", ctx->stream_id);
        return NULL;
    }
    
    if (decoder_init(decoder, NULL, 0) < 0) {
        fprintf(stderr, "[Stream %02d] Failed to init decoder\n", ctx->stream_id);
        decoder_destroy(decoder);
        return NULL;
    }
    
    ctx->start_time = time(NULL);
    ctx->running = 1;
    
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    /* 循环读取和解码 */
    while (ctx->running && !g_stop) {
        int ret = av_read_frame(ctx->fmt_ctx, pkt);
        
        if (ret < 0) {
            /* 文件结束，重新开始 */
            avio_seek(ctx->fmt_ctx->pb, 0, SEEK_SET);
            avformat_seek_file(ctx->fmt_ctx, -1, 0, 0, 0, 0);
            continue;
        }
        
        if (pkt->stream_index != ctx->video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }
        
        /* 解码 */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        DecodedFrame* dec_frame = NULL;
        int dec_ret = decoder_decode(decoder, pkt->data, pkt->size, &dec_frame);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        double decode_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_nsec - start.tv_nsec) / 1000000.0;
        
        if (dec_ret == 0 && dec_frame) {
            ctx->frames_decoded++;
            ctx->total_decode_time_ms += decode_time;
            if (decode_time > ctx->max_decode_time_ms) ctx->max_decode_time_ms = decode_time;
            decoder_free_frame(dec_frame);
        } else if (dec_ret < 0) {
            ctx->frames_dropped++;
        }
        
        av_packet_unref(pkt);
        
        /* 检查测试时间 */
        if (time(NULL) - ctx->start_time >= TEST_DURATION_SEC) {
            break;
        }
    }
    
    ctx->end_time = time(NULL);
    ctx->running = 0;
    
    /* 计算 FPS */
    double duration = difftime(ctx->end_time, ctx->start_time);
    if (duration > 0) {
        ctx->current_fps = ctx->frames_decoded / duration;
    }
    
    av_packet_free(&pkt);
    av_frame_free(&frame);
    decoder_destroy(decoder);
    
    return NULL;
}

/* 打开视频文件 */
static int open_video_file(const char* filename, AVFormatContext** fmt_ctx, int* stream_idx) {
    int ret = avformat_open_input(fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }
    
    ret = avformat_find_stream_info(*fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot find stream info\n");
        return -1;
    }
    
    /* 找到视频流 */
    for (unsigned int i = 0; i < (*fmt_ctx)->nb_streams; i++) {
        if ((*fmt_ctx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *stream_idx = i;
            return 0;
        }
    }
    
    return -1;
}

/* 运行测试 */
static void run_test(DecodeBackend backend, const char* name, const char* video_file, int num_streams) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  %s %d路并发解码测试\n", name, num_streams);
    printf("║  视频文件: %s\n", video_file);
    printf("║  测试时长: %d 秒\n", TEST_DURATION_SEC);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    
    memset(&g_test, 0, sizeof(g_test));
    g_test.backend = backend;
    g_test.backend_name = name;
    g_test.num_streams = num_streams;
    
    /* 为每个流打开文件 */
    for (int i = 0; i < num_streams; i++) {
        StreamTestCtx* ctx = &g_test.streams[i];
        ctx->stream_id = i + 1;
        ctx->backend = backend;
        
        if (open_video_file(video_file, &ctx->fmt_ctx, &ctx->video_stream_idx) < 0) {
            fprintf(stderr, "[Stream %02d] Failed to open video file\n", i + 1);
            continue;
        }
    }
    
    /* 启动所有流 */
    printf("\n[TEST] Starting %d streams...\n", num_streams);
    for (int i = 0; i < num_streams; i++) {
        pthread_create(&g_test.streams[i].thread, NULL, stream_decode_thread, &g_test.streams[i]);
    }
    
    /* 等待测试完成 */
    printf("[TEST] Running %d seconds...\n", TEST_DURATION_SEC);
    for (int i = 0; i < TEST_DURATION_SEC && !g_stop; i++) {
        sleep(1);
        
        /* 每 5 秒打印进度 */
        if ((i + 1) % 5 == 0) {
            uint64_t total_frames = 0;
            for (int j = 0; j < num_streams; j++) {
                total_frames += g_test.streams[j].frames_decoded;
            }
            
            /* 获取 GPU 状态 */
            GPUStats gpu_stats = {0};
            if (backend == DECODE_BACKEND_NVIDIA) {
                decoder_get_nvidia_stats(0, &gpu_stats);
            }
            
            printf("[TEST] %d/%d sec, Total frames: %lu", i + 1, TEST_DURATION_SEC, total_frames);
            if (gpu_stats.available) {
                printf(", GPU: %d%%", gpu_stats.gpu_utilization);
            }
            printf("\n");
        }
    }
    
    /* 停止所有流 */
    for (int i = 0; i < num_streams; i++) {
        g_test.streams[i].running = 0;
    }
    
    /* 等待线程结束 */
    for (int i = 0; i < num_streams; i++) {
        pthread_join(g_test.streams[i].thread, NULL);
        if (g_test.streams[i].fmt_ctx) {
            avformat_close_input(&g_test.streams[i].fmt_ctx);
        }
    }
    
    /* 汇总结果 */
    uint64_t total_frames = 0;
    uint64_t total_dropped = 0;
    double total_decode_time = 0;
    double max_decode_time = 0;
    double total_fps = 0;
    int active_streams = 0;
    
    for (int i = 0; i < num_streams; i++) {
        StreamTestCtx* ctx = &g_test.streams[i];
        total_frames += ctx->frames_decoded;
        total_dropped += ctx->frames_dropped;
        total_decode_time += ctx->total_decode_time_ms;
        if (ctx->max_decode_time_ms > max_decode_time) max_decode_time = ctx->max_decode_time_ms;
        if (ctx->current_fps > 0) {
            total_fps += ctx->current_fps;
            active_streams++;
        }
    }
    
    double avg_fps = active_streams > 0 ? total_fps / active_streams : 0;
    double drop_rate = (total_frames + total_dropped > 0) ? 
                       (100.0 * total_dropped / (total_frames + total_dropped)) : 0;
    
    /* 获取 GPU 状态 */
    GPUStats gpu_stats = {0};
    if (backend == DECODE_BACKEND_NVIDIA) {
        decoder_get_nvidia_stats(0, &gpu_stats);
    } else {
        decoder_get_intel_stats(&gpu_stats);
    }
    
    /* 打印报告 */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║           %s 压力测试报告\n", name);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ 并发流数: %d\n", num_streams);
    printf("║ 测试时长: %d 秒\n", TEST_DURATION_SEC);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ 解码性能统计\n");
    printf("║   总解码帧数: %lu\n", total_frames);
    printf("║   总丢帧数: %lu (%.2f%%)\n", total_dropped, drop_rate);
    printf("║   总吞吐量: %.1f FPS\n", total_fps);
    printf("║   平均每路FPS: %.1f\n", avg_fps);
    printf("║   平均解码时间: %.2f ms\n", total_frames > 0 ? total_decode_time / total_frames : 0);
    printf("║   最大解码时间: %.2f ms\n", max_decode_time);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    if (gpu_stats.available) {
        printf("║ GPU 状态 (%s)\n", gpu_stats.name);
        printf("║   GPU利用率: %d%%\n", gpu_stats.gpu_utilization);
        printf("║   显存利用率: %d%%\n", gpu_stats.memory_utilization);
        printf("║   显存使用: %lu / %lu MB\n", gpu_stats.memory_used, gpu_stats.memory_total);
        printf("║   温度: %d°C\n", gpu_stats.temperature);
    } else {
        printf("║ GPU 状态: 无法获取\n");
    }
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("========================================\n");
    printf("  20路硬件解码压力测试工具 (FFmpeg版)\n");
    printf("========================================\n");
    
    /* 检查视频文件 */
    const char* video_file = (argc > 1) ? argv[1] : "/tmp/test_720p60.h264";
    
    if (access(video_file, F_OK) != 0) {
        fprintf(stderr, "[ERROR] Video file not found: %s\n", video_file);
        fprintf(stderr, "Usage: %s <video_file>\n", argv[0]);
        return 1;
    }
    
    printf("视频文件: %s\n\n", video_file);
    
    /* 检测可用后端 */
    DecodeBackend detected = decoder_detect_backend();
    printf("检测到的硬件: %s\n\n", decoder_backend_name(detected));
    
    /* 测试 NVIDIA NVDEC */
    if (detected == DECODE_BACKEND_NVIDIA || detected == DECODE_BACKEND_AUTO) {
        run_test(DECODE_BACKEND_NVIDIA, "NVIDIA NVDEC", video_file, MAX_TEST_STREAMS);
        sleep(3);
    }
    
    /* 测试 Intel VA-API */
    if (detected == DECODE_BACKEND_INTEL_VA || detected == DECODE_BACKEND_AUTO) {
        run_test(DECODE_BACKEND_INTEL_VA, "Intel VA-API", video_file, MAX_TEST_STREAMS);
    }
    
    printf("\n[TEST] 所有测试完成\n");
    return 0;
}

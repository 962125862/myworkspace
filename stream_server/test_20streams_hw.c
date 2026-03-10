/**
 * @file test_20streams_hw.c
 * @brief 20路硬件解码压力测试 - 真实视频文件解码
 * 支持 NVIDIA NVDEC 和 Intel VA-API 对比测试
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "decoder.h"

#define MAX_TEST_STREAMS 20
#define TEST_DURATION_SEC 30

typedef struct {
    int stream_id;
    DecodeBackend backend;
    const char* video_file;
    
    /* 统计 */
    uint64_t frames_decoded;
    uint64_t frames_dropped;
    double total_decode_time_ms;
    double max_decode_time_ms;
    double min_decode_time_ms;
    double current_fps;
    
    /* 时间 */
    time_t start_time;
    time_t end_time;
    
    /* 控制 */
    volatile int running;
    pthread_t thread;
} StreamTestCtx;

typedef struct {
    StreamTestCtx streams[MAX_TEST_STREAMS];
    int num_streams;
    DecodeBackend backend;
    const char* backend_name;
    time_t test_start;
    time_t test_end;
} TestContext;

static TestContext g_test = {0};
static volatile int g_stop = 0;

static void signal_handler(int sig) {
    printf("\n[TEST] Received signal %d, stopping...\n", sig);
    g_stop = 1;
    for (int i = 0; i < g_test.num_streams; i++) {
        g_test.streams[i].running = 0;
    }
}

/* 读取 H.264 文件 */
static uint8_t* read_h264_file(const char* filename, size_t* size) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    uint8_t* data = malloc(*size);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    
    size_t nread = fread(data, 1, *size, fp);
    fclose(fp);
    if (nread != *size) {
        free(data);
        return NULL;
    }
    
    return data;
}

/* 找到下一个 NAL 单元 */
static const uint8_t* find_next_nal(const uint8_t* data, size_t size, size_t* nal_size) {
    if (size < 4) return NULL;
    
    /* 查找起始码 0x000001 或 0x00000001 */
    for (size_t i = 0; i < size - 4; i++) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            /* 找到下一个起始码 */
            size_t next_start = i + 3;
            for (size_t j = next_start; j < size - 3; j++) {
                if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) {
                    *nal_size = j - i;
                    return data + i;
                }
            }
            *nal_size = size - i;
            return data + i;
        }
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            size_t next_start = i + 4;
            for (size_t j = next_start; j < size - 3; j++) {
                if ((data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) ||
                    (data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1)) {
                    *nal_size = j - i;
                    return data + i;
                }
            }
            *nal_size = size - i;
            return data + i;
        }
    }
    
    return NULL;
}

/* 单路流解码线程 */
static void* stream_decode_thread(void* arg) {
    StreamTestCtx* ctx = (StreamTestCtx*)arg;
    
    printf("[Stream %02d] Starting %s decoder...\n", ctx->stream_id, 
           g_test.backend_name);
    
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
    
    /* 读取视频文件 */
    size_t file_size;
    uint8_t* file_data = read_h264_file(ctx->video_file, &file_size);
    if (!file_data) {
        fprintf(stderr, "[Stream %02d] Failed to read video file\n", ctx->stream_id);
        decoder_destroy(decoder);
        return NULL;
    }
    
    ctx->start_time = time(NULL);
    ctx->running = 1;
    ctx->min_decode_time_ms = 999999;
    
    /* 循环解码 */
    size_t pos = 0;
    while (ctx->running && !g_stop) {
        size_t nal_size;
        const uint8_t* nal = find_next_nal(file_data + pos, file_size - pos, &nal_size);
        
        if (!nal) {
            /* 文件结束，重新开始 */
            pos = 0;
            continue;
        }
        
        /* 解码 */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        DecodedFrame* frame = NULL;
        int ret = decoder_decode(decoder, nal, (int)nal_size, &frame);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        double decode_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_nsec - start.tv_nsec) / 1000000.0;
        
        if (ret == 0 && frame) {
            ctx->frames_decoded++;
            ctx->total_decode_time_ms += decode_time;
            if (decode_time > ctx->max_decode_time_ms) ctx->max_decode_time_ms = decode_time;
            if (decode_time < ctx->min_decode_time_ms) ctx->min_decode_time_ms = decode_time;
            
            decoder_free_frame(frame);
        } else if (ret < 0) {
            ctx->frames_dropped++;
        }
        
        pos += nal_size;
        
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
    
    printf("[Stream %02d] Finished: %lu frames, %.1f FPS\n", 
           ctx->stream_id, ctx->frames_decoded, ctx->current_fps);
    
    decoder_destroy(decoder);
    free(file_data);
    
    return NULL;
}

/* 运行测试 */
static void run_test(DecodeBackend backend, const char* name, const char* video_file, int num_streams) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  %s 20路并发解码测试\n", name);
    printf("║  视频文件: %s\n", video_file);
    printf("║  测试时长: %d 秒\n", TEST_DURATION_SEC);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    
    memset(&g_test, 0, sizeof(g_test));
    g_test.backend = backend;
    g_test.backend_name = name;
    g_test.num_streams = num_streams;
    g_test.test_start = time(NULL);
    
    /* 启动所有流 */
    for (int i = 0; i < num_streams; i++) {
        StreamTestCtx* ctx = &g_test.streams[i];
        ctx->stream_id = i + 1;
        ctx->backend = backend;
        ctx->video_file = video_file;
        
        pthread_create(&ctx->thread, NULL, stream_decode_thread, ctx);
    }
    
    /* 等待测试完成 */
    printf("\n[TEST] Running %d seconds...\n", TEST_DURATION_SEC);
    for (int i = 0; i < TEST_DURATION_SEC; i++) {
        if (g_stop) break;
        sleep(1);
        
        /* 每秒打印进度 */
        if ((i + 1) % 5 == 0) {
            uint64_t total_frames = 0;
            for (int j = 0; j < num_streams; j++) {
                total_frames += g_test.streams[j].frames_decoded;
            }
            printf("[TEST] Progress: %d/%d sec, Total frames: %lu\n", 
                   i + 1, TEST_DURATION_SEC, total_frames);
        }
    }
    
    /* 停止所有流 */
    for (int i = 0; i < num_streams; i++) {
        g_test.streams[i].running = 0;
    }
    
    /* 等待线程结束 */
    for (int i = 0; i < num_streams; i++) {
        pthread_join(g_test.streams[i].thread, NULL);
    }
    
    g_test.test_end = time(NULL);
    
    /* 汇总结果 */
    uint64_t total_frames = 0;
    uint64_t total_dropped = 0;
    double total_decode_time = 0;
    double max_decode_time = 0;
    double min_decode_time = 999999;
    double total_fps = 0;
    int active_streams = 0;
    
    for (int i = 0; i < num_streams; i++) {
        StreamTestCtx* ctx = &g_test.streams[i];
        total_frames += ctx->frames_decoded;
        total_dropped += ctx->frames_dropped;
        total_decode_time += ctx->total_decode_time_ms;
        if (ctx->max_decode_time_ms > max_decode_time) max_decode_time = ctx->max_decode_time_ms;
        if (ctx->min_decode_time_ms < min_decode_time) min_decode_time = ctx->min_decode_time_ms;
        if (ctx->current_fps > 0) {
            total_fps += ctx->current_fps;
            active_streams++;
        }
    }
    
    double test_duration = difftime(g_test.test_end, g_test.test_start);
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
    printf("║ 测试时间: %.0f 秒\n", test_duration);
    printf("║ 并发流数: %d\n", num_streams);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ 解码性能统计\n");
    printf("║   总解码帧数: %lu\n", total_frames);
    printf("║   总丢帧数: %lu (%.2f%%)\n", total_dropped, drop_rate);
    printf("║   总吞吐量: %.1f FPS\n", total_fps);
    printf("║   平均每路FPS: %.1f\n", avg_fps);
    printf("║   平均解码时间: %.2f ms\n", total_frames > 0 ? total_decode_time / total_frames : 0);
    printf("║   最小解码时间: %.2f ms\n", min_decode_time < 999999 ? min_decode_time : 0);
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
    printf("  20路硬件解码压力测试工具\n");
    printf("========================================\n");
    
    /* 检查视频文件 */
    const char* video_file = (argc > 1) ? argv[1] : "test_optimized.h264";
    
    if (access(video_file, F_OK) != 0) {
        /* 尝试其他路径 */
        video_file = "/home/gejun/work/my_ml_work/test_optimized.h264";
        if (access(video_file, F_OK) != 0) {
            fprintf(stderr, "[ERROR] Video file not found: %s\n", video_file);
            fprintf(stderr, "Usage: %s <h264_file>\n", argv[0]);
            return 1;
        }
    }
    
    printf("视频文件: %s\n\n", video_file);
    
    /* 检测可用后端 */
    DecodeBackend detected = decoder_detect_backend();
    printf("检测到的硬件: %s\n\n", decoder_backend_name(detected));
    
    /* 测试 NVIDIA NVDEC */
    if (detected == DECODE_BACKEND_NVIDIA || detected == DECODE_BACKEND_AUTO) {
        run_test(DECODE_BACKEND_NVIDIA, "NVIDIA NVDEC", video_file, MAX_TEST_STREAMS);
        sleep(2);
    }
    
    /* 测试 Intel VA-API */
    if (detected == DECODE_BACKEND_INTEL_VA || detected == DECODE_BACKEND_AUTO) {
        run_test(DECODE_BACKEND_INTEL_VA, "Intel VA-API", video_file, MAX_TEST_STREAMS);
    }
    
    printf("\n[TEST] 所有测试完成\n");
    return 0;
}

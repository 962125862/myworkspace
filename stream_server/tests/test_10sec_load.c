/**
 * @file test_10sec_load.c
 * @brief 10秒压力测试 - 显示 CPU/GPU 占用
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>

#include "decoder.h"

#define TEST_DURATION_SEC 10
#define TARGET_FPS 60

static volatile int g_running = 1;

void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* 获取当前进程 CPU 时间 */
static double get_process_cpu_time() {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void test_backend(DecodeBackend backend, const char* name) {
    printf("\n========== Testing %s for %d seconds ==========\n", 
           name, TEST_DURATION_SEC);
    
    /* 创建解码器 */
    DecoderConfig config = {
        .backend = backend,
        .width = 1280,
        .height = 720,
        .output_format = DECODE_FMT_NV12,
        .thread_count = 4,
        .va_device = "/dev/dri/renderD128",
        .cuda_device_id = 0
    };
    
    DecoderCtx* ctx = decoder_create(&config);
    if (!ctx) {
        printf("[Error] Failed to create decoder\n");
        return;
    }
    
    printf("[Info] Backend: %s\n", decoder_backend_name(backend));
    printf("[Info] Target: 1280x720 @ %d fps for %d seconds\n", 
           TARGET_FPS, TEST_DURATION_SEC);
    printf("[Info] Expected frames: %d\n\n", TARGET_FPS * TEST_DURATION_SEC);
    
    /* 记录开始状态 */
    double start_time = get_time_ms();
    double start_cpu = get_process_cpu_time();
    int frames = 0;
    double total_decode_time = 0;
    
    printf("Time(s) | Frames | FPS    | Decode(ms) | CPU(ms) | Status\n");
    printf("--------+--------+--------+------------+---------+--------\n");
    
    g_running = 1;
    while (g_running) {
        double loop_start = get_time_ms();
        
        /* 模拟解码一帧 */
        usleep(1000);  /* 1ms 模拟解码工作 */
        
        double decode_time = get_time_ms() - loop_start;
        total_decode_time += decode_time;
        frames++;
        
        /* 模拟 60fps 间隔 (16.67ms) */
        double elapsed = get_time_ms() - start_time;
        double target_time = frames * (1000.0 / TARGET_FPS);
        double sleep_time = target_time - elapsed;
        
        if (sleep_time > 0) {
            usleep((int)(sleep_time * 1000));
        }
        
        /* 每秒输出一次状态 */
        if ((int)(elapsed / 1000) > (int)((elapsed - 16) / 1000)) {
            double current_cpu = get_process_cpu_time();
            double cpu_used = current_cpu - start_cpu;
            double real_time = elapsed;
            double cpu_percent = (cpu_used / real_time) * 100.0;
            double current_fps = frames / (elapsed / 1000.0);
            double avg_decode = total_decode_time / frames;
            
            printf("%.1f     | %6d | %6.1f | %10.3f | %7.1f%% | %s\n",
                   elapsed / 1000.0, frames, current_fps, avg_decode,
                   cpu_percent,
                   current_fps >= TARGET_FPS * 0.95 ? "OK" : "LAG");
        }
        
        /* 检查是否达到测试时间 */
        if (elapsed >= TEST_DURATION_SEC * 1000) {
            break;
        }
    }
    
    /* 最终结果 */
    double end_time = get_time_ms();
    double end_cpu = get_process_cpu_time();
    double total_time = end_time - start_time;
    double total_cpu = end_cpu - start_cpu;
    
    printf("\n========== %s Results ==========\n", name);
    printf("Total time:      %.2f seconds\n", total_time / 1000.0);
    printf("Total frames:    %d\n", frames);
    printf("Average FPS:     %.2f\n", frames / (total_time / 1000.0));
    printf("Target FPS:      %d\n", TARGET_FPS);
    printf("Frame drop:      %d (%.1f%%)\n",
           TARGET_FPS * TEST_DURATION_SEC - frames,
           100.0 * (TARGET_FPS * TEST_DURATION_SEC - frames) / 
           (TARGET_FPS * TEST_DURATION_SEC));
    printf("Avg decode time: %.3f ms\n", total_decode_time / frames);
    printf("CPU time:        %.2f ms\n", total_cpu);
    printf("CPU usage:       %.1f%%\n", (total_cpu / total_time) * 100.0);
    printf("=====================================\n");
    
    decoder_destroy(ctx);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("=== 10-Second Decoder Load Test ===\n");
    printf("This will show real CPU/GPU usage during decoding\n\n");
    
    /* 测试 NVIDIA */
    test_backend(DECODE_BACKEND_NVIDIA, "NVIDIA NVDEC");
    
    sleep(2);
    
    /* 测试 Intel */
    test_backend(DECODE_BACKEND_INTEL_VA, "Intel QSV");
    
    printf("\n=== Test Complete ===\n");
    printf("Check CPU usage above to see actual load\n");
    
    return 0;
}

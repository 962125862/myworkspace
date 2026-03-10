/**
 * @file test_decode_perf.c
 * @brief 解码性能测试 - Intel vs NVIDIA
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "decoder.h"

#define TEST_FRAMES 100

static double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void test_backend(DecodeBackend backend, const char* name) {
    printf("\n========== Testing %s ==========\n", name);
    
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
    
    double create_start = get_time_ms();
    DecoderCtx* ctx = decoder_create(&config);
    double create_time = get_time_ms() - create_start;
    
    if (!ctx) {
        printf("[Error] Failed to create decoder\n");
        return;
    }
    
    printf("[OK] Decoder created in %.2f ms\n", create_time);
    printf("[Info] Backend: %s\n", decoder_backend_name(backend));
    
    /* 模拟解码测试 */
    printf("[Test] Simulating %d frames decode...\n", TEST_FRAMES);
    
    double total_decode_time = 0;
    double min_time = 999999, max_time = 0;
    
    for (int i = 0; i < TEST_FRAMES; i++) {
        double start = get_time_ms();
        
        /* 模拟解码工作 */
        usleep(500);  /* 0.5ms 模拟 */
        
        double decode_time = get_time_ms() - start;
        total_decode_time += decode_time;
        
        if (decode_time < min_time) min_time = decode_time;
        if (decode_time > max_time) max_time = decode_time;
    }
    
    double avg_time = total_decode_time / TEST_FRAMES;
    double fps = 1000.0 / avg_time;
    
    printf("\n[Results]\n");
    printf("  Frames: %d\n", TEST_FRAMES);
    printf("  Total time: %.2f ms\n", total_decode_time);
    printf("  Average: %.2f ms/frame\n", avg_time);
    printf("  Min: %.2f ms\n", min_time);
    printf("  Max: %.2f ms\n", max_time);
    printf("  Estimated FPS: %.2f\n", fps);
    printf("  Throughput: %.2f frames/sec\n", fps);
    
    decoder_destroy(ctx);
    printf("========== %s Complete ==========\n", name);
}

int main() {
    printf("=== Decoder Performance Test ===\n");
    printf("Testing Intel VA-API vs NVIDIA NVDEC\n");
    printf("Target resolution: 1280x720\n");
    printf("Test frames: %d\n\n", TEST_FRAMES);
    
    /* 测试 NVIDIA */
    test_backend(DECODE_BACKEND_NVIDIA, "NVIDIA NVDEC");
    
    sleep(1);
    
    /* 测试 Intel */
    test_backend(DECODE_BACKEND_INTEL_VA, "Intel VA-API");
    
    printf("\n=== Test Complete ===\n");
    return 0;
}

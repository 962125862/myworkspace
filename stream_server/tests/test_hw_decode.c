/**
 * @file test_hw_decode.c
 * @brief 测试硬件解码检测
 */

#include <stdio.h>
#include "decoder.h"

int main() {
    printf("=== Hardware Decoder Detection Test ===\n\n");
    
    /* 测试自动检测 */
    printf("[Test] Auto-detecting hardware...\n");
    DecodeBackend backend = decoder_detect_backend();
    printf("[Test] Detected: %s\n\n", decoder_backend_name(backend));
    
    /* 测试创建解码器 */
    DecoderConfig config = {
        .backend = DECODE_BACKEND_AUTO,
        .width = 1280,
        .height = 720,
        .output_format = DECODE_FMT_BGRA,
        .thread_count = 2,
        .va_device = "/dev/dri/renderD128",
        .cuda_device_id = 0
    };
    
    printf("[Test] Creating decoder...\n");
    DecoderCtx* ctx = decoder_create(&config);
    if (!ctx) {
        printf("[Test] Failed to create decoder\n");
        return 1;
    }
    
    printf("[Test] Decoder created successfully\n");
    printf("[Test] Backend: %s\n", decoder_backend_name(config.backend));
    
    decoder_destroy(ctx);
    
    printf("\n=== Test Complete ===\n");
    return 0;
}

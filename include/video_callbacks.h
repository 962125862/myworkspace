#ifndef VIDEO_CALLBACKS_H
#define VIDEO_CALLBACKS_H

#include <stdint.h>
#include <Limelight.h>
#include "tcp_sender.h"

typedef struct {
    /* TCP服务端配置 */
    char tcp_host[256];     /* 服务端IP地址 */
    uint16_t tcp_port;      /* 服务端端口 */
    uint16_t stream_id;     /* 流标识符 (1-65535) */

    /* 视频参数 */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate;

    /* 错误码指针 */
    volatile int* fatal_code;
} WorkerRenderConfig;

extern DECODER_RENDERER_CALLBACKS video_callbacks;

#endif

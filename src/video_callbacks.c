/**
 * @file video_callbacks.c
 * @brief ml_worker 侧视频回调：从 Moonlight embedded(Limelight) 回调获取编码后的 H.264 数据
 *        并通过自定义 TCP 协议推送到 stream_server。
 *
 * 背景：
 * - Moonlight embedded 在回调 submitDecodeUnit() 中提供“decode unit”，实际是编码后的 H.264 bytestream。
 * - 本模块不做转码/不做解码，只做拼包、可选 IDR 识别、发送与统计。
 */

#include "video_callbacks.h"
#include "worker_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* renderer capability：每帧 slice 数（影响回调行为），默认 4 */
#define SLICES_PER_FRAME 4

/* TCP发送器实例：负责把 H.264 bytestream 按 TLV 协议推送到 stream_server */
static TcpSender g_tcp_sender;
static int g_tcp_initialized = 0;
static WorkerRenderConfig* g_cfg = NULL;

/* 预分配帧缓冲区（避免每帧 malloc）：把 decodeUnit 的链表分片拼成连续 bytestream */
static uint8_t* g_frame_buffer = NULL;
static size_t g_frame_buffer_size = 0;

/* 统计信息 */
static uint64_t g_stats_last_ns = 0;
static uint64_t g_stats_frames_received = 0;
static uint64_t g_stats_frames_sent = 0;
static uint64_t g_stats_bytes_sent = 0;

/* 延迟统计 */
static uint64_t g_stats_total_delay_ns = 0;
static uint64_t g_stats_max_delay_ns = 0;
static uint64_t g_stats_delay_samples = 0;

/* 心跳间隔：5秒 */
#define HEARTBEAT_INTERVAL_NS (5ULL * 1000000000ULL)

/* 重连相关 */
static uint64_t g_last_reconnect_ns = 0;
#define RECONNECT_INTERVAL_NS (3ULL * 1000000000ULL)

static const char* video_format_name(int videoFormat) {
    switch (videoFormat) {
        case VIDEO_FORMAT_H264:            return "h264";
        case VIDEO_FORMAT_H264_HIGH8_444:  return "h264_high8_444";
        case VIDEO_FORMAT_H265:            return "hevc";
        case VIDEO_FORMAT_H265_MAIN10:     return "hevc_main10";
        case VIDEO_FORMAT_H265_REXT8_444:  return "hevc_rext8_444";
        case VIDEO_FORMAT_H265_REXT10_444: return "hevc_rext10_444";
        case VIDEO_FORMAT_AV1_MAIN8:       return "av1_main8";
        case VIDEO_FORMAT_AV1_MAIN10:      return "av1_main10";
        case VIDEO_FORMAT_AV1_HIGH8_444:   return "av1_high8_444";
        case VIDEO_FORMAT_AV1_HIGH10_444:  return "av1_high10_444";
        default:                           return "unknown";
    }
}

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void set_fatal_once(int code) {
    if (g_cfg && g_cfg->fatal_code && *g_cfg->fatal_code == WORKER_FATAL_NONE) {
        *g_cfg->fatal_code = code;
    }
}

/* 确保帧缓冲区足够大（零拷贝预分配） */
static int ensure_frame_buffer(size_t needed) {
    if (g_frame_buffer_size >= needed) {
        return 0;
    }
    /* 按2倍增长策略扩容 */
    size_t new_size = g_frame_buffer_size ? g_frame_buffer_size : (1024 * 1024);
    while (new_size < needed) {
        new_size *= 2;
    }
    uint8_t* new_buf = realloc(g_frame_buffer, new_size);
    if (!new_buf) {
        return -1;
    }
    g_frame_buffer = new_buf;
    g_frame_buffer_size = new_size;
    return 0;
}

static void maybe_report_stats(void) {
    uint64_t now = now_monotonic_ns();

    if (g_stats_last_ns == 0) {
        g_stats_last_ns = now;
        return;
    }

    uint64_t delta = now - g_stats_last_ns;
    if (delta >= 1000000000ull) {
        double secs = (double)delta / 1e9;
        double received_fps = g_stats_frames_received / secs;
        double sent_fps = g_stats_frames_sent / secs;
        double sent_mbps = (g_stats_bytes_sent * 8.0) / (secs * 1000000.0);

        /* 计算平均延迟 */
        double avg_delay_ms = 0;
        if (g_stats_delay_samples > 0) {
            avg_delay_ms = (double)(g_stats_total_delay_ns / g_stats_delay_samples) / 1e6;
        }
        double max_delay_ms = (double)g_stats_max_delay_ns / 1e6;

        fprintf(stderr,
                "[video] fps=%.1f/%.1f mbps=%.2f delay=%.2f/%.2fms buf=%.1fKB state=%s\n",
                received_fps, sent_fps, sent_mbps,
                avg_delay_ms, max_delay_ms,
                g_frame_buffer_size / 1024.0,
                tcp_sender_state_str(&g_tcp_sender));

        g_stats_last_ns = now;
        g_stats_frames_received = 0;
        g_stats_frames_sent = 0;
        g_stats_bytes_sent = 0;
        g_stats_total_delay_ns = 0;
        g_stats_max_delay_ns = 0;
        g_stats_delay_samples = 0;
    }
}

/* 尝试连接或重连 */
static int ensure_connected(void) {
    if (!g_tcp_initialized) {
        return -1;
    }

    /* 已连接，检查心跳 */
    if (g_tcp_sender.state == TCP_STATE_CONNECTED) {
        tcp_sender_check_heartbeat(&g_tcp_sender, HEARTBEAT_INTERVAL_NS);
        return 0;
    }

    /* 错误状态，需要重连 */
    if (g_tcp_sender.state == TCP_STATE_ERROR) {
        uint64_t now = now_monotonic_ns();
        if (now - g_last_reconnect_ns < RECONNECT_INTERVAL_NS) {
            return -1; /* 重连间隔未到 */
        }
        fprintf(stderr, "tcp_sender: attempting to reconnect...\n");
        tcp_sender_disconnect(&g_tcp_sender);
    }

    /* 尝试连接 */
    if (tcp_sender_connect(&g_tcp_sender) < 0) {
        g_last_reconnect_ns = now_monotonic_ns();
        return -1;
    }

    return 0;
}

static int worker_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    (void)redrawRate;
    (void)drFlags;

    g_cfg = (WorkerRenderConfig*)context;

    if (!g_cfg) {
        fprintf(stderr, "worker_setup: missing configuration\n");
        return -1;
    }

    /* 初始化TCP发送器配置 */
    TcpSenderConfig tcp_config;
    memset(&tcp_config, 0, sizeof(tcp_config));

    snprintf(tcp_config.host, sizeof(tcp_config.host), "%s",
             g_cfg->tcp_host[0] ? g_cfg->tcp_host : "127.0.0.1");
    tcp_config.port = g_cfg->tcp_port ? g_cfg->tcp_port : 9000;
    tcp_config.stream_id = g_cfg->stream_id ? g_cfg->stream_id : 1;
    tcp_config.width = (uint32_t)width;
    tcp_config.height = (uint32_t)height;
    tcp_config.fps = g_cfg->fps ? g_cfg->fps : 60;
    tcp_config.bitrate = g_cfg->bitrate ? g_cfg->bitrate : 10000;
    tcp_config.codec = g_cfg->codec;
    tcp_config.chroma = g_cfg->chroma;
    tcp_config.bitdepth = g_cfg->bitdepth;
    tcp_config.video_format = (uint32_t)videoFormat;
    tcp_config.color_space = g_cfg->color_space;
    tcp_config.color_range = g_cfg->color_range;

    /* 保存到全局配置 */
    g_cfg->width = (uint32_t)width;
    g_cfg->height = (uint32_t)height;
    g_cfg->video_format = (uint32_t)videoFormat;

    /* 初始化TCP发送器 */
    if (tcp_sender_init(&g_tcp_sender, &tcp_config) < 0) {
        fprintf(stderr, "worker_setup: tcp_sender_init failed\n");
        set_fatal_once(WORKER_FATAL_TCP_CONNECT);
        return -1;
    }

    g_tcp_initialized = 1;

    /* 尝试连接 */
    if (tcp_sender_connect(&g_tcp_sender) < 0) {
        fprintf(stderr, "worker_setup: tcp_sender_connect failed, will retry on first frame\n");
        /* 不立即返回错误，允许后续重连 */
    }

    fprintf(stderr,
            "video worker ready: stream_id=%u -> %s:%d (%dx%d@%u) negotiated=%s(0x%x) yuv444=%d\n",
            tcp_config.stream_id, tcp_config.host, tcp_config.port,
            width, height, tcp_config.fps,
            video_format_name(videoFormat), videoFormat,
            (videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 1 : 0);

    g_stats_last_ns = now_monotonic_ns();
    g_stats_frames_received = 0;
    g_stats_frames_sent = 0;
    g_stats_bytes_sent = 0;
    g_last_reconnect_ns = 0;

    return 0;
}

static void worker_cleanup(void) {
    if (g_tcp_initialized) {
        tcp_sender_destroy(&g_tcp_sender);
        g_tcp_initialized = 0;
    }

    /* 释放预分配缓冲区 */
    free(g_frame_buffer);
    g_frame_buffer = NULL;
    g_frame_buffer_size = 0;

    g_cfg = NULL;
}

/*
 * 检测是否包含 IDR：在 AnnexB bytestream 中扫描 start code，读取 NAL header 的 type。
 * - NAL type 5 表示 IDR
 *
 * 注意：这里只做轻量检测用于统计/策略。
 */
static int is_idr_frame(const uint8_t* data, size_t length) {
    if (length < 5) {
        return 0;
    }

    /* 查找NAL单元起始码 */
    size_t i = 0;
    while (i < length - 4) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            /* 找到起始码 0x00000001 */
            uint8_t nal_unit_type = data[i+4] & 0x1F;
            /* H.264 IDR帧的NAL单元类型为5 */
            if (nal_unit_type == 5) {
                return 1;
            }
            i += 4;
        } else if (i < length - 3 && data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            /* 找到起始码 0x000001 */
            uint8_t nal_unit_type = data[i+3] & 0x1F;
            if (nal_unit_type == 5) {
                return 1;
            }
            i += 3;
        } else {
            i++;
        }
    }

    return 0;
}

static int worker_submit_decode_unit(PDECODE_UNIT decodeUnit) {
    PLENTRY entry = decodeUnit->bufferList;

    g_stats_frames_received++;

    /* 记录帧接收时间（用于延迟统计） */
    uint64_t frame_receive_time = now_monotonic_ns();

    /* 确保连接 */
    if (ensure_connected() < 0) {
        maybe_report_stats();
        return DR_OK;
    }

    /*
     * 收集所有分片到连续缓冲区：
     * decodeUnit->bufferList 是一个链表，每个 entry 是一段 H.264 bytestream。
     * decodeUnit->fullLength 是总长度。
     */
    int total_length = decodeUnit->fullLength;
    if (total_length <= 0) {
        maybe_report_stats();
        return DR_OK;
    }

    /* 零拷贝：使用预分配缓冲区 */
    if (ensure_frame_buffer((size_t)total_length) < 0) {
        fprintf(stderr, "worker_submit_decode_unit: buffer allocation failed for %d bytes\n", total_length);
        maybe_report_stats();
        return DR_OK;
    }

    int offset = 0;
    while (entry != NULL) {
        memcpy(g_frame_buffer + offset, entry->data, entry->length);
        offset += entry->length;
        entry = entry->next;
    }

    /* 理论上 offset == fullLength；若不一致，按实际拼接长度为准，避免越界 */
    if (offset != total_length) {
        total_length = offset;
    }

    /* 检测是否为IDR帧 */
    int is_idr = is_idr_frame(g_frame_buffer, total_length);

    /* 通过 TCP 发送 H.264 bytestream（不转码） */
    if (tcp_sender_send_video(&g_tcp_sender, g_frame_buffer, total_length, is_idr) < 0) {
        fprintf(stderr, "worker_submit_decode_unit: tcp_sender_send_video failed\n");
        set_fatal_once(WORKER_FATAL_TCP_SEND);
        maybe_report_stats();
        return DR_OK;
    }

    /* 计算并记录延迟 */
    uint64_t send_complete_time = now_monotonic_ns();
    uint64_t delay_ns = send_complete_time - frame_receive_time;
    g_stats_total_delay_ns += delay_ns;
    g_stats_delay_samples++;
    if (delay_ns > g_stats_max_delay_ns) {
        g_stats_max_delay_ns = delay_ns;
    }

    g_stats_frames_sent++;
    g_stats_bytes_sent += total_length;

    maybe_report_stats();
    return DR_OK;
}

DECODER_RENDERER_CALLBACKS video_callbacks = {
    .setup = worker_setup,
    .start = NULL,
    .stop = NULL,
    .cleanup = worker_cleanup,
    .submitDecodeUnit = worker_submit_decode_unit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) |
                    CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
                    CAPABILITY_DIRECT_SUBMIT,
};

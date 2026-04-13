/**
 * @file decoder.h
 * @brief 硬件加速视频解码器接口
 *
 * 提供统一的 H.264 解码 API，支持三种后端:
 *   - Intel VA-API:  通过 FFmpeg hwaccel + VA-API 驱动，使用集成显卡
 *   - NVIDIA NVDEC:  通过 FFmpeg hwaccel + CUDA，使用独立显卡的专用解码引擎
 *   - CPU 软解:      FFmpeg 内置 H.264 软件解码器
 *
 * 使用流程:
 *   1. decoder_create()  -- 创建解码器实例，指定后端类型
 *   2. decoder_init()    -- 初始化（创建硬件设备上下文，打开编解码器）
 *   3. decoder_decode()  -- 喂入 H.264 数据，输出解码帧（循环调用）
 *   4. decoder_free_frame() -- 释放输出帧
 *   5. decoder_destroy() -- 销毁解码器，释放所有资源
 *
 * 解码帧支持两种内存模式:
 *   - 零拷贝: av_frame 非空，data[] 指向 AVFrame 内部缓冲区
 *   - 传统拷贝: av_frame 为空，data[] 为 malloc 分配的独立内存
 */

#ifndef DECODER_H
#define DECODER_H

#include "protocol.h"
#include <stdint.h>
#include <stdbool.h>
#include <libavcodec/codec_id.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 枚举类型 ==================== */

/** 解码后端类型 */
typedef enum {
    DECODE_BACKEND_AUTO = 0,       /* 自动检测: 优先 Intel VA-API > NVIDIA > CPU */
    DECODE_BACKEND_INTEL_VA,       /* Intel VA-API (集显硬解) */
    DECODE_BACKEND_NVIDIA,         /* NVIDIA NVDEC (独显硬解) */
    DECODE_BACKEND_CPU             /* FFmpeg CPU 软解 */
} DecodeBackend;

/** 解码输出帧的像素格式 */
typedef enum {
    DECODE_FMT_NONE = 0,
    DECODE_FMT_NV12,               /* YUV420 semi-planar (硬解默认输出) */
    DECODE_FMT_YUV420P,            /* YUV420 planar (软解默认输出) */
    DECODE_FMT_YUV444P,            /* YUV444 planar */
    DECODE_FMT_VUYX,               /* packed VUYX 4:4:4 (Intel VA-API hwdownload on some hosts) */
    DECODE_FMT_BGR24,              /* BGR 24bit (当前 ZMQ bridge 输出) */
    DECODE_FMT_BGRA,               /* BGRA 32bit */
    DECODE_FMT_RGB24               /* RGB 24bit */
} DecodeFormat;

/** 解码帧当前存放位置 */
typedef enum {
    DECODE_STORAGE_CPU = 0,        /* data[] 指向可直接访问的 CPU 内存 */
    DECODE_STORAGE_HW              /* av_frame 为硬件帧句柄，需按需下载 */
} DecodeStorage;

/* ==================== 解码帧 ==================== */

/**
 * @brief 解码输出帧
 *
 * 支持两种内存模式:
 * - 零拷贝模式: av_frame 非空，data[]/linesize[] 指向 AVFrame 内部缓冲
 *   调用 decoder_free_frame() 时释放 AVFrame 引用
 * - 传统模式: av_frame 为空，data[] 为独立 malloc 的内存
 *   调用 decoder_free_frame() 时逐个 free(data[i])
 */
typedef struct {
    void* av_frame;              /* AVFrame* (零拷贝时非空，由解码器管理) */
    uint8_t* data[4];            /* 平面数据指针 (Y/U/V/A 或 packed BGRA) */
    int linesize[4];             /* 每行字节数 (含对齐 padding) */
    int width;                   /* 帧宽度 (像素) */
    int height;                  /* 帧高度 (像素) */
    DecodeFormat format;         /* 像素格式 */
    DecodeStorage storage;       /* CPU 可访问帧 / 硬件帧 */
    int64_t pts;                 /* 展示时间戳 */
    bool key_frame;              /* 是否关键帧 (IDR) */
    void* _decoder_ctx;          /* 内部使用: 硬件帧传输统计上下文，外部不要访问 */
} DecodedFrame;

/* ==================== 解码器配置 ==================== */

/** 解码器创建参数 */
typedef struct {
    DecodeBackend backend;       /* 后端类型 (AUTO 会自动检测) */
    enum AVCodecID codec_id;     /* H264 / HEVC / AV1 */
    int width;                   /* 视频宽度 */
    int height;                  /* 视频高度 */
    DecodeFormat output_format;  /* 期望输出格式 */
    int thread_count;            /* CPU 软解线程数 (硬解忽略) */
    char va_device[64];          /* VA-API 设备路径 (如 /dev/dri/renderD128) */
    int cuda_device_id;          /* NVIDIA GPU 设备 ID (通常为 0) */
    bool defer_hw_download;      /* true: last_frame 保留硬件帧，按需下载 */
    int extra_hw_frames;         /* NVDEC/VAAPI 额外硬件帧池余量 */
} DecoderConfig;

/* ==================== 解码器上下文 ==================== */

/** 不透明的解码器上下文 (内部包含 FFmpeg 对象) */
typedef struct DecoderCtx DecoderCtx;

/* ==================== 核心 API ==================== */

/**
 * @brief 自动检测可用的硬件解码后端
 * @return 推荐的后端类型 (Intel VA-API > NVIDIA > CPU)
 *
 * 通过尝试创建 CUDA/VA-API 设备上下文来检测。
 * 创建成功则表示该后端可用，随即释放检测用的上下文。
 */
DecodeBackend decoder_detect_backend(void);

/** 获取后端的可读名称字符串 */
const char* decoder_backend_name(DecodeBackend backend);

/**
 * @brief 创建解码器实例
 * @param config 配置参数
 * @return 解码器上下文，失败返回 NULL
 *
 * 分配解码器上下文、AVCodecContext、AVFrame、AVPacket、H264 Parser。
 * 此时尚未打开编解码器，需后续调用 decoder_init()。
 */
DecoderCtx* decoder_create(const DecoderConfig* config);

/** 销毁解码器，释放所有资源 */
void decoder_destroy(DecoderCtx* ctx);

/**
 * @brief 初始化解码器（打开编解码器，创建硬件设备上下文）
 * @param ctx           解码器上下文
 * @param extradata     H.264 extradata (SPS/PPS)，可为 NULL
 * @param extradata_size extradata 大小
 * @return 0 成功，-1 失败
 *
 * 对于 VA-API: 创建 VAAPI 设备，设置 get_format/get_buffer2 回调
 * 对于 CUDA:   创建 CUDA 设备，设置 get_format 回调 + extra_hw_frames
 *              (参考 moonlight-qt: 不手动创建 hw_frames_ctx)
 * 对于 CPU:    设置多线程 slice 解码
 */
int decoder_init(DecoderCtx* ctx, const uint8_t* extradata, int extradata_size);

/**
 * @brief 解码一块 H.264 数据
 * @param ctx       解码器上下文
 * @param data      H.264 NAL 单元数据 (含 start code)
 * @param size      数据大小
 * @param out_frame 输出: 解码帧指针，需调用 decoder_free_frame() 释放
 * @return 0 成功得到帧，1 需要更多数据 (EAGAIN)，-1 错误
 *
 * 内部流程:
 *   1. av_parser_parse2() 循环消费全部输入（关键! 否则帧率减半）
 *   2. avcodec_send_packet() 送入解码器，EAGAIN 时先 drain 再重试
 *   3. avcodec_receive_frame() 循环取出所有解码帧，只保留最后一帧
 *   4. 根据配置选择：
 *      - 立即 av_hwframe_transfer_data() 下载到系统内存
 *      - 或保留硬件帧引用，后续按需 materialize
 */
int decoder_decode(DecoderCtx* ctx, const uint8_t* data, int size,
                   DecodedFrame** out_frame);

/** 刷新解码器，取出内部缓冲的剩余帧 */
int decoder_flush(DecoderCtx* ctx, DecodedFrame** out_frame);

/** 释放解码帧（自动区分零拷贝/传统模式） */
void decoder_free_frame(DecodedFrame* frame);

/** 引用/复制一帧，调用方需用 decoder_free_frame() 释放 */
DecodedFrame* decoder_ref_frame(const DecodedFrame* frame);

/** 硬件帧按需下载到 CPU；CPU 帧则返回一份引用/副本 */
int decoder_materialize_frame(const DecodedFrame* src, DecodedFrame** out_frame);

/**
 * @brief 像素格式转换 (支持 NV12/YUV420P/YUV444P/VUYX -> BGR24/BGRA/RGB24)
 * @note  内部会优先使用已知的快速路径；无法匹配时回退到 FFmpeg swscale
 */
int decoder_convert_format(DecoderCtx* ctx, const DecodedFrame* src,
                           DecodedFrame* dst, DecodeFormat target_format);

/**
 * @brief 带流颜色信息的像素格式转换
 * @note  对 BGR24 转换会优先使用 StreamInfo 中的 colorspace/range；适合对外输出链路
 */
int decoder_convert_format_with_info(DecoderCtx* ctx, const DecodedFrame* src,
                                     const StreamInfo* info,
                                     DecodedFrame* dst, DecodeFormat target_format);

/* ==================== 统计信息 ==================== */

/** 解码器性能统计 */
typedef struct {
    uint64_t frames_decoded;     /* 成功解码帧数 */
    uint64_t frames_dropped;     /* 丢弃帧数 (错误/分配失败) */
    uint64_t bytes_in;           /* 输入字节总量 */
    double avg_decode_time_ms;   /* 平均每帧解码耗时 (ms) */
    uint64_t hw_transfer_count;  /* av_hwframe_transfer_data() 次数 */
    double avg_hw_transfer_time_ms; /* 平均每次下载耗时 (ms) */
} DecoderStats;

void decoder_get_stats(DecoderCtx* ctx, DecoderStats* stats);

/** 全局像素格式转换路径统计（进程级累积） */
typedef struct {
    uint64_t bgr24_request_count;        /* 请求转成 BGR24 的总次数 */
    uint64_t libyuv_bgr24_count;         /* 命中 NV12/YUV420P/YUV444P libyuv 快路径次数 */
    double avg_libyuv_bgr24_time_ms;     /* libyuv 快路径平均耗时 */
    uint64_t vuyx_bgr24_count;           /* 命中 VUYX 专用快路径次数 */
    double avg_vuyx_bgr24_time_ms;       /* VUYX 专用快路径平均耗时 */
    uint64_t swscale_count;              /* 回退到 swscale 的次数 */
    double avg_swscale_time_ms;          /* swscale 平均耗时 */
} DecoderConvertStats;

void decoder_get_convert_stats(DecoderConvertStats* stats);

/** GPU 硬件状态信息 */
typedef struct {
    bool available;              /* 信息是否有效 */
    char name[64];               /* GPU 名称 */
    int gpu_utilization;         /* GPU 利用率 (0-100%) */
    int memory_utilization;      /* 显存利用率 (0-100%) */
    int temperature;             /* 温度 (摄氏度) */
    uint64_t memory_used;        /* 已用显存 (MB) */
    uint64_t memory_total;       /* 总显存 (MB) */
} GPUStats;

/** 获取 NVIDIA GPU 状态 (通过调用 nvidia-smi) */
int decoder_get_nvidia_stats(int device_id, GPUStats* stats);

/** 获取 Intel GPU 状态 (通过 intel_gpu_top 或 sysfs) */
int decoder_get_intel_stats(GPUStats* stats);

#ifdef __cplusplus
}
#endif

#endif /* DECODER_H */

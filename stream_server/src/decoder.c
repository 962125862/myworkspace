/**
 * @file decoder.c
 * @brief 硬件解码器实现（FFmpeg + VA-API + NVDEC）
 */

#define _GNU_SOURCE
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* FFmpeg 头文件 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* Intel VA-API */
#ifdef HAVE_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#endif

/* NVIDIA CUDA */
#ifdef HAVE_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif

/* 最大帧缓冲数 (参考 embedded) */
#define MAX_SURFACES 16

/* 解码器上下文 */
struct DecoderCtx {
    DecoderConfig config;
    
    /* FFmpeg */
    AVCodecContext* codec_ctx;
    const AVCodec* codec;
    AVFrame* frame;
    AVPacket* packet;
    
    /* H264 Parser - 自动解析 SPS/PPS */
    AVCodecParserContext* parser;
    
    /* 硬件设备 */
    AVBufferRef* hw_device_ctx;
    enum AVPixelFormat hw_pix_fmt;
    
    /* 格式转换 */
    struct SwsContext* sws_ctx;
    
    /* 状态 */
    bool initialized;
    int64_t pts_counter;
    
    /* NAL 统计 (调试用) */
    int nal_sps_count;
    int nal_pps_count;
    int nal_idr_count;
    
    /* 统计 */
    DecoderStats stats;
    double total_decode_time;
};

/* 检测硬件支持 */
DecodeBackend decoder_detect_backend(void) {
    /* 优先检测 NVIDIA */
    AVBufferRef* cuda_ctx = NULL;
    if (av_hwdevice_ctx_create(&cuda_ctx, AV_HWDEVICE_TYPE_CUDA, 
                               NULL, NULL, 0) >= 0) {
        av_buffer_unref(&cuda_ctx);
        printf("[Decoder] NVIDIA CUDA detected\n");
        return DECODE_BACKEND_NVIDIA;
    }
    
    /* 检测 Intel VA-API - 尝试多个设备 */
    const char* va_devices[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        NULL
    };
    
    for (int i = 0; va_devices[i]; i++) {
        AVBufferRef* va_ctx = NULL;
        if (av_hwdevice_ctx_create(&va_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                   va_devices[i], NULL, 0) >= 0) {
            av_buffer_unref(&va_ctx);
            printf("[Decoder] Intel VA-API detected (%s)\n", va_devices[i]);
            return DECODE_BACKEND_INTEL_VA;
        }
    }
    
    printf("[Decoder] No hardware acceleration, using CPU\n");
    return DECODE_BACKEND_CPU;
}

const char* decoder_backend_name(DecodeBackend backend) {
    switch (backend) {
        case DECODE_BACKEND_INTEL_VA: return "Intel VA-API";
        case DECODE_BACKEND_NVIDIA:   return "NVIDIA NVDEC";
        case DECODE_BACKEND_CPU:      return "CPU (FFmpeg)";
        default:                      return "Auto";
    }
}

/* 获取硬件像素格式 (参考 embedded ffmpeg_vaapi.c) */
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, 
                                        const enum AVPixelFormat* pix_fmts) {
    DecoderCtx* dec_ctx = (DecoderCtx*)ctx->opaque;
    const enum AVPixelFormat* p;
    
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == dec_ctx->hw_pix_fmt) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

/* VA-API 格式回调 (参考 embedded ffmpeg_vaapi.c va_get_format) */
static enum AVPixelFormat vaapi_get_format(AVCodecContext* ctx,
                                           const enum AVPixelFormat* pix_fmts) {
    (void)pix_fmts;
    DecoderCtx* dec_ctx = (DecoderCtx*)ctx->opaque;

    /* 创建硬件帧上下文 */
    AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(dec_ctx->hw_device_ctx);
    if (!hw_frames_ref) {
        fprintf(stderr, "[Decoder] Failed to allocate VAAPI frame context\n");
        return AV_PIX_FMT_NONE;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = ctx->coded_width;
    frames_ctx->height = ctx->coded_height;
    frames_ctx->initial_pool_size = MAX_SURFACES + 1;

    if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
        fprintf(stderr, "[Decoder] Failed to initialize VAAPI frame context\n");
        av_buffer_unref(&hw_frames_ref);
        return AV_PIX_FMT_NONE;
    }

    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->hw_device_ctx = av_buffer_ref(dec_ctx->hw_device_ctx);
    ctx->hw_frames_ctx = hw_frames_ref;

    printf("[Decoder] VAAPI frame context initialized (%dx%d)\n",
           ctx->coded_width, ctx->coded_height);
    return AV_PIX_FMT_VAAPI;
}

/*
 * 硬件缓冲区分配回调 (VA-API 使用)
 * NVIDIA CUDA 不需要此回调 - FFmpeg 会自动管理
 */
static int hw_get_buffer(AVCodecContext* ctx, AVFrame* frame, int flags) {
    (void)flags;
    return av_hwframe_get_buffer(ctx->hw_frames_ctx, frame, 0);
}

/*
 * 计算给定像素格式下各 plane 的有效高度（用于深拷贝）。
 * 注意：frame->linesize 可能包含 padding，但“行数”必须按格式计算。
 */
static int plane_height_for_pix_fmt(enum AVPixelFormat fmt, int plane, int height) {
    switch (fmt) {
        case AV_PIX_FMT_NV12:
            /* Y: H, UV: H/2 */
            return (plane == 0) ? height : (plane == 1 ? height / 2 : 0);
        case AV_PIX_FMT_YUV420P:
            /* Y: H, U/V: H/2 */
            return (plane == 0) ? height : ((plane == 1 || plane == 2) ? height / 2 : 0);
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_RGB24:
            /* packed */
            return (plane == 0) ? height : 0;
        default:
            /* 其它格式：保守处理，只拷贝 plane0，避免越界 */
            return (plane == 0) ? height : 0;
    }
}

DecoderCtx* decoder_create(const DecoderConfig* config) {
    DecoderCtx* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    
    memcpy(&ctx->config, config, sizeof(*config));
    
    /* 自动检测后端 */
    if (ctx->config.backend == DECODE_BACKEND_AUTO) {
        ctx->config.backend = decoder_detect_backend();
    }
    
    /* 查找解码器 */
    switch (ctx->config.backend) {
        case DECODE_BACKEND_INTEL_VA:
            /* 
             * Intel VA-API 硬件解码 (参考 embedded ffmpeg.c)
             * 使用通用 h264 解码器 + VA-API hwaccel
             * 这种方式比 h264_qsv 更稳定，不需要 Intel Media SDK
             */
            ctx->codec = avcodec_find_decoder_by_name("h264");
            if (!ctx->codec) {
                fprintf(stderr, "[Decoder] H.264 decoder not found\n");
                free(ctx);
                return NULL;
            }
            printf("[Decoder] Using H.264 decoder with VA-API hwaccel\n");
            break;
            
        case DECODE_BACKEND_NVIDIA:
            /* 
             * NVIDIA NVDEC 硬件解码
             * 使用通用 h264 解码器 + CUDA hwaccel
             * 配合 H264 parser 自动解析 SPS/PPS
             */
            ctx->codec = avcodec_find_decoder_by_name("h264");
            if (!ctx->codec) {
                fprintf(stderr, "[Decoder] H.264 decoder not found\n");
                free(ctx);
                return NULL;
            }
            printf("[Decoder] Using H.264 decoder with CUDA hwaccel\n");
            break;
            
        default:
            ctx->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            break;
    }
    
    if (!ctx->codec) {
        fprintf(stderr, "[Decoder] H.264 decoder not found\n");
        free(ctx);
        return NULL;
    }
    
    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx) {
        free(ctx);
        return NULL;
    }
    
    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    
    if (!ctx->frame || !ctx->packet) {
        decoder_destroy(ctx);
        return NULL;
    }
    
    /* 初始化 H264 Parser - 自动解析 SPS/PPS */
    ctx->parser = av_parser_init(AV_CODEC_ID_H264);
    if (!ctx->parser) {
        fprintf(stderr, "[Decoder] Failed to create H264 parser\n");
        decoder_destroy(ctx);
        return NULL;
    }
    
    printf("[Decoder] Created (%s)\n", decoder_backend_name(ctx->config.backend));
    return ctx;
}

void decoder_destroy(DecoderCtx* ctx) {
    if (!ctx) return;
    
    if (ctx->parser) av_parser_close(ctx->parser);
    if (ctx->codec_ctx) {
        avcodec_free_context(&ctx->codec_ctx);
    }
    if (ctx->frame) av_frame_free(&ctx->frame);
    if (ctx->packet) av_packet_free(&ctx->packet);
    if (ctx->hw_device_ctx) av_buffer_unref(&ctx->hw_device_ctx);
    if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
    
    free(ctx);
}

int decoder_init(DecoderCtx* ctx, const uint8_t* extradata, int extradata_size) {
    if (!ctx || ctx->initialized) return -1;
    
    ctx->codec_ctx->opaque = ctx;
    
    /* 设置硬件加速 */
    if (ctx->config.backend == DECODE_BACKEND_INTEL_VA) {
        /* 
         * Intel VA-API 初始化 (参考 embedded ffmpeg_vaapi.c)
         * 1. 创建 VA-API 设备上下文
         * 2. 设置 get_format 和 get_buffer2 回调
         */
        ctx->hw_pix_fmt = AV_PIX_FMT_VAAPI;
        
        /* 尝试多个 DRM 设备 */
        const char* va_devices[] = {
            "/dev/dri/renderD128",
            "/dev/dri/renderD129",
            NULL
        };
        
        int va_init_ok = 0;
        for (int i = 0; va_devices[i] && !va_init_ok; i++) {
            if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                       va_devices[i], NULL, 0) >= 0) {
                printf("[Decoder] VA-API device created: %s\n", va_devices[i]);
                va_init_ok = 1;
            }
        }
        
        if (!va_init_ok) {
            fprintf(stderr, "[Decoder] Failed to create VA-API device, fallback to CPU\n");
            ctx->config.backend = DECODE_BACKEND_CPU;
            ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
        } else {
            /* 设置回调函数 (关键!) */
            ctx->codec_ctx->get_format = vaapi_get_format;
            ctx->codec_ctx->get_buffer2 = hw_get_buffer;
        }
    }
    else if (ctx->config.backend == DECODE_BACKEND_NVIDIA) {
        /*
         * NVIDIA CUDA 初始化 (参考 moonlight-qt CUDARenderer)
         * 关键：只设置 hw_device_ctx，让 FFmpeg/NVDEC 内部创建 hw_frames_ctx
         * 不要手动创建 hw_frames_ctx，否则会干扰 NVDEC 内部帧管理
         */
        ctx->hw_pix_fmt = AV_PIX_FMT_CUDA;

        /* 创建 CUDA 设备上下文 */
        char cuda_dev[16];
        snprintf(cuda_dev, sizeof(cuda_dev), "%d", ctx->config.cuda_device_id);

        int ret = av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA,
                                         cuda_dev, NULL, 0);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[Decoder] Failed to create CUDA device: %s\n", errbuf);
            ctx->config.backend = DECODE_BACKEND_CPU;
            ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
        } else {
            /* 只设置 get_format 选择像素格式，不手动创建 hw_frames_ctx */
            ctx->codec_ctx->get_format = get_hw_format;
            ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);

            /* 额外帧池大小 (参考 moonlight-qt extra_hw_frames) */
            ctx->codec_ctx->extra_hw_frames = 4;

            /* 硬件解码不需要多线程 */
            ctx->codec_ctx->thread_count = 1;

            printf("[Decoder] NVIDIA CUDA device created (GPU %d)\n", ctx->config.cuda_device_id);
        }
    }
    
    /* 设置 extradata (SPS/PPS) */
    if (extradata && extradata_size > 0) {
        ctx->codec_ctx->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ctx->codec_ctx->extradata) {
            memcpy(ctx->codec_ctx->extradata, extradata, extradata_size);
            ctx->codec_ctx->extradata_size = extradata_size;
        }
    }
    
    /* 
     * 关键配置 (参考 moonlight-qt ffmpeg.cpp)
     */
    /* 设置视频尺寸 */
    ctx->codec_ctx->width = ctx->config.width;
    ctx->codec_ctx->height = ctx->config.height;
    
    /* 低延迟模式 */
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    
    /* 允许输出损坏帧和缺少引用的帧 */
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx->codec_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    
    /* 设置时间基 */
    ctx->codec_ctx->pkt_timebase.num = 1;
    ctx->codec_ctx->pkt_timebase.den = 90000;
    
    /* 线程数 */
    if (ctx->config.backend == DECODE_BACKEND_CPU) {
        ctx->codec_ctx->thread_type = FF_THREAD_SLICE;
        ctx->codec_ctx->thread_count = ctx->config.thread_count > 0 ? ctx->config.thread_count : 4;
    }
    
    /* 打开解码器 */
    if (avcodec_open2(ctx->codec_ctx, ctx->codec, NULL) < 0) {
        fprintf(stderr, "[Decoder] Failed to open codec\n");
        return -1;
    }
    
    ctx->initialized = true;
    printf("[Decoder] Initialized (%dx%d)\n", ctx->config.width, ctx->config.height);
    return 0;
}

int decoder_decode(DecoderCtx* ctx, const uint8_t* data, int size, 
                   DecodedFrame** out_frame) {
    if (!ctx || !ctx->initialized || !out_frame) return -1;
    
    *out_frame = NULL;
    
    /* 统计 NAL 类型 (调试用) - 支持 3/4 字节 start code */
    int nal_type = -1;
    if (size >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        nal_type = data[4] & 0x1F;  /* 4字节 start code */
    } else if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        nal_type = data[3] & 0x1F;  /* 3字节 start code */
    }
    
    if (nal_type >= 0) {
        static int nal_log_count = 0;
        if (nal_type == 7) {
            ctx->nal_sps_count++;
            if (nal_log_count++ < 10) printf("[Decoder] NAL: SPS (total: %d)\n", ctx->nal_sps_count);
        } else if (nal_type == 8) {
            ctx->nal_pps_count++;
            if (nal_log_count++ < 10) printf("[Decoder] NAL: PPS (total: %d)\n", ctx->nal_pps_count);
        } else if (nal_type == 5) {
            ctx->nal_idr_count++;
            if (nal_log_count++ < 10) printf("[Decoder] NAL: IDR (total: %d)\n", ctx->nal_idr_count);
        } else if (nal_log_count < 3) {
            printf("[Decoder] NAL: type %d, size %d\n", nal_type, size);
            nal_log_count++;
        }
    }
    
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    
    /*
     * 使用 parser 循环解析输入数据
     * 关键：av_parser_parse2 可能只消费部分输入，必须循环消费全部
     * 否则剩余数据丢失，导致帧率减半（Frames:Decoded = 2:1）
     */
    DecodedFrame* last_frame = NULL;
    int total_decoded = 0;
    const uint8_t* parse_ptr = data;
    int parse_remaining = size;
    
    while (parse_remaining > 0) {
        uint8_t* pkt_data = NULL;
        int pkt_size = 0;
        int parsed = av_parser_parse2(ctx->parser, ctx->codec_ctx,
                                      &pkt_data, &pkt_size,
                                      parse_ptr, parse_remaining,
                                      AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        
        if (parsed < 0) {
            ctx->stats.frames_dropped++;
            break;
        }
        
        /* 推进输入指针 */
        parse_ptr += parsed;
        parse_remaining -= parsed;
        
        /* parser 还没有输出完整 packet，继续消费 */
        if (pkt_size == 0) {
            continue;
        }
        
        /* 准备 packet */
        av_packet_unref(ctx->packet);
        ctx->packet->data = pkt_data;
        ctx->packet->size = pkt_size;
        ctx->packet->pts = ctx->pts_counter++;
        
        /* 发送 packet */
        int ret = avcodec_send_packet(ctx->codec_ctx, ctx->packet);
        if (ret == AVERROR(EAGAIN)) {
            /* 解码器内部缓冲区满，先 drain 再重试 */
            av_frame_unref(ctx->frame);
            avcodec_receive_frame(ctx->codec_ctx, ctx->frame);
            ret = avcodec_send_packet(ctx->codec_ctx, ctx->packet);
        }
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            static int err_log_count = 0;
            if (err_log_count++ < 5) {
                fprintf(stderr, "[Decoder] send_packet error: %s\n", errbuf);
            }
            ctx->stats.frames_dropped++;
            continue;  /* 继续处理剩余数据 */
        }
        
        /* 循环接收所有可用帧 */
        while (1) {
            av_frame_unref(ctx->frame);
            ret = avcodec_receive_frame(ctx->codec_ctx, ctx->frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                static int err_count = 0;
                if (err_count++ < 5) {
                    fprintf(stderr, "[Decoder] receive_frame error: %s (fmt=%d, hw=%d)\n", 
                            errbuf, ctx->frame->format, ctx->hw_pix_fmt);
                }
                break;
            }
            
            /* 成功解码一帧 */
            total_decoded++;
            ctx->stats.frames_decoded++;
            
            /* 释放之前缓存的帧（只保留最新帧） */
            if (last_frame) {
                decoder_free_frame(last_frame);
                last_frame = NULL;
            }
            
            /* 处理硬件帧 - 从 GPU 下载到 CPU */
            AVFrame* sw_frame = ctx->frame;
            AVFrame* tmp_frame = NULL;
            
            if (ctx->config.backend == DECODE_BACKEND_NVIDIA || 
                ctx->config.backend == DECODE_BACKEND_INTEL_VA) {
                if (ctx->frame->format == ctx->hw_pix_fmt) {
                    tmp_frame = av_frame_alloc();
                    if (!tmp_frame) {
                        ctx->stats.frames_dropped++;
                        continue;
                    }
                    
                    ret = av_hwframe_transfer_data(tmp_frame, ctx->frame, 0);
                    if (ret < 0) {
                        av_frame_free(&tmp_frame);
                        ctx->stats.frames_dropped++;
                        continue;
                    }
                    
                    av_frame_copy_props(tmp_frame, ctx->frame);
                    sw_frame = tmp_frame;
                }
            }
            
            /* 分配输出帧 */
            DecodedFrame* frame = calloc(1, sizeof(*frame));
            if (!frame) {
                if (tmp_frame) av_frame_free(&tmp_frame);
                continue;
            }
            
            frame->width = sw_frame->width;
            frame->height = sw_frame->height;
            frame->pts = sw_frame->pts;
            frame->key_frame = !!(sw_frame->flags & AV_FRAME_FLAG_KEY);
            frame->_decoder_ctx = ctx;
            
            if (sw_frame->format == AV_PIX_FMT_NV12) {
                frame->format = DECODE_FMT_NV12;
            } else if (sw_frame->format == AV_PIX_FMT_YUV420P) {
                frame->format = DECODE_FMT_YUV420P;
            } else {
                frame->format = DECODE_FMT_NONE;
            }
            
            if (tmp_frame) {
                frame->av_frame = tmp_frame;
            } else {
                frame->av_frame = NULL;
                for (int i = 0; i < 4; i++) {
                    if (sw_frame->data[i]) {
                        int lines = plane_height_for_pix_fmt((enum AVPixelFormat)sw_frame->format,
                                                           i, frame->height);
                        if (lines <= 0) {
                            continue;
                        }
                        frame->linesize[i] = sw_frame->linesize[i];
                        frame->data[i] = malloc(frame->linesize[i] * lines);
                        if (frame->data[i]) {
                            memcpy(frame->data[i], sw_frame->data[i],
                                   (size_t)frame->linesize[i] * (size_t)lines);
                        }
                    }
                }
            }
            
            if (frame->av_frame) {
                AVFrame* avf = (AVFrame*)frame->av_frame;
                for (int i = 0; i < 4; i++) {
                    frame->data[i] = avf->data[i];
                    frame->linesize[i] = avf->linesize[i];
                }
            }
            
            last_frame = frame;
        }
    } /* end parser loop */
    
    /* 统计本次调用处理的字节数 (只计一次，避免 parser 循环重复计) */
    if (total_decoded > 0) {
        ctx->stats.bytes_in += size;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double decode_time = (end_ts.tv_sec - start_ts.tv_sec) * 1000.0 + 
                         (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000.0;
    ctx->total_decode_time += decode_time;
    if (ctx->stats.frames_decoded > 0) {
        ctx->stats.avg_decode_time_ms = ctx->total_decode_time / ctx->stats.frames_decoded;
    }
    
    if (last_frame) {
        *out_frame = last_frame;
        return 0;
    }
    
    if (total_decoded == 0) {
        return 1;  /* 需要更多数据 */
    }
    ctx->stats.frames_dropped++;
    return -1;
}

void decoder_free_frame(DecodedFrame* frame) {
    if (!frame) return;
    
    /* 零拷贝模式：释放 AVFrame 引用 */
    if (frame->av_frame) {
        AVFrame* avf = (AVFrame*)frame->av_frame;
        av_frame_free(&avf);  /* 这会减少引用计数并释放内存 */
        frame->av_frame = NULL;
    } else {
        /* 传统模式：释放数据缓冲区 */
        for (int i = 0; i < 4; i++) {
            free(frame->data[i]);
        }
    }
    
    free(frame);
}

int decoder_flush(DecoderCtx* ctx, DecodedFrame** out_frame) {
    if (!ctx || !ctx->initialized) return -1;
    
    *out_frame = NULL;
    
    /* 发送 NULL packet 刷新解码器 */
    int ret = avcodec_send_packet(ctx->codec_ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        return -1;
    }
    
    /* 接收缓冲的帧 */
    ret = avcodec_receive_frame(ctx->codec_ctx, ctx->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 1;  /* 没有更多帧 */
    }
    if (ret < 0) {
        return -1;
    }
    
    /* 分配输出帧，完整复制数据（与 decoder_decode 保持一致） */
    DecodedFrame* frame = calloc(1, sizeof(*frame));
    if (!frame) return -1;

    frame->width = ctx->frame->width;
    frame->height = ctx->frame->height;
    frame->pts = ctx->frame->pts;
    frame->key_frame = !!(ctx->frame->flags & AV_FRAME_FLAG_KEY);
    frame->_decoder_ctx = ctx;

    /* 硬件帧需要先下载到 CPU */
    AVFrame* sw_frame = ctx->frame;
    AVFrame* tmp_frame = NULL;
    if (ctx->config.backend == DECODE_BACKEND_NVIDIA ||
        ctx->config.backend == DECODE_BACKEND_INTEL_VA) {
        if (ctx->frame->format == ctx->hw_pix_fmt) {
            tmp_frame = av_frame_alloc();
            if (!tmp_frame) { free(frame); return -1; }
            if (av_hwframe_transfer_data(tmp_frame, ctx->frame, 0) < 0) {
                av_frame_free(&tmp_frame);
                free(frame);
                return -1;
            }
            av_frame_copy_props(tmp_frame, ctx->frame);
            sw_frame = tmp_frame;
        }
    }

    if (sw_frame->format == AV_PIX_FMT_NV12) {
        frame->format = DECODE_FMT_NV12;
    } else if (sw_frame->format == AV_PIX_FMT_YUV420P) {
        frame->format = DECODE_FMT_YUV420P;
    } else {
        frame->format = DECODE_FMT_NONE;
    }

    if (tmp_frame) {
        /* 零拷贝：frame 持有 AVFrame 所有权 */
        frame->av_frame = tmp_frame;
        for (int i = 0; i < 4; i++) {
            frame->data[i] = tmp_frame->data[i];
            frame->linesize[i] = tmp_frame->linesize[i];
        }
    } else {
        /* CPU 帧：深拷贝数据 */
        for (int i = 0; i < 4; i++) {
            if (sw_frame->data[i]) {
                int lines = plane_height_for_pix_fmt((enum AVPixelFormat)sw_frame->format,
                                                   i, frame->height);
                if (lines <= 0) {
                    continue;
                }
                frame->linesize[i] = sw_frame->linesize[i];
                frame->data[i] = malloc(frame->linesize[i] * lines);
                if (frame->data[i]) {
                    memcpy(frame->data[i], sw_frame->data[i],
                           (size_t)frame->linesize[i] * lines);
                }
            }
        }
    }

    *out_frame = frame;
    return 0;
}

int decoder_convert_format(DecoderCtx* ctx, const DecodedFrame* src,
                           DecodedFrame* dst, DecodeFormat target_format) {
    if (!ctx || !src || !dst) return -1;
    
    /* 允许 NV12 / YUV420P -> BGRA（CPU fallback 时通常为 YUV420P） */
    if ((src->format != DECODE_FMT_NV12 && src->format != DECODE_FMT_YUV420P) ||
        target_format != DECODE_FMT_BGRA) {
        return -1;
    }

    enum AVPixelFormat src_pix_fmt = (src->format == DECODE_FMT_NV12)
                                       ? AV_PIX_FMT_NV12
                                       : AV_PIX_FMT_YUV420P;
    
    /* 初始化 SwsContext（如果未初始化） */
    if (!ctx->sws_ctx) {
        ctx->sws_ctx = sws_getContext(
            src->width, src->height, src_pix_fmt,
            src->width, src->height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, NULL, NULL, NULL
        );
        if (!ctx->sws_ctx) {
            return -1;
        }
    }
    
    /* 分配目标帧内存 */
    dst->width = src->width;
    dst->height = src->height;
    dst->format = target_format;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    
    /* BGRA 每像素 4 字节 */
    dst->linesize[0] = src->width * 4;
    dst->data[0] = malloc(dst->linesize[0] * src->height);
    if (!dst->data[0]) {
        return -1;
    }
    
    /* 执行转换 */
    const uint8_t* src_data[4] = { src->data[0], src->data[1], src->data[2], NULL };
    uint8_t* dst_data[4] = { dst->data[0], NULL, NULL, NULL };
    int src_linesize[4] = { src->linesize[0], src->linesize[1], 0, 0 };
    int dst_linesize[4] = { dst->linesize[0], 0, 0, 0 };

    if (src->format == DECODE_FMT_YUV420P) {
        src_linesize[2] = src->linesize[2];
    }
    
    sws_scale(ctx->sws_ctx, src_data, src_linesize, 0, src->height,
              dst_data, dst_linesize);
    
    return 0;
}

void decoder_get_stats(DecoderCtx* ctx, DecoderStats* stats) {
    if (!ctx || !stats) return;
    memcpy(stats, &ctx->stats, sizeof(*stats));
}

int decoder_get_nvidia_stats(int device_id, GPUStats* stats) {
    if (!stats) return -1;
    
    memset(stats, 0, sizeof(*stats));
    stats->available = false;
    
    /* 使用 nvidia-smi 获取 GPU 信息 */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "nvidia-smi --query-gpu=name,utilization.gpu,utilization.memory,"
             "temperature.gpu,memory.used,memory.total --format=csv,noheader,nounits "
             "-i %d 2>/dev/null",
             device_id);
    
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    
    char line[256];
    if (fgets(line, sizeof(line), fp) != NULL) {
        /* 解析 CSV 输出: name, gpu_util, mem_util, temp, mem_used, mem_total */
        char name[64];
        int gpu_util, mem_util, temp;
        uint64_t mem_used, mem_total;
        
        if (sscanf(line, "%63[^,], %d, %d, %d, %lu, %lu",
                   name, &gpu_util, &mem_util, &temp, &mem_used, &mem_total) == 6) {
            snprintf(stats->name, sizeof(stats->name), "%s", name);
            stats->gpu_utilization = gpu_util;
            stats->memory_utilization = mem_util;
            stats->temperature = temp;
            stats->memory_used = mem_used;
            stats->memory_total = mem_total;
            stats->available = true;
        }
    }
    
    pclose(fp);
    return stats->available ? 0 : -1;
}

int decoder_get_intel_stats(GPUStats* stats) {
    if (!stats) return -1;
    
    memset(stats, 0, sizeof(*stats));
    stats->available = false;
    strncpy(stats->name, "Intel GPU", sizeof(stats->name) - 1);
    
    /* 尝试使用 intel_gpu_top（如果可用） */
    /* 注意：intel_gpu_top 需要 root 权限，这里仅尝试读取 */
    FILE* fp = popen("timeout 1 intel_gpu_top -s 500 -l 1 2>/dev/null | tail -1", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp) != NULL) {
            /* intel_gpu_top 输出格式解析 */
            float render_busy, video_busy;
            if (sscanf(line, "%f, %f", &render_busy, &video_busy) >= 1) {
                stats->gpu_utilization = (int)render_busy;
                stats->available = true;
            }
        }
        pclose(fp);
    }
    
    /* 如果 intel_gpu_top 不可用，尝试读取 sysfs */
    if (!stats->available) {
        FILE* freq_fp = fopen("/sys/class/drm/card0/gt_cur_freq_mhz", "r");
        FILE* max_freq_fp = fopen("/sys/class/drm/card0/gt_max_freq_mhz", "r");
        
        if (freq_fp && max_freq_fp) {
            int cur_freq, max_freq;
            if (fscanf(freq_fp, "%d", &cur_freq) == 1 &&
                fscanf(max_freq_fp, "%d", &max_freq) == 1 && max_freq > 0) {
                stats->gpu_utilization = (cur_freq * 100) / max_freq;
                stats->available = true;
            }
        }
        
        if (freq_fp) fclose(freq_fp);
        if (max_freq_fp) fclose(max_freq_fp);
    }
    
    return stats->available ? 0 : -1;
}

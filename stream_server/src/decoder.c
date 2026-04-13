/**
 * @file decoder.c
 * @brief 硬件解码器实现（FFmpeg + VA-API + NVDEC）
 */

#define _GNU_SOURCE
#include "decoder.h"
#include <pthread.h>
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
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#ifdef HAVE_LIBYUV
#include <libyuv/convert_argb.h>
#endif

#if defined(HAVE_LIBYUV) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#endif

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

typedef struct DecoderTransferStats {
    pthread_mutex_t lock;
    unsigned refcount;
    uint64_t hw_transfer_count;
    double total_hw_transfer_time;
} DecoderTransferStats;

typedef struct DecoderConvertStatsInternal {
    pthread_mutex_t lock;
    uint64_t bgr24_request_count;
    uint64_t libyuv_bgr24_count;
    double total_libyuv_bgr24_time_ms;
    uint64_t vuyx_bgr24_count;
    double total_vuyx_bgr24_time_ms;
    uint64_t swscale_count;
    double total_swscale_time_ms;
} DecoderConvertStatsInternal;

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
    bool uses_shared_cuda_device;
    
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
    DecoderTransferStats* transfer_stats;
};

static pthread_mutex_t g_cuda_device_mu = PTHREAD_MUTEX_INITIALIZER;
static AVBufferRef* g_cuda_device_master = NULL;
static int g_cuda_device_id = -1;
static unsigned g_cuda_device_users = 0;
static DecoderConvertStatsInternal g_convert_stats = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_once_t g_tls_sws_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_tls_sws_key;
static int g_tls_sws_key_ready = 0;

#define STREAM_COLORSPACE_REC_601 0u
#define STREAM_COLORSPACE_REC_709 1u
#define STREAM_COLOR_RANGE_LIMITED 0u
#define STREAM_COLOR_RANGE_FULL 1u

static void free_tls_sws_ctx(void* ptr) {
    if (ptr) {
        sws_freeContext((struct SwsContext*)ptr);
    }
}

static void init_tls_sws_key(void) {
    if (pthread_key_create(&g_tls_sws_key, free_tls_sws_ctx) == 0) {
        g_tls_sws_key_ready = 1;
    }
}

static struct SwsContext* decoder_get_cached_sws_ctx(DecoderCtx* ctx, int* uses_tls_cache) {
    if (uses_tls_cache) {
        *uses_tls_cache = 0;
    }

    if (ctx) {
        return ctx->sws_ctx;
    }

    pthread_once(&g_tls_sws_key_once, init_tls_sws_key);
    if (!g_tls_sws_key_ready) {
        return NULL;
    }

    if (uses_tls_cache) {
        *uses_tls_cache = 1;
    }
    return (struct SwsContext*)pthread_getspecific(g_tls_sws_key);
}

static void decoder_store_cached_sws_ctx(DecoderCtx* ctx, struct SwsContext* sws_ctx,
                                         int uses_tls_cache) {
    if (ctx) {
        ctx->sws_ctx = sws_ctx;
        return;
    }

    if (uses_tls_cache) {
        (void)pthread_setspecific(g_tls_sws_key, sws_ctx);
    }
}

static const char* pix_fmt_name_or_unknown(enum AVPixelFormat fmt) {
    const char* name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
}

static double elapsed_ms_since(const struct timespec* start,
                               const struct timespec* end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

typedef enum DecoderConvertPathKind {
    DECODER_CONVERT_PATH_LIBYUV_BGR24 = 0,
    DECODER_CONVERT_PATH_VUYX_BGR24,
    DECODER_CONVERT_PATH_SWSCALE,
} DecoderConvertPathKind;

static void record_bgr24_request(void) {
    pthread_mutex_lock(&g_convert_stats.lock);
    g_convert_stats.bgr24_request_count++;
    pthread_mutex_unlock(&g_convert_stats.lock);
}

static void record_convert_path_time(DecoderConvertPathKind kind, double elapsed_ms) {
    pthread_mutex_lock(&g_convert_stats.lock);
    switch (kind) {
        case DECODER_CONVERT_PATH_LIBYUV_BGR24:
            g_convert_stats.libyuv_bgr24_count++;
            g_convert_stats.total_libyuv_bgr24_time_ms += elapsed_ms;
            break;
        case DECODER_CONVERT_PATH_VUYX_BGR24:
            g_convert_stats.vuyx_bgr24_count++;
            g_convert_stats.total_vuyx_bgr24_time_ms += elapsed_ms;
            break;
        case DECODER_CONVERT_PATH_SWSCALE:
            g_convert_stats.swscale_count++;
            g_convert_stats.total_swscale_time_ms += elapsed_ms;
            break;
    }
    pthread_mutex_unlock(&g_convert_stats.lock);
}

static DecoderTransferStats* transfer_stats_create(void) {
    DecoderTransferStats* stats = calloc(1, sizeof(*stats));
    if (!stats) {
        return NULL;
    }

    pthread_mutex_init(&stats->lock, NULL);
    stats->refcount = 1;
    return stats;
}

static void transfer_stats_ref(DecoderTransferStats* stats) {
    if (!stats) {
        return;
    }

    pthread_mutex_lock(&stats->lock);
    stats->refcount++;
    pthread_mutex_unlock(&stats->lock);
}

static void transfer_stats_unref(DecoderTransferStats** stats_ptr) {
    if (!stats_ptr || !*stats_ptr) {
        return;
    }

    DecoderTransferStats* stats = *stats_ptr;
    bool destroy = false;

    pthread_mutex_lock(&stats->lock);
    if (stats->refcount > 0) {
        stats->refcount--;
    }
    destroy = (stats->refcount == 0);
    pthread_mutex_unlock(&stats->lock);

    if (destroy) {
        pthread_mutex_destroy(&stats->lock);
        free(stats);
    }

    *stats_ptr = NULL;
}

static void record_hw_transfer_time(DecoderTransferStats* stats, double transfer_ms) {
    if (!stats) {
        return;
    }

    pthread_mutex_lock(&stats->lock);
    stats->hw_transfer_count++;
    stats->total_hw_transfer_time += transfer_ms;
    pthread_mutex_unlock(&stats->lock);
}

static void snapshot_hw_transfer_stats(DecoderTransferStats* stats, DecoderStats* out) {
    if (!out) {
        return;
    }

    out->hw_transfer_count = 0;
    out->avg_hw_transfer_time_ms = 0.0;
    if (!stats) {
        return;
    }

    pthread_mutex_lock(&stats->lock);
    out->hw_transfer_count = stats->hw_transfer_count;
    if (stats->hw_transfer_count > 0) {
        out->avg_hw_transfer_time_ms =
            stats->total_hw_transfer_time / (double)stats->hw_transfer_count;
    }
    pthread_mutex_unlock(&stats->lock);
}

static const char* codec_name_from_id(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264: return "H.264";
        case AV_CODEC_ID_HEVC: return "HEVC";
        case AV_CODEC_ID_AV1:  return "AV1";
        default:               return "unknown";
    }
}

static enum AVPixelFormat av_format_from_decode(DecodeFormat fmt) {
    switch (fmt) {
        case DECODE_FMT_NV12:
            return AV_PIX_FMT_NV12;
        case DECODE_FMT_YUV420P:
            return AV_PIX_FMT_YUV420P;
        case DECODE_FMT_YUV444P:
            return AV_PIX_FMT_YUV444P;
        case DECODE_FMT_VUYX:
            return AV_PIX_FMT_VUYX;
        case DECODE_FMT_BGR24:
            return AV_PIX_FMT_BGR24;
        case DECODE_FMT_BGRA:
            return AV_PIX_FMT_BGRA;
        case DECODE_FMT_RGB24:
            return AV_PIX_FMT_RGB24;
        default:
            return AV_PIX_FMT_NONE;
    }
}

static enum AVPixelFormat hw_frame_sw_format(const AVFrame* frame) {
    if (!frame || !frame->hw_frames_ctx) {
        return AV_PIX_FMT_NONE;
    }

    const AVHWFramesContext* frames_ctx =
        (const AVHWFramesContext*)frame->hw_frames_ctx->data;
    if (!frames_ctx) {
        return AV_PIX_FMT_NONE;
    }

    return frames_ctx->sw_format;
}

static void finalize_transferred_sw_frame(AVFrame* sw_frame, const AVFrame* hw_frame) {
    if (!sw_frame || !hw_frame) {
        return;
    }

    if (sw_frame->format == AV_PIX_FMT_NONE) {
        enum AVPixelFormat sw_fmt = hw_frame_sw_format(hw_frame);
        if (sw_fmt != AV_PIX_FMT_NONE) {
            sw_frame->format = sw_fmt;
        }
    }
    if (sw_frame->width <= 0) {
        sw_frame->width = hw_frame->width;
    }
    if (sw_frame->height <= 0) {
        sw_frame->height = hw_frame->height;
    }
}

/* 检测硬件支持 */
DecodeBackend decoder_detect_backend(void) {
    /* 优先检测 Intel VA-API - 尝试多个设备 */
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

    /* 检测 NVIDIA */
    AVBufferRef* cuda_ctx = NULL;
    if (av_hwdevice_ctx_create(&cuda_ctx, AV_HWDEVICE_TYPE_CUDA,
                               NULL, NULL, 0) >= 0) {
        av_buffer_unref(&cuda_ctx);
        printf("[Decoder] NVIDIA CUDA detected\n");
        return DECODE_BACKEND_NVIDIA;
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

static int acquire_shared_cuda_device(int device_id, AVBufferRef** out_ctx) {
    if (!out_ctx) {
        return AVERROR(EINVAL);
    }

    *out_ctx = NULL;

    pthread_mutex_lock(&g_cuda_device_mu);

    if (g_cuda_device_master) {
        if (g_cuda_device_id != device_id) {
            pthread_mutex_unlock(&g_cuda_device_mu);
            return AVERROR(EINVAL);
        }

        *out_ctx = av_buffer_ref(g_cuda_device_master);
        if (*out_ctx) {
            g_cuda_device_users++;
            pthread_mutex_unlock(&g_cuda_device_mu);
            return 0;
        }

        pthread_mutex_unlock(&g_cuda_device_mu);
        return AVERROR(ENOMEM);
    }

    pthread_mutex_unlock(&g_cuda_device_mu);

    char cuda_dev[16];
    snprintf(cuda_dev, sizeof(cuda_dev), "%d", device_id);

    AVBufferRef* created = NULL;
    int ret = av_hwdevice_ctx_create(&created, AV_HWDEVICE_TYPE_CUDA,
                                     cuda_dev, NULL, 0);
    if (ret < 0) {
        return ret;
    }

    pthread_mutex_lock(&g_cuda_device_mu);

    if (!g_cuda_device_master) {
        g_cuda_device_master = created;
        g_cuda_device_id = device_id;
    } else if (g_cuda_device_id != device_id) {
        pthread_mutex_unlock(&g_cuda_device_mu);
        av_buffer_unref(&created);
        return AVERROR(EINVAL);
    } else {
        av_buffer_unref(&created);
    }

    *out_ctx = av_buffer_ref(g_cuda_device_master);
    if (*out_ctx) {
        g_cuda_device_users++;
        pthread_mutex_unlock(&g_cuda_device_mu);
        return 0;
    }

    if (g_cuda_device_users == 0 && g_cuda_device_master) {
        av_buffer_unref(&g_cuda_device_master);
        g_cuda_device_master = NULL;
        g_cuda_device_id = -1;
    }
    pthread_mutex_unlock(&g_cuda_device_mu);
    return AVERROR(ENOMEM);
}

static void release_shared_cuda_device(void) {
    pthread_mutex_lock(&g_cuda_device_mu);
    if (g_cuda_device_users > 0) {
        g_cuda_device_users--;
    }
    if (g_cuda_device_users == 0 && g_cuda_device_master) {
        av_buffer_unref(&g_cuda_device_master);
        g_cuda_device_master = NULL;
        g_cuda_device_id = -1;
    }
    pthread_mutex_unlock(&g_cuda_device_mu);
}

/* 获取硬件像素格式 (参考 embedded ffmpeg_vaapi.c) */
static enum AVPixelFormat choose_software_format(const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (!desc) {
            continue;
        }
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

static enum AVPixelFormat choose_decoder_sw_format(AVCodecContext* ctx,
                                                   const enum AVPixelFormat* pix_fmts) {
    if (ctx && ctx->sw_pix_fmt != AV_PIX_FMT_NONE) {
        return ctx->sw_pix_fmt;
    }
    return choose_software_format(pix_fmts);
}

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, 
                                        const enum AVPixelFormat* pix_fmts) {
    DecoderCtx* dec_ctx = (DecoderCtx*)ctx->opaque;
    const enum AVPixelFormat* p;
    
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == dec_ctx->hw_pix_fmt) {
            return *p;
        }
    }

    enum AVPixelFormat sw_fmt = choose_software_format(pix_fmts);
    if (sw_fmt != AV_PIX_FMT_NONE) {
        fprintf(stderr,
                "[Decoder] hw format %d unavailable for this stream, fallback sw fmt=%d\n",
                dec_ctx->hw_pix_fmt, sw_fmt);
        return sw_fmt;
    }
    return AV_PIX_FMT_NONE;
}

/* VA-API 格式回调 (参考 embedded ffmpeg_vaapi.c va_get_format) */
static enum AVPixelFormat vaapi_get_format(AVCodecContext* ctx,
                                           const enum AVPixelFormat* pix_fmts) {
    enum AVPixelFormat hw_sw_fmt = choose_decoder_sw_format(ctx, pix_fmts);
    enum AVPixelFormat chosen = get_hw_format(ctx, pix_fmts);

    if (chosen == AV_PIX_FMT_VAAPI) {
        printf("[Decoder] VAAPI selected hw format (sw_format=%s)\n",
               pix_fmt_name_or_unknown(hw_sw_fmt));
    } else if (chosen != AV_PIX_FMT_NONE) {
        fprintf(stderr, "[Decoder] VAAPI fallback to software format=%s\n",
                pix_fmt_name_or_unknown(chosen));
    }

    return chosen;
}

static DecodeFormat decode_format_from_av(enum AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_NV12:
            return DECODE_FMT_NV12;
        case AV_PIX_FMT_YUV420P:
            return DECODE_FMT_YUV420P;
        case AV_PIX_FMT_YUV444P:
            return DECODE_FMT_YUV444P;
        case AV_PIX_FMT_VUYX:
            return DECODE_FMT_VUYX;
        case AV_PIX_FMT_BGR24:
            return DECODE_FMT_BGR24;
        case AV_PIX_FMT_RGB24:
            return DECODE_FMT_RGB24;
        default:
            return DECODE_FMT_NONE;
    }
}

static DecodeFormat decode_format_from_frame(const AVFrame* frame) {
    if (!frame) {
        return DECODE_FMT_NONE;
    }

    DecodeFormat format = decode_format_from_av((enum AVPixelFormat)frame->format);
    if (format != DECODE_FMT_NONE) {
        return format;
    }

    return decode_format_from_av(hw_frame_sw_format(frame));
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
        case AV_PIX_FMT_YUV444P:
            /* Y/U/V: H */
            return (plane >= 0 && plane <= 2) ? height : 0;
        case AV_PIX_FMT_VUYX:
        case AV_PIX_FMT_BGR24:
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

static int copy_cpu_frame_data(const DecodedFrame* src, DecodedFrame* dst) {
    enum AVPixelFormat src_fmt = av_format_from_decode(src->format);
    if (src_fmt == AV_PIX_FMT_NONE) {
        return -1;
    }

    for (int i = 0; i < 4; i++) {
        if (!src->data[i]) {
            continue;
        }

        int lines = plane_height_for_pix_fmt(src_fmt, i, src->height);
        if (lines <= 0) {
            continue;
        }

        dst->linesize[i] = src->linesize[i];
        dst->data[i] = malloc((size_t)dst->linesize[i] * (size_t)lines);
        if (!dst->data[i]) {
            return -1;
        }

        memcpy(dst->data[i], src->data[i],
               (size_t)dst->linesize[i] * (size_t)lines);
    }

    return 0;
}

static int decoded_frame_set_avframe(DecodedFrame* frame, DecoderTransferStats* transfer_stats,
                                     AVFrame* avf, DecodeStorage storage,
                                     bool take_ownership) {
    if (!frame || !avf) {
        return -1;
    }

    AVFrame* owned = avf;
    if (!take_ownership) {
        owned = av_frame_clone(avf);
        if (!owned) {
            return -1;
        }
    }

    frame->av_frame = owned;
    frame->_decoder_ctx = NULL;
    frame->width = owned->width;
    frame->height = owned->height;
    frame->pts = owned->pts;
    frame->key_frame = !!(owned->flags & AV_FRAME_FLAG_KEY);
    frame->format = decode_format_from_frame(owned);
    frame->storage = storage;

    if (storage == DECODE_STORAGE_CPU) {
        for (int i = 0; i < 4; i++) {
            frame->data[i] = owned->data[i];
            frame->linesize[i] = owned->linesize[i];
        }
    } else {
        if (transfer_stats) {
            transfer_stats_ref(transfer_stats);
            frame->_decoder_ctx = transfer_stats;
        }
        memset(frame->data, 0, sizeof(frame->data));
        memset(frame->linesize, 0, sizeof(frame->linesize));
    }

    return 0;
}

DecoderCtx* decoder_create(const DecoderConfig* config) {
    DecoderCtx* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    
    memcpy(&ctx->config, config, sizeof(*config));
    ctx->transfer_stats = transfer_stats_create();
    if (!ctx->transfer_stats) {
        free(ctx);
        return NULL;
    }
    
    enum AVCodecID codec_id = config->codec_id ? config->codec_id : AV_CODEC_ID_H264;

    /* 自动检测后端 */
    if (ctx->config.backend == DECODE_BACKEND_AUTO) {
        ctx->config.backend = decoder_detect_backend();
    }
    
    /* 查找解码器 */
    switch (ctx->config.backend) {
        case DECODE_BACKEND_INTEL_VA:
            ctx->codec = avcodec_find_decoder(codec_id);
            if (!ctx->codec) {
                fprintf(stderr, "[Decoder] %s decoder not found\n", codec_name_from_id(codec_id));
                decoder_destroy(ctx);
                return NULL;
            }
            printf("[Decoder] Using %s decoder with VA-API hwaccel\n", codec_name_from_id(codec_id));
            break;
            
        case DECODE_BACKEND_NVIDIA:
            ctx->codec = avcodec_find_decoder(codec_id);
            if (!ctx->codec) {
                fprintf(stderr, "[Decoder] %s decoder not found\n", codec_name_from_id(codec_id));
                decoder_destroy(ctx);
                return NULL;
            }
            printf("[Decoder] Using %s decoder with CUDA hwaccel\n", codec_name_from_id(codec_id));
            break;
            
        default:
            ctx->codec = avcodec_find_decoder(codec_id);
            break;
    }
    
    if (!ctx->codec) {
        fprintf(stderr, "[Decoder] %s decoder not found\n", codec_name_from_id(codec_id));
        decoder_destroy(ctx);
        return NULL;
    }
    
    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
    if (!ctx->codec_ctx) {
        decoder_destroy(ctx);
        return NULL;
    }
    
    ctx->frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    
    if (!ctx->frame || !ctx->packet) {
        decoder_destroy(ctx);
        return NULL;
    }
    
    /* 初始化 codec parser */
    ctx->parser = av_parser_init(codec_id);
    if (!ctx->parser) {
        fprintf(stderr, "[Decoder] Failed to create %s parser\n", codec_name_from_id(codec_id));
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
    if (ctx->uses_shared_cuda_device) {
        release_shared_cuda_device();
    }
    if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
    if (ctx->transfer_stats) {
        transfer_stats_unref(&ctx->transfer_stats);
    }
    
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
            /* 按 FFmpeg hw_decode 示例走标准 VAAPI decode 初始化 */
            ctx->codec_ctx->get_format = vaapi_get_format;
            ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
            if (ctx->config.extra_hw_frames > 0) {
                ctx->codec_ctx->extra_hw_frames = ctx->config.extra_hw_frames;
            } else if (ctx->config.defer_hw_download) {
                ctx->codec_ctx->extra_hw_frames = 24;
            }
            ctx->codec_ctx->thread_count = 1;
        }
    }
    else if (ctx->config.backend == DECODE_BACKEND_NVIDIA) {
        /*
         * NVIDIA CUDA 初始化 (参考 moonlight-qt CUDARenderer)
         * 关键：只设置 hw_device_ctx，让 FFmpeg/NVDEC 内部创建 hw_frames_ctx
         * 不要手动创建 hw_frames_ctx，否则会干扰 NVDEC 内部帧管理
         */
        ctx->hw_pix_fmt = AV_PIX_FMT_CUDA;
        ctx->uses_shared_cuda_device = false;

        /* 共享 CUDA 设备上下文，避免 20 路各建一套上下文/事件线程 */
        int ret = acquire_shared_cuda_device(ctx->config.cuda_device_id,
                                             &ctx->hw_device_ctx);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[Decoder] Failed to create CUDA device: %s\n", errbuf);
            ctx->config.backend = DECODE_BACKEND_CPU;
            ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
        } else {
            ctx->uses_shared_cuda_device = true;
            /* 只设置 get_format 选择像素格式，不手动创建 hw_frames_ctx */
            ctx->codec_ctx->get_format = get_hw_format;
            ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);

            /*
             * 当 last_frame 保留为硬件帧时，需要更大的 surface 余量，
             * 否则容易因为引用未释放而把 NVDEC 帧池顶满。
             */
            if (ctx->config.extra_hw_frames > 0) {
                ctx->codec_ctx->extra_hw_frames = ctx->config.extra_hw_frames;
            } else if (ctx->config.defer_hw_download) {
                ctx->codec_ctx->extra_hw_frames = 24;
            } else {
                ctx->codec_ctx->extra_hw_frames = 8;
            }

            /* 硬件解码不需要多线程 */
            ctx->codec_ctx->thread_count = 1;

            printf("[Decoder] NVIDIA CUDA device created (GPU %d, extra_hw_frames=%d)\n",
                   ctx->config.cuda_device_id, ctx->codec_ctx->extra_hw_frames);
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
            
            /* 分配输出帧 */
            DecodedFrame* frame = calloc(1, sizeof(*frame));
            if (!frame) {
                continue;
            }

            AVFrame* out_avf = ctx->frame;
            DecodeStorage storage = DECODE_STORAGE_CPU;
            bool take_ownership = false;

            if ((ctx->config.backend == DECODE_BACKEND_NVIDIA ||
                 ctx->config.backend == DECODE_BACKEND_INTEL_VA) &&
                ctx->frame->format == ctx->hw_pix_fmt) {
                if (ctx->config.defer_hw_download) {
                    storage = DECODE_STORAGE_HW;
                } else {
                    AVFrame* tmp_frame = av_frame_alloc();
                    if (!tmp_frame) {
                        free(frame);
                        ctx->stats.frames_dropped++;
                        continue;
                    }

                    struct timespec xfer_start, xfer_end;
                    clock_gettime(CLOCK_MONOTONIC, &xfer_start);
                    ret = av_hwframe_transfer_data(tmp_frame, ctx->frame, 0);
                    clock_gettime(CLOCK_MONOTONIC, &xfer_end);
                    if (ret < 0) {
                        av_frame_free(&tmp_frame);
                        free(frame);
                        ctx->stats.frames_dropped++;
                        continue;
                    }
                    record_hw_transfer_time(ctx->transfer_stats,
                                            elapsed_ms_since(&xfer_start, &xfer_end));

                    av_frame_copy_props(tmp_frame, ctx->frame);
                    finalize_transferred_sw_frame(tmp_frame, ctx->frame);
                    out_avf = tmp_frame;
                    take_ownership = true;
                }
            }

            if (decoded_frame_set_avframe(frame, ctx->transfer_stats, out_avf,
                                          storage, take_ownership) != 0) {
                if (take_ownership) {
                    av_frame_free(&out_avf);
                }
                free(frame);
                ctx->stats.frames_dropped++;
                continue;
            }

            last_frame = frame;
        }
    } /* end parser loop */
    
    /* 统计本次调用处理的字节数 (只计一次，避免 parser 循环重复计) */
    if (total_decoded > 0) {
        ctx->stats.bytes_in += size;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double decode_time = elapsed_ms_since(&start_ts, &end_ts);
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
    DecoderTransferStats* transfer_stats = (DecoderTransferStats*)frame->_decoder_ctx;
    
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
    
    transfer_stats_unref(&transfer_stats);
    frame->_decoder_ctx = NULL;
    free(frame);
}

DecodedFrame* decoder_ref_frame(const DecodedFrame* src) {
    if (!src) {
        return NULL;
    }

    DecodedFrame* dst = calloc(1, sizeof(*dst));
    if (!dst) {
        return NULL;
    }

    dst->width = src->width;
    dst->height = src->height;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    dst->format = src->format;
    dst->storage = src->storage;

    if (src->av_frame) {
        if (decoded_frame_set_avframe(dst, (DecoderTransferStats*)src->_decoder_ctx,
                                      (AVFrame*)src->av_frame, src->storage, false) != 0) {
            free(dst);
            return NULL;
        }
        return dst;
    }

    if (copy_cpu_frame_data(src, dst) != 0) {
        decoder_free_frame(dst);
        return NULL;
    }

    return dst;
}

int decoder_materialize_frame(const DecodedFrame* src, DecodedFrame** out_frame) {
    if (!src || !out_frame) {
        return -1;
    }

    *out_frame = NULL;

    if (src->storage == DECODE_STORAGE_CPU) {
        DecodedFrame* clone = decoder_ref_frame(src);
        if (!clone) {
            return -1;
        }
        *out_frame = clone;
        return 0;
    }

    if (!src->av_frame) {
        return -1;
    }

    AVFrame* hw_frame = (AVFrame*)src->av_frame;
    AVFrame* sw_frame = av_frame_alloc();
    if (!sw_frame) {
        return -1;
    }

    struct timespec xfer_start, xfer_end;
    clock_gettime(CLOCK_MONOTONIC, &xfer_start);
    int ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    clock_gettime(CLOCK_MONOTONIC, &xfer_end);
    if (ret < 0) {
        av_frame_free(&sw_frame);
        return -1;
    }
    record_hw_transfer_time((DecoderTransferStats*)src->_decoder_ctx,
                            elapsed_ms_since(&xfer_start, &xfer_end));
    av_frame_copy_props(sw_frame, hw_frame);
    finalize_transferred_sw_frame(sw_frame, hw_frame);

    DecodedFrame* dst = calloc(1, sizeof(*dst));
    if (!dst) {
        av_frame_free(&sw_frame);
        return -1;
    }

    if (decoded_frame_set_avframe(dst, NULL,
                                  sw_frame, DECODE_STORAGE_CPU, true) != 0) {
        av_frame_free(&sw_frame);
        free(dst);
        return -1;
    }

    *out_frame = dst;
    return 0;
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

    AVFrame* out_avf = ctx->frame;
    DecodeStorage storage = DECODE_STORAGE_CPU;
    bool take_ownership = false;

    if ((ctx->config.backend == DECODE_BACKEND_NVIDIA ||
         ctx->config.backend == DECODE_BACKEND_INTEL_VA) &&
        ctx->frame->format == ctx->hw_pix_fmt) {
        if (ctx->config.defer_hw_download) {
            storage = DECODE_STORAGE_HW;
        } else {
            AVFrame* tmp_frame = av_frame_alloc();
            if (!tmp_frame) {
                free(frame);
                return -1;
            }
            struct timespec xfer_start, xfer_end;
            clock_gettime(CLOCK_MONOTONIC, &xfer_start);
            if (av_hwframe_transfer_data(tmp_frame, ctx->frame, 0) < 0) {
                av_frame_free(&tmp_frame);
                free(frame);
                return -1;
            }
            clock_gettime(CLOCK_MONOTONIC, &xfer_end);
            record_hw_transfer_time(ctx->transfer_stats,
                                    elapsed_ms_since(&xfer_start, &xfer_end));
            av_frame_copy_props(tmp_frame, ctx->frame);
            finalize_transferred_sw_frame(tmp_frame, ctx->frame);
            out_avf = tmp_frame;
            take_ownership = true;
        }
    }

    if (decoded_frame_set_avframe(frame, ctx->transfer_stats, out_avf,
                                  storage, take_ownership) != 0) {
        if (take_ownership) {
            av_frame_free(&out_avf);
        }
        free(frame);
        return -1;
    }

    *out_frame = frame;
    return 0;
}

static int decode_format_bytes_per_pixel(DecodeFormat fmt) {
    switch (fmt) {
        case DECODE_FMT_BGR24:
        case DECODE_FMT_RGB24:
            return 3;
        case DECODE_FMT_BGRA:
            return 4;
        default:
            return 0;
    }
}

#ifdef HAVE_LIBYUV
typedef struct VuyxBgrCoeffs {
    int y_offset;
    int y_mul;
    int vr_mul;
    int gu_mul;
    int gv_mul;
    int bu_mul;
} VuyxBgrCoeffs;

static uint8_t clamp_u8_from_int(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static const struct YuvConstants* bgr_yuv_constants_from_info(const StreamInfo* info) {
    uint32_t color_space = info ? info->color_space : STREAM_COLORSPACE_REC_709;
    uint32_t color_range = info ? info->color_range : STREAM_COLOR_RANGE_LIMITED;

    if (color_space == STREAM_COLORSPACE_REC_601) {
        return (color_range == STREAM_COLOR_RANGE_FULL)
             ? &kYvuJPEGConstants
             : &kYvuI601Constants;
    }

    return (color_range == STREAM_COLOR_RANGE_FULL)
         ? &kYvuF709Constants
         : &kYvuH709Constants;
}

static void vuyx_bgr_coeffs_from_info(const StreamInfo* info, VuyxBgrCoeffs* coeffs) {
    uint32_t color_space = info ? info->color_space : STREAM_COLORSPACE_REC_709;
    uint32_t color_range = info ? info->color_range : STREAM_COLOR_RANGE_LIMITED;

    if (!coeffs) {
        return;
    }

    if (color_space == STREAM_COLORSPACE_REC_601) {
        if (color_range == STREAM_COLOR_RANGE_FULL) {
            *coeffs = (VuyxBgrCoeffs){
                .y_offset = 0,
                .y_mul = 256,
                .vr_mul = 359,
                .gu_mul = 88,
                .gv_mul = 183,
                .bu_mul = 454,
            };
            return;
        }

        *coeffs = (VuyxBgrCoeffs){
            .y_offset = 16,
            .y_mul = 298,
            .vr_mul = 409,
            .gu_mul = 100,
            .gv_mul = 208,
            .bu_mul = 516,
        };
        return;
    }

    if (color_range == STREAM_COLOR_RANGE_FULL) {
        *coeffs = (VuyxBgrCoeffs){
            .y_offset = 0,
            .y_mul = 256,
            .vr_mul = 403,
            .gu_mul = 48,
            .gv_mul = 120,
            .bu_mul = 475,
        };
        return;
    }

    *coeffs = (VuyxBgrCoeffs){
        .y_offset = 16,
        .y_mul = 298,
        .vr_mul = 459,
        .gu_mul = 55,
        .gv_mul = 136,
        .bu_mul = 541,
    };
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
static int decoder_can_use_vuyx_avx2(void) {
    return __builtin_cpu_supports("avx2") &&
           __builtin_cpu_supports("sse4.1") &&
           __builtin_cpu_supports("ssse3");
}

__attribute__((target("avx2,sse4.1,ssse3")))
static int decoder_fast_convert_vuyx_to_bgr24_row_avx2(const uint8_t* src_row,
                                                       uint8_t* dst_row,
                                                       int width,
                                                       const VuyxBgrCoeffs* coeffs) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i y_offset = _mm256_set1_epi32(coeffs->y_offset);
    const __m256i c128 = _mm256_set1_epi32(128);
    const __m256i c255 = _mm256_set1_epi32(255);
    const __m256i c_u8_bias = _mm256_set1_epi32(128);
    const __m256i y_mul = _mm256_set1_epi32(coeffs->y_mul);
    const __m256i vr_mul = _mm256_set1_epi32(coeffs->vr_mul);
    const __m256i gu_mul = _mm256_set1_epi32(coeffs->gu_mul);
    const __m256i gv_mul = _mm256_set1_epi32(coeffs->gv_mul);
    const __m256i bu_mul = _mm256_set1_epi32(coeffs->bu_mul);
    const __m256i pick_v = _mm256_setr_epi8(
        0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i pick_u = _mm256_setr_epi8(
        1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i pick_y = _mm256_setr_epi8(
        2, 6, 10, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        2, 6, 10, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);

    int x = 0;
    for (; x + 8 <= width; x += 8) {
        const __m256i raw = _mm256_loadu_si256((const __m256i*)(src_row + (size_t)x * 4u));
        const __m256i v_shuf = _mm256_shuffle_epi8(raw, pick_v);
        const __m256i u_shuf = _mm256_shuffle_epi8(raw, pick_u);
        const __m256i y_shuf = _mm256_shuffle_epi8(raw, pick_y);

        const __m128i v_bytes = _mm_unpacklo_epi32(
            _mm256_castsi256_si128(v_shuf), _mm256_extracti128_si256(v_shuf, 1));
        const __m128i u_bytes = _mm_unpacklo_epi32(
            _mm256_castsi256_si128(u_shuf), _mm256_extracti128_si256(u_shuf, 1));
        const __m128i y_bytes = _mm_unpacklo_epi32(
            _mm256_castsi256_si128(y_shuf), _mm256_extracti128_si256(y_shuf, 1));

        const __m256i v32 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(v_bytes), c_u8_bias);
        const __m256i u32 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(u_bytes), c_u8_bias);
        __m256i y32 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(y_bytes), y_offset);

        y32 = _mm256_max_epi32(y32, zero);
        const __m256i y_term = _mm256_mullo_epi32(y32, y_mul);
        const __m256i b32 = _mm256_srai_epi32(
            _mm256_add_epi32(_mm256_add_epi32(y_term, _mm256_mullo_epi32(u32, bu_mul)), c128), 8);
        const __m256i g32 = _mm256_srai_epi32(
            _mm256_add_epi32(
                _mm256_sub_epi32(
                    _mm256_sub_epi32(y_term, _mm256_mullo_epi32(u32, gu_mul)),
                    _mm256_mullo_epi32(v32, gv_mul)),
                c128),
            8);
        const __m256i r32 = _mm256_srai_epi32(
            _mm256_add_epi32(_mm256_add_epi32(y_term, _mm256_mullo_epi32(v32, vr_mul)), c128), 8);

        const __m256i b_clamped = _mm256_min_epi32(_mm256_max_epi32(b32, zero), c255);
        const __m256i g_clamped = _mm256_min_epi32(_mm256_max_epi32(g32, zero), c255);
        const __m256i r_clamped = _mm256_min_epi32(_mm256_max_epi32(r32, zero), c255);

        const __m128i b16 = _mm_packus_epi32(
            _mm256_castsi256_si128(b_clamped), _mm256_extracti128_si256(b_clamped, 1));
        const __m128i g16 = _mm_packus_epi32(
            _mm256_castsi256_si128(g_clamped), _mm256_extracti128_si256(g_clamped, 1));
        const __m128i r16 = _mm_packus_epi32(
            _mm256_castsi256_si128(r_clamped), _mm256_extracti128_si256(r_clamped, 1));
        const __m128i b8 = _mm_packus_epi16(b16, b16);
        const __m128i g8 = _mm_packus_epi16(g16, g16);
        const __m128i r8 = _mm_packus_epi16(r16, r16);

        uint8_t b_bytes[8];
        uint8_t g_bytes[8];
        uint8_t r_bytes[8];
        _mm_storel_epi64((__m128i*)b_bytes, b8);
        _mm_storel_epi64((__m128i*)g_bytes, g8);
        _mm_storel_epi64((__m128i*)r_bytes, r8);

        for (int i = 0; i < 8; i++) {
            dst_row[0] = b_bytes[i];
            dst_row[1] = g_bytes[i];
            dst_row[2] = r_bytes[i];
            dst_row += 3;
        }
    }

    return x;
}
#else
static int decoder_can_use_vuyx_avx2(void) {
    return 0;
}
#endif

static int decoder_fast_convert_vuyx_to_bgr24(const DecodedFrame* src,
                                              const StreamInfo* info,
                                              DecodedFrame* dst) {
    if (!src || !dst || !src->data[0] || src->width <= 0 || src->height <= 0 ||
        src->linesize[0] < src->width * 4) {
        return -1;
    }

    VuyxBgrCoeffs coeffs;
    vuyx_bgr_coeffs_from_info(info, &coeffs);

    dst->width = src->width;
    dst->height = src->height;
    dst->format = DECODE_FMT_BGR24;
    dst->storage = DECODE_STORAGE_CPU;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    dst->av_frame = NULL;
    dst->_decoder_ctx = NULL;
    memset(dst->data, 0, sizeof(dst->data));
    memset(dst->linesize, 0, sizeof(dst->linesize));
    dst->linesize[0] = src->width * 3;
    dst->data[0] = malloc((size_t)dst->linesize[0] * (size_t)src->height);
    if (!dst->data[0]) {
        return -1;
    }

    for (int y = 0; y < src->height; y++) {
        const uint8_t* src_row = src->data[0] + (size_t)y * (size_t)src->linesize[0];
        uint8_t* dst_row = dst->data[0] + (size_t)y * (size_t)dst->linesize[0];
        int x = 0;

        if (decoder_can_use_vuyx_avx2()) {
            x = decoder_fast_convert_vuyx_to_bgr24_row_avx2(src_row, dst_row,
                                                            src->width, &coeffs);
            src_row += (size_t)x * 4u;
            dst_row += (size_t)x * 3u;
        }

        for (; x < src->width; x++) {
            const int v = src_row[0];
            const int u = src_row[1];
            int y_sample = src_row[2] - coeffs.y_offset;
            const int du = u - 128;
            const int dv = v - 128;

            if (y_sample < 0) {
                y_sample = 0;
            }

            const int y_term = coeffs.y_mul * y_sample;
            const int b = (y_term + coeffs.bu_mul * du + 128) >> 8;
            const int g = (y_term - coeffs.gu_mul * du - coeffs.gv_mul * dv + 128) >> 8;
            const int r = (y_term + coeffs.vr_mul * dv + 128) >> 8;

            dst_row[0] = clamp_u8_from_int(b);
            dst_row[1] = clamp_u8_from_int(g);
            dst_row[2] = clamp_u8_from_int(r);

            src_row += 4;
            dst_row += 3;
        }
    }

    return 0;
}

static int decoder_fast_convert_to_bgr24(const DecodedFrame* src,
                                         const StreamInfo* info,
                                         DecodedFrame* dst) {
    if (!src || !dst || !src->data[0] || src->width <= 0 || src->height <= 0) {
        return -1;
    }

    if (src->format == DECODE_FMT_VUYX) {
        return decoder_fast_convert_vuyx_to_bgr24(src, info, dst);
    }

    const struct YuvConstants* yuv_constants = bgr_yuv_constants_from_info(info);

    dst->width = src->width;
    dst->height = src->height;
    dst->format = DECODE_FMT_BGR24;
    dst->storage = DECODE_STORAGE_CPU;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    dst->av_frame = NULL;
    dst->_decoder_ctx = NULL;
    memset(dst->data, 0, sizeof(dst->data));
    memset(dst->linesize, 0, sizeof(dst->linesize));
    dst->linesize[0] = src->width * 3;
    dst->data[0] = malloc((size_t)dst->linesize[0] * (size_t)src->height);
    if (!dst->data[0]) {
        return -1;
    }

    int ret = -1;
    switch (src->format) {
        case DECODE_FMT_NV12:
            ret = NV12ToRGB24Matrix(
                src->data[0], src->linesize[0],
                src->data[1], src->linesize[1],
                dst->data[0], dst->linesize[0],
                yuv_constants,
                src->width, src->height);
            break;
        case DECODE_FMT_YUV420P:
            ret = I420ToRGB24Matrix(
                src->data[0], src->linesize[0],
                src->data[1], src->linesize[1],
                src->data[2], src->linesize[2],
                dst->data[0], dst->linesize[0],
                yuv_constants,
                src->width, src->height);
            break;
        case DECODE_FMT_YUV444P:
            ret = I444ToRGB24Matrix(
                src->data[0], src->linesize[0],
                src->data[1], src->linesize[1],
                src->data[2], src->linesize[2],
                dst->data[0], dst->linesize[0],
                yuv_constants,
                src->width, src->height);
            break;
        default:
            ret = -1;
            break;
    }

    if (ret != 0) {
        free(dst->data[0]);
        dst->data[0] = NULL;
        dst->linesize[0] = 0;
        return -1;
    }

    return 0;
}
#endif

static void infer_sws_color_details(const StreamInfo* info, const DecodedFrame* src,
                                    int* coeffs_out, int* src_range_out) {
    int coeffs = SWS_CS_ITU709;
    int src_range = 0;

    if (info) {
        coeffs = (info->color_space == STREAM_COLORSPACE_REC_601)
               ? SWS_CS_ITU601
               : SWS_CS_ITU709;
        src_range = (info->color_range == STREAM_COLOR_RANGE_FULL) ? 1 : 0;
    } else if (src && src->av_frame) {
        const AVFrame* avf = (const AVFrame*)src->av_frame;
        switch (avf->colorspace) {
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_FCC:
                coeffs = SWS_CS_ITU601;
                break;
            case AVCOL_SPC_BT709:
            default:
                coeffs = SWS_CS_ITU709;
                break;
        }

        if (avf->color_range == AVCOL_RANGE_JPEG) {
            src_range = 1;
        } else if (avf->color_range == AVCOL_RANGE_MPEG) {
            src_range = 0;
        }
    }

    if (coeffs_out) {
        *coeffs_out = coeffs;
    }
    if (src_range_out) {
        *src_range_out = src_range;
    }
}

static int decoder_convert_with_sws(DecoderCtx* ctx, const DecodedFrame* src,
                                    const StreamInfo* info,
                                    DecodedFrame* dst, DecodeFormat target_format) {
    enum AVPixelFormat src_pix_fmt = av_format_from_decode(src->format);
    enum AVPixelFormat dst_pix_fmt = av_format_from_decode(target_format);
    const int bytes_per_pixel = decode_format_bytes_per_pixel(target_format);
    int uses_tls_cache = 0;
    struct SwsContext* sws_ctx = decoder_get_cached_sws_ctx(ctx, &uses_tls_cache);
    const int free_after_use = (!ctx && !uses_tls_cache);

    if (src_pix_fmt == AV_PIX_FMT_NONE || dst_pix_fmt == AV_PIX_FMT_NONE || bytes_per_pixel <= 0) {
        return -1;
    }

    sws_ctx = sws_getCachedContext(
        sws_ctx,
        src->width, src->height, src_pix_fmt,
        src->width, src->height, dst_pix_fmt,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        return -1;
    }
    decoder_store_cached_sws_ctx(ctx, sws_ctx, uses_tls_cache);

    int coeffs = SWS_CS_ITU709;
    int src_range = 0;
    infer_sws_color_details(info, src, &coeffs, &src_range);
    if (sws_setColorspaceDetails(
            sws_ctx,
            sws_getCoefficients(coeffs), src_range,
            sws_getCoefficients(coeffs), 1,
            0, 1 << 16, 1 << 16) < 0) {
        if (free_after_use) {
            sws_freeContext(sws_ctx);
        }
        return -1;
    }

    dst->width = src->width;
    dst->height = src->height;
    dst->format = target_format;
    dst->storage = DECODE_STORAGE_CPU;
    dst->pts = src->pts;
    dst->key_frame = src->key_frame;
    dst->av_frame = NULL;
    dst->_decoder_ctx = NULL;
    memset(dst->data, 0, sizeof(dst->data));
    memset(dst->linesize, 0, sizeof(dst->linesize));
    dst->linesize[0] = src->width * bytes_per_pixel;
    dst->data[0] = malloc((size_t)dst->linesize[0] * (size_t)src->height);
    if (!dst->data[0]) {
        if (free_after_use) {
            sws_freeContext(sws_ctx);
        }
        return -1;
    }

    const uint8_t* src_data[4] = { src->data[0], NULL, NULL, NULL };
    int src_linesize[4] = { src->linesize[0], 0, 0, 0 };
    uint8_t* dst_data[4] = { dst->data[0], NULL, NULL, NULL };
    int dst_linesize[4] = { dst->linesize[0], 0, 0, 0 };

    if (src->format == DECODE_FMT_YUV420P || src->format == DECODE_FMT_YUV444P) {
        src_data[1] = src->data[1];
        src_data[2] = src->data[2];
        src_linesize[1] = src->linesize[1];
        src_linesize[2] = src->linesize[2];
    } else if (src->format == DECODE_FMT_NV12) {
        src_data[1] = src->data[1];
        src_linesize[1] = src->linesize[1];
    }

    const int scaled = sws_scale(sws_ctx, src_data, src_linesize, 0, src->height,
                                 dst_data, dst_linesize);
    if (free_after_use) {
        sws_freeContext(sws_ctx);
    }
    if (scaled <= 0) {
        free(dst->data[0]);
        dst->data[0] = NULL;
        dst->linesize[0] = 0;
        return -1;
    }

    return 0;
}

int decoder_convert_format_with_info(DecoderCtx* ctx, const DecodedFrame* src,
                                     const StreamInfo* info,
                                     DecodedFrame* dst, DecodeFormat target_format) {
    if (!src || !dst) return -1;

    const DecodedFrame* conv_src = src;
    DecodedFrame* materialized = NULL;
    if (src->storage == DECODE_STORAGE_HW) {
        if (decoder_materialize_frame(src, &materialized) != 0) {
            return -1;
        }
        conv_src = materialized;
    }

    if (target_format != DECODE_FMT_BGRA &&
        target_format != DECODE_FMT_BGR24 &&
        target_format != DECODE_FMT_RGB24) {
        decoder_free_frame(materialized);
        return -1;
    }

    if (target_format == DECODE_FMT_BGR24) {
        record_bgr24_request();
    }

    memset(dst, 0, sizeof(*dst));

#ifdef HAVE_LIBYUV
    if (target_format == DECODE_FMT_BGR24) {
        struct timespec convert_start, convert_end;
        clock_gettime(CLOCK_MONOTONIC, &convert_start);
        if (decoder_fast_convert_to_bgr24(conv_src, info, dst) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &convert_end);
            record_convert_path_time(
                conv_src->format == DECODE_FMT_VUYX
                    ? DECODER_CONVERT_PATH_VUYX_BGR24
                    : DECODER_CONVERT_PATH_LIBYUV_BGR24,
                elapsed_ms_since(&convert_start, &convert_end));
            decoder_free_frame(materialized);
            return 0;
        }
    }
#endif

    struct timespec convert_start, convert_end;
    clock_gettime(CLOCK_MONOTONIC, &convert_start);
    if (decoder_convert_with_sws(ctx, conv_src, info, dst, target_format) != 0) {
        decoder_free_frame(materialized);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &convert_end);
    record_convert_path_time(DECODER_CONVERT_PATH_SWSCALE,
                             elapsed_ms_since(&convert_start, &convert_end));

    decoder_free_frame(materialized);
    return 0;
}

int decoder_convert_format(DecoderCtx* ctx, const DecodedFrame* src,
                           DecodedFrame* dst, DecodeFormat target_format) {
    return decoder_convert_format_with_info(ctx, src, NULL, dst, target_format);
}

void decoder_get_stats(DecoderCtx* ctx, DecoderStats* stats) {
    if (!ctx || !stats) return;
    memcpy(stats, &ctx->stats, sizeof(*stats));
    snapshot_hw_transfer_stats(ctx->transfer_stats, stats);
}

void decoder_get_convert_stats(DecoderConvertStats* stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    pthread_mutex_lock(&g_convert_stats.lock);
    stats->bgr24_request_count = g_convert_stats.bgr24_request_count;
    stats->libyuv_bgr24_count = g_convert_stats.libyuv_bgr24_count;
    if (g_convert_stats.libyuv_bgr24_count > 0) {
        stats->avg_libyuv_bgr24_time_ms =
            g_convert_stats.total_libyuv_bgr24_time_ms /
            (double)g_convert_stats.libyuv_bgr24_count;
    }
    stats->vuyx_bgr24_count = g_convert_stats.vuyx_bgr24_count;
    if (g_convert_stats.vuyx_bgr24_count > 0) {
        stats->avg_vuyx_bgr24_time_ms =
            g_convert_stats.total_vuyx_bgr24_time_ms /
            (double)g_convert_stats.vuyx_bgr24_count;
    }
    stats->swscale_count = g_convert_stats.swscale_count;
    if (g_convert_stats.swscale_count > 0) {
        stats->avg_swscale_time_ms =
            g_convert_stats.total_swscale_time_ms /
            (double)g_convert_stats.swscale_count;
    }
    pthread_mutex_unlock(&g_convert_stats.lock);
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

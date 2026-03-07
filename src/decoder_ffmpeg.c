#include "decoder_ffmpeg.h"

#include <Limelight.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;

static int dec_frames_cnt;
static int current_frame, next_frame;

int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count) {
    (void)perf_lvl;

    av_log_set_level(AV_LOG_QUIET);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
    avcodec_register_all();
#endif

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Couldn't allocate packet\n");
        return -1;
    }

    decoder = NULL;

    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        decoder = avcodec_find_decoder_by_name("h264");
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        decoder = avcodec_find_decoder_by_name("hevc");
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        decoder = avcodec_find_decoder_by_name("libdav1d");
        if (!decoder) decoder = avcodec_find_decoder_by_name("av1");
    }

    if (!decoder) {
        fprintf(stderr, "Couldn't find decoder\n");
        return -1;
    }

    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        fprintf(stderr, "Couldn't allocate context\n");
        return -1;
    }

    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decoder_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    decoder_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    decoder_ctx->err_recognition = AV_EF_EXPLODE;
    decoder_ctx->thread_type = FF_THREAD_SLICE;
    decoder_ctx->thread_count = thread_count > 0 ? thread_count : 1;
    decoder_ctx->width = width;
    decoder_ctx->height = height;
    decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    int err = avcodec_open2(decoder_ctx, decoder, NULL);
    if (err < 0) {
        char errorstring[256];
        av_strerror(err, errorstring, sizeof(errorstring));
        fprintf(stderr, "Couldn't open codec: %s (%s)\n", decoder->name, errorstring);
        avcodec_free_context(&decoder_ctx);
        return -1;
    }

    fprintf(stderr, "Using FFmpeg decoder: %s\n", decoder->name);

    dec_frames_cnt = buffer_count;
    dec_frames = malloc(buffer_count * sizeof(AVFrame*));
    if (!dec_frames) {
        fprintf(stderr, "Couldn't allocate frames\n");
        return -1;
    }

    for (int i = 0; i < buffer_count; i++) {
        dec_frames[i] = av_frame_alloc();
        if (!dec_frames[i]) {
            fprintf(stderr, "Couldn't allocate frame\n");
            return -1;
        }
    }

    current_frame = 0;
    next_frame = 0;
    return 0;
}

void ffmpeg_destroy(void) {
    av_packet_free(&pkt);

    if (decoder_ctx) {
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = NULL;
    }

    if (dec_frames) {
        for (int i = 0; i < dec_frames_cnt; i++) {
            if (dec_frames[i]) {
                av_frame_free(&dec_frames[i]);
            }
        }
        free(dec_frames);
        dec_frames = NULL;
    }
}

AVFrame* ffmpeg_get_frame(bool native_frame) {
    (void)native_frame;

    int err = avcodec_receive_frame(decoder_ctx, dec_frames[next_frame]);
    if (err == 0) {
        current_frame = next_frame;
        next_frame = (current_frame + 1) % dec_frames_cnt;

        AVFrame *frame = dec_frames[current_frame];
        const char *fmt = av_get_pix_fmt_name(frame->format);

        fprintf(stderr, "frame: %dx%d fmt=%s pts=%lld\n",
                frame->width,
                frame->height,
                fmt ? fmt : "unknown",
                (long long)frame->pts);

        return frame;
    }
    else if (err != AVERROR(EAGAIN)) {
        char errorstring[256];
        av_strerror(err, errorstring, sizeof(errorstring));
        fprintf(stderr, "Receive failed - %d/%s\n", err, errorstring);
    }

    return NULL;
}

int ffmpeg_decode(unsigned char* indata, int inlen) {
    pkt->data = indata;
    pkt->size = inlen;

    int err = avcodec_send_packet(decoder_ctx, pkt);
    if (err < 0) {
        char errorstring[256];
        av_strerror(err, errorstring, sizeof(errorstring));
        fprintf(stderr, "Decode failed - %s\n", errorstring);
    }

    return err < 0 ? err : 0;
}

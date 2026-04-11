#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "decoder.h"

static const char* av_pix_fmt_name_or_unknown(enum AVPixelFormat fmt) {
    const char* name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
}

static const char* decode_format_name(DecodeFormat fmt) {
    switch (fmt) {
        case DECODE_FMT_NV12: return "nv12";
        case DECODE_FMT_YUV420P: return "yuv420p";
        case DECODE_FMT_YUV444P: return "yuv444p";
        case DECODE_FMT_VUYX: return "vuyx";
        case DECODE_FMT_BGR24: return "bgr24";
        case DECODE_FMT_BGRA: return "bgra";
        case DECODE_FMT_RGB24: return "rgb24";
        default: return "none";
    }
}

static const char* decode_storage_name(DecodeStorage storage) {
    switch (storage) {
        case DECODE_STORAGE_CPU: return "cpu";
        case DECODE_STORAGE_HW: return "hw";
        default: return "unknown";
    }
}

static int parse_backend(const char* text, DecodeBackend* out) {
    if (!text || !out) {
        return -1;
    }
    if (strcasecmp(text, "intel") == 0 || strcasecmp(text, "vaapi") == 0) {
        *out = DECODE_BACKEND_INTEL_VA;
        return 0;
    }
    if (strcasecmp(text, "nvidia") == 0 || strcasecmp(text, "cuda") == 0) {
        *out = DECODE_BACKEND_NVIDIA;
        return 0;
    }
    if (strcasecmp(text, "cpu") == 0) {
        *out = DECODE_BACKEND_CPU;
        return 0;
    }
    if (strcasecmp(text, "auto") == 0) {
        *out = DECODE_BACKEND_AUTO;
        return 0;
    }
    return -1;
}

static int open_video_file(const char* filename, AVFormatContext** fmt_ctx, int* stream_idx) {
    if (avformat_open_input(fmt_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }

    if (avformat_find_stream_info(*fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find stream info: %s\n", filename);
        return -1;
    }

    for (unsigned int i = 0; i < (*fmt_ctx)->nb_streams; i++) {
        if ((*fmt_ctx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            *stream_idx = (int)i;
            return 0;
        }
    }

    fprintf(stderr, "No video stream found: %s\n", filename);
    return -1;
}

static void print_frame_info(const char* tag, int frame_index, const DecodedFrame* frame) {
    if (!frame) {
        return;
    }
    printf("[%s] frame=%d storage=%s format=%s size=%dx%d key=%d pts=%lld\n",
           tag,
           frame_index,
           decode_storage_name(frame->storage),
           decode_format_name(frame->format),
           frame->width,
           frame->height,
           frame->key_frame ? 1 : 0,
           (long long)frame->pts);

    if (frame->av_frame) {
        const AVFrame* avf = (const AVFrame*)frame->av_frame;
        enum AVPixelFormat hw_sw_fmt = AV_PIX_FMT_NONE;
        if (avf->hw_frames_ctx) {
            const AVHWFramesContext* frames_ctx =
                (const AVHWFramesContext*)avf->hw_frames_ctx->data;
            if (frames_ctx) {
                hw_sw_fmt = frames_ctx->sw_format;
            }
        }
        printf("        raw_avfmt=%s hw_sw=%s linesize0=%d\n",
               av_pix_fmt_name_or_unknown((enum AVPixelFormat)avf->format),
               av_pix_fmt_name_or_unknown(hw_sw_fmt),
               avf->linesize[0]);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <intel|nvidia|cpu|auto> <video_file> [max_frames]\n", argv[0]);
        return 1;
    }

    DecodeBackend backend = DECODE_BACKEND_AUTO;
    if (parse_backend(argv[1], &backend) != 0) {
        fprintf(stderr, "Invalid backend: %s\n", argv[1]);
        return 1;
    }

    const char* video_file = argv[2];
    int max_frames = (argc > 3) ? atoi(argv[3]) : 10;
    if (max_frames <= 0) {
        max_frames = 10;
    }

    AVFormatContext* fmt_ctx = NULL;
    int stream_idx = -1;
    if (open_video_file(video_file, &fmt_ctx, &stream_idx) != 0) {
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
        }
        return 1;
    }

    AVStream* stream = fmt_ctx->streams[stream_idx];
    AVCodecParameters* codecpar = stream->codecpar;

    DecoderConfig config;
    memset(&config, 0, sizeof(config));
    config.backend = backend;
    config.codec_id = codecpar->codec_id;
    config.width = codecpar->width > 0 ? codecpar->width : 1024;
    config.height = codecpar->height > 0 ? codecpar->height : 768;
    config.output_format = DECODE_FMT_NV12;
    config.thread_count = 2;
    snprintf(config.va_device, sizeof(config.va_device), "%s", "/dev/dri/renderD128");
    config.cuda_device_id = 0;
    config.defer_hw_download = true;
    config.extra_hw_frames = 8;

    printf("backend=%s codec_id=%d file=%s max_frames=%d\n",
           decoder_backend_name(backend), codecpar->codec_id, video_file, max_frames);

    DecoderCtx* decoder = decoder_create(&config);
    if (!decoder) {
        avformat_close_input(&fmt_ctx);
        fprintf(stderr, "Failed to create decoder\n");
        return 1;
    }

    if (decoder_init(decoder, codecpar->extradata, codecpar->extradata_size) != 0) {
        decoder_destroy(decoder);
        avformat_close_input(&fmt_ctx);
        fprintf(stderr, "Failed to initialize decoder\n");
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        decoder_destroy(decoder);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int decoded_frames = 0;
    int decode_errors = 0;

    while (decoded_frames < max_frames && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            DecodedFrame* frame = NULL;
            int ret = decoder_decode(decoder, pkt->data, pkt->size, &frame);
            if (ret == 0 && frame) {
                decoded_frames++;
                print_frame_info("decoded", decoded_frames, frame);

                if (frame->storage == DECODE_STORAGE_HW) {
                    DecodedFrame* cpu_frame = NULL;
                    if (decoder_materialize_frame(frame, &cpu_frame) != 0) {
                        fprintf(stderr, "Failed to materialize hardware frame %d\n", decoded_frames);
                        decoder_free_frame(frame);
                        av_packet_unref(pkt);
                        av_packet_free(&pkt);
                        decoder_destroy(decoder);
                        avformat_close_input(&fmt_ctx);
                        return 2;
                    }
                    print_frame_info("materialized", decoded_frames, cpu_frame);
                    decoder_free_frame(cpu_frame);
                }

                decoder_free_frame(frame);
            } else if (ret < 0) {
                decode_errors++;
            }
        }

        av_packet_unref(pkt);
    }

    while (decoded_frames < max_frames) {
        DecodedFrame* frame = NULL;
        int ret = decoder_flush(decoder, &frame);
        if (ret != 0 || !frame) {
            break;
        }
        decoded_frames++;
        print_frame_info("flush", decoded_frames, frame);
        if (frame->storage == DECODE_STORAGE_HW) {
            DecodedFrame* cpu_frame = NULL;
            if (decoder_materialize_frame(frame, &cpu_frame) == 0) {
                print_frame_info("materialized", decoded_frames, cpu_frame);
                decoder_free_frame(cpu_frame);
            }
        }
        decoder_free_frame(frame);
    }

    DecoderStats stats;
    memset(&stats, 0, sizeof(stats));
    decoder_get_stats(decoder, &stats);

    printf("summary decoded_frames=%d decode_errors=%d stats.frames_decoded=%lu avg_decode_ms=%.3f avg_xfer_ms=%.3f\n",
           decoded_frames,
           decode_errors,
           stats.frames_decoded,
           stats.avg_decode_time_ms,
           stats.avg_hw_transfer_time_ms);

    av_packet_free(&pkt);
    decoder_destroy(decoder);
    avformat_close_input(&fmt_ctx);

    return decoded_frames > 0 ? 0 : 2;
}

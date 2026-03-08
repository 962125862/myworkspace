#include "video_callbacks.h"
#include "decoder_ffmpeg.h"
#include "worker_defs.h"

#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKER_BUFFER_FRAMES 8
#define SLICES_PER_FRAME 4
#define INITIAL_DECODER_BUFFER_SIZE 1048576

static unsigned char *ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;

static ShmWriter g_shm_writer;
static int g_shm_ready = 0;
static WorkerRenderConfig* g_cfg = NULL;

static uint64_t g_stats_last_ns = 0;
static uint64_t g_stats_frames_decoded = 0;
static uint64_t g_stats_frames_written = 0;

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

static void maybe_report_stats(void) {
    uint64_t now = now_monotonic_ns();

    if (g_stats_last_ns == 0) {
        g_stats_last_ns = now;
        return;
    }

    uint64_t delta = now - g_stats_last_ns;
    if (delta >= 1000000000ull) {
        double secs = (double)delta / 1e9;
        double decoded_fps = g_stats_frames_decoded / secs;
        double written_fps = g_stats_frames_written / secs;
        unsigned long long latest_frame_id =
            (g_shm_ready && g_shm_writer.hdr) ? (unsigned long long)g_shm_writer.hdr->latest_frame_id : 0ull;

        fprintf(stderr,
                "[video] decoded_fps=%.1f written_fps=%.1f latest_frame_id=%llu\n",
                decoded_fps, written_fps, latest_frame_id);

        g_stats_last_ns = now;
        g_stats_frames_decoded = 0;
        g_stats_frames_written = 0;
    }
}

static int ensure_buf_size(size_t needed) {
    if (ffmpeg_buffer_size >= needed) {
        return 0;
    }

    unsigned char *new_buf = realloc(ffmpeg_buffer, needed);
    if (!new_buf) {
        fprintf(stderr, "realloc failed\n");
        return -1;
    }

    ffmpeg_buffer = new_buf;
    ffmpeg_buffer_size = needed;
    memset(ffmpeg_buffer, 0, ffmpeg_buffer_size);
    return 0;
}

static int worker_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    (void)redrawRate;
    (void)drFlags;

    g_cfg = (WorkerRenderConfig*)context;

    if (ffmpeg_init(videoFormat, width, height, 0, WORKER_BUFFER_FRAMES, SLICES_PER_FRAME) < 0) {
        fprintf(stderr, "Couldn't initialize video decoding\n");
        return -1;
    }

    if (ensure_buf_size(INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE) < 0) {
        return -1;
    }

    const char* shm_name = "/ml_stream_00";
    uint32_t slot_count = 2;
    uint32_t color_space = ML_COLOR_SPACE_UNKNOWN;
    uint32_t color_range = ML_COLOR_RANGE_UNKNOWN;
    uint32_t fps = 0;

    if (g_cfg) {
        if (g_cfg->shm_name[0] != '\0') {
            shm_name = g_cfg->shm_name;
        }
        if (g_cfg->slot_count != 0) {
            slot_count = g_cfg->slot_count;
        }
        color_space = g_cfg->color_space;
        color_range = g_cfg->color_range;
        fps = g_cfg->fps;
    }

    if (shm_writer_open(&g_shm_writer, shm_name, width, height, slot_count, color_space, color_range, fps) < 0) {
        fprintf(stderr, "Couldn't create shared memory writer\n");
        set_fatal_once(WORKER_FATAL_SHM_OPEN);
        return -1;
    }

    g_shm_ready = 1;
    shm_writer_set_status(&g_shm_writer, ML_STREAM_STATUS_RUNNING, 0);

    fprintf(stderr, "shm writer ready: %s (%dx%d)\n", shm_name, width, height);

    g_stats_last_ns = now_monotonic_ns();
    g_stats_frames_decoded = 0;
    g_stats_frames_written = 0;

    return 0;
}

static void worker_cleanup(void) {
    if (g_shm_ready) {
        shm_writer_set_status(&g_shm_writer, ML_STREAM_STATUS_STOPPED, 0);
        shm_writer_close(&g_shm_writer, 0);
        g_shm_ready = 0;
    }

    ffmpeg_destroy();

    free(ffmpeg_buffer);
    ffmpeg_buffer = NULL;
    ffmpeg_buffer_size = 0;

    g_cfg = NULL;
}

static int worker_submit_decode_unit(PDECODE_UNIT decodeUnit) {
    PLENTRY entry = decodeUnit->bufferList;
    int length = 0;

    if (ensure_buf_size((size_t)decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE) < 0) {
        return DR_NEED_IDR;
    }

    while (entry != NULL) {
        memcpy(ffmpeg_buffer + length, entry->data, entry->length);
        length += entry->length;
        entry = entry->next;
    }

    if (ffmpeg_decode(ffmpeg_buffer, length) < 0) {
        return DR_NEED_IDR;
    }

    AVFrame* frame;
    while ((frame = ffmpeg_get_frame(false)) != NULL) {
        g_stats_frames_decoded++;

        if (g_shm_ready) {
            int rc = shm_writer_write_i420(&g_shm_writer, frame);
            if (rc == 0) {
                g_stats_frames_written++;
            } else if (rc == -2) {
                fprintf(stderr,
                        "fatal: frame size changed from shm %ux%u to decoded %dx%d\n",
                        g_shm_writer.hdr ? g_shm_writer.hdr->width : 0,
                        g_shm_writer.hdr ? g_shm_writer.hdr->height : 0,
                        frame->width, frame->height);
                shm_writer_set_status(&g_shm_writer, ML_STREAM_STATUS_ERROR, rc);
                set_fatal_once(WORKER_FATAL_FRAME_SIZE_CHANGED);
                return DR_NEED_IDR;
            } else {
                fprintf(stderr, "fatal: shm_writer_write_i420 failed: %d\n", rc);
                shm_writer_set_status(&g_shm_writer, ML_STREAM_STATUS_ERROR, rc);
                set_fatal_once(WORKER_FATAL_SHM_WRITE);
                return DR_NEED_IDR;
            }
        }
    }

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

#include "video_callbacks.h"
#include "decoder_ffmpeg.h"

#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_BUFFER_FRAMES 8
#define SLICES_PER_FRAME 4
#define INITIAL_DECODER_BUFFER_SIZE 1048576

static unsigned char *ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;

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
    (void)context;
    (void)drFlags;

    if (ffmpeg_init(videoFormat, width, height, 0, WORKER_BUFFER_FRAMES, SLICES_PER_FRAME) < 0) {
        fprintf(stderr, "Couldn't initialize video decoding\n");
        return -1;
    }

    if (ensure_buf_size(INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE) < 0) {
        return -1;
    }

    fprintf(stderr, "worker_setup ok: %dx%d\n", width, height);
    return 0;
}

static void worker_cleanup(void) {
    ffmpeg_destroy();
    free(ffmpeg_buffer);
    ffmpeg_buffer = NULL;
    ffmpeg_buffer_size = 0;
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

    while (ffmpeg_get_frame(false) != NULL) {
        // 先只打印 frame 信息
    }

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

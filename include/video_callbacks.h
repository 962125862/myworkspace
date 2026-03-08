#ifndef VIDEO_CALLBACKS_H
#define VIDEO_CALLBACKS_H

#include <stdint.h>
#include <Limelight.h>
#include "shm_writer.h"

typedef struct {
    char shm_name[ML_SHM_NAME_MAX];
    uint32_t slot_count;
    uint32_t color_space;   /* ML_COLOR_SPACE_* */
    uint32_t color_range;   /* ML_COLOR_RANGE_* */
    uint32_t fps;
    volatile int* fatal_code;
} WorkerRenderConfig;

extern DECODER_RENDERER_CALLBACKS video_callbacks;

#endif

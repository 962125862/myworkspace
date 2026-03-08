#ifndef SHM_WRITER_H
#define SHM_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <libavutil/frame.h>

#define ML_SHM_MAGIC 0x4d4c5955u
#define ML_SHM_VERSION 2u
#define ML_SHM_PIXFMT_I420 1u
#define ML_SHM_DEFAULT_SLOT_COUNT 2u
#define ML_SHM_NAME_MAX 128

#define ML_COLOR_SPACE_UNKNOWN 0u
#define ML_COLOR_SPACE_BT601   1u
#define ML_COLOR_SPACE_BT709   2u

#define ML_COLOR_RANGE_UNKNOWN 0u
#define ML_COLOR_RANGE_LIMITED 1u
#define ML_COLOR_RANGE_FULL    2u

#define ML_STREAM_STATUS_INIT    0u
#define ML_STREAM_STATUS_RUNNING 1u
#define ML_STREAM_STATUS_ERROR   2u
#define ML_STREAM_STATUS_STOPPED 3u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t slot_header_bytes;

    uint32_t width;
    uint32_t height;
    uint32_t pix_fmt;
    uint32_t slot_count;

    uint32_t stride_y;
    uint32_t stride_u;
    uint32_t stride_v;
    uint32_t reserved0;

    uint32_t bytes_y;
    uint32_t bytes_u;
    uint32_t bytes_v;
    uint32_t reserved1;

    uint32_t color_space;      /* ML_COLOR_SPACE_* */
    uint32_t color_range;      /* ML_COLOR_RANGE_* */
    uint32_t fps;
    uint32_t status;           /* ML_STREAM_STATUS_* */

    uint32_t writer_pid;
    int32_t  last_error_code;

    uint64_t slot_bytes;
    uint64_t total_bytes;
    uint64_t latest_frame_id;
    uint64_t heartbeat_ns;

    uint32_t current_slot;
    uint32_t reserved[9];
} MlShmHeader;

typedef struct {
    uint32_t seq;           /* odd=writer busy, even=stable */
    uint32_t valid;         /* 0=no frame yet, 1=valid */
    uint64_t frame_id;
    uint64_t monotonic_ns;
    uint32_t reserved[10];
} MlShmSlotHeader;

typedef struct {
    int fd;
    void* base;
    size_t map_size;
    MlShmHeader* hdr;
    uint64_t next_frame_id;
    char name[ML_SHM_NAME_MAX];
} ShmWriter;

int shm_writer_open(ShmWriter* w,
                    const char* shm_name,
                    int width,
                    int height,
                    uint32_t slot_count,
                    uint32_t color_space,
                    uint32_t color_range,
                    uint32_t fps);

void shm_writer_set_status(ShmWriter* w, uint32_t status, int32_t last_error_code);
void shm_writer_touch_heartbeat(ShmWriter* w);
void shm_writer_close(ShmWriter* w, int unlink_on_close);

/* 0=ok, -2=size changed, other<0=generic failure */
int shm_writer_write_i420(ShmWriter* w, const AVFrame* frame);

#endif

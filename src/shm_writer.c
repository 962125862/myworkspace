#include "shm_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/pixfmt.h>

static void writer_reset(ShmWriter* w) {
    memset(w, 0, sizeof(*w));
    w->fd = -1;
}

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint8_t* slot_base(const ShmWriter* w, uint32_t slot_index) {
    return (uint8_t*)w->base +
           (size_t)w->hdr->header_bytes +
           (size_t)slot_index * (size_t)w->hdr->slot_bytes;
}

static MlShmSlotHeader* slot_hdr(const ShmWriter* w, uint32_t slot_index) {
    return (MlShmSlotHeader*)slot_base(w, slot_index);
}

static uint8_t* slot_data(const ShmWriter* w, uint32_t slot_index) {
    return slot_base(w, slot_index) + w->hdr->slot_header_bytes;
}

static void copy_plane(uint8_t* dst, int dst_stride,
                       const uint8_t* src, int src_stride,
                       int row_bytes, int rows) {
    for (int y = 0; y < rows; y++) {
        memcpy(dst + (size_t)y * (size_t)dst_stride,
               src + (size_t)y * (size_t)src_stride,
               (size_t)row_bytes);
    }
}

void shm_writer_set_status(ShmWriter* w, uint32_t status, int32_t last_error_code) {
    if (!w || !w->hdr) {
        return;
    }

    w->hdr->status = status;
    w->hdr->last_error_code = last_error_code;
    w->hdr->heartbeat_ns = now_monotonic_ns();
    __sync_synchronize();
}

void shm_writer_touch_heartbeat(ShmWriter* w) {
    if (!w || !w->hdr) {
        return;
    }

    w->hdr->heartbeat_ns = now_monotonic_ns();
    __sync_synchronize();
}

int shm_writer_open(ShmWriter* w,
                    const char* shm_name,
                    int width,
                    int height,
                    uint32_t slot_count,
                    uint32_t color_space,
                    uint32_t color_range,
                    uint32_t fps) {
    if (!w || !shm_name) {
        fprintf(stderr, "shm_writer_open: invalid args\n");
        return -1;
    }

    if (shm_name[0] != '/') {
        fprintf(stderr, "shm_writer_open: shm name must start with '/': %s\n", shm_name);
        return -1;
    }

    if ((width & 1) || (height & 1)) {
        fprintf(stderr, "shm_writer_open: I420 requires even width/height, got %dx%d\n", width, height);
        return -1;
    }

    if (slot_count == 0) {
        slot_count = ML_SHM_DEFAULT_SLOT_COUNT;
    }

    writer_reset(w);
    snprintf(w->name, sizeof(w->name), "%s", shm_name);

    uint32_t stride_y = (uint32_t)width;
    uint32_t stride_u = (uint32_t)(width / 2);
    uint32_t stride_v = (uint32_t)(width / 2);

    uint32_t bytes_y = stride_y * (uint32_t)height;
    uint32_t bytes_u = stride_u * (uint32_t)(height / 2);
    uint32_t bytes_v = stride_v * (uint32_t)(height / 2);

    uint64_t slot_bytes =
        (uint64_t)sizeof(MlShmSlotHeader) +
        (uint64_t)bytes_y +
        (uint64_t)bytes_u +
        (uint64_t)bytes_v;

    uint64_t total_bytes =
        (uint64_t)sizeof(MlShmHeader) +
        (uint64_t)slot_count * slot_bytes;

    if (total_bytes > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "shm_writer_open: total_bytes too large\n");
        return -1;
    }

    w->fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (w->fd < 0) {
        fprintf(stderr, "shm_open(%s) failed: %s\n", shm_name, strerror(errno));
        shm_writer_close(w, 0);
        return -1;
    }

    if (ftruncate(w->fd, (off_t)total_bytes) < 0) {
        fprintf(stderr, "ftruncate(%s) failed: %s\n", shm_name, strerror(errno));
        shm_writer_close(w, 0);
        return -1;
    }

    w->map_size = (size_t)total_bytes;
    w->base = mmap(NULL, w->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
    if (w->base == MAP_FAILED) {
        fprintf(stderr, "mmap(%s) failed: %s\n", shm_name, strerror(errno));
        w->base = NULL;
        shm_writer_close(w, 0);
        return -1;
    }

    memset(w->base, 0, w->map_size);
    w->hdr = (MlShmHeader*)w->base;
    w->next_frame_id = 0;

    w->hdr->magic = ML_SHM_MAGIC;
    w->hdr->version = ML_SHM_VERSION;
    w->hdr->header_bytes = (uint32_t)sizeof(MlShmHeader);
    w->hdr->slot_header_bytes = (uint32_t)sizeof(MlShmSlotHeader);

    w->hdr->width = (uint32_t)width;
    w->hdr->height = (uint32_t)height;
    w->hdr->pix_fmt = ML_SHM_PIXFMT_I420;
    w->hdr->slot_count = slot_count;

    w->hdr->stride_y = stride_y;
    w->hdr->stride_u = stride_u;
    w->hdr->stride_v = stride_v;

    w->hdr->bytes_y = bytes_y;
    w->hdr->bytes_u = bytes_u;
    w->hdr->bytes_v = bytes_v;

    w->hdr->color_space = color_space;
    w->hdr->color_range = color_range;
    w->hdr->fps = fps;
    w->hdr->status = ML_STREAM_STATUS_INIT;

    w->hdr->writer_pid = (uint32_t)getpid();
    w->hdr->last_error_code = 0;

    w->hdr->slot_bytes = slot_bytes;
    w->hdr->total_bytes = total_bytes;
    w->hdr->latest_frame_id = 0;
    w->hdr->heartbeat_ns = now_monotonic_ns();

    w->hdr->current_slot = 0;

    return 0;
}

void shm_writer_close(ShmWriter* w, int unlink_on_close) {
    if (!w) {
        return;
    }

    if (w->hdr) {
        shm_writer_set_status(w, ML_STREAM_STATUS_STOPPED, 0);
    }

    if (w->base) {
        munmap(w->base, w->map_size);
        w->base = NULL;
    }

    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }

    if (unlink_on_close && w->name[0] != '\0') {
        shm_unlink(w->name);
    }

    writer_reset(w);
}

int shm_writer_write_i420(ShmWriter* w, const AVFrame* frame) {
    if (!w || !w->hdr || !frame) {
        return -1;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        fprintf(stderr, "shm_writer_write_i420: expected AV_PIX_FMT_YUV420P, got %d\n", frame->format);
        return -1;
    }

    if ((uint32_t)frame->width != w->hdr->width || (uint32_t)frame->height != w->hdr->height) {
        return -2;
    }

    uint32_t slot_index = 0;
    if (w->hdr->latest_frame_id != 0) {
        slot_index = (w->hdr->current_slot + 1) % w->hdr->slot_count;
    }

    MlShmSlotHeader* sh = slot_hdr(w, slot_index);
    uint8_t* data = slot_data(w, slot_index);

    uint8_t* y_dst = data;
    uint8_t* u_dst = y_dst + w->hdr->bytes_y;
    uint8_t* v_dst = u_dst + w->hdr->bytes_u;

    uint32_t start_seq = sh->seq;
    if (start_seq & 1u) {
        start_seq++;
    }

    sh->seq = start_seq + 1u;
    __sync_synchronize();

    copy_plane(
        y_dst, (int)w->hdr->stride_y,
        frame->data[0], frame->linesize[0],
        (int)w->hdr->width, (int)w->hdr->height
    );

    copy_plane(
        u_dst, (int)w->hdr->stride_u,
        frame->data[1], frame->linesize[1],
        (int)(w->hdr->width / 2), (int)(w->hdr->height / 2)
    );

    copy_plane(
        v_dst, (int)w->hdr->stride_v,
        frame->data[2], frame->linesize[2],
        (int)(w->hdr->width / 2), (int)(w->hdr->height / 2)
    );

    uint64_t frame_id = ++w->next_frame_id;
    sh->frame_id = frame_id;
    sh->monotonic_ns = now_monotonic_ns();
    sh->valid = 1;

    __sync_synchronize();
    sh->seq = start_seq + 2u;
    __sync_synchronize();

    w->hdr->latest_frame_id = frame_id;
    w->hdr->current_slot = slot_index;
    w->hdr->heartbeat_ns = sh->monotonic_ns;
    __sync_synchronize();

    return 0;
}

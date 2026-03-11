/**
 * @file shm_frame.c
 * @brief POSIX shared memory 最新帧发布（writer）
 */

#define _GNU_SOURCE

#include "shm_frame.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

static inline uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t align_up(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

static int compute_layout(uint32_t w, uint32_t h, ShmPixFmt fmt,
                          uint32_t plane_size[4], uint32_t linesize[4],
                          uint32_t plane_offset[4], uint32_t* total_data) {
    memset(plane_size, 0, 4 * sizeof(uint32_t));
    memset(linesize, 0, 4 * sizeof(uint32_t));
    memset(plane_offset, 0, 4 * sizeof(uint32_t));

    if (w == 0 || h == 0) return -1;

    if (fmt == SHM_PIXFMT_NV12) {
        linesize[0] = w;
        linesize[1] = w;
        plane_size[0] = w * h;
        plane_size[1] = w * (h / 2);
    } else if (fmt == SHM_PIXFMT_YUV420P) {
        linesize[0] = w;
        linesize[1] = w / 2;
        linesize[2] = w / 2;
        plane_size[0] = w * h;
        plane_size[1] = (w / 2) * (h / 2);
        plane_size[2] = (w / 2) * (h / 2);
    } else {
        return -1;
    }

    uint32_t off = 0;
    for (int i = 0; i < 4; i++) {
        if (plane_size[i] == 0) continue;
        plane_offset[i] = off;
        off += plane_size[i];
    }
    *total_data = off;
    return 0;
}

/* 为了支持“先创建 shm，再根据实际解码帧格式发布”，计算 4:2:0 最大数据量 */
static uint32_t max_420_size(uint32_t w, uint32_t h) {
    /* NV12 / YUV420P 都是 1.5 * w * h */
    return (uint32_t)((uint64_t)w * (uint64_t)h * 3ull / 2ull);
}

int shm_frame_writer_open(ShmFrameWriter* w,
                          const char* shm_name,
                          uint32_t width,
                          uint32_t height) {
    if (!w || !shm_name) return -1;
    memset(w, 0, sizeof(*w));
    w->fd = -1;

    snprintf(w->name, sizeof(w->name), "%s", shm_name);

    /* 若 name 不以 '/' 开头，补上（POSIX shm_open 要求） */
    char fixed_name[128];
    if (shm_name[0] != '/') {
        snprintf(fixed_name, sizeof(fixed_name), "/%s", shm_name);
        snprintf(w->name, sizeof(w->name), "%s", fixed_name);
    }

    /* 先按最大 4:2:0 大小分配；真正的 plane_* 在 publish 时更新 */
    uint32_t total_data = max_420_size(width, height);
    uint32_t plane_size[4] = {0}, linesize[4] = {0}, plane_offset[4] = {0};

    size_t header_sz = align_up(sizeof(ShmFrameHeader), 64);
    size_t shm_sz = header_sz + (size_t)total_data;

    /* 重建 shm（简单策略：unlink 旧对象） */
    shm_unlink(w->name);
    int fd = shm_open(w->name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "[SHM] shm_open(%s) failed: %s\n", w->name, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)shm_sz) < 0) {
        fprintf(stderr, "[SHM] ftruncate failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(w->name);
        return -1;
    }

    void* base = mmap(NULL, shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "[SHM] mmap failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(w->name);
        return -1;
    }

    w->fd = fd;
    w->base = base;
    w->size = shm_sz;
    w->hdr = (ShmFrameHeader*)base;

    memset(w->hdr, 0, sizeof(*w->hdr));
    w->hdr->magic = SHM_FRAME_MAGIC;
    w->hdr->version = SHM_FRAME_VERSION;
    w->hdr->header_size = (uint16_t)sizeof(ShmFrameHeader);
    atomic_store_explicit(&w->hdr->write_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&w->hdr->request_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&w->hdr->publish_seq, 0, memory_order_relaxed);
    w->hdr->width = width;
    w->hdr->height = height;
    w->hdr->pixfmt = (uint32_t)SHM_PIXFMT_NONE;
    for (int i = 0; i < 4; i++) {
        w->hdr->linesize[i] = linesize[i];
        w->hdr->plane_size[i] = plane_size[i];
        w->hdr->plane_offset[i] = (uint32_t)header_sz + plane_offset[i];
    }
    /* total_data_size 这里表示“容量”，实际发布的数据量会在 publish 时更新 */
    w->hdr->total_data_size = total_data;
    w->hdr->total_shm_size = (uint32_t)shm_sz;

    const char* always = getenv("SHM_ALWAYS");
    w->always_publish = (always && atoi(always) > 0);
    w->last_served_request = 0;
    w->publish_counter = 0;
    return 0;
}

void shm_frame_writer_close(ShmFrameWriter* w) {
    if (!w) return;
    if (w->base && w->base != MAP_FAILED) {
        munmap(w->base, w->size);
    }
    if (w->fd >= 0) {
        close(w->fd);
    }
    memset(w, 0, sizeof(*w));
    w->fd = -1;
}

static ShmPixFmt to_shm_fmt(DecodeFormat fmt) {
    if (fmt == DECODE_FMT_NV12) return SHM_PIXFMT_NV12;
    if (fmt == DECODE_FMT_YUV420P) return SHM_PIXFMT_YUV420P;
    return SHM_PIXFMT_NONE;
}

static int copy_compact_nv12(uint8_t* dst_y, uint8_t* dst_uv,
                             const DecodedFrame* f) {
    const uint8_t* src_y = f->data[0];
    const uint8_t* src_uv = f->data[1];
    if (!src_y || !src_uv) return -1;

    int w = f->width, h = f->height;
    int ls_y = f->linesize[0];
    int ls_uv = f->linesize[1];
    for (int r = 0; r < h; r++) {
        memcpy(dst_y + (size_t)r * (size_t)w, src_y + (size_t)r * (size_t)ls_y, (size_t)w);
    }
    for (int r = 0; r < h / 2; r++) {
        memcpy(dst_uv + (size_t)r * (size_t)w, src_uv + (size_t)r * (size_t)ls_uv, (size_t)w);
    }
    return 0;
}

/* 将 YUV420P 转成 NV12（紧凑布局）：dst_y(w*h) + dst_uv(w*h/2)
 * 仅做像素格式重排，不做缩放/颜色空间变化。
 */
static int copy_yuv420p_to_nv12(uint8_t* dst_y, uint8_t* dst_uv, const DecodedFrame* f) {
    const uint8_t* src_y = f->data[0];
    const uint8_t* src_u = f->data[1];
    const uint8_t* src_v = f->data[2];
    if (!src_y || !src_u || !src_v) return -1;

    int w = f->width, h = f->height;
    int w2 = w / 2, h2 = h / 2;
    int ls_y = f->linesize[0];
    int ls_u = f->linesize[1];
    int ls_v = f->linesize[2];

    /* copy Y */
    for (int r = 0; r < h; r++) {
        memcpy(dst_y + (size_t)r * (size_t)w, src_y + (size_t)r * (size_t)ls_y, (size_t)w);
    }

    /* interleave UV */
    for (int r = 0; r < h2; r++) {
        const uint8_t* urow = src_u + (size_t)r * (size_t)ls_u;
        const uint8_t* vrow = src_v + (size_t)r * (size_t)ls_v;
        uint8_t* uvrow = dst_uv + (size_t)r * (size_t)w;
        for (int c = 0; c < w2; c++) {
            uvrow[2 * c + 0] = urow[c];
            uvrow[2 * c + 1] = vrow[c];
        }
    }
    return 0;
}

/* 预留：将来如果需要对外发布 YUV420P 可启用（当前统一发布 NV12，因此未使用） */
/* 预留：将来如果需要对外发布 YUV420P 可启用（当前统一发布 NV12，因此未使用） */
static int copy_compact_yuv420p(uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                                const DecodedFrame* f) __attribute__((unused));
static int copy_compact_yuv420p(uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                                const DecodedFrame* f) {
    const uint8_t* src_y = f->data[0];
    const uint8_t* src_u = f->data[1];
    const uint8_t* src_v = f->data[2];
    if (!src_y || !src_u || !src_v) return -1;

    int w = f->width, h = f->height;
    int w2 = w / 2, h2 = h / 2;
    int ls_y = f->linesize[0];
    int ls_u = f->linesize[1];
    int ls_v = f->linesize[2];

    for (int r = 0; r < h; r++) {
        memcpy(dst_y + (size_t)r * (size_t)w, src_y + (size_t)r * (size_t)ls_y, (size_t)w);
    }
    for (int r = 0; r < h2; r++) {
        memcpy(dst_u + (size_t)r * (size_t)w2, src_u + (size_t)r * (size_t)ls_u, (size_t)w2);
        memcpy(dst_v + (size_t)r * (size_t)w2, src_v + (size_t)r * (size_t)ls_v, (size_t)w2);
    }
    return 0;
}

int shm_frame_writer_maybe_publish(ShmFrameWriter* w, const DecodedFrame* frame) {
    if (!w || !w->hdr || !frame) return -1;

    /* 对外统一发布 NV12。
     * - 解码输出 NV12: 直接拷贝
     * - 解码输出 YUV420P: publish 时转成 NV12（仅在下游请求时发生）
     */
    ShmPixFmt src_fmt = to_shm_fmt(frame->format);
    if (src_fmt == SHM_PIXFMT_NONE) return -1;
    /* 对外固定 NV12 */
    if (frame->width != (int)w->hdr->width || frame->height != (int)w->hdr->height) {
        /* 目前简单实现：尺寸变化不支持，避免 reader 读错布局 */
        return -1;
    }

    /* 计算本帧(对外NV12)布局，并写回 header（seqlock 保护下） */
    uint32_t plane_size[4], linesize[4], plane_offset[4], actual_total = 0;
    if (compute_layout((uint32_t)frame->width, (uint32_t)frame->height, SHM_PIXFMT_NV12,
                       plane_size, linesize, plane_offset, &actual_total) < 0) {
        return -1;
    }
    size_t header_sz = align_up(sizeof(ShmFrameHeader), 64);
    if (actual_total > (uint32_t)(w->size - header_sz)) {
        return -1;
    }

    uint32_t req = atomic_load_explicit(&w->hdr->request_seq, memory_order_acquire);
    if (!w->always_publish) {
        if (req == w->last_served_request) {
            return 0; /* 没有新请求，不拷贝 */
        }
    }

    /* seqlock begin: write_seq++ -> odd */
    uint32_t seq = atomic_load_explicit(&w->hdr->write_seq, memory_order_relaxed);
    atomic_store_explicit(&w->hdr->write_seq, seq + 1, memory_order_release);

    /* 写 header 元信息 + 本帧布局 */
    w->hdr->pts = (uint64_t)frame->pts;
    w->hdr->mono_ns = monotonic_ns();
    w->hdr->key_frame = frame->key_frame ? 1u : 0u;
    w->hdr->pixfmt = (uint32_t)SHM_PIXFMT_NV12;
    for (int i = 0; i < 4; i++) {
        w->hdr->linesize[i] = linesize[i];
        w->hdr->plane_size[i] = plane_size[i];
        w->hdr->plane_offset[i] = (uint32_t)header_sz + plane_offset[i];
    }
    w->hdr->total_data_size = actual_total;

    /* copy 数据 */
    uint8_t* base = (uint8_t*)w->base;
    uint8_t* dst_y = base + w->hdr->plane_offset[0];
    uint8_t* dst_uv = base + w->hdr->plane_offset[1];
    if (src_fmt == SHM_PIXFMT_NV12) {
        if (copy_compact_nv12(dst_y, dst_uv, frame) < 0) {
            atomic_store_explicit(&w->hdr->write_seq, seq + 2, memory_order_release);
            return -1;
        }
    } else if (src_fmt == SHM_PIXFMT_YUV420P) {
        if (copy_yuv420p_to_nv12(dst_y, dst_uv, frame) < 0) {
            atomic_store_explicit(&w->hdr->write_seq, seq + 2, memory_order_release);
            return -1;
        }
    }

    /* seqlock end: write_seq++ -> even */
    atomic_store_explicit(&w->hdr->write_seq, seq + 2, memory_order_release);

    /* 发布序号 */
    w->publish_counter++;
    if (w->always_publish) {
        atomic_store_explicit(&w->hdr->publish_seq, w->publish_counter, memory_order_release);
    } else {
        w->last_served_request = req;
        atomic_store_explicit(&w->hdr->publish_seq, req, memory_order_release);
    }
    return 0;
}

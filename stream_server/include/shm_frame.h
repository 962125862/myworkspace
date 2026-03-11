/**
 * @file shm_frame.h
 * @brief 将最新一帧 YUV 写入 POSIX shared memory，供下游进程读取
 *
 * 设计目标：
 *   - 高性能：下游不再通过函数调用拿 last_frame，而是直接读共享内存
 *   - 按需拷贝：默认仅当下游“请求”时才发布一帧，避免每帧都 memcpy
 *   - 跨进程安全：使用 seqlock(写序号) 让读者看到一致的元数据+数据
 *
 * 使用方式（Writer = stream_server）：
 *   1) shm_frame_writer_open(&w, "/stream_server_stream_01", width, height, fmt)
 *   2) 每次解码得到 frame 后调用 shm_frame_writer_maybe_publish(&w, frame)
 *
 * 使用方式（Reader = 下游进程）：
 *   - shm_open 同名对象并 mmap
 *   - atomic_fetch_add(request_seq, 1) 发起一次“请求新帧”
 *   - 等待 publish_seq >= request_seq
 *   - 按 seqlock 读取：
 *       do { seq1=write_seq; if(seq1&1) continue; 读 header+data; seq2=write_seq; }
 *       while(seq1!=seq2 || (seq2&1));
 */

#ifndef SHM_FRAME_H
#define SHM_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* shm header magic/version */
#define SHM_FRAME_MAGIC   0x53534652u /* 'SSFR' */
#define SHM_FRAME_VERSION 1

/* 共享内存中存储的像素格式（与 DecodeFormat 一致，但固定为 YUV） */
typedef enum {
    SHM_PIXFMT_NONE    = 0,
    SHM_PIXFMT_NV12    = 1,
    SHM_PIXFMT_YUV420P = 2,
} ShmPixFmt;

/*
 * 共享内存布局：
 *   [ShmFrameHeader][padding][plane0][plane1][plane2]
 * plane 数据使用“紧凑打包”布局：
 *   NV12:   Y(w*h) + UV(w*h/2)
 *   YUV420P:Y(w*h) + U(w*h/4) + V(w*h/4)
 *
 * 当前默认发布策略：
 *   - shm 对外统一发布 NV12
 *   - 若解码输出为 YUV420P，则在 publish 时转换为 NV12（仅在下游请求时发生）
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    /* seqlock：writer 写入期间为奇数，写完变回偶数 */
    _Atomic uint32_t write_seq;

    /* 按需发布：reader 自增 request_seq 请求“下一帧”；writer 发布后更新 publish_seq */
    _Atomic uint32_t request_seq;
    _Atomic uint32_t publish_seq;

    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;     /* ShmPixFmt */

    uint32_t linesize[4];
    uint32_t plane_offset[4]; /* offset from shm base */
    uint32_t plane_size[4];

    uint64_t pts;
    uint64_t mono_ns;    /* CLOCK_MONOTONIC 时间戳 */
    uint32_t key_frame;

    uint32_t total_data_size;
    uint32_t total_shm_size;

    uint8_t  reserved[64];
} ShmFrameHeader;

typedef struct ShmFrameWriter {
    int fd;
    void* base;
    size_t size;
    ShmFrameHeader* hdr;
    char name[128];

    uint32_t last_served_request;
    bool always_publish;   /* env: SHM_ALWAYS=1 */
    uint32_t publish_counter;
} ShmFrameWriter;

/**
 * 打开/创建 shm writer（若已有旧对象会先 unlink 再重建）。
 *
 * 说明：
 * - 这里不要求提前知道最终输出 pixfmt（NV12 还是 YUV420P）。
 * - 会按 width/height 分配“足够容纳两种 4:2:0 格式”的最大缓冲。
 * - 实际 pixfmt/plane_offset/plane_size 会在每次 publish 时更新。
 */
int shm_frame_writer_open(ShmFrameWriter* w,
                          const char* shm_name,
                          uint32_t width,
                          uint32_t height);

/** 关闭 shm writer（unmap + close；默认不 unlink，避免影响 reader 的已映射对象） */
void shm_frame_writer_close(ShmFrameWriter* w);

/**
 * @brief 按需发布一帧到共享内存
 * - 默认：仅当 request_seq 发生变化时才会复制并发布
 * - 若设置 SHM_ALWAYS=1：每次都发布（publish_seq 自增）
 */
int shm_frame_writer_maybe_publish(ShmFrameWriter* w, const DecodedFrame* frame);

#ifdef __cplusplus
}
#endif

#endif /* SHM_FRAME_H */

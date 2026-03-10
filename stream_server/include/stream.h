/**
 * @file stream.h
 * @brief 视频流生命周期管理与解码调度
 *
 * 管理多路视频流的完整生命周期:
 *   IDLE -> CONNECTING -> ACTIVE -> IDLE
 *
 * 每路流 (StreamContext) 包含:
 *   - 流元信息 (StreamInfo: 分辨率/帧率/码率)
 *   - 独立的解码器实例 (DecoderCtx)
 *   - 解码统计 (FPS/延迟/丢帧)
 *   - 最后一帧缓存 (供下游 YOLO 推理使用)
 *
 * 流管理器 (StreamManager) 维护固定大小的流数组 (MAX_STREAMS)，
 * 并提供压力测试功能: 将一路实时流复制到 N 个虚拟流并行解码。
 *
 * 线程安全:
 *   - StreamManager 有全局锁 (mgr->lock)
 *   - 每个 StreamContext 有独立锁 (stream->lock)
 *   - 压力测试线程池通过 per-thread queue lock + cond 通信
 */

#ifndef STREAM_H
#define STREAM_H

#include "protocol.h"
#include "decoder.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 流状态机 ==================== */

typedef enum {
    STREAM_STATE_IDLE = 0,       /* 空闲，未使用 */
    STREAM_STATE_CONNECTING,     /* 正在建立连接 */
    STREAM_STATE_ACTIVE,         /* 活跃，正在接收/解码 */
    STREAM_STATE_ERROR           /* 错误状态 */
} StreamState;

/* ==================== 解码统计 ==================== */

/** 单路流的解码性能统计 */
typedef struct {
    uint64_t frames_decoded;        /* 累计解码帧数 */
    uint64_t frames_dropped;        /* 累计丢帧数 */
    double avg_decode_time_ms;      /* 平均解码耗时 (ms/frame) */
    double current_fps;             /* 当前实时 FPS (每秒更新) */
    time_t last_fps_calc_time;      /* 上次 FPS 计算时刻 */
    uint64_t last_frames_decoded;   /* 上次计算时的累计帧数 */
    double total_decode_time_ms;    /* 累计解码耗时 (ms) */
    uint64_t max_decode_time_ms;    /* 单帧最大解码耗时 (ms) */
} StreamDecodeStats;

/* ==================== 流上下文 ==================== */

/**
 * @brief 单路视频流的完整上下文
 *
 * 包含流的元信息、状态、统计、解码器和最后一帧缓存。
 * 每个 StreamContext 有独立的 mutex 保护，支持多线程并发访问。
 */
typedef struct {
    uint16_t stream_id;            /* 流 ID (1 ~ MAX_STREAMS) */
    char name[32];                 /* 可读名称 (如 "stream_01") */
    StreamState state;             /* 当前状态 */

    /* 流元信息 (由 STREAM_START 消息填充) */
    StreamInfo info;               /* 视频参数: 宽/高/帧率/码率 */
    bool info_received;            /* 是否已收到 STREAM_START */

    /* 网络接收统计 */
    uint64_t frames_received;      /* 接收到的视频帧数 */
    uint64_t bytes_received;       /* 接收到的总字节数 */
    uint64_t packets_received;     /* 接收到的协议包数 */
    time_t connect_time;           /* 连接建立时间 */
    time_t last_frame_time;        /* 最后一帧接收时间 */

    /* 解码器 */
    void* decoder_ctx;             /* DecoderCtx* (不透明指针) */
    StreamDecodeStats decode_stats;/* 解码性能统计 */
    bool decoder_initialized;      /* 解码器是否已初始化 */
    DecodedFrame* last_frame;      /* 最后解码帧 (供 YOLO 推理使用) */

    /* 线程安全 */
    pthread_mutex_t lock;          /* 保护本结构所有字段 */
} StreamContext;

/* ==================== 压力测试配置 ==================== */

/**
 * @brief 压力测试模式配置
 *
 * 将一路实时流的数据复制到 N 个虚拟流，每个虚拟流有独立的:
 * - 解码器实例 (独立的 DecoderCtx)
 * - 解码线程 (stress_decode_worker)
 * - 数据队列 (queue_data/queue_lock/queue_cond)
 *
 * 数据流: 源流数据 -> stream_stress_test_decode() 分发
 *          -> 各线程队列 -> stress_decode_worker() 独立解码
 */
typedef struct {
    bool enabled;                  /* 是否启用 */
    uint16_t source_stream_id;     /* 源流 ID (真实接收的流) */
    int num_virtual_streams;       /* 虚拟流数量 */
    int* virtual_stream_ids;       /* 虚拟流 ID 数组 */
    DecodeBackend backend;         /* 解码后端类型 */
    time_t start_time;             /* 测试开始时间 */
    time_t end_time;               /* 测试结束时间 */

    /* 并行解码线程池 (每个虚拟流一个线程) */
    pthread_t* threads;            /* 解码线程数组 */
    pthread_mutex_t* queue_locks;  /* 每线程的数据队列锁 */
    pthread_cond_t*  queue_conds;  /* 每线程的条件变量 (有新数据时通知) */
    uint8_t** queue_data;          /* 每线程的数据缓冲区 */
    int* queue_size;               /* 每线程当前数据大小 */
    bool* queue_ready;             /* 每线程是否有新数据待处理 */
    bool threads_running;          /* 线程池运行标志 */
} StressTestConfig;

/* ==================== 流管理器 ==================== */

/**
 * @brief 全局流管理器
 *
 * 维护 MAX_STREAMS 个流槽位，按 stream_id (1-based) 索引。
 * 流管理器由 main.c 栈上分配，生命周期覆盖整个进程。
 */
typedef struct {
    StreamContext streams[MAX_STREAMS]; /* 流数组 (stream_id - 1 作索引) */
    uint32_t active_count;             /* 活跃流数量 */
    pthread_mutex_t lock;              /* 全局锁 (保护 stress_test 等) */
    StressTestConfig stress_test;      /* 压力测试配置 */
} StreamManager;

/* ==================== 流管理器 API ==================== */

/** 初始化流管理器 (初始化所有流槽位和锁) */
int stream_manager_init(StreamManager* mgr);

/** 销毁流管理器 (关闭所有解码器，销毁所有 mutex，释放资源) */
void stream_manager_destroy(StreamManager* mgr);

/** 根据 stream_id 获取流上下文 (1-based, 返回 NULL 表示越界) */
StreamContext* stream_manager_get(StreamManager* mgr, uint16_t stream_id);

/** 打印所有活跃流的统计信息 */
void stream_manager_print_stats(StreamManager* mgr);

/* ==================== 流状态操作 ==================== */

/** 设置流状态 (线程安全) */
void stream_set_state(StreamContext* stream, StreamState state);

/** 设置流元信息 (线程安全) */
void stream_set_info(StreamContext* stream, const StreamInfo* info);

/** 更新网络接收统计 (线程安全) */
void stream_update_stats(StreamContext* stream, uint32_t bytes, bool is_frame);

/** 获取状态的可读字符串 */
const char* stream_state_str(StreamState state);

/* ==================== 解码操作 ==================== */

/**
 * @brief 初始化流的解码器
 * @param stream  流上下文
 * @param backend 解码后端类型 (会被强制转为 DecodeBackend)
 * @return 0 成功，-1 失败
 */
int stream_init_decoder(StreamContext* stream, int backend);

/** 关闭流的解码器 (释放解码器和最后一帧缓存) */
void stream_close_decoder(StreamContext* stream);

/**
 * @brief 解码视频数据并更新统计
 * @param stream 流上下文
 * @param data   H.264 NAL 数据
 * @param size   数据大小
 * @return 0 成功，1 需要更多数据，-1 错误
 *
 * 内部调用 decoder_decode()，并额外:
 *   - 计时并更新 decode_stats
 *   - 每秒计算一次实时 FPS
 *   - 管理 last_frame 生命周期（释放旧帧、保存新帧）
 */
int stream_decode_video(StreamContext* stream, const uint8_t* data, int size);

/** 获取解码统计快照 (线程安全复制) */
void stream_get_decode_stats(StreamContext* stream, StreamDecodeStats* stats);

/**
 * @brief 获取最后一帧引用
 * @note 返回的帧由 stream 管理，调用方不得释放!
 */
DecodedFrame* stream_get_last_frame(StreamContext* stream);

/**
 * @brief 获取最后一帧的 BGRA 格式副本 (用于 YOLO 推理)
 * @param bgr_frame 调用方提供的帧结构，data[0] 由函数内分配
 * @return 0 成功，-1 失败
 * @note 调用方需调用 stream_free_bgr_frame() 释放 data[0]
 */
int stream_get_last_frame_bgr(StreamContext* stream, DecodedFrame* bgr_frame);

/** 释放 stream_get_last_frame_bgr() 分配的 BGRA 数据 */
void stream_free_bgr_frame(DecodedFrame* frame);

/* ==================== 压力测试 API ==================== */

/**
 * @brief 启动压力测试
 * @param mgr              流管理器
 * @param source_stream_id 源流 ID (真实流)
 * @param num_copies       虚拟流数量
 * @param backend          解码后端
 * @return 0 成功，-1 失败
 *
 * 创建 num_copies 个虚拟流，每个流:
 *   1. 复制源流的 StreamInfo
 *   2. 创建独立的 DecoderCtx
 *   3. 启动独立的解码线程
 */
int stream_manager_start_stress_test(StreamManager* mgr, uint16_t source_stream_id,
                                      int num_copies, DecodeBackend backend);

/** 停止压力测试 (等待所有线程退出，清理资源) */
void stream_manager_stop_stress_test(StreamManager* mgr);

/**
 * @brief 将源流数据分发到所有虚拟流的解码队列
 * @note 非阻塞: 如果某线程还没消费完旧数据，覆盖之（丢旧帧）
 */
int stream_stress_test_decode(StreamManager* mgr, uint16_t source_stream_id,
                               const uint8_t* data, int size);

/** 生成压力测试的汇总报告 (文本格式) */
void stream_manager_get_stress_report(StreamManager* mgr, char* report, size_t report_size);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_H */

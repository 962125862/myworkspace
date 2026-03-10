/**
 * @file protocol.h
 * @brief TCP 视频流传输协议定义
 *
 * 定义了 stream_server 与 Docker 内 ml_worker 之间的二进制协议。
 * 协议为简单的 TLV（Type-Length-Value）格式，大端序编码。
 *
 * 协议包结构:
 *   [4字节 length][1字节 type][2字节 stream_id][payload...]
 *   - length:    uint32_t 大端序，包含头部在内的总长度
 *   - type:      uint8_t  消息类型（VIDEO_DATA/HEARTBEAT/STREAM_START/STREAM_STOP）
 *   - stream_id: uint16_t 大端序，标识哪一路流（1 ~ MAX_STREAMS）
 *   - payload:   变长数据，具体含义取决于 type
 *
 * 消息类型:
 *   - STREAM_START (0x03): payload 为 StreamInfo（16字节: width/height/fps/bitrate）
 *   - VIDEO_DATA  (0x01): payload 为 H.264 NAL 单元原始数据
 *   - HEARTBEAT   (0x02): 保活消息，无 payload
 *   - STREAM_STOP  (0x04): 停止流，无 payload
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 消息类型常量 ==================== */

#define TCP_MSG_TYPE_VIDEO_DATA     0x01    /* H.264 视频数据帧 */
#define TCP_MSG_TYPE_HEARTBEAT      0x02    /* 心跳保活 */
#define TCP_MSG_TYPE_STREAM_START   0x03    /* 流启动（携带 StreamInfo） */
#define TCP_MSG_TYPE_STREAM_STOP    0x04    /* 流停止 */

/* ==================== 协议参数 ==================== */

/** 协议头部固定长度: 4(length) + 1(type) + 2(stream_id) = 7 字节 */
#define TCP_HEADER_SIZE 7

/** 单个数据包最大允许大小 (10MB，防止恶意/异常大包) */
#define TCP_MAX_PACKET_SIZE (10 * 1024 * 1024)

/** 最大并发流数量 (同时支持的视频流路数) */
#define MAX_STREAMS 20

/** 默认 TCP 监听端口 */
#define DEFAULT_LISTEN_PORT 9000

/* ==================== 数据结构 ==================== */

/**
 * @brief 视频流信息（STREAM_START 消息的 payload）
 *
 * 由推流端（Docker ml_worker）在建立流时发送，告知接收端视频参数。
 * 所有字段均为大端序 uint32_t，共 16 字节。
 */
typedef struct {
    uint32_t width;     /* 视频宽度（像素） */
    uint32_t height;    /* 视频高度（像素） */
    uint32_t fps;       /* 帧率 */
    uint32_t bitrate;   /* 码率 (kbps) */
} StreamInfo;

/**
 * @brief 解析后的协议包头（主机字节序）
 *
 * 从网络接收的 7 字节二进制头部解析而来。
 */
typedef struct {
    uint32_t length;    /* 包总长度（含头部），已转为主机序 */
    uint8_t  type;      /* 消息类型 (TCP_MSG_TYPE_*) */
    uint16_t stream_id; /* 流 ID（1 ~ MAX_STREAMS），已转为主机序 */
} PacketHeader;

/* ==================== 函数接口 ==================== */

/**
 * @brief 从网络缓冲区解析协议包头
 * @param buf    输入缓冲区（至少 TCP_HEADER_SIZE 字节）
 * @param header 输出: 解析后的包头（主机字节序）
 * @return 0 成功，-1 失败（格式非法/字段越界）
 */
int protocol_parse_header(const uint8_t* buf, PacketHeader* header);

/**
 * @brief 解析 STREAM_START 消息中的流信息
 * @param buf  输入缓冲区（至少 16 字节）
 * @param info 输出: 流信息（主机字节序）
 * @return 0 成功，-1 失败
 */
int protocol_parse_stream_info(const uint8_t* buf, StreamInfo* info);

/**
 * @brief 构建协议包头（用于发送响应）
 * @param buf         输出缓冲区（至少 TCP_HEADER_SIZE 字节）
 * @param type        消息类型
 * @param stream_id   流 ID
 * @param payload_len 数据负载长度（不含头部）
 */
void protocol_build_header(uint8_t* buf, uint8_t type, uint16_t stream_id, uint32_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */

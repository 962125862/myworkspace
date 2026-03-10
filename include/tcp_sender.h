#ifndef TCP_SENDER_H
#define TCP_SENDER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* TCP消息类型 */
#define TCP_MSG_TYPE_VIDEO_DATA     0x01
#define TCP_MSG_TYPE_HEARTBEAT      0x02
#define TCP_MSG_TYPE_STREAM_START   0x03
#define TCP_MSG_TYPE_STREAM_STOP    0x04

/* TCP头部大小: 4(长度) + 1(类型) + 2(stream_id) = 7字节 */
#define TCP_HEADER_SIZE 7

/* 最大数据包大小 (10MB，足够容纳大帧) */
#define TCP_MAX_PACKET_SIZE (10 * 1024 * 1024)

/* TCP发送器状态 */
typedef enum {
    TCP_STATE_DISCONNECTED = 0,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_ERROR
} TcpSenderState;

/* TCP发送器配置 */
typedef struct {
    char host[256];         /* 服务端IP地址 */
    uint16_t port;          /* 服务端端口 */
    uint16_t stream_id;     /* 流标识符 (1-65535) */
    uint32_t width;         /* 视频宽度 */
    uint32_t height;        /* 视频高度 */
    uint32_t fps;           /* 帧率 */
    uint32_t bitrate;       /* 码率 */
} TcpSenderConfig;

/* TCP发送器上下文 */
typedef struct {
    int sock_fd;                    /* socket文件描述符 */
    TcpSenderState state;           /* 当前状态 */
    TcpSenderConfig config;         /* 配置信息 */
    uint64_t packets_sent;          /* 已发送包数 */
    uint64_t bytes_sent;            /* 已发送字节数 */
    uint64_t last_heartbeat_ns;     /* 上次心跳时间 */
    uint64_t connect_time_ns;       /* 连接建立时间 */
    int last_error;                 /* 上次错误码 */
    uint8_t* send_buffer;           /* 发送缓冲区 */
    size_t send_buffer_size;        /* 缓冲区大小 */
} TcpSender;

/**
 * 初始化TCP发送器
 * @param sender TCP发送器指针
 * @param config 配置参数
 * @return 0成功，-1失败
 */
int tcp_sender_init(TcpSender* sender, const TcpSenderConfig* config);

/**
 * 连接到TCP服务端
 * @param sender TCP发送器指针
 * @return 0成功，-1失败
 */
int tcp_sender_connect(TcpSender* sender);

/**
 * 断开TCP连接
 * @param sender TCP发送器指针
 */
void tcp_sender_disconnect(TcpSender* sender);

/**
 * 发送视频数据包
 * @param sender TCP发送器指针
 * @param data H.264数据指针
 * @param length 数据长度
 * @param frame_type 帧类型 (0=P帧, 1=IDR帧)
 * @return 0成功，-1失败
 */
int tcp_sender_send_video(TcpSender* sender, const uint8_t* data, size_t length, int frame_type);

/**
 * 发送心跳包
 * @param sender TCP发送器指针
 * @return 0成功，-1失败
 */
int tcp_sender_send_heartbeat(TcpSender* sender);

/**
 * 发送流开始消息
 * @param sender TCP发送器指针
 * @return 0成功，-1失败
 */
int tcp_sender_send_stream_start(TcpSender* sender);

/**
 * 发送流停止消息
 * @param sender TCP发送器指针
 * @return 0成功，-1失败
 */
int tcp_sender_send_stream_stop(TcpSender* sender);

/**
 * 检查并发送心跳（如果间隔超过指定时间）
 * @param sender TCP发送器指针
 * @param interval_ns 心跳间隔（纳秒）
 * @return 0成功，-1失败
 */
int tcp_sender_check_heartbeat(TcpSender* sender, uint64_t interval_ns);

/**
 * 销毁TCP发送器，释放资源
 * @param sender TCP发送器指针
 */
void tcp_sender_destroy(TcpSender* sender);

/**
 * 获取发送器状态字符串
 * @param sender TCP发送器指针
 * @return 状态描述字符串
 */
const char* tcp_sender_state_str(const TcpSender* sender);

/**
 * 获取最后错误信息
 * @param sender TCP发送器指针
 * @return 错误描述字符串
 */
const char* tcp_sender_last_error_str(const TcpSender* sender);

#endif /* TCP_SENDER_H */

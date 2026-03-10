/**
 * @file server.h
 * @brief TCP 服务器 (单端口多流接收)
 *
 * 架构设计:
 *   - 一个 accept 线程: 监听端口，接受新连接
 *   - 每个客户端一个 handler 线程: 读取协议包，分发给 StreamManager 处理
 *   - 客户端以链表管理，支持动态增删
 *
 * 线程模型:
 *   main thread    -> server_start() -> accept_loop (thread)
 *   accept_loop    -> 每个新连接 -> client_handler (detached thread)
 *   client_handler -> handle_packet() -> stream_decode_video() / 压力测试分发
 *
 * 连接生命周期:
 *   1. accept() -> client_create() -> 加入链表
 *   2. client_handler 循环: recv header -> recv payload -> handle_packet
 *   3. 断开连接 -> should_remove=1 -> server_stop() 时统一清理
 */

#ifndef SERVER_H
#define SERVER_H

#include "stream.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <netinet/in.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 服务器配置 ==================== */

typedef struct {
    char bind_host[64];          /* 绑定地址 (默认 "0.0.0.0") */
    uint16_t bind_port;          /* 绑定端口 (默认 9000) */
    int max_connections;         /* 最大同时连接数 */
    int recv_buffer_size;        /* 接收缓冲区大小 (预留, 当前未使用) */
} ServerConfig;

/* ==================== 客户端连接 ==================== */

/**
 * @brief 单个客户端连接
 *
 * 一个 Docker ml_worker 对应一个 ClientConn。
 * 通过链表 (next) 串联，由 clients_lock 保护。
 */
typedef struct ClientConn {
    int fd;                         /* TCP socket fd */
    struct sockaddr_in addr;        /* 客户端地址 */
    time_t connect_time;            /* 连接建立时间 */

    /* 接收缓冲区 (预分配，用于将来的缓冲优化) */
    uint8_t* recv_buf;
    size_t recv_buf_size;
    size_t recv_buf_len;

    /* 当前处理的流 (由 STREAM_START 消息设置) */
    uint16_t current_stream_id;

    /* 线程安全标记 */
    volatile int active;            /* 是否正在被 handler 线程处理 */
    volatile int should_remove;     /* 标记待删除 (断开连接后设置) */

    struct ClientConn* next;        /* 链表指针 */
} ClientConn;

/* ==================== TCP 服务器 ==================== */

typedef struct {
    ServerConfig config;             /* 服务器配置 */
    int listen_fd;                   /* 监听 socket fd */

    /* 客户端连接管理 (链表) */
    ClientConn* clients;             /* 客户端链表头 */
    pthread_mutex_t clients_lock;    /* 保护链表和 client_count */
    int client_count;                /* 当前连接数 */

    /* 流管理器 (外部传入，由 main.c 管理生命周期) */
    StreamManager* stream_mgr;

    /* 运行状态 */
    volatile bool running;           /* 服务器运行标志 */
    pthread_t accept_thread;         /* accept 循环线程 */

    /* 全局统计 */
    uint64_t total_connections;      /* 累计连接数 */
    uint64_t total_packets;          /* 累计处理包数 */
    uint64_t total_bytes;            /* 累计接收字节数 */
} TcpServer;

/* ==================== 服务器 API ==================== */

/**
 * @brief 初始化服务器 (不启动)
 * @param server     服务器实例
 * @param config     配置参数
 * @param stream_mgr 流管理器 (外部创建并初始化)
 */
int server_init(TcpServer* server, const ServerConfig* config, StreamManager* stream_mgr);

/** 启动服务器 (创建 socket、bind、listen、启动 accept 线程) */
int server_start(TcpServer* server);

/** 停止服务器 (关闭 socket、等待线程退出、清理连接) */
void server_stop(TcpServer* server);

/** 检查服务器是否仍在运行 */
bool server_is_running(TcpServer* server);

/** 获取服务器统计信息 */
void server_get_stats(TcpServer* server, uint64_t* connections, uint64_t* packets, uint64_t* bytes);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */

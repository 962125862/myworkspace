#include "tcp_sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* 错误码定义 */
#define TCP_ERR_NONE            0
#define TCP_ERR_INVALID_ARG     1
#define TCP_ERR_NO_MEMORY       2
#define TCP_ERR_SOCKET_CREATE   3
#define TCP_ERR_CONNECT         4
#define TCP_ERR_SEND            5
#define TCP_ERR_NOT_CONNECTED   6
#define TCP_ERR_PACKET_TOO_LARGE 7

/* 默认发送缓冲区大小 */
#define DEFAULT_SEND_BUFFER_SIZE (2 * 1024 * 1024)

/* 心跳间隔：5秒 */
#define HEARTBEAT_INTERVAL_NS (5ULL * 1000000000ULL)

/* 连接超时：10秒 */
#define CONNECT_TIMEOUT_SEC 10

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void set_nonblocking(int fd, int nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    fcntl(fd, F_SETFL, flags);
}

static int ensure_buffer_size(TcpSender* sender, size_t needed) {
    if (sender->send_buffer_size >= needed) {
        return 0;
    }

    size_t new_size = sender->send_buffer_size;
    if (new_size == 0) {
        new_size = DEFAULT_SEND_BUFFER_SIZE;
    }
    while (new_size < needed) {
        new_size *= 2;
    }

    uint8_t* new_buf = realloc(sender->send_buffer, new_size);
    if (!new_buf) {
        fprintf(stderr, "tcp_sender: realloc failed for %zu bytes\n", new_size);
        sender->last_error = TCP_ERR_NO_MEMORY;
        return -1;
    }

    sender->send_buffer = new_buf;
    sender->send_buffer_size = new_size;
    return 0;
}

static int send_all(int sock_fd, const uint8_t* data, size_t length) {
    size_t sent = 0;
    int retry_count = 0;
    const int max_retries = 100; /* 最多重试100次 */
    
    while (sent < length && retry_count < max_retries) {
        ssize_t n = send(sock_fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞模式下短暂等待 */
                usleep(100); /* 100微秒 */
                retry_count++;
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1; /* 连接关闭 */
        }
        sent += (size_t)n;
        retry_count = 0; /* 成功发送后重置重试计数 */
    }
    
    if (sent < length) {
        /* 发送超时 */
        return -1;
    }
    return 0;
}

int tcp_sender_init(TcpSender* sender, const TcpSenderConfig* config) {
    if (!sender || !config) {
        fprintf(stderr, "tcp_sender_init: invalid arguments\n");
        return -1;
    }

    if (config->port == 0 || config->host[0] == '\0') {
        fprintf(stderr, "tcp_sender_init: invalid host or port\n");
        return -1;
    }

    if (config->stream_id == 0) {
        fprintf(stderr, "tcp_sender_init: stream_id must be non-zero\n");
        return -1;
    }

    memset(sender, 0, sizeof(*sender));
    sender->sock_fd = -1;
    sender->state = TCP_STATE_DISCONNECTED;
    sender->last_error = TCP_ERR_NONE;

    memcpy(&sender->config, config, sizeof(*config));

    /* 预分配发送缓冲区 */
    sender->send_buffer = malloc(DEFAULT_SEND_BUFFER_SIZE);
    if (!sender->send_buffer) {
        fprintf(stderr, "tcp_sender_init: malloc failed\n");
        sender->last_error = TCP_ERR_NO_MEMORY;
        return -1;
    }
    sender->send_buffer_size = DEFAULT_SEND_BUFFER_SIZE;

    fprintf(stderr, "tcp_sender: initialized for %s:%d, stream_id=%u\n",
            config->host, config->port, config->stream_id);

    return 0;
}

int tcp_sender_connect(TcpSender* sender) {
    if (!sender) {
        return -1;
    }

    if (sender->state == TCP_STATE_CONNECTED) {
        return 0; /* 已连接 */
    }

    if (sender->sock_fd >= 0) {
        close(sender->sock_fd);
        sender->sock_fd = -1;
    }

    sender->state = TCP_STATE_CONNECTING;
    sender->last_error = TCP_ERR_NONE;

    /* 创建socket */
    sender->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sender->sock_fd < 0) {
        fprintf(stderr, "tcp_sender: socket creation failed: %s\n", strerror(errno));
        sender->last_error = TCP_ERR_SOCKET_CREATE;
        sender->state = TCP_STATE_ERROR;
        return -1;
    }

    /* 设置TCP_NODELAY减少延迟 */
    int nodelay = 1;
    setsockopt(sender->sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* 设置发送缓冲区大小 */
    int sndbuf = 2 * 1024 * 1024; /* 2MB */
    setsockopt(sender->sock_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* 设置非阻塞模式进行连接 */
    set_nonblocking(sender->sock_fd, 1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(sender->config.port);

    if (inet_pton(AF_INET, sender->config.host, &addr.sin_addr) <= 0) {
        /* 尝试DNS解析 */
        struct hostent* he = gethostbyname(sender->config.host);
        if (!he || !he->h_addr_list[0]) {
            fprintf(stderr, "tcp_sender: failed to resolve host: %s\n", sender->config.host);
            close(sender->sock_fd);
            sender->sock_fd = -1;
            sender->last_error = TCP_ERR_CONNECT;
            sender->state = TCP_STATE_ERROR;
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    int rc = connect(sender->sock_fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        fprintf(stderr, "tcp_sender: connect failed: %s\n", strerror(errno));
        close(sender->sock_fd);
        sender->sock_fd = -1;
        sender->last_error = TCP_ERR_CONNECT;
        sender->state = TCP_STATE_ERROR;
        return -1;
    }

    /* 等待连接完成 */
    if (rc < 0) {
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sender->sock_fd, &fdset);

        struct timeval tv;
        tv.tv_sec = CONNECT_TIMEOUT_SEC;
        tv.tv_usec = 0;

        rc = select(sender->sock_fd + 1, NULL, &fdset, NULL, &tv);
        if (rc <= 0) {
            fprintf(stderr, "tcp_sender: connect timeout or error\n");
            close(sender->sock_fd);
            sender->sock_fd = -1;
            sender->last_error = TCP_ERR_CONNECT;
            sender->state = TCP_STATE_ERROR;
            return -1;
        }

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(sender->sock_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            fprintf(stderr, "tcp_sender: connect failed: %s\n", strerror(so_error));
            close(sender->sock_fd);
            sender->sock_fd = -1;
            sender->last_error = TCP_ERR_CONNECT;
            sender->state = TCP_STATE_ERROR;
            return -1;
        }
    }

    /* 保持非阻塞模式，避免发送阻塞影响实时性 */
    /* set_nonblocking(sender->sock_fd, 0); */

    sender->state = TCP_STATE_CONNECTED;
    sender->connect_time_ns = now_monotonic_ns();
    sender->last_heartbeat_ns = sender->connect_time_ns;
    sender->packets_sent = 0;
    sender->bytes_sent = 0;

    fprintf(stderr, "tcp_sender: connected to %s:%d\n", sender->config.host, sender->config.port);

    /* 发送流开始消息 */
    tcp_sender_send_stream_start(sender);

    return 0;
}

void tcp_sender_disconnect(TcpSender* sender) {
    if (!sender) {
        return;
    }

    if (sender->state == TCP_STATE_CONNECTED) {
        tcp_sender_send_stream_stop(sender);
    }

    if (sender->sock_fd >= 0) {
        close(sender->sock_fd);
        sender->sock_fd = -1;
    }

    sender->state = TCP_STATE_DISCONNECTED;
    fprintf(stderr, "tcp_sender: disconnected\n");
}

int tcp_sender_send_video(TcpSender* sender, const uint8_t* data, size_t length, int frame_type) {
    /* frame_type 当前未进入协议编码，预留给未来扩展（例如在视频包头中携带 keyframe 标记）。 */
    (void)frame_type;

    if (!sender || !data || length == 0) {
        return -1;
    }

    if (sender->state != TCP_STATE_CONNECTED) {
        sender->last_error = TCP_ERR_NOT_CONNECTED;
        return -1;
    }

    if (length > TCP_MAX_PACKET_SIZE) {
        fprintf(stderr, "tcp_sender: packet too large: %zu bytes\n", length);
        sender->last_error = TCP_ERR_PACKET_TOO_LARGE;
        return -1;
    }

    /* 确保缓冲区足够: 头部 + 数据 */
    size_t total_size = TCP_HEADER_SIZE + length;
    if (ensure_buffer_size(sender, total_size) < 0) {
        return -1;
    }

    /* 构建数据包 */
    uint8_t* buf = sender->send_buffer;

    /* 4字节: 数据包总长度（大端序） */
    buf[0] = (uint8_t)((total_size >> 24) & 0xFF);
    buf[1] = (uint8_t)((total_size >> 16) & 0xFF);
    buf[2] = (uint8_t)((total_size >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_size & 0xFF);

    /* 1字节: 消息类型 */
    buf[4] = TCP_MSG_TYPE_VIDEO_DATA;

    /* 2字节: stream_id（大端序） */
    buf[5] = (uint8_t)((sender->config.stream_id >> 8) & 0xFF);
    buf[6] = (uint8_t)(sender->config.stream_id & 0xFF);

    /* 复制视频数据 */
    memcpy(buf + TCP_HEADER_SIZE, data, length);

    /* 发送数据 */
    if (send_all(sender->sock_fd, buf, total_size) < 0) {
        fprintf(stderr, "tcp_sender: send failed: %s\n", strerror(errno));
        sender->last_error = TCP_ERR_SEND;
        sender->state = TCP_STATE_ERROR;
        return -1;
    }

    sender->packets_sent++;
    sender->bytes_sent += total_size;

    return 0;
}

int tcp_sender_send_heartbeat(TcpSender* sender) {
    if (!sender) {
        return -1;
    }

    if (sender->state != TCP_STATE_CONNECTED) {
        return -1;
    }

    uint8_t buf[TCP_HEADER_SIZE];
    uint32_t total_size = TCP_HEADER_SIZE;

    /* 4字节: 数据包总长度 */
    buf[0] = (uint8_t)((total_size >> 24) & 0xFF);
    buf[1] = (uint8_t)((total_size >> 16) & 0xFF);
    buf[2] = (uint8_t)((total_size >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_size & 0xFF);

    /* 1字节: 消息类型 */
    buf[4] = TCP_MSG_TYPE_HEARTBEAT;

    /* 2字节: stream_id */
    buf[5] = (uint8_t)((sender->config.stream_id >> 8) & 0xFF);
    buf[6] = (uint8_t)(sender->config.stream_id & 0xFF);

    if (send_all(sender->sock_fd, buf, total_size) < 0) {
        fprintf(stderr, "tcp_sender: heartbeat send failed: %s\n", strerror(errno));
        sender->last_error = TCP_ERR_SEND;
        sender->state = TCP_STATE_ERROR;
        return -1;
    }

    sender->last_heartbeat_ns = now_monotonic_ns();
    return 0;
}

int tcp_sender_send_stream_start(TcpSender* sender) {
    if (!sender) {
        return -1;
    }

    if (sender->state != TCP_STATE_CONNECTED) {
        return -1;
    }

    /* 构建流信息: 基础16字节 + codec/chroma/bitdepth/video_format/color = 40字节 */
    size_t payload_size = 40;
    size_t total_size = TCP_HEADER_SIZE + payload_size;

    if (ensure_buffer_size(sender, total_size) < 0) {
        return -1;
    }

    uint8_t* buf = sender->send_buffer;

    /* 头部 */
    buf[0] = (uint8_t)((total_size >> 24) & 0xFF);
    buf[1] = (uint8_t)((total_size >> 16) & 0xFF);
    buf[2] = (uint8_t)((total_size >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_size & 0xFF);
    buf[4] = TCP_MSG_TYPE_STREAM_START;
    buf[5] = (uint8_t)((sender->config.stream_id >> 8) & 0xFF);
    buf[6] = (uint8_t)(sender->config.stream_id & 0xFF);

    /* 流信息（大端序） */
    uint8_t* p = buf + TCP_HEADER_SIZE;
    p[0] = (uint8_t)((sender->config.width >> 24) & 0xFF);
    p[1] = (uint8_t)((sender->config.width >> 16) & 0xFF);
    p[2] = (uint8_t)((sender->config.width >> 8) & 0xFF);
    p[3] = (uint8_t)(sender->config.width & 0xFF);

    p[4] = (uint8_t)((sender->config.height >> 24) & 0xFF);
    p[5] = (uint8_t)((sender->config.height >> 16) & 0xFF);
    p[6] = (uint8_t)((sender->config.height >> 8) & 0xFF);
    p[7] = (uint8_t)(sender->config.height & 0xFF);

    p[8] = (uint8_t)((sender->config.fps >> 24) & 0xFF);
    p[9] = (uint8_t)((sender->config.fps >> 16) & 0xFF);
    p[10] = (uint8_t)((sender->config.fps >> 8) & 0xFF);
    p[11] = (uint8_t)(sender->config.fps & 0xFF);

    p[12] = (uint8_t)((sender->config.bitrate >> 24) & 0xFF);
    p[13] = (uint8_t)((sender->config.bitrate >> 16) & 0xFF);
    p[14] = (uint8_t)((sender->config.bitrate >> 8) & 0xFF);
    p[15] = (uint8_t)(sender->config.bitrate & 0xFF);
    p[16] = (uint8_t)((sender->config.codec >> 24) & 0xFF);
    p[17] = (uint8_t)((sender->config.codec >> 16) & 0xFF);
    p[18] = (uint8_t)((sender->config.codec >> 8) & 0xFF);
    p[19] = (uint8_t)(sender->config.codec & 0xFF);
    p[20] = (uint8_t)((sender->config.chroma >> 24) & 0xFF);
    p[21] = (uint8_t)((sender->config.chroma >> 16) & 0xFF);
    p[22] = (uint8_t)((sender->config.chroma >> 8) & 0xFF);
    p[23] = (uint8_t)(sender->config.chroma & 0xFF);
    p[24] = (uint8_t)((sender->config.bitdepth >> 24) & 0xFF);
    p[25] = (uint8_t)((sender->config.bitdepth >> 16) & 0xFF);
    p[26] = (uint8_t)((sender->config.bitdepth >> 8) & 0xFF);
    p[27] = (uint8_t)(sender->config.bitdepth & 0xFF);
    p[28] = (uint8_t)((sender->config.video_format >> 24) & 0xFF);
    p[29] = (uint8_t)((sender->config.video_format >> 16) & 0xFF);
    p[30] = (uint8_t)((sender->config.video_format >> 8) & 0xFF);
    p[31] = (uint8_t)(sender->config.video_format & 0xFF);
    p[32] = (uint8_t)((sender->config.color_space >> 24) & 0xFF);
    p[33] = (uint8_t)((sender->config.color_space >> 16) & 0xFF);
    p[34] = (uint8_t)((sender->config.color_space >> 8) & 0xFF);
    p[35] = (uint8_t)(sender->config.color_space & 0xFF);
    p[36] = (uint8_t)((sender->config.color_range >> 24) & 0xFF);
    p[37] = (uint8_t)((sender->config.color_range >> 16) & 0xFF);
    p[38] = (uint8_t)((sender->config.color_range >> 8) & 0xFF);
    p[39] = (uint8_t)(sender->config.color_range & 0xFF);

    if (send_all(sender->sock_fd, buf, total_size) < 0) {
        fprintf(stderr, "tcp_sender: stream_start send failed: %s\n", strerror(errno));
        sender->last_error = TCP_ERR_SEND;
        sender->state = TCP_STATE_ERROR;
        return -1;
    }

    fprintf(stderr, "tcp_sender: stream_start sent (stream_id=%u, %ux%u@%u, %u kbps, codec=%u chroma=%u bitdepth=%u fmt=0x%x cs=%u cr=%u)\n",
            sender->config.stream_id, sender->config.width, sender->config.height,
            sender->config.fps, sender->config.bitrate,
            sender->config.codec, sender->config.chroma,
            sender->config.bitdepth, sender->config.video_format,
            sender->config.color_space, sender->config.color_range);

    return 0;
}

int tcp_sender_send_stream_stop(TcpSender* sender) {
    if (!sender) {
        return -1;
    }

    if (sender->state != TCP_STATE_CONNECTED) {
        return -1;
    }

    uint8_t buf[TCP_HEADER_SIZE];
    uint32_t total_size = TCP_HEADER_SIZE;

    buf[0] = (uint8_t)((total_size >> 24) & 0xFF);
    buf[1] = (uint8_t)((total_size >> 16) & 0xFF);
    buf[2] = (uint8_t)((total_size >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_size & 0xFF);
    buf[4] = TCP_MSG_TYPE_STREAM_STOP;
    buf[5] = (uint8_t)((sender->config.stream_id >> 8) & 0xFF);
    buf[6] = (uint8_t)(sender->config.stream_id & 0xFF);

    if (send_all(sender->sock_fd, buf, total_size) < 0) {
        fprintf(stderr, "tcp_sender: stream_stop send failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "tcp_sender: stream_stop sent (stream_id=%u)\n", sender->config.stream_id);
    return 0;
}

int tcp_sender_check_heartbeat(TcpSender* sender, uint64_t interval_ns) {
    if (!sender) {
        return -1;
    }

    if (sender->state != TCP_STATE_CONNECTED) {
        return 0;
    }

    uint64_t now = now_monotonic_ns();
    if (now - sender->last_heartbeat_ns >= interval_ns) {
        return tcp_sender_send_heartbeat(sender);
    }

    return 0;
}

void tcp_sender_destroy(TcpSender* sender) {
    if (!sender) {
        return;
    }

    tcp_sender_disconnect(sender);

    free(sender->send_buffer);
    sender->send_buffer = NULL;
    sender->send_buffer_size = 0;

    memset(sender, 0, sizeof(*sender));
    sender->sock_fd = -1;
    sender->state = TCP_STATE_DISCONNECTED;
}

const char* tcp_sender_state_str(const TcpSender* sender) {
    if (!sender) {
        return "invalid";
    }

    switch (sender->state) {
        case TCP_STATE_DISCONNECTED: return "disconnected";
        case TCP_STATE_CONNECTING:   return "connecting";
        case TCP_STATE_CONNECTED:    return "connected";
        case TCP_STATE_ERROR:        return "error";
        default:                     return "unknown";
    }
}

const char* tcp_sender_last_error_str(const TcpSender* sender) {
    if (!sender) {
        return "invalid sender";
    }

    switch (sender->last_error) {
        case TCP_ERR_NONE:            return "no error";
        case TCP_ERR_INVALID_ARG:     return "invalid argument";
        case TCP_ERR_NO_MEMORY:       return "out of memory";
        case TCP_ERR_SOCKET_CREATE:   return "socket creation failed";
        case TCP_ERR_CONNECT:         return "connection failed";
        case TCP_ERR_SEND:            return "send failed";
        case TCP_ERR_NOT_CONNECTED:   return "not connected";
        case TCP_ERR_PACKET_TOO_LARGE: return "packet too large";
        default:                      return "unknown error";
    }
}

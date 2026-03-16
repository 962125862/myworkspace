/**
 * @file server.c
 * @brief TCP服务器实现（单端口多流）
 */

#define _GNU_SOURCE
#include "server.h"
#include "protocol.h"
#include "decoder.h"
#include "stream.h"
#include "h264_tap.h"
#include "mlctl_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>

#define DEFAULT_RECV_BUF_SIZE (1024 * 1024)  /* 1MB接收缓冲区 */
#define RECV_TIMEOUT_SEC 5

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_recv_timeout(int fd, int sec) {
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static ClientConn* client_create(int fd, struct sockaddr_in* addr) {
    ClientConn* client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    
    client->fd = fd;
    memcpy(&client->addr, addr, sizeof(*addr));
    client->connect_time = time(NULL);
    client->current_stream_id = 0;
    client->active = 0;
    client->should_remove = 0;
    
    client->recv_buf_size = DEFAULT_RECV_BUF_SIZE;
    client->recv_buf = malloc(client->recv_buf_size);
    if (!client->recv_buf) {
        free(client);
        return NULL;
    }
    client->recv_buf_len = 0;
    
    return client;
}

static void client_destroy(ClientConn* client) {
    if (!client) return;
    
    if (client->fd >= 0) {
        close(client->fd);
    }
    
    free(client->recv_buf);
    free(client);
}

/*
 * 精确读取 n 字节。
 * - client_fd 是阻塞 socket，并设置了 SO_RCVTIMEO
 * - 超时时 recv() 通常返回 -1 且 errno=EAGAIN/EWOULDBLOCK
 * - 这里不做 sleep 忙等，避免不必要的 1ms 粒度延迟和 CPU 浪费
 */
static int recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t received = 0;
    while (received < n) {
        ssize_t r = recv(fd, buf + received, n - received, 0);
        if (r > 0) {
            received += (size_t)r;
            continue;
        }
        if (r == 0) {
            return -1; /* peer closed */
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1; /* timeout */
        }
        return -1;
    }
    return 0;
}



/* 压力测试模式开关（通过环境变量控制） */
static int g_stress_test_enabled = 0;
static int g_stress_test_copies = DEFAULT_MAX_STREAMS;

typedef struct {
    int valid;
    char ip[64];
    uint16_t port;
} WorkerCtrlEndpoint;

static WorkerCtrlEndpoint g_worker_ctrl_map[MAX_STREAMS + 1];
static pthread_once_t g_worker_ctrl_map_once = PTHREAD_ONCE_INIT;

static int server_runtime_max_streams(void) {
    const char* value = getenv("STREAM_MAX_STREAMS");
    if (!value || !*value) {
        return DEFAULT_MAX_STREAMS;
    }

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return DEFAULT_MAX_STREAMS;
    }
    if (parsed < 1) {
        return 1;
    }
    if (parsed > MAX_STREAMS) {
        return MAX_STREAMS;
    }
    return (int)parsed;
}

static uint64_t monotonic_ns_server(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_req_idr_to_endpoint(const char* ip, uint16_t port) {
    if (!ip || !*ip || port == 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    MlControlCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = ML_CTRL_MAGIC;
    cmd.version = (uint16_t)ML_CTRL_VERSION;
    cmd.type = (uint16_t)ML_CTRL_CMD_REQ_IDR;
    cmd.seq = monotonic_ns_server();

    int rc = (int)sendto(fd, &cmd, sizeof(cmd), 0, (struct sockaddr*)&a, sizeof(a));
    close(fd);
    return (rc == (int)sizeof(cmd)) ? 0 : -1;
}

static void trim_ascii(char* s) {
    if (!s) return;
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static void set_worker_ctrl_endpoint(unsigned stream_id, const char* ip, int port) {
    const int runtime_max_streams = server_runtime_max_streams();
    if (stream_id == 0 || stream_id > (unsigned)runtime_max_streams ||
        !ip || !*ip || port <= 0 || port > 65535) {
        return;
    }
    g_worker_ctrl_map[stream_id].valid = 1;
    snprintf(g_worker_ctrl_map[stream_id].ip, sizeof(g_worker_ctrl_map[stream_id].ip), "%s", ip);
    g_worker_ctrl_map[stream_id].port = (uint16_t)port;
}

static void parse_worker_ctrl_map_string(const char* map_str) {
    if (!map_str || !*map_str) {
        return;
    }

    char* copy = strdup(map_str);
    if (!copy) {
        return;
    }

    char* saveptr = NULL;
    for (char* item = strtok_r(copy, ",", &saveptr);
         item != NULL;
         item = strtok_r(NULL, ",", &saveptr)) {
        trim_ascii(item);
        if (!*item) continue;

        unsigned stream_id = 0;
        char ip[64] = {0};
        int port = 0;
        if (sscanf(item, "%u:%63[^:]:%d", &stream_id, ip, &port) == 3) {
            set_worker_ctrl_endpoint(stream_id, ip, port);
        }
    }

    free(copy);
}

static void parse_worker_ctrl_map_file(const char* path) {
    if (!path || !*path) {
        return;
    }

    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[Server] Failed to open ctrl map file: %s\n", path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        trim_ascii(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }

        unsigned stream_id = 0;
        char ip[64] = {0};
        int port = 0;
        if (sscanf(line, "%u %63s %d", &stream_id, ip, &port) == 3 ||
            sscanf(line, "%u:%63[^:]:%d", &stream_id, ip, &port) == 3) {
            set_worker_ctrl_endpoint(stream_id, ip, port);
        }
    }

    fclose(fp);
}

static void init_worker_ctrl_map_once(void) {
    memset(g_worker_ctrl_map, 0, sizeof(g_worker_ctrl_map));
    parse_worker_ctrl_map_string(getenv("ML_WORKER_CTRL_MAP"));
    parse_worker_ctrl_map_file(getenv("ML_WORKER_CTRL_MAP_FILE"));
}

static int request_idr_best_effort_main_path(uint16_t stream_id) {
    pthread_once(&g_worker_ctrl_map_once, init_worker_ctrl_map_once);

    const int runtime_max_streams = server_runtime_max_streams();
    if (stream_id > 0 && stream_id <= runtime_max_streams && g_worker_ctrl_map[stream_id].valid) {
        return send_req_idr_to_endpoint(g_worker_ctrl_map[stream_id].ip,
                                        g_worker_ctrl_map[stream_id].port);
    }
    return -1;
}

static void request_idr_for_stream(StreamContext* stream, const char* reason) {
    if (!stream) {
        return;
    }

    const uint64_t now_ns = monotonic_ns_server();
    const uint64_t request_interval_ns = 1000ull * 1000ull * 1000ull;
    int should_request = 0;

    pthread_mutex_lock(&stream->lock);
    if (stream->last_idr_request_ns == 0 ||
        now_ns - stream->last_idr_request_ns >= request_interval_ns) {
        stream->last_idr_request_ns = now_ns;
        should_request = 1;
    }
    pthread_mutex_unlock(&stream->lock);

    if (should_request && request_idr_best_effort_main_path(stream->stream_id) == 0) {
        fprintf(stderr, "[Server] Stream %u requested upstream IDR (%s)\n",
                stream->stream_id, reason ? reason : "unspecified");
    }
}

static void maybe_request_idr_for_no_decode(StreamContext* stream) {
    if (!stream) {
        return;
    }

    const uint64_t now_ns = monotonic_ns_server();
    const uint64_t request_interval_ns = 1000ull * 1000ull * 1000ull;
    int should_request = 0;
    uint64_t frames_received = 0;
    uint64_t frames_decoded = 0;

    pthread_mutex_lock(&stream->lock);
    frames_received = stream->frames_received;
    frames_decoded = stream->decode_stats.frames_decoded;
    if (stream->state == STREAM_STATE_ACTIVE &&
        stream->decoder_initialized &&
        frames_decoded == 0 &&
        frames_received >= 30 &&
        (stream->last_idr_request_ns == 0 ||
         now_ns - stream->last_idr_request_ns >= request_interval_ns)) {
        should_request = 1;
    }
    pthread_mutex_unlock(&stream->lock);

    if (should_request) {
        char reason[96];
        snprintf(reason, sizeof(reason), "decoded=0 after %llu frames",
                 (unsigned long long)frames_received);
        request_idr_for_stream(stream, reason);
    }
}

/* H264 tap */
static int g_h264_tap_started = 0;
static void maybe_start_h264_tap(void) {
    if (g_h264_tap_started) return;
    const char* port_env = getenv("H264_TAP_PORT");
    if (!port_env) return;
    int port = atoi(port_env);
    if (port <= 0 || port > 65535) return;
    const char* bind_env = getenv("H264_TAP_BIND");

    if (h264_tap_start(bind_env ? bind_env : "127.0.0.1", (uint16_t)port) == 0) {
        g_h264_tap_started = 1;
    }
}

/* 获取解码后端（支持环境变量强制指定） */
static DecodeBackend get_decode_backend(void) {
    const char* backend_env = getenv("DECODE_BACKEND");
    if (backend_env) {
        if (strcasecmp(backend_env, "nvidia") == 0 || 
            strcasecmp(backend_env, "cuvid") == 0) {
            printf("[Server] Forcing NVIDIA NVDEC backend (from env)\n");
            return DECODE_BACKEND_NVIDIA;
        }
        if (strcasecmp(backend_env, "intel") == 0 || 
            strcasecmp(backend_env, "intel_va") == 0 ||
            strcasecmp(backend_env, "vaapi") == 0 ||
            strcasecmp(backend_env, "qsv") == 0) {
            printf("[Server] Forcing Intel VA-API backend (from env)\n");
            return DECODE_BACKEND_INTEL_VA;
        }
        if (strcasecmp(backend_env, "cpu") == 0) {
            printf("[Server] Forcing CPU backend (from env)\n");
            return DECODE_BACKEND_CPU;
        }
    }
    return decoder_detect_backend();
}

static void handle_packet(TcpServer* server, ClientConn* client, 
                         const PacketHeader* header, const uint8_t* payload) {
    StreamManager* mgr = server->stream_mgr;
    StreamContext* stream = stream_manager_get(mgr, header->stream_id);
    
    if (!stream) {
        fprintf(stderr, "[Server] Unknown stream_id: %d\n", header->stream_id);
        return;
    }
    
    switch (header->type) {
        case TCP_MSG_TYPE_STREAM_START:
            if (header->length >= TCP_HEADER_SIZE + 16) {
                StreamInfo info;
                uint32_t payload_len = header->length - TCP_HEADER_SIZE;
                if (protocol_parse_stream_info(payload, payload_len, &info) == 0) {
                    stream_set_info(stream, &info);
                    stream_set_state(stream, STREAM_STATE_ACTIVE);
                    client->current_stream_id = header->stream_id;

                    pthread_mutex_lock(&stream->lock);
                    stream->last_idr_request_ns = 0;
                    pthread_mutex_unlock(&stream->lock);
                    
                    /* 自动检测或强制指定解码器 */
                    DecodeBackend backend = get_decode_backend();
                    if (stream_init_decoder(stream, backend) < 0) {
                        fprintf(stderr, "[Server] Failed to init decoder for stream %d\n",
                                header->stream_id);
                    }
                    
                    printf("[Server] Stream %d started: %dx%d@%dfps codec=%u chroma=%u bitdepth=%u fmt=0x%x cs=%u cr=%u\n",
                           header->stream_id, info.width, info.height, info.fps,
                           info.codec, info.chroma, info.bitdepth, info.video_format,
                           info.color_space, info.color_range);

                    /*
                     * Late-join recovery:
                     * if stream_server starts after ml_worker has already entered steady-state
                     * HEVC/H264 slices may arrive before a fresh IDR/VPS/SPS/PPS. Request one
                     * immediately so the decoder can lock without requiring a worker restart.
                     */
                    request_idr_for_stream(stream, "stream_start");
                    
                    /* 检查是否启用压力测试模式 */
                    if (g_stress_test_enabled == 0) {
                        const char* stress_env = getenv("STRESS_TEST");
                        if (stress_env && atoi(stress_env) > 0) {
                            g_stress_test_enabled = 1;
                            const char* copies_env = getenv("STRESS_COPIES");
                            if (copies_env) {
                                g_stress_test_copies = atoi(copies_env);
                                if (g_stress_test_copies < 1) {
                                    g_stress_test_copies = DEFAULT_MAX_STREAMS;
                                }
                                if (g_stress_test_copies > server_runtime_max_streams()) {
                                    g_stress_test_copies = server_runtime_max_streams();
                                }
                            }
                            
                            printf("[Server] Enabling stress test mode: %d copies\n", 
                                   g_stress_test_copies);
                            
                            /* 启动压力测试 */
                            if (stream_manager_start_stress_test(mgr, header->stream_id,
                                                                  g_stress_test_copies, backend) < 0) {
                                fprintf(stderr, "[Server] Failed to start stress test\n");
                                g_stress_test_enabled = 0;
                            }
                        }
                    }
                }
            }
            break;
            
        case TCP_MSG_TYPE_VIDEO_DATA:
            stream_update_stats(stream, header->length, true);
            /* 解码视频数据 */
            if (payload && header->length > TCP_HEADER_SIZE) {
                uint32_t payload_len = header->length - TCP_HEADER_SIZE;

                /* 可选：旁路输出 H264（不转码） */
                if (!g_h264_tap_started) {
                    maybe_start_h264_tap();
                }
                if (g_h264_tap_started) {
                    h264_tap_publish(header->stream_id, payload, (int)payload_len);
                }
                
                /* 先解码源流 */
                int ret = stream_decode_video(stream, payload, payload_len);
                if (ret < 0) {
                    static time_t last_error = 0;
                    time_t now = time(NULL);
                    if (now - last_error >= 5) {
                        fprintf(stderr, "[Server] Stream %d decode error\n", 
                                header->stream_id);
                        last_error = now;
                    }
                }
                maybe_request_idr_for_no_decode(stream);
                
                /* 如果启用了压力测试，复制到虚拟流 */
                if (g_stress_test_enabled) {
                    stream_stress_test_decode(mgr, header->stream_id, payload, payload_len);
                }
            }
            break;
            
        case TCP_MSG_TYPE_STREAM_STOP:
            /* 关闭解码器 */
            stream_close_decoder(stream);
            stream_set_state(stream, STREAM_STATE_IDLE);
            printf("[Server] Stream %d stopped\n", header->stream_id);
            break;
            
        case TCP_MSG_TYPE_HEARTBEAT:
            stream_update_stats(stream, header->length, false);
            break;
    }
    
    /* 更新服务器统计 */
    server->total_packets++;
    server->total_bytes += header->length;
}

/* 客户端处理线程参数 */
typedef struct {
    TcpServer* server;
    ClientConn* client;
} ClientHandlerArg;

static void* client_handler(void* arg) {
    ClientHandlerArg* handler_arg = (ClientHandlerArg*)arg;
    TcpServer* server = handler_arg->server;
    ClientConn* client = handler_arg->client;
    
    free(handler_arg);
    
    if (!client || client->fd < 0) {
        return NULL;
    }
    
    /* 标记为活跃 */
    client->active = 1;
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->addr.sin_addr, client_ip, sizeof(client_ip));
    printf("[Server] Handling client %s:%d\n", client_ip, ntohs(client->addr.sin_port));
    
    while (server->running && client->fd >= 0) {
        /* 读取包头 */
        uint8_t header_buf[TCP_HEADER_SIZE];
        if (recv_exact(client->fd, header_buf, TCP_HEADER_SIZE) < 0) {
            break;
        }
        
        PacketHeader header;
        if (protocol_parse_header(header_buf, &header) < 0) {
            fprintf(stderr, "[Server] Invalid packet header from %s\n", client_ip);
            break;
        }
        
        /* 读取数据负载 */
        uint32_t payload_len = header.length - TCP_HEADER_SIZE;
        uint8_t* payload = NULL;
        
        if (payload_len > 0) {
            if (payload_len > TCP_MAX_PACKET_SIZE) {
                fprintf(stderr, "[Server] Packet too large: %u\n", payload_len);
                break;
            }

            /* 复用 client->recv_buf，避免每包 malloc/free */
            if (payload_len > client->recv_buf_size) {
                size_t new_size = client->recv_buf_size;
                while (new_size < payload_len) {
                    new_size *= 2;
                    if (new_size < client->recv_buf_size) { /* overflow */
                        new_size = payload_len;
                        break;
                    }
                }
                uint8_t* new_buf = realloc(client->recv_buf, new_size);
                if (!new_buf) {
                    break;
                }
                client->recv_buf = new_buf;
                client->recv_buf_size = new_size;
            }

            payload = client->recv_buf;
            if (recv_exact(client->fd, payload, payload_len) < 0) {
                break;
            }
        }
        
        /* 处理包 */
        handle_packet(server, client, &header, payload);
    }
    
    printf("[Server] Client %s disconnected\n", client_ip);

    /* 从链表中移除并释放 client 资源
     * 注意：必须先关闭 fd（让 recv 返回错误），再从链表摘除并 free
     * server_stop() 关闭 fd 后会触发此处 recv 失败退出循环，
     * 然后此处负责从链表和内存中清理，避免 server_stop() 与 handler 并发 free
     */
    pthread_mutex_lock(&server->clients_lock);

    /* 从单链表中找到并摘除当前 client */
    ClientConn** pp = &server->clients;
    while (*pp && *pp != client) {
        pp = &(*pp)->next;
    }
    if (*pp == client) {
        *pp = client->next;
    }
    server->client_count--;

    pthread_mutex_unlock(&server->clients_lock);

    /* 关闭 fd（如果还没被 server_stop 关闭）并释放内存 */
    client_destroy(client);
    
    return NULL;
}

static void* accept_loop(void* arg) {
    TcpServer* server = (TcpServer*)arg;
    
    printf("[Server] Accept loop started on %s:%d\n", 
           server->config.bind_host, server->config.bind_port);
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);  /* 10ms */
                continue;
            }
            if (server->running) {
                perror("[Server] accept failed");
            }
            break;
        }
        
        /* 检查连接数限制 */
        pthread_mutex_lock(&server->clients_lock);
        if (server->client_count >= server->config.max_connections) {
            pthread_mutex_unlock(&server->clients_lock);
            fprintf(stderr, "[Server] Max connections reached, rejecting new client\n");
            close(client_fd);
            continue;
        }
        pthread_mutex_unlock(&server->clients_lock);
        
        /* 创建客户端连接 */
        ClientConn* client = client_create(client_fd, &client_addr);
        if (!client) {
            close(client_fd);
            continue;
        }
        
        /* 设置socket选项 */
        set_recv_timeout(client_fd, RECV_TIMEOUT_SEC);
        
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        /* 启动客户端处理线程（先准备参数，避免加入链表后再失败导致悬挂指针） */
        ClientHandlerArg* arg = malloc(sizeof(*arg));
        if (!arg) {
            client_destroy(client);
            continue;
        }
        arg->server = server;
        arg->client = client;

        /* 添加到链表 */
        pthread_mutex_lock(&server->clients_lock);
        client->next = server->clients;
        server->clients = client;
        server->client_count++;
        server->total_connections++;
        pthread_mutex_unlock(&server->clients_lock);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[Server] New connection from %s:%d (total: %d)\n",
               client_ip, ntohs(client_addr.sin_port), server->client_count);

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_handler, arg) != 0) {
            fprintf(stderr, "[Server] Failed to create client handler thread\n");
            free(arg);

            /* 从链表中摘除并销毁 client */
            pthread_mutex_lock(&server->clients_lock);
            ClientConn** pp = &server->clients;
            while (*pp && *pp != client) {
                pp = &(*pp)->next;
            }
            if (*pp == client) {
                *pp = client->next;
                server->client_count--;
            }
            pthread_mutex_unlock(&server->clients_lock);

            client_destroy(client);
            continue;
        }
        pthread_detach(thread);
    }
    
    printf("[Server] Accept loop stopped\n");
    return NULL;
}

int server_init(TcpServer* server, const ServerConfig* config, StreamManager* stream_mgr) {
    if (!server || !config || !stream_mgr) {
        return -1;
    }
    
    memset(server, 0, sizeof(*server));
    memcpy(&server->config, config, sizeof(*config));
    server->stream_mgr = stream_mgr;
    server->listen_fd = -1;
    server->running = false;
    
    pthread_mutex_init(&server->clients_lock, NULL);
    
    return 0;
}

int server_start(TcpServer* server) {
    if (!server) return -1;
    
    /* 创建监听socket */
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        perror("[Server] socket failed");
        return -1;
    }
    
    /* 设置选项 */
    int reuse = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    /* 绑定 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->config.bind_port);
    
    if (inet_pton(AF_INET, server->config.bind_host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[Server] Invalid bind address: %s\n", server->config.bind_host);
        close(server->listen_fd);
        return -1;
    }
    
    if (bind(server->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Server] bind failed");
        close(server->listen_fd);
        return -1;
    }
    
    /* 监听 */
    if (listen(server->listen_fd, server->config.max_connections) < 0) {
        perror("[Server] listen failed");
        close(server->listen_fd);
        return -1;
    }
    
    /* 设置为非阻塞 */
    set_nonblocking(server->listen_fd);
    
    server->running = true;
    
    /* 启动接受线程 */
    if (pthread_create(&server->accept_thread, NULL, accept_loop, server) != 0) {
        perror("[Server] Failed to create accept thread");
        close(server->listen_fd);
        return -1;
    }
    
    printf("[Server] Started on %s:%d (max connections: %d)\n",
           server->config.bind_host, server->config.bind_port, 
           server->config.max_connections);
    
    return 0;
}

void server_stop(TcpServer* server) {
    if (!server) return;
    
    printf("[Server] Stopping...\n");
    server->running = false;
    
    /* 关闭监听socket */
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    
    /* 等待接受线程结束 */
    pthread_join(server->accept_thread, NULL);
    
    /* 关闭所有客户端 fd，触发 handler 线程的 recv 返回错误并自行退出+清理 */
    pthread_mutex_lock(&server->clients_lock);
    ClientConn* client = server->clients;
    while (client) {
        if (client->fd >= 0) {
            close(client->fd);
            client->fd = -1;
        }
        client = client->next;
    }
    pthread_mutex_unlock(&server->clients_lock);

    /* 等待 handler 线程完成链表摘除 + client_destroy()
     *
     * 注意：handler 线程是 detach 的，无法 join。
     * 这里采用轮询等待链表清空的方式，避免在 server_stop() 中并发 free 导致 UAF/double-free。
     */
    const int max_wait_ms = (RECV_TIMEOUT_SEC + 1) * 1000;
    int waited_ms = 0;
    while (waited_ms < max_wait_ms) {
        pthread_mutex_lock(&server->clients_lock);
        int empty = (server->clients == NULL);
        int left = server->client_count;
        pthread_mutex_unlock(&server->clients_lock);

        if (empty) {
            break;
        }

        /* 每 200ms 打印一次提示（避免刷屏） */
        if (waited_ms % 200 == 0) {
            fprintf(stderr, "[Server] waiting clients to exit... left=%d\n", left);
        }

        usleep(50 * 1000);
        waited_ms += 50;
    }

    pthread_mutex_lock(&server->clients_lock);
    if (server->clients != NULL) {
        fprintf(stderr,
                "[Server] warning: server_stop timeout, %d client(s) still in list; "
                "skip forced free to avoid races\n",
                server->client_count);
    }
    pthread_mutex_unlock(&server->clients_lock);
    
    printf("[Server] Stopped\n");

    /* stop h264 tap (optional) */
    if (g_h264_tap_started) {
        h264_tap_stop();
        g_h264_tap_started = 0;
    }
}

bool server_is_running(TcpServer* server) {
    return server && server->running;
}

void server_get_stats(TcpServer* server, uint64_t* connections, 
                      uint64_t* packets, uint64_t* bytes) {
    if (!server) return;
    
    if (connections) *connections = server->total_connections;
    if (packets) *packets = server->total_packets;
    if (bytes) *bytes = server->total_bytes;
}

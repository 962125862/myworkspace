/**
 * @file server.c
 * @brief TCP服务器实现（单端口多流）
 */

#define _GNU_SOURCE
#include "server.h"
#include "protocol.h"
#include "decoder.h"
#include "stream.h"
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

static int recv_exact(int fd, uint8_t* buf, size_t n, int timeout_sec) {
    size_t received = 0;
    time_t start = time(NULL);
    
    while (received < n) {
        if (time(NULL) - start > timeout_sec) {
            return -1;  /* 超时 */
        }
        
        ssize_t r = recv(fd, buf + received, n - received, 0);
        if (r > 0) {
            received += r;
        } else if (r == 0) {
            return -1;  /* 连接关闭 */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;  /* 错误 */
        }
        
        usleep(1000);  /* 1ms */
    }
    
    return 0;
}



/* 压力测试模式开关（通过环境变量控制） */
static int g_stress_test_enabled = 0;
static int g_stress_test_copies = 20;

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
            if (header->length >= TCP_HEADER_SIZE + sizeof(StreamInfo)) {
                StreamInfo info;
                if (protocol_parse_stream_info(payload, &info) == 0) {
                    stream_set_info(stream, &info);
                    stream_set_state(stream, STREAM_STATE_ACTIVE);
                    client->current_stream_id = header->stream_id;
                    
                    /* 自动检测或强制指定解码器 */
                    DecodeBackend backend = get_decode_backend();
                    if (stream_init_decoder(stream, backend) < 0) {
                        fprintf(stderr, "[Server] Failed to init decoder for stream %d\n",
                                header->stream_id);
                    }
                    
                    printf("[Server] Stream %d started: %dx%d@%dfps\n",
                           header->stream_id, info.width, info.height, info.fps);
                    
                    /* 检查是否启用压力测试模式 */
                    if (g_stress_test_enabled == 0) {
                        const char* stress_env = getenv("STRESS_TEST");
                        if (stress_env && atoi(stress_env) > 0) {
                            g_stress_test_enabled = 1;
                            const char* copies_env = getenv("STRESS_COPIES");
                            if (copies_env) {
                                g_stress_test_copies = atoi(copies_env);
                                if (g_stress_test_copies < 1) g_stress_test_copies = 20;
                                if (g_stress_test_copies > MAX_STREAMS) g_stress_test_copies = MAX_STREAMS;
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
        if (recv_exact(client->fd, header_buf, TCP_HEADER_SIZE, RECV_TIMEOUT_SEC) < 0) {
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
            
            payload = malloc(payload_len);
            if (!payload) {
                break;
            }
            
            if (recv_exact(client->fd, payload, payload_len, RECV_TIMEOUT_SEC) < 0) {
                free(payload);
                break;
            }
        }
        
        /* 处理包 */
        handle_packet(server, client, &header, payload);
        
        free(payload);
    }
    
    printf("[Server] Client %s disconnected\n", client_ip);
    
    /* 标记为待删除 */
    client->should_remove = 1;
    client->active = 0;
    
    pthread_mutex_lock(&server->clients_lock);
    server->client_count--;
    pthread_mutex_unlock(&server->clients_lock);
    
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
        
        /* 启动客户端处理线程 */
        ClientHandlerArg* arg = malloc(sizeof(ClientHandlerArg));
        if (!arg) {
            client_destroy(client);
            pthread_mutex_lock(&server->clients_lock);
            server->client_count--;
            pthread_mutex_unlock(&server->clients_lock);
            continue;
        }
        arg->server = server;
        arg->client = client;
        
        pthread_t thread;
        pthread_create(&thread, NULL, client_handler, arg);
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
    
    /* 关闭所有客户端连接 */
    pthread_mutex_lock(&server->clients_lock);
    ClientConn* client = server->clients;
    while (client) {
        ClientConn* next = client->next;
        if (client->fd >= 0) {
            close(client->fd);
            client->fd = -1;
        }
        client = next;
    }
    pthread_mutex_unlock(&server->clients_lock);
    
    /* 等待客户端线程退出 */
    usleep(500000);  /* 500ms */
    
    /* 清理客户端资源 */
    pthread_mutex_lock(&server->clients_lock);
    client = server->clients;
    while (client) {
        ClientConn* next = client->next;
        client_destroy(client);
        client = next;
    }
    server->clients = NULL;
    server->client_count = 0;
    pthread_mutex_unlock(&server->clients_lock);
    
    printf("[Server] Stopped\n");
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

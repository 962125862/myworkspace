/**
 * @file main.c
 * @brief 多路视频流接收服务器主程序
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "protocol.h"
#include "stream.h"
#include "server.h"

static volatile int g_running = 1;

static void signal_handler(int sig) {
    printf("\n[Main] Received signal %d, shutting down...\n", sig);
    g_running = 0;
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -h, --host HOST       Bind host (default: 0.0.0.0)\n");
    printf("  -p, --port PORT       Bind port (default: 9000)\n");
    printf("  -c, --connections N   Max connections (default: 20)\n");
    printf("  -s, --stats-interval  Stats print interval in seconds (default: 10)\n");
    printf("  -d, --daemon          Run as daemon\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  --help                Show this help\n");
}

int main(int argc, char* argv[]) {
    /* 默认配置 */
    ServerConfig config;
    memset(&config, 0, sizeof(config));
    strncpy(config.bind_host, "0.0.0.0", sizeof(config.bind_host) - 1);
    config.bind_port = DEFAULT_LISTEN_PORT;
    config.max_connections = MAX_STREAMS;
    config.recv_buffer_size = 1024 * 1024;
    
    int stats_interval = 10;
    int daemon_mode = 0;
    
    /* 解析命令行参数 */
    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"connections", required_argument, 0, 'c'},
        {"stats-interval", required_argument, 0, 's'},
        {"daemon", no_argument, 0, 'd'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 0},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "h:p:c:s:dv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                strncpy(config.bind_host, optarg, sizeof(config.bind_host) - 1);
                break;
            case 'p':
                config.bind_port = atoi(optarg);
                break;
            case 'c':
                config.max_connections = atoi(optarg);
                if (config.max_connections > MAX_STREAMS) {
                    config.max_connections = MAX_STREAMS;
                }
                break;
            case 's':
                stats_interval = atoi(optarg);
                break;
            case 'd':
                daemon_mode = 1;
                break;
            case 'v':
                /* verbose 模式预留，当前无额外输出 */
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "help") == 0) {
                    print_usage(argv[0]);
                    return 0;
                }
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("========================================\n");
    printf("  Stream Server - Multi-Stream Receiver\n");
    printf("========================================\n");
    printf("Bind: %s:%d\n", config.bind_host, config.bind_port);
    printf("Max connections: %d\n", config.max_connections);
    printf("Stats interval: %ds\n", stats_interval);
    printf("========================================\n\n");
    
    /* 守护进程模式 */
    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            perror("daemon failed");
            return 1;
        }
    }
    
    /* 初始化流管理器 */
    StreamManager stream_mgr;
    if (stream_manager_init(&stream_mgr) < 0) {
        fprintf(stderr, "[Main] Failed to init stream manager\n");
        return 1;
    }
    
    /* 初始化服务器 */
    TcpServer server;
    if (server_init(&server, &config, &stream_mgr) < 0) {
        fprintf(stderr, "[Main] Failed to init server\n");
        return 1;
    }
    
    /* 启动服务器 */
    if (server_start(&server) < 0) {
        fprintf(stderr, "[Main] Failed to start server\n");
        return 1;
    }
    
    /* 主循环 */
    time_t last_stats = time(NULL);
    
    while (g_running && server_is_running(&server)) {
        sleep(1);
        
        /* 定期打印统计 */
        if (stats_interval > 0 && time(NULL) - last_stats >= stats_interval) {
            stream_manager_print_stats(&stream_mgr);
            
            uint64_t conn, pkt, bytes;
            server_get_stats(&server, &conn, &pkt, &bytes);
            printf("[Stats] Connections: %lu, Packets: %lu, Bytes: %.2f MB\n\n",
                   conn, pkt, bytes / (1024.0 * 1024.0));
            
            last_stats = time(NULL);
        }
    }
    
    /* 停止服务器 */
    server_stop(&server);
    
    /* 如果启用了压力测试，输出报告 */
    const char* stress_env = getenv("STRESS_TEST");
    if (stress_env && atoi(stress_env) > 0) {
        char report[4096];
        stream_manager_get_stress_report(&stream_mgr, report, sizeof(report));
        printf("%s", report);
    }
    
    printf("[Main] Server stopped\n");

    /* 销毁流管理器，释放所有 mutex 和解码器资源 */
    stream_manager_destroy(&stream_mgr);

    return 0;
}

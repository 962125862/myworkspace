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
#include "zmq_bridge.h"

static volatile int g_running = 1;

static int parse_int_env_clamped_local(const char* name, int default_value,
                                       int min_value, int max_value) {
    const char* value = getenv(name);
    if (!value || !*value) {
        return default_value;
    }

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return default_value;
    }
    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return (int)parsed;
}

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
    printf("      --max-streams N   Runtime active stream slots (default: 20, hard limit: %d)\n", MAX_STREAMS);
    printf("  -s, --stats-interval  Stats print interval in seconds (default: 10)\n");
    printf("  -d, --daemon          Run as daemon\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("\nOptional feature flags (prefer CLI over env):\n");
    printf("  --decode-backend <auto|intel|nvidia|cpu>\n");
    printf("  --zmq-bridge-bind <addr>  Enable built-in ZMQ BGR bridge, e.g. tcp://0.0.0.0:5566\n");
    printf("  --stress-test         Enable stress test mode/report\n");
    printf("  --stress-copies <n>   Stress test copies\n");
    printf("\nH264 tap (AnnexB H264 TCP output):\n");
    printf("  --h264-tap-port <p>   Enable H264 tap port (default off)\n");
    printf("  --h264-tap-bind <ip>  H264 tap bind ip (default 127.0.0.1)\n");
    printf("  --h264-tap-stall-ms <ms>  Tap stall threshold (default 200)\n");
    printf("  --h264-tap-drop-idr <0|1> Tap recovery policy (default 1)\n");
    printf("  --ml-worker-ctrl-map <map> Per-stream routing, e.g. 1:127.0.0.1:50001,2:127.0.0.1:50002\n");
    printf("  --ml-worker-ctrl-map-file <path>  File-based per-stream routing; each line: <stream_id> <ip> <port>\n");
    printf("  --help                Show this help\n");
}

int main(int argc, char* argv[]) {
    /* 默认配置 */
    ServerConfig config;
    memset(&config, 0, sizeof(config));
    strncpy(config.bind_host, "0.0.0.0", sizeof(config.bind_host) - 1);
    config.bind_port = DEFAULT_LISTEN_PORT;
    config.max_connections = DEFAULT_MAX_STREAMS;
    config.recv_buffer_size = 1024 * 1024;
    int runtime_max_streams = parse_int_env_clamped_local("STREAM_MAX_STREAMS",
                                                          DEFAULT_MAX_STREAMS, 1, MAX_STREAMS);
    int requested_connections = config.max_connections;
    
    int stats_interval = 10;
    int daemon_mode = 0;

    /* Optional feature flags (kept as locals, then exported to env for backward compatibility)
     * NOTE: Internal modules still support env vars; CLI options override them.
     */
    char decode_backend[32] = {0};
    char zmq_bridge_bind[256] = {0};
    int stress_test = 0;
    int stress_copies = 0;
    int h264_tap_port = 0;
    char h264_tap_bind[64] = {0};
    int h264_tap_stall_ms = 0;
    int h264_tap_drop_idr = -1;
    char ml_worker_ctrl_map[2048] = {0};
    char ml_worker_ctrl_map_file[512] = {0};
    
    /* 解析命令行参数 */
    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"connections", required_argument, 0, 'c'},
        {"max-streams", required_argument, 0, 1001},
        {"stats-interval", required_argument, 0, 's'},
        {"daemon", no_argument, 0, 'd'},
        {"verbose", no_argument, 0, 'v'},
        {"decode-backend", required_argument, 0, 1000},
        {"zmq-bridge-bind", required_argument, 0, 1003},
        {"stress-test", no_argument, 0, 1004},
        {"stress-copies", required_argument, 0, 1005},
        {"h264-tap-port", required_argument, 0, 1006},
        {"h264-tap-bind", required_argument, 0, 1007},
        {"h264-tap-stall-ms", required_argument, 0, 1008},
        {"h264-tap-drop-idr", required_argument, 0, 1009},
        {"ml-worker-ctrl-map", required_argument, 0, 1012},
        {"ml-worker-ctrl-map-file", required_argument, 0, 1013},
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
                requested_connections = atoi(optarg);
                break;
            case 1001:
                runtime_max_streams = atoi(optarg);
                if (runtime_max_streams < 1) {
                    runtime_max_streams = DEFAULT_MAX_STREAMS;
                }
                if (runtime_max_streams > MAX_STREAMS) {
                    runtime_max_streams = MAX_STREAMS;
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

            case 1000: /* --decode-backend */
                strncpy(decode_backend, optarg, sizeof(decode_backend) - 1);
                break;
            case 1003: /* --zmq-bridge-bind */
                strncpy(zmq_bridge_bind, optarg, sizeof(zmq_bridge_bind) - 1);
                break;
            case 1004: /* --stress-test */
                stress_test = 1;
                break;
            case 1005: /* --stress-copies */
                stress_copies = atoi(optarg);
                if (stress_copies < 0) stress_copies = 0;
                break;
            case 1006: /* --h264-tap-port */
                h264_tap_port = atoi(optarg);
                break;
            case 1007: /* --h264-tap-bind */
                strncpy(h264_tap_bind, optarg, sizeof(h264_tap_bind) - 1);
                break;
            case 1008: /* --h264-tap-stall-ms */
                h264_tap_stall_ms = atoi(optarg);
                break;
            case 1009: /* --h264-tap-drop-idr */
                h264_tap_drop_idr = atoi(optarg) > 0 ? 1 : 0;
                break;
            case 1012: /* --ml-worker-ctrl-map */
                strncpy(ml_worker_ctrl_map, optarg, sizeof(ml_worker_ctrl_map) - 1);
                break;
            case 1013: /* --ml-worker-ctrl-map-file */
                strncpy(ml_worker_ctrl_map_file, optarg, sizeof(ml_worker_ctrl_map_file) - 1);
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (requested_connections < 1) {
        requested_connections = DEFAULT_MAX_STREAMS;
    }
    if (requested_connections > MAX_STREAMS) {
        requested_connections = MAX_STREAMS;
    }
    if (requested_connections < runtime_max_streams) {
        requested_connections = runtime_max_streams;
    }
    config.max_connections = requested_connections;

    /* Export optional flags to env for backward compatibility.
     * The rest of the codebase currently reads env vars.
     */
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", runtime_max_streams);
        setenv("STREAM_MAX_STREAMS", buf, 1);
    }
    if (decode_backend[0]) {
        setenv("DECODE_BACKEND", decode_backend, 1);
    }
    if (zmq_bridge_bind[0]) {
        setenv("ZMQ_BRIDGE_BIND", zmq_bridge_bind, 1);
    }
    if (stress_test) {
        setenv("STRESS_TEST", "1", 1);
    }
    if (stress_copies > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", stress_copies);
        setenv("STRESS_COPIES", buf, 1);
    }
    if (h264_tap_port > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", h264_tap_port);
        setenv("H264_TAP_PORT", buf, 1);
    }
    if (h264_tap_bind[0]) {
        setenv("H264_TAP_BIND", h264_tap_bind, 1);
    }
    if (h264_tap_stall_ms > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", h264_tap_stall_ms);
        setenv("H264_TAP_STALL_MS", buf, 1);
    }
    if (h264_tap_drop_idr != -1) {
        setenv("H264_TAP_DROP_IDR", h264_tap_drop_idr ? "1" : "0", 1);
    }
    if (ml_worker_ctrl_map[0]) {
        setenv("ML_WORKER_CTRL_MAP", ml_worker_ctrl_map, 1);
    }
    if (ml_worker_ctrl_map_file[0]) {
        setenv("ML_WORKER_CTRL_MAP_FILE", ml_worker_ctrl_map_file, 1);
    }
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("========================================\n");
    printf("  Stream Server - Multi-Stream Receiver\n");
    printf("========================================\n");
    printf("Bind: %s:%d\n", config.bind_host, config.bind_port);
    printf("Max streams: %d (hard limit: %d)\n", runtime_max_streams, MAX_STREAMS);
    printf("Max connections: %d\n", config.max_connections);
    printf("Stats interval: %ds\n", stats_interval);
    printf("========================================\n\n");

    /* 可选：启动内置 ZMQ bridge（ROUTER），用于对外提供 GET_LATEST_BGR。
     * 通过环境变量控制，避免改变默认行为。
     *   ZMQ_BRIDGE_BIND=tcp://0.0.0.0:5566
     */
    const char* zmq_bind_env = getenv("ZMQ_BRIDGE_BIND");
    
    /* 守护进程模式 */
    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            perror("daemon failed");
            return 1;
        }
    }
    
    /* 初始化流管理器 */
    StreamManager stream_mgr;
    if (stream_manager_init(&stream_mgr, (uint16_t)runtime_max_streams) < 0) {
        fprintf(stderr, "[Main] Failed to init stream manager\n");
        return 1;
    }

    if (zmq_bind_env && zmq_bind_env[0]) {
        if (zmq_bridge_start(&stream_mgr, zmq_bind_env, &g_running) == 0) {
            printf("[Main] ZMQ bridge enabled: %s\n", zmq_bind_env);
        } else {
            fprintf(stderr, "[Main] Failed to start ZMQ bridge (maybe not built with libzmq)\n");
        }
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

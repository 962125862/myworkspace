/**
 * @file main.c
 * @brief ml_worker 主程序：
 *
 * 功能概览：
 * - 作为“推流端”：连接 Sunshine/NVIDIA GameStream 主机，启动指定 app 的串流
 * - 通过 Moonlight embedded (Limelight) 的回调拿到编码后的 H.264 bytestream
 * - 使用自定义 TLV 协议经 TCP 推送到 stream_server（接收端负责解码/共享内存发布/旁路桥接）
 * - 可选启用控制通道：监听 UDP 控制包，将鼠标/键盘/文本事件注入到串流会话
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <Limelight.h>
#include "client.h"

#include "connection_callbacks.h"
#include "video_callbacks.h"
#include "worker_defs.h"
#include "control_socket.h"

extern char* gs_error;

static volatile int g_running = 1;

typedef struct {
    const char* host;
    const char* app;
    const char* key_dir;

    int width;
    int height;
    int fps;
    int bitrate;
    int packet_size;

    int ll_color_space;
    int ll_color_range;

    /* TCP输出配置 */
    char tcp_host[256];
    uint16_t tcp_port;
    uint16_t stream_id;

    const char* control_bind;
    int control_port;
} StreamOptions;

typedef struct {
    const char* host;
    const char* key_dir;
    const char* pin;
} PairOptions;

typedef struct {
    const char* host;
    const char* key_dir;
} ListOptions;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static bool is_all_digits(const char* s) {
    if (s == NULL || *s == '\0') {
        return false;
    }

    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    return true;
}

static const char* default_key_dir(void) {
    static char buf[512];
    static int initialized = 0;

    if (!initialized) {
        const char* home = getenv("HOME");
        if (home && *home) {
            snprintf(buf, sizeof(buf), "%s/.cache/moonlight", home);
        } else {
            snprintf(buf, sizeof(buf), "./keys");
        }
        initialized = 1;
    }

    return buf;
}

static void generate_pair_pin(char out[5]) {
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        srand((unsigned int)(time(NULL) ^ (unsigned int)getpid()));
    }

    unsigned int pin = (unsigned int)(rand() % 10000);
    snprintf(out, 5, "%04u", pin);
}

static int resolve_app_id(PSERVER_DATA server, const char* app_arg) {
    PAPP_LIST app_list = NULL;

    if (gs_applist(server, &app_list) != 0) {
        fprintf(stderr, "gs_applist failed: %s\n", gs_error ? gs_error : "(null)");
        return -1;
    }

    bool by_index = is_all_digits(app_arg);
    int wanted_index = by_index ? atoi(app_arg) : -1;

    int index = 1;
    for (PAPP_LIST p = app_list; p != NULL; p = p->next, index++) {
        fprintf(stderr, "[%d] real_app_id=%d name=%s\n",
                index, p->id, p->name ? p->name : "(null)");

        if (by_index) {
            if (index == wanted_index) {
                return p->id;
            }
        } else {
            if (p->name && strcmp(p->name, app_arg) == 0) {
                return p->id;
            }
        }
    }

    return -1;
}

static int default_limelight_color_space(void) {
#ifdef COLORSPACE_REC_709
    return COLORSPACE_REC_709;
#elif defined(COLORSPACE_REC_601)
    return COLORSPACE_REC_601;
#else
    return 0;
#endif
}

static int default_limelight_color_range(void) {
#ifdef COLOR_RANGE_LIMITED
    return COLOR_RANGE_LIMITED;
#else
    return 0;
#endif
}

static int parse_color_space_arg(const char* s, int* out_ll) {
    if (!s || !out_ll) {
        return -1;
    }

    if (!strcasecmp(s, "709") || !strcasecmp(s, "bt709") || !strcasecmp(s, "rec709")) {
#ifdef COLORSPACE_REC_709
        *out_ll = COLORSPACE_REC_709;
        return 0;
#else
        return -1;
#endif
    }

    if (!strcasecmp(s, "601") || !strcasecmp(s, "bt601") || !strcasecmp(s, "rec601")) {
#ifdef COLORSPACE_REC_601
        *out_ll = COLORSPACE_REC_601;
        return 0;
#else
        return -1;
#endif
    }

    return -1;
}

static int parse_color_range_arg(const char* s, int* out_ll) {
    if (!s || !out_ll) {
        return -1;
    }

    if (!strcasecmp(s, "limited")) {
#ifdef COLOR_RANGE_LIMITED
        *out_ll = COLOR_RANGE_LIMITED;
        return 0;
#else
        return -1;
#endif
    }

    if (!strcasecmp(s, "full")) {
#ifdef COLOR_RANGE_FULL
        *out_ll = COLOR_RANGE_FULL;
        return 0;
#else
        return -1;
#endif
    }

    return -1;
}

static void stream_options_defaults(StreamOptions* o) {
    memset(o, 0, sizeof(*o));

    o->key_dir = default_key_dir();

    /* 默认TCP输出配置 */
    snprintf(o->tcp_host, sizeof(o->tcp_host), "127.0.0.1");
    o->tcp_port = 9000;
    o->stream_id = 1;

    o->width = 1280;
    o->height = 720;
    o->fps = 60;
    o->bitrate = 10000;
    o->packet_size = 1392;  /* Moonlight 标准包大小，减少网络开销 */

    o->ll_color_space = default_limelight_color_space();
    o->ll_color_range = default_limelight_color_range();

    o->control_bind = "127.0.0.1";
    o->control_port = 0;
}

static void pair_options_defaults(PairOptions* o) {
    memset(o, 0, sizeof(*o));
    o->key_dir = default_key_dir();
}

static void list_options_defaults(ListOptions* o) {
    memset(o, 0, sizeof(*o));
    o->key_dir = default_key_dir();
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s pair <host> [pin] [--key-dir <path>]\n"
            "  %s list <host> [--key-dir <path>]\n"
            "  %s <host> <app_index_or_name>\n"
            "  %s stream --host <ip> --app <name_or_index> [options]\n"
            "\n"
            "stream options:\n"
            "  --key-dir <path>\n"
            "  --tcp-host <ip>           default: 127.0.0.1\n"
            "  --tcp-port <port>         default: 9000\n"
            "  --stream-id <id>          default: 1 (1-65535)\n"
            "  --width <n>               default: 1280\n"
            "  --height <n>              default: 720\n"
            "  --fps <n>                 default: 60\n"
            "  --bitrate <n>             default: 10000 (kbps)\n"
            "  --packet-size <n>         default: 1024\n"
            "  --colorspace <601|709>    default: 709\n"
            "  --range <limited|full>    default: limited\n"
            "  --control-bind <ip>       default: 127.0.0.1\n"
            "  --control-port <port>     default: 0 (disabled)\n"
            "\n"
            "examples:\n"
            "  %s pair 192.168.11.50\n"
            "  %s list 192.168.11.50\n"
            "  %s 192.168.11.50 Desktop\n"
            "  %s stream --host 192.168.11.50 --app Desktop --tcp-host 192.168.1.100 --tcp-port 9000 --stream-id 1\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static int parse_pair_args(int argc, char** argv, PairOptions* o) {
    pair_options_defaults(o);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            o->host = argv[++i];
        } else if (!strcmp(argv[i], "--key-dir") && i + 1 < argc) {
            o->key_dir = argv[++i];
        } else if (!strcmp(argv[i], "--pin") && i + 1 < argc) {
            o->pin = argv[++i];
        } else if (argv[i][0] != '-' && !o->host) {
            o->host = argv[i];
        } else if (argv[i][0] != '-' && !o->pin) {
            o->pin = argv[i];
        } else {
            fprintf(stderr, "unknown or incomplete pair argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!o->host) {
        fprintf(stderr, "pair: missing host\n");
        return -1;
    }

    if (o->pin) {
        if (!is_all_digits(o->pin) || strlen(o->pin) != 4) {
            fprintf(stderr, "pair: pin must be exactly 4 digits\n");
            return -1;
        }
    }

    return 0;
}

static int parse_list_args(int argc, char** argv, ListOptions* o) {
    list_options_defaults(o);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            o->host = argv[++i];
        } else if (!strcmp(argv[i], "--key-dir") && i + 1 < argc) {
            o->key_dir = argv[++i];
        } else if (argv[i][0] != '-' && !o->host) {
            o->host = argv[i];
        } else {
            fprintf(stderr, "unknown or incomplete list argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!o->host) {
        fprintf(stderr, "list: missing host\n");
        return -1;
    }

    return 0;
}

static int parse_stream_args(int argc, char** argv, StreamOptions* o) {
    stream_options_defaults(o);

    if (argc == 3 && argv[1][0] != '-' && argv[2][0] != '-') {
        o->host = argv[1];
        o->app = argv[2];
    } else {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--host") && i + 1 < argc) {
                o->host = argv[++i];
            } else if (!strcmp(argv[i], "--app") && i + 1 < argc) {
                o->app = argv[++i];
            } else if (!strcmp(argv[i], "--key-dir") && i + 1 < argc) {
                o->key_dir = argv[++i];
            } else if (!strcmp(argv[i], "--tcp-host") && i + 1 < argc) {
                snprintf(o->tcp_host, sizeof(o->tcp_host), "%s", argv[++i]);
            } else if (!strcmp(argv[i], "--tcp-port") && i + 1 < argc) {
                o->tcp_port = (uint16_t)atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--stream-id") && i + 1 < argc) {
                int id = atoi(argv[++i]);
                if (id < 1 || id > 65535) {
                    fprintf(stderr, "invalid --stream-id (must be 1-65535)\n");
                    return -1;
                }
                o->stream_id = (uint16_t)id;
            } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
                o->width = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
                o->height = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
                o->fps = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) {
                o->bitrate = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--packet-size") && i + 1 < argc) {
                o->packet_size = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--colorspace") && i + 1 < argc) {
                if (parse_color_space_arg(argv[++i], &o->ll_color_space) != 0) {
                    fprintf(stderr, "invalid --colorspace\n");
                    return -1;
                }
            } else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
                if (parse_color_range_arg(argv[++i], &o->ll_color_range) != 0) {
                    fprintf(stderr, "invalid --range\n");
                    return -1;
                }
            } else if (!strcmp(argv[i], "--control-bind") && i + 1 < argc) {
                o->control_bind = argv[++i];
            } else if (!strcmp(argv[i], "--control-port") && i + 1 < argc) {
                o->control_port = atoi(argv[++i]);
            } else {
                fprintf(stderr, "unknown or incomplete stream argument: %s\n", argv[i]);
                return -1;
            }
        }
    }

    if (!o->host || !o->app) {
        fprintf(stderr, "stream: missing host or app\n");
        return -1;
    }

    if (o->width <= 0 || o->height <= 0 || o->fps <= 0 || o->bitrate <= 0 || o->packet_size <= 0) {
        fprintf(stderr, "invalid numeric stream options\n");
        return -1;
    }

    if (o->tcp_port == 0) {
        fprintf(stderr, "invalid --tcp-port\n");
        return -1;
    }

    if (o->control_port < 0 || o->control_port > 65535) {
        fprintf(stderr, "invalid --control-port\n");
        return -1;
    }

    return 0;
}

static int run_pair_command(const PairOptions* opt) {
    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    if (gs_init(&server, (char*)opt->host, 0, opt->key_dir, 1, false) != 0) {
        fprintf(stderr, "gs_init failed: %s\n", gs_error ? gs_error : "(null)");
        return 2;
    }

    fprintf(stderr, "server appversion=%s paired=%d\n",
            server.serverInfo.serverInfoAppVersion ? server.serverInfo.serverInfoAppVersion : "(null)",
            server.paired ? 1 : 0);

    if (server.paired) {
        fprintf(stderr, "already paired for key directory: %s\n", opt->key_dir);
        return 0;
    }

    char pin_buf[5];
    const char* pin = opt->pin;
    if (!pin) {
        generate_pair_pin(pin_buf);
        pin = pin_buf;
    }

    fprintf(stderr, "Pair with host %s using PIN: %s\n", opt->host, pin);
    fprintf(stderr, "Enter this PIN in Sunshine / NVIDIA host UI now...\n");

    if (gs_pair(&server, (char*)pin) != 0) {
        fprintf(stderr, "gs_pair failed: %s\n", gs_error ? gs_error : "(null)");
        return 3;
    }

    fprintf(stderr, "pair successful\n");
    return 0;
}

static int run_list_command(const ListOptions* opt) {
    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    if (gs_init(&server, (char*)opt->host, 0, opt->key_dir, 1, false) != 0) {
        fprintf(stderr, "gs_init failed: %s\n", gs_error ? gs_error : "(null)");
        return 2;
    }

    fprintf(stderr, "server appversion=%s paired=%d\n",
            server.serverInfo.serverInfoAppVersion ? server.serverInfo.serverInfoAppVersion : "(null)",
            server.paired ? 1 : 0);

    if (!server.paired) {
        fprintf(stderr, "host is not paired for this key directory: %s\n", opt->key_dir);
        fprintf(stderr, "run: ml_worker pair %s --key-dir %s\n", opt->host, opt->key_dir);
        return 3;
    }

    PAPP_LIST app_list = NULL;
    if (gs_applist(&server, &app_list) != 0) {
        fprintf(stderr, "gs_applist failed: %s\n", gs_error ? gs_error : "(null)");
        return 4;
    }

    int index = 1;
    for (PAPP_LIST p = app_list; p != NULL; p = p->next, index++) {
        printf("%d. %s (app_id=%d)\n",
               index,
               p->name ? p->name : "(null)",
               p->id);
    }

    return 0;
}

static int run_stream_command(const StreamOptions* opt) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
            "options: host=%s app=%s tcp=%s:%d stream_id=%u %dx%d@%d bitrate=%d key_dir=%s control=%s:%d\n",
            opt->host, opt->app, opt->tcp_host, opt->tcp_port, opt->stream_id,
            opt->width, opt->height, opt->fps, opt->bitrate,
            opt->key_dir, opt->control_bind, opt->control_port);

    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);

    streamConfig.width = opt->width;
    streamConfig.height = opt->height;
    streamConfig.fps = opt->fps;
    streamConfig.bitrate = opt->bitrate;
    streamConfig.packetSize = opt->packet_size;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    streamConfig.clientRefreshRateX100 = 6000;
    streamConfig.colorSpace = opt->ll_color_space;
    streamConfig.colorRange = opt->ll_color_range;

    if (gs_init(&server, (char*)opt->host, 0, opt->key_dir, 1, false) != 0) {
        fprintf(stderr, "gs_init failed: %s\n", gs_error ? gs_error : "(null)");
        return 2;
    }

    fprintf(stderr, "server appversion=%s codecSupport=0x%x paired=%d currentGame=%d http=%u https=%u\n",
            server.serverInfo.serverInfoAppVersion ? server.serverInfo.serverInfoAppVersion : "(null)",
            server.serverInfo.serverCodecModeSupport,
            server.paired ? 1 : 0,
            server.currentGame,
            server.httpPort,
            server.httpsPort);

    if (!server.paired) {
        fprintf(stderr, "host is not paired for this key directory\n");
        return 3;
    }

    int app_id = resolve_app_id(&server, opt->app);
    if (app_id < 0) {
        fprintf(stderr, "could not resolve app: %s\n", opt->app);
        return 4;
    }

    fprintf(stderr, "resolved app_id=%d\n", app_id);

    if (gs_start_app(&server, &streamConfig, app_id, true, false, 0) != 0) {
        fprintf(stderr, "gs_start_app failed for app_id=%d: %s\n",
                app_id, gs_error ? gs_error : "(null)");
        return 5;
    }

    fprintf(stderr, "rtsp url: %s\n",
            server.serverInfo.rtspSessionUrl ? server.serverInfo.rtspSessionUrl : "(null)");

    volatile int fatal_code = WORKER_FATAL_NONE;

    WorkerRenderConfig render_cfg;
    memset(&render_cfg, 0, sizeof(render_cfg));
    snprintf(render_cfg.tcp_host, sizeof(render_cfg.tcp_host), "%s", opt->tcp_host);
    render_cfg.tcp_port = opt->tcp_port;
    render_cfg.stream_id = opt->stream_id;
    render_cfg.width = (uint32_t)opt->width;
    render_cfg.height = (uint32_t)opt->height;
    render_cfg.fps = (uint32_t)opt->fps;
    render_cfg.bitrate = (uint32_t)opt->bitrate;
    render_cfg.fatal_code = &fatal_code;

    connection_callbacks_set_fatal_code(&fatal_code);

    int err = LiStartConnection(&server.serverInfo,
                                &streamConfig,
                                &connection_callbacks,
                                &video_callbacks,
                                NULL,
                                &render_cfg,
                                0,
                                NULL,
                                0);
    if (err != 0) {
        fprintf(stderr, "LiStartConnection failed: %d\n", err);
        connection_callbacks_set_fatal_code(NULL);
        return 6;
    }

    ControlSocket control_socket;
    memset(&control_socket, 0, sizeof(control_socket));
    control_socket.fd = -1;

    int control_enabled = 0;
    if (opt->control_port > 0) {
        if (control_socket_open(&control_socket, opt->control_bind, (uint16_t)opt->control_port) != 0) {
            fprintf(stderr, "control socket open failed\n");
            connection_callbacks_set_fatal_code(NULL);
            LiStopConnection();
            return 7;
        }
        control_enabled = 1;
    }

    fprintf(stderr, "streaming started, press Ctrl+C to stop\n");

    int exit_code = 0;

    while (g_running) {
        if (control_enabled) {
            control_socket_process_all(&control_socket, opt->width, opt->height);
        }

        if (fatal_code != WORKER_FATAL_NONE) {
            fprintf(stderr, "fatal_code=%d, stopping worker\n", fatal_code);
            exit_code = 100 + fatal_code;
            break;
        }

        usleep(control_enabled ? 2000 : 10000);
    }

    if (control_enabled) {
        control_socket_close(&control_socket);
    }

    connection_callbacks_set_fatal_code(NULL);
    LiStopConnection();

    return exit_code;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "pair")) {
        PairOptions opt;
        if (parse_pair_args(argc - 1, argv + 1, &opt) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        return run_pair_command(&opt);
    }

    if (!strcmp(argv[1], "list")) {
        ListOptions opt;
        if (parse_list_args(argc - 1, argv + 1, &opt) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        return run_list_command(&opt);
    }

    if (!strcmp(argv[1], "stream")) {
        StreamOptions opt;
        if (parse_stream_args(argc - 1, argv + 1, &opt) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        return run_stream_command(&opt);
    }

    /* 兼容旧用法：ml_worker <host> <app> */
    {
        StreamOptions opt;
        if (parse_stream_args(argc, argv, &opt) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        return run_stream_command(&opt);
    }
}

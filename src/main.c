#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include "control_socket.h"
#include <Limelight.h>
#include "client.h"

#include "connection_callbacks.h"
#include "video_callbacks.h"
#include "worker_defs.h"

extern char* gs_error;

static volatile int g_running = 1;

typedef struct {
    const char* host;
    const char* app;
    const char* key_dir;
    char shm_name[ML_SHM_NAME_MAX];

    int width;
    int height;
    int fps;
    int bitrate;
    int packet_size;

    int ll_color_space;
    int ll_color_range;
    uint32_t shm_color_space;
    uint32_t shm_color_range;
    const char* control_bind;
    int control_port;

} AppOptions;

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
        fprintf(stderr, "[%d] real_app_id=%d name=%s\n", index, p->id, p->name);

        if (by_index) {
            if (index == wanted_index) {
                return p->id;
            }
        } else {
            if (strcmp(p->name, app_arg) == 0) {
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

static int parse_color_space_arg(const char* s, int* out_ll, uint32_t* out_shm) {
    if (!s || !out_ll || !out_shm) {
        return -1;
    }

    if (!strcasecmp(s, "709") || !strcasecmp(s, "bt709") || !strcasecmp(s, "rec709")) {
#ifdef COLORSPACE_REC_709
        *out_ll = COLORSPACE_REC_709;
        *out_shm = ML_COLOR_SPACE_BT709;
        return 0;
#else
        return -1;
#endif
    }

    if (!strcasecmp(s, "601") || !strcasecmp(s, "bt601") || !strcasecmp(s, "rec601")) {
#ifdef COLORSPACE_REC_601
        *out_ll = COLORSPACE_REC_601;
        *out_shm = ML_COLOR_SPACE_BT601;
        return 0;
#else
        return -1;
#endif
    }

    return -1;
}

static int parse_color_range_arg(const char* s, int* out_ll, uint32_t* out_shm) {
    if (!s || !out_ll || !out_shm) {
        return -1;
    }

    if (!strcasecmp(s, "limited")) {
#ifdef COLOR_RANGE_LIMITED
        *out_ll = COLOR_RANGE_LIMITED;
        *out_shm = ML_COLOR_RANGE_LIMITED;
        return 0;
#else
        return -1;
#endif
    }

    if (!strcasecmp(s, "full")) {
#ifdef COLOR_RANGE_FULL
        *out_ll = COLOR_RANGE_FULL;
        *out_shm = ML_COLOR_RANGE_FULL;
        return 0;
#else
        return -1;
#endif
    }

    return -1;
}

static void options_defaults(AppOptions* o) {
    memset(o, 0, sizeof(*o));

    o->key_dir = "/home/gejun/.cache/moonlight";
    snprintf(o->shm_name, sizeof(o->shm_name), "/ml_stream_00");

    o->width = 1280;
    o->height = 720;
    o->fps = 60;
    o->bitrate = 10000;
    o->packet_size = 1024;

    o->ll_color_space = default_limelight_color_space();
    o->ll_color_range = default_limelight_color_range();
    o->shm_color_space = ML_COLOR_SPACE_BT709;
    o->shm_color_range = ML_COLOR_RANGE_LIMITED;
    o->control_bind = "127.0.0.1";
    o->control_port = 0;   /* 0 = disabled */

}

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s <host> <app_index_or_name>\n"
            "or\n"
            "  %s --host <ip> --app <name_or_index> [options]\n"
            "\n"
            "options:\n"
            "  --key-dir <path>\n"
            "  --shm-name <name>         default: /ml_stream_00\n"
            "  --width <n>               default: 1280\n"
            "  --height <n>              default: 720\n"
            "  --fps <n>                 default: 60\n"
            "  --bitrate <n>             default: 10000\n"
            "  --packet-size <n>         default: 1024\n"
            "  --colorspace <601|709>    default: 709\n"
            "  --range <limited|full>    default: limited\n"
            "  --control-bind <ip>        default: 127.0.0.1\n"
            "  --control-port <port>      default: 0 (disabled)\n"
            "\n"
            "examples:\n"
            "  %s 192.168.11.50 Desktop\n"
            "  %s --host 192.168.11.50 --app Desktop --shm-name /ml_stream_00\n",
            argv0, argv0, argv0, argv0);
}

static int parse_args(int argc, char** argv, AppOptions* o) {
    options_defaults(o);

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
            } else if (!strcmp(argv[i], "--shm-name") && i + 1 < argc) {
                snprintf(o->shm_name, sizeof(o->shm_name), "%s", argv[++i]);
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
            } else if (!strcmp(argv[i], "--control-bind") && i + 1 < argc) {
                o->control_bind = argv[++i];
            } else if (!strcmp(argv[i], "--control-port") && i + 1 < argc) {
                o->control_port = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--colorspace") && i + 1 < argc) {
                if (parse_color_space_arg(argv[++i], &o->ll_color_space, &o->shm_color_space) != 0) {
                    fprintf(stderr, "invalid --colorspace\n");
                    return -1;
                }
            } else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
                if (parse_color_range_arg(argv[++i], &o->ll_color_range, &o->shm_color_range) != 0) {
                    fprintf(stderr, "invalid --range\n");
                    return -1;
                }
            } else {
                fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
                return -1;
            }
        }
    }

    if (!o->host || !o->app) {
        return -1;
    }

    if (o->width <= 0 || o->height <= 0 || o->fps <= 0 || o->bitrate <= 0 || o->packet_size <= 0) {
        fprintf(stderr, "invalid numeric options\n");
        return -1;
    }

    if ((o->width & 1) || (o->height & 1)) {
        fprintf(stderr, "width/height must be even for I420\n");
        return -1;
    }

    if (o->shm_name[0] != '/') {
        fprintf(stderr, "--shm-name must start with '/'\n");
        return -1;
    }
    if (o->control_port < 0 || o->control_port > 65535) {
        fprintf(stderr, "invalid --control-port\n");
        return -1;
    }

    return 0;
}

int main(int argc, char** argv) {
    AppOptions opt;
    if (parse_args(argc, argv, &opt) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
            "options: host=%s app=%s shm=%s %dx%d@%d bitrate=%d cs=%u range=%u key_dir=%s control=%s:%d\n",
            opt.host, opt.app, opt.shm_name,
            opt.width, opt.height, opt.fps, opt.bitrate,
            opt.shm_color_space, opt.shm_color_range, opt.key_dir,
            opt.control_bind, opt.control_port);


    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);

    streamConfig.width = opt.width;
    streamConfig.height = opt.height;
    streamConfig.fps = opt.fps;
    streamConfig.bitrate = opt.bitrate;
    streamConfig.packetSize = opt.packet_size;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    streamConfig.clientRefreshRateX100 = 6000;
    streamConfig.colorSpace = opt.ll_color_space;
    streamConfig.colorRange = opt.ll_color_range;

    if (gs_init(&server, (char*)opt.host, 0, opt.key_dir, 1, false) != 0) {
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

    int app_id = resolve_app_id(&server, opt.app);
    if (app_id < 0) {
        fprintf(stderr, "could not resolve app: %s\n", opt.app);
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
    snprintf(render_cfg.shm_name, sizeof(render_cfg.shm_name), "%s", opt.shm_name);
    render_cfg.slot_count = 2;
    render_cfg.color_space = opt.shm_color_space;
    render_cfg.color_range = opt.shm_color_range;
    render_cfg.fps = (uint32_t)opt.fps;
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
    if (opt.control_port > 0) {
        if (control_socket_open(&control_socket, opt.control_bind, (uint16_t)opt.control_port) != 0) {
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
            control_socket_process_all(&control_socket, opt.width, opt.height);
        }

        if (fatal_code != WORKER_FATAL_NONE) {
            fprintf(stderr, "fatal_code=%d, stopping worker\n", fatal_code);
            exit_code = 100 + fatal_code;
            break;
        }
        usleep(control_enabled ? 2000 : 10000);
    }


    connection_callbacks_set_fatal_code(NULL);
    if (control_enabled) {
        control_socket_close(&control_socket);
    }

    LiStopConnection();

    return exit_code;
}

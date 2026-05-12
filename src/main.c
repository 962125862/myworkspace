/**
 * @file main.c
 * @brief ml_worker 主程序：
 *
 * 功能概览：
 * - 作为“推流端”：连接 Sunshine/NVIDIA GameStream 主机，启动指定 app 的串流
 * - 通过 Moonlight embedded (Limelight) 的回调拿到编码后的 H.264 bytestream
 * - 使用自定义 TLV 协议经 TCP 推送到 stream_server（接收端负责解码和按需桥接）
 * - 可选启用控制通道：监听 UDP 控制包，将鼠标/键盘/文本事件注入到串流会话
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <Limelight.h>
#include "client.h"
#include "errors.h"

#include "connection_callbacks.h"
#include "video_callbacks.h"
#include "worker_defs.h"
#include "control_socket.h"

extern const char* gs_error;

static volatile int g_running = 1;

#define HOST_OFFLINE_RETRY_SEC 30
#define NEGOTIATION_MISMATCH_RETRY_DELAY_SEC 60

static void delay_before_fatal_exit_if_needed(int fatal_code) {
    if (fatal_code != WORKER_FATAL_NEGOTIATION_MISMATCH) {
        return;
    }

    fprintf(stderr,
            "[ml_worker] fatal_code=%d, sleeping %d seconds before exit\n",
            fatal_code, NEGOTIATION_MISMATCH_RETRY_DELAY_SEC);
    for (int i = 0; i < NEGOTIATION_MISMATCH_RETRY_DELAY_SEC && g_running; ++i) {
        sleep(1);
    }
}

static const char* gs_rc_name(int rc) {
    switch (rc) {
        case GS_OK: return "GS_OK";
        case GS_FAILED: return "GS_FAILED";
        case GS_OUT_OF_MEMORY: return "GS_OUT_OF_MEMORY";
        case GS_INVALID: return "GS_INVALID";
        case GS_WRONG_STATE: return "GS_WRONG_STATE";
        case GS_IO_ERROR: return "GS_IO_ERROR";
        case GS_NOT_SUPPORTED_4K: return "GS_NOT_SUPPORTED_4K";
        case GS_UNSUPPORTED_VERSION: return "GS_UNSUPPORTED_VERSION";
        case GS_NOT_SUPPORTED_MODE: return "GS_NOT_SUPPORTED_MODE";
        case GS_ERROR: return "GS_ERROR";
        case GS_NOT_SUPPORTED_SOPS_RESOLUTION: return "GS_NOT_SUPPORTED_SOPS_RESOLUTION";
        default: return "GS_<unknown>";
    }
}

static void dump_server_modes(const SERVER_DATA* server) {
    if (!server) return;
    fprintf(stderr, "[ml_worker] server modes (width x height @ refresh):\n");
    const PDISPLAY_MODE* mode = &server->modes;
    if (!*mode) {
        fprintf(stderr, "  (none)\n");
        return;
    }
    for (PDISPLAY_MODE p = server->modes; p != NULL; p = p->next) {
        fprintf(stderr, "  - %dx%d@%d\n", p->width, p->height, p->refresh);
    }
}

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

    /* If true, skip server mode validation (useful when Sunshine doesn't report modes). */
    bool skip_mode_check;

    int codec;
    int chroma;
    int bitdepth;
    bool codec_explicit;
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

enum {
    STREAM_CODEC_H264 = 0,
    STREAM_CODEC_HEVC = 1,
    STREAM_CODEC_AV1 = 2,
};

enum {
    STREAM_CHROMA_420 = 0,
    STREAM_CHROMA_444 = 1,
};

static const char* stream_codec_name(int codec) {
    switch (codec) {
        case STREAM_CODEC_H264: return "h264";
        case STREAM_CODEC_HEVC: return "hevc";
        case STREAM_CODEC_AV1:  return "av1";
        default:                return "unknown";
    }
}

static const char* stream_chroma_name(int chroma) {
    switch (chroma) {
        case STREAM_CHROMA_420: return "420";
        case STREAM_CHROMA_444: return "444";
        default:                return "unknown";
    }
}

static int parse_codec_arg(const char* s, int* out_codec) {
    if (!s || !out_codec) {
        return -1;
    }

    if (!strcasecmp(s, "h264")) {
        *out_codec = STREAM_CODEC_H264;
        return 0;
    }
    if (!strcasecmp(s, "h265") || !strcasecmp(s, "hevc")) {
        *out_codec = STREAM_CODEC_HEVC;
        return 0;
    }
    if (!strcasecmp(s, "av1")) {
        *out_codec = STREAM_CODEC_AV1;
        return 0;
    }

    return -1;
}

static int parse_chroma_arg(const char* s, int* out_chroma) {
    if (!s || !out_chroma) {
        return -1;
    }

    if (!strcmp(s, "420")) {
        *out_chroma = STREAM_CHROMA_420;
        return 0;
    }
    if (!strcmp(s, "444")) {
        *out_chroma = STREAM_CHROMA_444;
        return 0;
    }

    return -1;
}

static int parse_bitdepth_arg(const char* s, int* out_bitdepth) {
    if (!s || !out_bitdepth) {
        return -1;
    }

    if (!strcmp(s, "8")) {
        *out_bitdepth = 8;
        return 0;
    }
    if (!strcmp(s, "10")) {
        *out_bitdepth = 10;
        return 0;
    }

    return -1;
}

static int build_supported_video_formats(const StreamOptions* opt, int* out_mask) {
    if (!opt || !out_mask) {
        return -1;
    }

    switch (opt->codec) {
        case STREAM_CODEC_H264:
            if (opt->bitdepth != 8) {
                fprintf(stderr, "invalid codec profile: h264 only supports 8-bit in current SDK\n");
                return -1;
            }
            *out_mask = (opt->chroma == STREAM_CHROMA_444)
                      ? VIDEO_FORMAT_H264_HIGH8_444
                      : VIDEO_FORMAT_H264;
            return 0;

        case STREAM_CODEC_HEVC:
            if (opt->chroma == STREAM_CHROMA_444) {
                *out_mask = (opt->bitdepth == 10)
                          ? VIDEO_FORMAT_H265_REXT10_444
                          : VIDEO_FORMAT_H265_REXT8_444;
            } else {
                *out_mask = (opt->bitdepth == 10)
                          ? VIDEO_FORMAT_H265_MAIN10
                          : VIDEO_FORMAT_H265;
            }
            return 0;

        case STREAM_CODEC_AV1:
            if (opt->chroma == STREAM_CHROMA_444) {
                *out_mask = (opt->bitdepth == 10)
                          ? VIDEO_FORMAT_AV1_HIGH10_444
                          : VIDEO_FORMAT_AV1_HIGH8_444;
            } else {
                *out_mask = (opt->bitdepth == 10)
                          ? VIDEO_FORMAT_AV1_MAIN10
                          : VIDEO_FORMAT_AV1_MAIN8;
            }
            return 0;

        default:
            return -1;
    }
}

static void apply_default_codec_policy(StreamOptions* opt) {
    if (!opt) {
        return;
    }
    if (!opt->codec_explicit &&
        opt->chroma == STREAM_CHROMA_444 &&
        opt->codec == STREAM_CODEC_H264) {
        opt->codec = STREAM_CODEC_HEVC;
        fprintf(stderr,
                "[ml_worker] chroma=444 without explicit codec, defaulting to HEVC for compatibility\n");
    }
}

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

static int contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) {
        return 0;
    }

    size_t needle_len = strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        size_t i = 0;
        while (h[i] && i < needle_len &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return 1;
        }
    }

    return 0;
}

static int gs_error_is_transient_connect_failure(const char* err) {
    static const char* const needles[] = {
        "connection refused",
        "connect failed",
        "failed to connect",
        "could not connect",
        "unable to connect",
        "timed out",
        "timeout",
        "network is unreachable",
        "host is unreachable",
        "no route to host",
    };

    if (!err || !*err) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (contains_case_insensitive(err, needles[i])) {
            return 1;
        }
    }

    return 0;
}

static int ping_host_once(const char* host) {
    if (!host || !*host) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[ml_worker] ping probe unavailable: fork failed: %s\n", strerror(errno));
        return 2;
    }

    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("ping", "ping", "-n", "-q", "-c", "1", "-W", "1", host, (char*)NULL);
        _exit(127);
    }

    int status = 0;
    while (1) {
        pid_t wait_rc = waitpid(pid, &status, 0);
        if (wait_rc == pid) {
            break;
        }
        if (wait_rc < 0 && errno == EINTR) {
            if (!g_running) {
                return -1;
            }
            continue;
        }
        if (wait_rc < 0) {
            fprintf(stderr, "[ml_worker] ping probe unavailable: waitpid failed: %s\n", strerror(errno));
            return 2;
        }
    }

    if (!WIFEXITED(status)) {
        return 1;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
        return 0;
    }
    if (exit_code == 127) {
        return 2;
    }

    return 1;
}

static int wait_for_host_online(const char* host) {
    while (g_running) {
        int ping_rc = ping_host_once(host);
        if (ping_rc == 0) {
            return 0;
        }
        if (ping_rc < 0) {
            return -1;
        }
        if (ping_rc > 1) {
            fprintf(stderr, "[ml_worker] ping probe unavailable, trying direct connection\n");
            return 0;
        }

        fprintf(stderr,
                "[ml_worker] host %s is offline, sleeping %d seconds before retry\n",
                host, HOST_OFFLINE_RETRY_SEC);
        for (int i = 0; i < HOST_OFFLINE_RETRY_SEC && g_running; ++i) {
            sleep(1);
        }
    }

    return -1;
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

    o->width = 1024;
    o->height = 768;
    o->fps = 30;
    o->bitrate = 10000;
    o->packet_size = 1392;  /* Moonlight 标准包大小，减少网络开销 */

    o->ll_color_space = default_limelight_color_space();
    o->ll_color_range = COLOR_RANGE_FULL;

    o->control_bind = "127.0.0.1";
    o->control_port = 0;
    /* Default ON: Sunshine may not report modes on some setups (e.g. VM). */
    o->skip_mode_check = true;
    o->codec = STREAM_CODEC_HEVC;
    o->chroma = STREAM_CHROMA_444;
    o->bitdepth = 8;
    o->codec_explicit = false;
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
            "  --width <n>               default: 1024\n"
            "  --height <n>              default: 768\n"
            "  --fps <n>                 default: 30\n"
            "  --bitrate <n>             default: 10000 (kbps)\n"
            "  --packet-size <n>         default: 1024\n"
            "  --colorspace <601|709>    default: 709\n"
            "  --range <limited|full>    default: full\n"
            "  --codec <h264|hevc|av1>   default: hevc\n"
            "  --chroma <420|444>        default: 444\n"
            "  --bitdepth <8|10>         default: 8\n"
            "  --skip-mode-check         default: on (allow starting even if server doesn't report modes)\n"
            "  --enforce-mode-check      force server mode validation (may fail if server doesn't report modes)\n"
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
            } else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
                if (parse_codec_arg(argv[++i], &o->codec) != 0) {
                    fprintf(stderr, "invalid --codec\n");
                    return -1;
                }
                o->codec_explicit = true;
            } else if (!strcmp(argv[i], "--chroma") && i + 1 < argc) {
                if (parse_chroma_arg(argv[++i], &o->chroma) != 0) {
                    fprintf(stderr, "invalid --chroma\n");
                    return -1;
                }
            } else if (!strcmp(argv[i], "--bitdepth") && i + 1 < argc) {
                if (parse_bitdepth_arg(argv[++i], &o->bitdepth) != 0) {
                    fprintf(stderr, "invalid --bitdepth\n");
                    return -1;
                }
            } else if (!strcmp(argv[i], "--skip-mode-check")) {
                o->skip_mode_check = true;
            } else if (!strcmp(argv[i], "--enforce-mode-check")) {
                o->skip_mode_check = false;
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

    apply_default_codec_policy(o);

    int supported_video_formats = 0;
    if (build_supported_video_formats(o, &supported_video_formats) != 0) {
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

    StreamOptions effective = *opt;
    apply_default_codec_policy(&effective);

    int supported_video_formats = 0;
    if (build_supported_video_formats(&effective, &supported_video_formats) != 0) {
        return 1;
    }

    fprintf(stderr,
            "options: host=%s app=%s tcp=%s:%d stream_id=%u %dx%d@%d bitrate=%d key_dir=%s control=%s:%d skip_mode_check=%d codec=%s chroma=%s bitdepth=%d\n",
            effective.host, effective.app, effective.tcp_host, effective.tcp_port, effective.stream_id,
            effective.width, effective.height, effective.fps, effective.bitrate,
            effective.key_dir, effective.control_bind, effective.control_port,
            effective.skip_mode_check ? 1 : 0,
            stream_codec_name(effective.codec), stream_chroma_name(effective.chroma), effective.bitdepth);

    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);

    streamConfig.width = effective.width;
    streamConfig.height = effective.height;
    streamConfig.fps = effective.fps;
    streamConfig.bitrate = effective.bitrate;
    streamConfig.packetSize = effective.packet_size;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = supported_video_formats;
    streamConfig.clientRefreshRateX100 = 6000;
    streamConfig.colorSpace = effective.ll_color_space;
    streamConfig.colorRange = effective.ll_color_range;

    fprintf(stderr,
            "[ml_worker] requesting video format mask=0x%x codec=%s chroma=%s bitdepth=%d\n",
            supported_video_formats,
            stream_codec_name(effective.codec),
            stream_chroma_name(effective.chroma),
            effective.bitdepth);

    while (g_running) {
        if (wait_for_host_online(effective.host) != 0) {
            break;
        }

        if (gs_init(&server, (char*)effective.host, 0, effective.key_dir, 1, effective.skip_mode_check) != 0) {
            const char* err = gs_error ? gs_error : "(null)";
            if (gs_error_is_transient_connect_failure(err)) {
                fprintf(stderr,
                        "[ml_worker] gs_init failed because %s; sleeping %d seconds before retry\n",
                        err, HOST_OFFLINE_RETRY_SEC);
                for (int i = 0; i < HOST_OFFLINE_RETRY_SEC && g_running; ++i) {
                    sleep(1);
                }
                continue;
            }

            fprintf(stderr, "gs_init failed: %s\n", err);
            return 2;
        }

        break;
    }

    if (!g_running) {
        return 0;
    }

    fprintf(stderr, "[ml_worker] gs_init: server.unsupported=%d (skip_mode_check=%d)\n",
            server.unsupported ? 1 : 0, effective.skip_mode_check ? 1 : 0);

    fprintf(stderr, "server appversion=%s codecSupport=0x%x paired=%d currentGame=%d http=%u https=%u\n",
            server.serverInfo.serverInfoAppVersion ? server.serverInfo.serverInfoAppVersion : "(null)",
            server.serverInfo.serverCodecModeSupport,
            server.paired ? 1 : 0,
            server.currentGame,
            server.httpPort,
            server.httpsPort);

    if (effective.codec != STREAM_CODEC_H264 || effective.chroma != STREAM_CHROMA_420 || effective.bitdepth != 8) {
        fprintf(stderr,
                "[ml_worker] note: negotiation includes codec/chroma/bitdepth metadata for downstream decode routing.\n");
    }

    if (!server.paired) {
        fprintf(stderr, "host is not paired for this key directory\n");
        return 3;
    }

    int app_id = resolve_app_id(&server, effective.app);
    if (app_id < 0) {
        fprintf(stderr, "could not resolve app: %s\n", effective.app);
        return 4;
    }

    fprintf(stderr, "resolved app_id=%d\n", app_id);

    gs_error = NULL; /* some failure paths don't set it */
    int gs_rc = gs_start_app(&server, &streamConfig, app_id, true, false, 0);
    if (gs_rc != GS_OK) {
        fprintf(stderr,
                "gs_start_app failed for app_id=%d rc=%d(%s) err=%s\n",
                app_id, gs_rc, gs_rc_name(gs_rc), gs_error ? gs_error : "(null)");
        if (gs_rc == GS_NOT_SUPPORTED_MODE || gs_rc == GS_NOT_SUPPORTED_SOPS_RESOLUTION) {
            fprintf(stderr,
                    "[ml_worker] requested mode: %dx%d@%d\n",
                    effective.width, effective.height, effective.fps);
            dump_server_modes(&server);
            fprintf(stderr,
                    "[ml_worker] hint: try a refresh rate that exists in the mode list (often 60), "
                    "or a different resolution.\n");
        }
        return 5;
    }

    fprintf(stderr, "rtsp url: %s\n",
            server.serverInfo.rtspSessionUrl ? server.serverInfo.rtspSessionUrl : "(null)");

    volatile int fatal_code = WORKER_FATAL_NONE;

    WorkerRenderConfig render_cfg;
    memset(&render_cfg, 0, sizeof(render_cfg));
    snprintf(render_cfg.tcp_host, sizeof(render_cfg.tcp_host), "%s", effective.tcp_host);
    render_cfg.tcp_port = effective.tcp_port;
    render_cfg.stream_id = effective.stream_id;
    render_cfg.width = (uint32_t)effective.width;
    render_cfg.height = (uint32_t)effective.height;
    render_cfg.fps = (uint32_t)effective.fps;
    render_cfg.bitrate = (uint32_t)effective.bitrate;
    render_cfg.codec = (uint32_t)effective.codec;
    render_cfg.chroma = (uint32_t)effective.chroma;
    render_cfg.bitdepth = (uint32_t)effective.bitdepth;
    render_cfg.video_format = (uint32_t)supported_video_formats;
    render_cfg.color_space = (uint32_t)effective.ll_color_space;
    render_cfg.color_range = (uint32_t)effective.ll_color_range;
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
        if (fatal_code != WORKER_FATAL_NONE) {
            fprintf(stderr, "fatal_code=%d, stopping worker\n", fatal_code);
            delay_before_fatal_exit_if_needed(fatal_code);
            return 100 + fatal_code;
        }
        return 6;
    }

    ControlSocket control_socket;
    memset(&control_socket, 0, sizeof(control_socket));
    control_socket.fd = -1;

    int control_enabled = 0;
    if (effective.control_port > 0) {
        if (control_socket_open(&control_socket, effective.control_bind, (uint16_t)effective.control_port) != 0) {
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
            control_socket_process_all(&control_socket, effective.width, effective.height);
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
    if (exit_code == 100 + WORKER_FATAL_NEGOTIATION_MISMATCH) {
        delay_before_fatal_exit_if_needed(WORKER_FATAL_NEGOTIATION_MISMATCH);
    }

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

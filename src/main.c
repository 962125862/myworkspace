extern char* gs_error;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>

#include <Limelight.h>
#include "client.h"

#include "connection_callbacks.h"
#include "video_callbacks.h"

static volatile int g_running = 1;

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
        fprintf(stderr, "gs_applist failed\n");
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

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host> <app_index_or_name>\n", argv[0]);
        fprintf(stderr, "examples:\n");
        fprintf(stderr, "  %s 192.168.11.50 2\n", argv[0]);
        fprintf(stderr, "  %s 192.168.11.50 Desktop\n", argv[0]);
        fprintf(stderr, "  %s 192.168.11.50 \"Steam Big Picture\"\n", argv[0]);
        return 1;
    }

    const char* host = argv[1];
    const char* app_arg = argv[2];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    SERVER_DATA server;
    memset(&server, 0, sizeof(server));

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);

    // 第一版先保守配置
    streamConfig.width = 1280;
    streamConfig.height = 720;
    streamConfig.fps = 60;
    streamConfig.bitrate = 10000;
    streamConfig.packetSize = 1024;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    streamConfig.clientRefreshRateX100 = 6000;


    // 复用你已经 pair 成功的 Moonlight keys
    const char* key_dir = "/home/gejun/.cache/moonlight";

    if (gs_init(&server, (char*)host, 0, key_dir, 1, false) != 0) {
        fprintf(stderr, "gs_init failed: %s\n", gs_error ? gs_error : "(null)");
        return 1;
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
        return 1;
    }

    int app_id = resolve_app_id(&server, app_arg);
    if (app_id < 0) {
        fprintf(stderr, "could not resolve app: %s\n", app_arg);
        return 1;
    }

    fprintf(stderr, "resolved app_id=%d\n", app_id);

    if (gs_start_app(&server, &streamConfig, app_id, true, false, 0) != 0) {
        fprintf(stderr, "gs_start_app failed for app_id=%d: %s\n",
                app_id, gs_error ? gs_error : "(null)");
        return 1;
    }


    fprintf(stderr, "rtsp url: %s\n",
            server.serverInfo.rtspSessionUrl ? server.serverInfo.rtspSessionUrl : "(null)");

    int err = LiStartConnection(&server.serverInfo,
                                &streamConfig,
                                &connection_callbacks,
                                &video_callbacks,
                                NULL,
                                NULL,
                                0,
                                NULL,
                                0);
    if (err != 0) {
        fprintf(stderr, "LiStartConnection failed: %d\n", err);
        return 1;
    }

    fprintf(stderr, "streaming started, press Ctrl+C to stop\n");

    while (g_running) {
        sleep(1);
    }

    LiStopConnection();
    return 0;
}

#define _POSIX_C_SOURCE 200809L

#include "agent_protocol.h"
#include "auth.h"
#include "h264_tap.h"
#include "mlctl_cmd.h"
#include "tcp_util.h"
#include "tlv_protocol.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* usleep needs XSI */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

extern int udp_send_to(const char* ip, uint16_t port, const uint8_t* data, size_t len);

typedef struct {
    char in_host[64];
    uint16_t in_port;
    char video_bind[64];
    uint16_t video_port;

    char ctrl_bind[64];
    uint16_t ctrl_port;

    char worker_ctrl_ip[64];
    uint16_t worker_ctrl_port;

    char token[256];
} AgentCfg;

static volatile int g_running = 1;

typedef struct {
    int fd;
    char token[256];
    char worker_ctrl_ip[64];
    uint16_t worker_ctrl_port;
} CtrlClientArgs;

static int ctrl_read_optional_sub(int fd, uint16_t* out_stream_id) {
    if (!out_stream_id) return -1;

    /* Optional compatibility line after AUTH:
     *   SUB <stream_id>\n
     * Legacy clients immediately send binary frames, which start with a 4-byte
     * big-endian length where MSB should be 0x00 (n <= 4096). So if the next
     * byte is 'S', we treat it as the SUB line and consume it.
     */
    char pfx[4] = {0};
    ssize_t n = recv(fd, pfx, 4, MSG_PEEK);
    if (n <= 0) return 0;
    if (n < 4) return 0;
    if (pfx[0] != 'S' || pfx[1] != 'U' || pfx[2] != 'B') return 0;

    char line[256];
    size_t len = 0;
    while (len < sizeof(line) - 1) {
        char c = 0;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        line[len++] = c;
        if (c == '\n') break;
    }
    line[len] = '\0';

    int sid = 0;
    if (sscanf(line, "SUB %d", &sid) == 1) {
        if (sid < 1) sid = 1;
        if (sid > 65535) sid = 65535;
        *out_stream_id = (uint16_t)sid;
        return 1; /* consumed */
    }
    return -1;
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --in-host <ip>           (default 0.0.0.0)\n"
            "  --in-port <port>         (default 19000)\n"
            "  --video-bind <ip>        (default 0.0.0.0)\n"
            "  --video-port <port>      (default 31234)\n"
            "  --ctrl-bind <ip>         (default 0.0.0.0)\n"
            "  --ctrl-port <port>       (default 31235)\n"
            "  --worker-ctrl-ip <ip>    (default 127.0.0.1)\n"
            "  --worker-ctrl-port <p>   (default 30001)\n"
            "  --token <token>          (default empty: auth disabled)\n",
            prog);
}

static void cfg_init(AgentCfg* c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->in_host, sizeof(c->in_host), "0.0.0.0");
    c->in_port = 19000;
    snprintf(c->video_bind, sizeof(c->video_bind), "0.0.0.0");
    c->video_port = 31234;
    snprintf(c->ctrl_bind, sizeof(c->ctrl_bind), "0.0.0.0");
    c->ctrl_port = 31235;
    snprintf(c->worker_ctrl_ip, sizeof(c->worker_ctrl_ip), "127.0.0.1");
    c->worker_ctrl_port = 30001;
    c->token[0] = '\0';
}

static int parse_args(int argc, char** argv, AgentCfg* c) {
    static struct option opts[] = {
        {"in-host", required_argument, 0, 0},
        {"in-port", required_argument, 0, 0},
        {"video-bind", required_argument, 0, 0},
        {"video-port", required_argument, 0, 0},
        {"ctrl-bind", required_argument, 0, 0},
        {"ctrl-port", required_argument, 0, 0},
        {"worker-ctrl-ip", required_argument, 0, 0},
        {"worker-ctrl-port", required_argument, 0, 0},
        {"token", required_argument, 0, 0},
        {"help", no_argument, 0, 0},
        {0, 0, 0, 0},
    };

    int idx = 0;
    while (1) {
        int rc = getopt_long(argc, argv, "", opts, &idx);
        if (rc == -1) break;
        if (rc != 0) continue;
        const char* name = opts[idx].name;
        if (!strcmp(name, "help")) {
            return -1;
        } else if (!strcmp(name, "in-host")) {
            snprintf(c->in_host, sizeof(c->in_host), "%s", optarg);
        } else if (!strcmp(name, "in-port")) {
            c->in_port = (uint16_t)atoi(optarg);
        } else if (!strcmp(name, "video-bind")) {
            snprintf(c->video_bind, sizeof(c->video_bind), "%s", optarg);
        } else if (!strcmp(name, "video-port")) {
            c->video_port = (uint16_t)atoi(optarg);
        } else if (!strcmp(name, "ctrl-bind")) {
            snprintf(c->ctrl_bind, sizeof(c->ctrl_bind), "%s", optarg);
        } else if (!strcmp(name, "ctrl-port")) {
            c->ctrl_port = (uint16_t)atoi(optarg);
        } else if (!strcmp(name, "worker-ctrl-ip")) {
            snprintf(c->worker_ctrl_ip, sizeof(c->worker_ctrl_ip), "%s", optarg);
        } else if (!strcmp(name, "worker-ctrl-port")) {
            c->worker_ctrl_port = (uint16_t)atoi(optarg);
        } else if (!strcmp(name, "token")) {
            snprintf(c->token, sizeof(c->token), "%s", optarg);
        }
    }
    return 0;
}

static void* ctrl_client_thread(void* p) {
    CtrlClientArgs* a = (CtrlClientArgs*)p;
    int cfd = a->fd;

    uint16_t sid = 1;
    if (agent_auth_read_and_check(cfd, a->token, 3000) != 0) {
        close(cfd);
        free(a);
        return NULL;
    }
    /* Optional SUB line (for per-stream bookkeeping on the proxy side). */
    (void)ctrl_read_optional_sub(cfd, &sid);

    /* agent_auth_read_and_check() sets SO_RCVTIMEO for the AUTH line. Clear it so
     * idle ctrl connections don't get dropped by recv_exact() timeouts. */
    {
        struct timeval tv;
        memset(&tv, 0, sizeof(tv));
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    for (;;) {
        uint8_t lenb[4];
        if (recv_exact(cfd, lenb, 4) < 0) break;
        uint32_t n = be32_read(lenb);
        if (n == 0 || n > 4096) break;
        uint8_t buf[4096];
        if (recv_exact(cfd, buf, n) < 0) break;
        (void)udp_send_to(a->worker_ctrl_ip, a->worker_ctrl_port, buf, n);
    }

    close(cfd);
    free(a);
    return NULL;
}

static void* ctrl_tcp_thread(void* p) {
    AgentCfg* cfg = (AgentCfg*)p;
    int lfd = tcp_listen(cfg->ctrl_bind, cfg->ctrl_port, 32);
    if (lfd < 0) {
        fprintf(stderr, "[agent] ctrl listen failed %s:%u\n", cfg->ctrl_bind, (unsigned)cfg->ctrl_port);
        return NULL;
    }
    fprintf(stderr, "[agent] ctrl tcp listening on %s:%u\n", cfg->ctrl_bind, (unsigned)cfg->ctrl_port);

    while (g_running) {
        int cfd = tcp_accept(lfd);
        if (cfd < 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
            continue;
        }
        CtrlClientArgs* a = (CtrlClientArgs*)calloc(1, sizeof(*a));
        if (!a) {
            close(cfd);
            continue;
        }
        a->fd = cfd;
        snprintf(a->token, sizeof(a->token), "%s", cfg->token);
        snprintf(a->worker_ctrl_ip, sizeof(a->worker_ctrl_ip), "%s", cfg->worker_ctrl_ip);
        a->worker_ctrl_port = cfg->worker_ctrl_port;

        pthread_t th;
        if (pthread_create(&th, NULL, ctrl_client_thread, a) != 0) {
            close(cfd);
            free(a);
            continue;
        }
        pthread_detach(th);
    }
    close(lfd);
    return NULL;
}

static void* ingest_thread(void* p) {
    AgentCfg* cfg = (AgentCfg*)p;
    int lfd = tcp_listen(cfg->in_host, cfg->in_port, 32);
    if (lfd < 0) {
        fprintf(stderr, "[agent] ingest listen failed %s:%u\n", cfg->in_host, (unsigned)cfg->in_port);
        return NULL;
    }
    fprintf(stderr, "[agent] ingest listening on %s:%u\n", cfg->in_host, (unsigned)cfg->in_port);

    while (g_running) {
        int cfd = tcp_accept(lfd);
        if (cfd < 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
            continue;
        }
        fprintf(stderr, "[agent] ingest client connected\n");
        while (g_running) {
            uint8_t hdrb[TCP_HEADER_SIZE];
            if (recv_exact(cfd, hdrb, TCP_HEADER_SIZE) < 0) break;
            PacketHeader hdr;
            if (tlv_parse_header(hdrb, &hdr) != 0) break;
            uint32_t payload_len = hdr.length - TCP_HEADER_SIZE;
            if (payload_len > TCP_MAX_PACKET_SIZE) break;

            /* Keep ingest loop quiet by default. If you need debug logs, add your
             * own prints here or temporarily enable verbose mode.
             */

            uint8_t* payload = NULL;
            if (payload_len > 0) {
                payload = (uint8_t*)malloc(payload_len);
                if (!payload) break;
                if (recv_exact(cfd, payload, payload_len) < 0) {
                    free(payload);
                    payload = NULL;
                    break;
                }
            }

            (void)payload;

            if (hdr.type == TCP_MSG_TYPE_VIDEO_DATA && payload && payload_len > 0) {
                h264_tap_publish(hdr.stream_id, payload, (int)payload_len);
            }

            free(payload);
        }
        fprintf(stderr, "[agent] ingest client disconnected\n");
        close(cfd);
    }

    close(lfd);
    return NULL;
}

int main(int argc, char** argv) {
    AgentCfg cfg;
    cfg_init(&cfg);
    if (parse_args(argc, argv, &cfg) != 0) {
        usage(argv[0]);
        return 2;
    }

    if (h264_tap_start(cfg.video_bind, cfg.video_port) != 0) {
        fprintf(stderr, "[agent] h264 tap start failed\n");
        return 3;
    }

    /* token applies to both video and ctrl tcp */
    h264_tap_set_token(cfg.token);
    h264_tap_set_worker_ctrl(cfg.worker_ctrl_ip, cfg.worker_ctrl_port);

    pthread_t th_ingest;
    pthread_t th_ctrl;
    pthread_create(&th_ingest, NULL, ingest_thread, &cfg);
    pthread_create(&th_ctrl, NULL, ctrl_tcp_thread, &cfg);

    fprintf(stderr, "[agent] running. video=%s:%u ctrl=%s:%u worker_ctrl=%s:%u\n",
            cfg.video_bind, (unsigned)cfg.video_port,
            cfg.ctrl_bind, (unsigned)cfg.ctrl_port,
            cfg.worker_ctrl_ip, (unsigned)cfg.worker_ctrl_port);

    while (1) {
        sleep(1);
    }

    g_running = 0;
    pthread_join(th_ingest, NULL);
    pthread_join(th_ctrl, NULL);
    h264_tap_stop();
    return 0;
}

/**
 * @file stream_receiver_decode.c
 * @brief 接收TCP流并实时解码测试
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "decoder.h"
#include "protocol.h"

static volatile int g_running = 1;

static void signal_handler(int sig) {
    printf("\n[Receiver] Received signal %d, stopping...\n", sig);
    g_running = 0;
}

typedef struct {
    int sock_fd;
    FILE* dump_file;  // 可选保存原始流
    
    DecoderCtx* decoder;
    int width, height;
    int frame_count;
    time_t start_time;
    
    uint8_t* recv_buffer;
    size_t buffer_size;
} StreamReceiver;

static StreamReceiver* receiver_create(int width, int height, const char* dump_filename) {
    StreamReceiver* rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;
    
    rec->width = width;
    rec->height = height;
    rec->buffer_size = 2 * 1024 * 1024;  // 2MB
    rec->recv_buffer = malloc(rec->buffer_size);
    if (!rec->recv_buffer) {
        free(rec);
        return NULL;
    }
    
    if (dump_filename) {
        rec->dump_file = fopen(dump_filename, "wb");
        if (!rec->dump_file) {
            perror("fopen dump file");
            free(rec->recv_buffer);
            free(rec);
            return NULL;
        }
    }
    
    // 创建解码器
    DecoderConfig config = {
        .backend = DECODE_BACKEND_NVIDIA,
        .width = width,
        .height = height,
        .output_format = DECODE_FMT_NV12,
        .thread_count = 1,
        .cuda_device_id = 0
    };
    
    rec->decoder = decoder_create(&config);
    if (!rec->decoder) {
        fprintf(stderr, "[Receiver] Failed to create decoder\n");
        if (rec->dump_file) fclose(rec->dump_file);
        free(rec->recv_buffer);
        free(rec);
        return NULL;
    }
    
    if (decoder_init(rec->decoder, NULL, 0) < 0) {
        fprintf(stderr, "[Receiver] Failed to init decoder\n");
        decoder_destroy(rec->decoder);
        if (rec->dump_file) fclose(rec->dump_file);
        free(rec->recv_buffer);
        free(rec);
        return NULL;
    }
    
    printf("[Receiver] Created (NVIDIA NVDEC %dx%d)\n", width, height);
    return rec;
}

static void receiver_destroy(StreamReceiver* rec) {
    if (!rec) return;
    
    if (rec->decoder) decoder_destroy(rec->decoder);
    if (rec->dump_file) fclose(rec->dump_file);
    if (rec->recv_buffer) free(rec->recv_buffer);
    if (rec->sock_fd > 0) close(rec->sock_fd);
    free(rec);
}

static int receiver_run(StreamReceiver* rec, int listen_port, int duration_sec) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    rec->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rec->sock_fd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(rec->sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);
    
    if (bind(rec->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    
    if (listen(rec->sock_fd, 1) < 0) {
        perror("listen");
        return -1;
    }
    
    printf("[Receiver] Listening on port %d...\n", listen_port);
    
    int client_fd = accept(rec->sock_fd, (struct sockaddr*)&addr, &addr_len);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    printf("[Receiver] Client connected from %s:%d\n", client_ip, ntohs(addr.sin_port));
    
    rec->start_time = time(NULL);
    time_t end_time = duration_sec > 0 ? rec->start_time + duration_sec : 0;
    
    /* 使用当前 TLV 协议接收并解码帧 */
    int stream_started = 0;

    while (g_running && (duration_sec == 0 || time(NULL) < end_time)) {
        /* 读取 7 字节协议头 */
        uint8_t hdr_buf[TCP_HEADER_SIZE];
        ssize_t n = recv(client_fd, hdr_buf, TCP_HEADER_SIZE, MSG_WAITALL);
        if (n != TCP_HEADER_SIZE) {
            if (n == 0) {
                printf("[Receiver] Client disconnected\n");
            } else if (n > 0) {
                fprintf(stderr, "[Receiver] Incomplete header: got %zd/%d\n", n, TCP_HEADER_SIZE);
            } else {
                perror("recv header");
            }
            break;
        }

        PacketHeader pkt_hdr;
        if (protocol_parse_header(hdr_buf, &pkt_hdr) < 0) {
            fprintf(stderr, "[Receiver] Invalid packet header\n");
            break;
        }

        /* 读取 payload */
        uint32_t payload_len = pkt_hdr.length - TCP_HEADER_SIZE;
        if (payload_len > rec->buffer_size) {
            fprintf(stderr, "[Receiver] Payload too large: %u bytes\n", payload_len);
            break;
        }

        if (payload_len > 0) {
            n = recv(client_fd, rec->recv_buffer, payload_len, MSG_WAITALL);
            if (n != (ssize_t)payload_len) {
                fprintf(stderr, "[Receiver] Incomplete payload: got %zd/%u\n", n, payload_len);
                break;
            }
        }

        /* 处理不同消息类型 */
        if (pkt_hdr.type == TCP_MSG_TYPE_STREAM_START) {
            if (payload_len >= sizeof(StreamInfo)) {
                StreamInfo info;
                protocol_parse_stream_info(rec->recv_buffer, &info);
                printf("[Receiver] Stream %u started: %ux%u@%u, %u kbps\n",
                       pkt_hdr.stream_id, info.width, info.height, info.fps, info.bitrate);
                stream_started = 1;
            }
            continue;
        }

        if (pkt_hdr.type == TCP_MSG_TYPE_STREAM_STOP) {
            printf("[Receiver] Stream %u stopped\n", pkt_hdr.stream_id);
            break;
        }

        if (pkt_hdr.type == TCP_MSG_TYPE_HEARTBEAT) {
            continue;
        }

        /* VIDEO_DATA */
        if (pkt_hdr.type != TCP_MSG_TYPE_VIDEO_DATA || payload_len == 0) {
            continue;
        }

        if (!stream_started) {
            /* 即使没收到 STREAM_START 也尝试解码 */
            stream_started = 1;
        }

        /* 保存到文件（可选） */
        if (rec->dump_file) {
            fwrite(rec->recv_buffer, 1, payload_len, rec->dump_file);
        }

        /* 解码 */
        DecodedFrame* decoded = NULL;
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        int ret = decoder_decode(rec->decoder, rec->recv_buffer, (int)payload_len, &decoded);

        clock_gettime(CLOCK_MONOTONIC, &end);
        double decode_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_nsec - start.tv_nsec) / 1000000.0;

        if (ret == 0 && decoded) {
            rec->frame_count++;
            if (rec->frame_count <= 5 || rec->frame_count % 60 == 0) {
                printf("[Receiver] Frame %d: %dx%d, %.2f ms\n",
                       rec->frame_count, decoded->width, decoded->height, decode_time);
            }
            decoder_free_frame(decoded);
        } else if (ret < 0) {
            static int err_count = 0;
            if (err_count++ < 5) {
                fprintf(stderr, "[Receiver] Decode failed\n");
            }
        }

        /* 每 60 帧打印一次统计 */
        if (rec->frame_count > 0 && rec->frame_count % 60 == 0) {
            double elapsed = difftime(time(NULL), rec->start_time);
            if (elapsed > 0) {
                double fps = rec->frame_count / elapsed;
                printf("[Receiver] Stats: %d frames, %.1f FPS, avg %.2f ms/frame\n",
                       rec->frame_count, fps, decode_time);
            }
        }
    }
    
    close(client_fd);
    
    double total_time = difftime(time(NULL), rec->start_time);
    printf("\n[Receiver] Summary:\n");
    printf("  Duration: %.1f seconds\n", total_time);
    printf("  Frames decoded: %d\n", rec->frame_count);
    if (total_time > 0) {
        printf("  Average FPS: %.1f\n", rec->frame_count / total_time);
    }
    
    DecoderStats stats;
    decoder_get_stats(rec->decoder, &stats);
    printf("  Avg decode time: %.2f ms\n", stats.avg_decode_time_ms);
    
    return 0;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int port = 19000;
    int duration = 30;  // seconds
    int width = 1280, height = 720;
    char* dump_file = NULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "p:d:w:h:o:")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'd': duration = atoi(optarg); break;
            case 'w': width = atoi(optarg); break;
            case 'h': height = atoi(optarg); break;
            case 'o': dump_file = optarg; break;
            default:
                fprintf(stderr, "Usage: %s [-p port] [-d duration] [-w width] [-h height] [-o dump_file]\n", argv[0]);
                return 1;
        }
    }
    
    printf("========================================\n");
    printf("  Stream Receiver + NVIDIA Decoder\n");
    printf("========================================\n");
    printf("Port: %d\n", port);
    printf("Duration: %d seconds\n", duration);
    printf("Resolution: %dx%d\n", width, height);
    if (dump_file) printf("Dump to: %s\n", dump_file);
    printf("\n");
    
    StreamReceiver* receiver = receiver_create(width, height, dump_file);
    if (!receiver) {
        return 1;
    }
    
    int ret = receiver_run(receiver, port, duration);
    receiver_destroy(receiver);
    
    return ret;
}

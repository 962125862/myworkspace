/**
 * @file h264_tap.h
 * @brief 将收到的 H.264 NAL(bytestream/AnnexB) 通过 TCP 旁路输出给外部客户端
 *
 * 目标场景：
 *   - 远端(Java/Python)客户端用 FFmpeg/ffplay 展示实时画面
 *   - 不转码、不解码：直接转发 VIDEO_DATA 的 payload
 *   - 允许连接初期黑屏几秒（等待 IDR/SPS/PPS），但连接后尽量稳定持续
 *
 * 启用方式：
 *   - 环境变量 H264_TAP_PORT>0 时启用
 *   - 可选 H264_TAP_BIND (默认 127.0.0.1)
 *
 * 实时策略：
 *   - 为避免 TCP 堆积导致延迟无限增长，本模块采用“丢弃旧数据追最新”的策略。
 *   - 每个客户端仅保留一个待发送缓冲(out_buf，近似 1 帧/AU)。
 *   - 若发送阻塞超过阈值，则丢弃旧缓冲并进入 need_idr 状态：丢弃直到下一个 IDR 再恢复。
 *
 * 可配置环境变量：
 *   - H264_TAP_STALL_MS  : 发送阻塞阈值，默认 200ms
 *   - H264_TAP_DROP_IDR  : 是否丢弃到 IDR 恢复，默认 1
 *
 * 协议：
 *   - TCP 连接建立后，客户端可在 3 秒内发送一行："SUB <stream_id>\n" 或直接发送 "<stream_id>\n"
 *   - 未发送则默认订阅 stream_id=1
 *   - 服务端持续发送 AnnexB bytestream（每个 NAL 原样输出；若无 start code，会自动补 0x00000001）
 */

#ifndef H264_TAP_H
#define H264_TAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int h264_tap_start(const char* bind_ip, uint16_t port);
void h264_tap_stop(void);

/**
 * @brief 发布一个 NAL 单元到 tap
 * @note 该函数应尽量轻量；内部为每个订阅客户端做 memcpy（用于调试/桥接用途）
 */
void h264_tap_publish(uint16_t stream_id, const uint8_t* data, int size);

/* Optional: configure upstream ml_worker UDP control endpoint.
 * If set, stream_server will request an IDR when a new tap subscriber connects.
 */
void h264_tap_set_worker_ctrl(const char* ip, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* H264_TAP_H */

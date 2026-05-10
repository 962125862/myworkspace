/**
 * @file zmq_bridge.h
 * @brief 内置 ZMQ bridge：从 stream_server 内部的 last_frame 直接对外提供 BGR24 帧。
 *
 * 协议：
 *   Client (DEALER / REQ) -> ROUTER multipart
 *     DEALER: [cmd][json]
 *     REQ:    [cmd][json]   (ROUTER 侧看到: [identity][""][cmd][json])
 *
 *   cmd:
 *     - "GET_LATEST_BGR" (推荐)
 *     - "PING"
 *
 *   json:
 *     {"stream_id": 1, "timeout_ms": 1000}
 *     {"stream_id": 1, "timeout_ms": 1000, "roi": {"x": 100, "y": 80, "w": 640, "h": 360}}
 *   说明：收到请求时，会从对应流的 last_frame 按需生成 BGR24 并返回。
 *        如果 last_frame 还是硬件帧，则先下载到 CPU，再走统一像素格式适配层。
 *        对同一帧的重复请求，bridge 内部会复用最近一次生成的 BGR24 结果。
 *        roi 是可选输出裁剪参数，坐标基于源帧左上角；省 IPC payload，不改变解码路径。
 *        旧客户端传入的 request_new 会被忽略。
 *
 *   Reply multipart (client 看到):
 *     [status][meta_json][bgr24]
 *
 *   meta_json 至少包含 width/height/source_width/source_height/roi_x/roi_y/
 *   roi_width/roi_height/pts/key_frame/mono_ns/stream_id/pixfmt/stride。
 */

#ifndef ZMQ_BRIDGE_H
#define ZMQ_BRIDGE_H

#include "stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动内置 ZMQ bridge（后台线程）。
 *
 * @param mgr          StreamManager
 * @param bind_addr    主 ZMQ bind 地址（如 "tcp://0.0.0.0:5566"），可为 NULL/空串
 * @param ipc_bind_addr 可选第二个 ZMQ bind 地址（通常为 "ipc:///tmp/stream_server_bgr.sock"），可为 NULL/空串
 * @param running_flag 指向主进程运行标志（为 0 时线程退出）；可为 NULL（则永不退出）
 *
 * @return 0 成功启动；-1 不支持/失败（例如未编译 libzmq 支持）
 */
int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, const char* ipc_bind_addr,
                     volatile int* running_flag);

/**
 * @brief 保留的 no-op 钩子。
 * @note  当前实现改为“请求时从 last_frame 按需生成 BGR24”，不在该钩子里缓存帧。
 */
void zmq_bridge_on_new_frame(uint16_t stream_id, const DecodedFrame* frame);

/**
 * @brief 释放内部缓存（通常进程退出时无需显式调用；用于测试/重载场景）。
 */
void zmq_bridge_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMQ_BRIDGE_H */

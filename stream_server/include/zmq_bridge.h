/**
 * @file zmq_bridge.h
 * @brief 内置 ZMQ bridge：从 stream_server 内部的 last_frame 直接对外提供 NV12 帧。
 *
 * 目标：替代 python_dir/shm_zmq_bridge.py 的“对外接口”部分，避免额外进程与 SHM 二次拷贝。
 *
 * 协议（与 Python 版保持一致，便于复用现有 client）：
 *   Client (DEALER / REQ) -> ROUTER multipart
 *     DEALER: [cmd][json]
 *     REQ:    [cmd][json]   (ROUTER 侧看到: [identity][""][cmd][json])
 *
 *   cmd:
 *     - "GET_LATEST_NV12" (推荐)
 *     - "GET_SHM_NV12"    (兼容；在内置版里等价于 GET_LATEST_NV12)
 *     - "PING"
 *
 *   json:
 *     {"stream_id": 1, "timeout_ms": 1000, "request_new": true}
 *   说明：内置版不实现 request_new 的 shm request_seq 语义；始终返回当前 latest。
 *
 *   Reply multipart (client 看到):
 *     [status][meta_json][y_plane][uv_plane]
 *
 *   meta_json 至少包含 width/height/pts/key_frame/mono_ns/stream_id。
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
 * @param bind_addr    ZMQ bind 地址（如 "tcp://0.0.0.0:5566"）
 * @param running_flag 指向主进程运行标志（为 0 时线程退出）；可为 NULL（则永不退出）
 *
 * @return 0 成功启动；-1 不支持/失败（例如未编译 libzmq 支持）
 */
int zmq_bridge_start(StreamManager* mgr, const char* bind_addr, volatile int* running_flag);

/**
 * @brief 在解码线程得到新帧后调用，用于更新 ZMQ “最新帧缓存”。
 *
 * 说明：
 * - 该缓存面向 GET_LATEST_NV12：把 frame 打包成“紧凑 NV12”（无 padding）并缓存。
 * - 开启内置 ZMQ bridge 后，此路径可以显著降低高请求频率下的 CPU（避免每请求都做大拷贝）。
 * - 若内置 ZMQ bridge 未启用/未编译 libzmq，则该函数是低成本 no-op。
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

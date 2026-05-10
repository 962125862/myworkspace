# 当前架构设计

本文是当前仓库的有效架构说明，只保留仍在使用的模块和交互关系。

## 1. 模块

### 1.1 Sunshine Host

外部组件。

负责：

- 提供 GameStream / Sunshine 串流服务
- 输出编码视频
- 接收控制输入

### 1.2 ml_worker

仓库根目录 `src/` 下的主程序。

负责：

- 连接 Sunshine
- 拉取编码视频
- 通过 TCP TLV 推给下游
- 接收本地 UDP 控制命令
- 把 `REQ_IDR` 转成 `LiRequestIdrFrame()`

### 1.3 stream_server

`stream_server/` 子项目。

负责：

- 接收多路 TCP TLV 视频流
- 解析 `STREAM_START`
- 初始化解码器
- 按路由规则选择 `Intel / NVIDIA / CPU`
- 维护每路 `last_frame`
- 硬解默认保留 `last_frame` 的硬件帧引用，不立即下载到 CPU
- 通过内置 ZMQ bridge 输出 `BGR24`
- 当前 bridge 为单个 `ROUTER` 线程，请求路径串行执行 `hwdownload + convert + reply`

### 1.4 strem_agent_server

远程代理服务。

负责：

- 接收 `ml_worker` 的 TLV 视频流
- 对外输出 AnnexB H264
- 对外接收控制输入
- 转发到 `ml_worker` 的 UDP 控制口

当前兼容性结论：

- 和 `ml_worker` 的基础 `TCP TLV + 控制转发` 仍然兼容
- 但它面向的是 `H264 AnnexB` 代理链路
- 不是当前 `HEVC444 -> stream_server -> ZMQ BGR` 主链路的等价替代
- 目前没有完整适配当前新增的：
  - `40B STREAM_START`
  - `codec/chroma/bitdepth/video_format/color_space/color_range`
  - `HEVC444` 代理/客户端解码链

### 1.5 strem_agent_client

远程代理客户端。

负责：

- 连接 `strem_agent_server`
- 收视频
- 发键鼠控制

## 2. 模块互通关系

### 2.1 主链路：ml_worker -> stream_server

```text
Sunshine Host
  -> ml_worker
  -> TCP TLV
  -> stream_server
  -> decode
  -> ZMQ BGR
  -> client
```

### 2.2 代理链路：ml_worker -> strem_agent_server -> strem_agent_client

```text
Sunshine Host
  -> ml_worker
  -> TCP TLV
  -> strem_agent_server
  -> video tcp
  -> strem_agent_client

strem_agent_client
  -> ctrl tcp
  -> strem_agent_server
  -> udp mlctl
  -> ml_worker
  -> Sunshine Host
```

注意：

- 当前 `strem_agent_client` 代码和文档都仍以 `H264` 解码为主
- 所以这条代理链当前更适合 `H264/420`
- 不建议直接拿它承接当前 `HEVC444` 场景

## 3. 数据流

### 3.1 视频数据

- Sunshine 输出编码视频
- `ml_worker` 不解码，只做回调收包和转发
- `stream_server` / `strem_agent_server` 负责接收

### 3.2 控制数据

- 控制协议统一使用 `MlControlCmd`
- `REQ_IDR` 命令值是 `10`
- `ml_worker` 收到后触发 `LiRequestIdrFrame()`

## 4. 解码架构

### 4.1 路由规则

当前固定规则：

- `DECODE_BACKEND=auto` 时按流元信息走路由表
- 当前主机上 `HEVC444 -> Intel`
- 其他流也优先 `Intel`
- 如果显式指定 `intel/nvidia/cpu`，则直接跳过路由表

### 4.2 回退规则

- `Intel -> NVIDIA -> CPU`
- `NVIDIA -> CPU`

### 4.3 颜色元数据

颜色元数据从 `ml_worker` 传到 `stream_server`：

- `color_space`
- `color_range`

`stream_server` 再据此选择 libyuv 矩阵，把 `last_frame` 转成 `BGR24`。

补充说明：

- 如果 `last_frame` 当前是 NVDEC/VAAPI 硬件帧，先按需下载到 CPU
- 之后统一走 `decoder_convert_format_with_info(..., DECODE_FMT_BGR24)`
- 已知快路径：
  - `NV12/YUV420P/YUV444P -> BGR24` 走 `libyuv`
  - `VUYX -> BGR24` 走专门快路径；x86 主机上带运行时 SIMD 分发
- 兜底路径：
  - 其它当前支持但没有专门快路径的 CPU 格式，回退到 `FFmpeg swscale`
- `libyuv` 和 `swscale` 都使用 `STREAM_START` 透传下来的 `color_space/color_range`

## 5. ZMQ 架构

当前使用的是 `stream_server` 内置 bridge，不再经过外部 `shm` 进程。

特点：

- 请求式获取，不主动推流
- 从 `last_frame` 按需生成 `BGR24`
- 默认不会在每帧 decode 后立刻做 `GPU -> CPU` 下载
- 多个客户端统一走同一个 ZMQ 入口
- 当前 bridge 线程模型仍是“单个 `ROUTER` 线程 + 每帧 BGR cache”

协议：

- request: `GET_LATEST_BGR`
- json:
  - 全帧：`{"stream_id":1,"timeout_ms":1000}`
  - ROI：`{"stream_id":1,"timeout_ms":1000,"roi":{"x":100,"y":80,"w":640,"h":360}}`
- reply: `[status][meta_json][bgr24]`
- `roi` 是输出裁剪，不进入解码层；服务端仍从 `last_frame` 生成/复用整帧 `BGR24`，再返回 ROI payload
- `request_new` 是旧字段，当前服务端忽略，新客户端不再发送

### 5.1 相关启动环境变量

- `STREAM_MAX_STREAMS`
  - 默认 `20`
  - 用于控制运行时实际启用多少路流槽位
  - 适合 `systemd` 场景通过 `Environment=` 覆盖
- `STREAM_DEFER_HW_DOWNLOAD`
  - 默认 `on`
  - 控制硬解时 `last_frame` 是立即下载成 CPU 帧，还是保留为硬件帧引用
- `STREAM_NVDEC_EXTRA_HW_FRAMES`
  - 默认 `24`（延迟下载开启时）
  - 控制 NVDEC/VAAPI 额外硬件帧池余量，避免保留 `last_frame` 时 surface 不够

### 5.2 路数扩容策略

- 编译期硬上限已预留到 `256`
- 运行时实际路数优先通过 `--max-streams <N>` 或 `STREAM_MAX_STREAMS=<N>` 调整
- 不需要再为了 `20 -> 35 -> 48` 这种扩容反复改代码重编
- `-c/--connections` 只控制 TCP 连接数，建议略高于 `max-streams`，给重连抖动留余量

### 5.3 当前主机上的实际格式路径

- `192.168.11.31` 的 Intel 栈当前是 `i915 + Intel iHD 24.1.0`
- 当前主机上 `HEVC444 + Intel` 下载到 CPU 后常见落地格式是 `VUYX`
- 当前主机上 `HEVC444 + NVIDIA` 下载到 CPU 后常见落地格式是 `YUV444P`
- 这也是为什么 `Intel` 和 `NVIDIA` 在 `GET_LATEST_BGR` 的 `Xfer + Convert` 成本上差异明显

### 5.4 当前性能边界

- 在 `STREAM_DEFER_HW_DOWNLOAD=on` 下，`GET_LATEST_BGR` 的吞吐上限主要受 `Xfer + Convert` 影响
- `192.168.11.31` 的 20 路真实流、`30 fps/路` IPC BGR benchmark 里：
  - `Intel` 约 `17.17 fps/路`
  - `NVIDIA` 约 `29.78 fps/路`
- 这部分结论以 `deploy/BENCHMARK_stream_server_2026-04-13.md` 为准

## 6. 自动恢复

当前主链路已支持：

- `Decoded == 0` 时自动请求上游 `IDR`

目标是解决：

- 晚加入
- HEVC 参数集缺失
- 启动后第一帧长时间出不来

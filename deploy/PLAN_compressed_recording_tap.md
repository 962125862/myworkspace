# Plan: 20 路压缩流旁路录像

日期：2026-06-12

状态：历史设计记录。当前不采用“31 本地缓存 + 同步到 35 + 本地兜底清理”的主方案；当前有效方案以 `deploy/COMPRESSED_RECORDING_CURRENT.md` 为准：31 直接写 35 NFS，NFS 不可用时允许丢录像。

目标：在不影响 `192.168.11.31` 当前挂机取帧链路的前提下，把 20 路 `ml_worker` 推来的原始压缩视频流旁路保存，并支持同步到远端机器。

## 1. 当前结论

当前主链路是：

```text
Sunshine
  -> ml_worker
  -> TCP TLV / VIDEO_DATA
  -> stream_server:9000
  -> decode
  -> last_frame
  -> ZMQ GET_LATEST_BGR
  -> com.jun 业务取图
```

`stream_server` 现在不落盘，只保存每路 `last_frame`。业务取图时才把硬件帧下载到 CPU，再转成 `BGR24`。

如果要低成本录像，优先保存 `VIDEO_DATA` 里的原始 HEVC 压缩数据，不走 BGR、不 resize、不重新编码。

## 2. 实际码率估算

从 2026-06-12 查看 20 个主 worker 的 Docker log：

- 最近 30 分钟合计平均约 `74 Mbps`
- 最近 5 分钟合计平均约 `63 Mbps`
- 单路常见约 `3.5 - 4.0 Mbps`
- 单路峰值日志里见到过 `5 - 6.5 Mbps`

按 `74 Mbps` 估算：

```text
20 路合计：约 9.25 MB/s
1 小时：约 33 GB
1 天：约 800 GB
```

本地环形缓存建议：

```text
1 小时：约 33 GB
2 小时：约 66 GB
4 小时：约 133 GB
```

千兆局域网同步压力不大，平均带宽约 `60 - 80 Mbps`，远低于千兆。

## 3. 推荐架构

推荐在 `stream_server` 上做压缩流旁路，不改 20 个 worker 的主推流目标。

```text
ml_worker
  -> 31 stream_server
       -> 原逻辑：decode -> ZMQ BGR -> 业务
       -> 新逻辑：compressed recorder/tap -> 本地文件或远端 recorder
```

旁路必须满足：

- 异步处理
- 远端断线、磁盘慢、网络慢时不能阻塞 `stream_decode_video`
- 录像失败只影响录像，不影响挂机取帧
- 按 `stream_id` 分流
- 能记录 `STREAM_START` 元信息：宽高、fps、codec、chroma、bitdepth、colorspace、range

## 4. 分阶段实现

### 阶段 1：本地原始流落盘验证

在 `stream_server` 收到 `TCP_MSG_TYPE_VIDEO_DATA` 后、调用 `stream_decode_video` 前旁路复制 payload。

先做最小版本：

```text
VIDEO_DATA payload
  -> recorder queue
  -> 按 stream_id 写 raw .hevc
```

文件布局建议：

```text
/data/stream_records/raw/s01/20260612_100000.hevc
/data/stream_records/raw/s02/20260612_100000.hevc
...
```

切片策略：

- 每路每 10 分钟一个文件
- 本地保留 2 小时起步
- 超过保留时间自动删除

验收：

- 20 路业务取帧正常
- `stream_server` CPU/GPU 没明显抬升
- 文件持续增长
- `ffprobe` 或 `ffplay` 能打开至少部分切片
- 本地磁盘清理策略不会失控

注意：raw `.hevc` 是否好拖动/准确回放取决于 VPS/SPS/PPS/IDR 是否完整，需要实测。

### 阶段 2：封装为 mkv，仍然不重编码

如果 raw `.hevc` 回放体验不好，改成写入容器：

```text
HEVC packet -> muxer -> .mkv
```

关键要求：

```text
-c copy
不重新编码
```

推荐输出：

```text
/data/stream_records/mkv/s01/20260612_100000.mkv
```

验收：

- VLC/ffplay 可播放
- 按 10 分钟切片
- 切片边界尽量从关键帧附近开始
- 文件时间范围可追踪

### 阶段 3：远端 recorder

把本地落盘改成或扩展成远端接收：

```text
31 stream_server compressed_tap
  -> TCP/ZMQ/自定义 TLV
  -> 远端 recorder
  -> 远端落盘
```

建议协议不要直接沿用旧 `h264_tap` 的纯 bytestream，要带上元信息：

```text
STREAM_START
VIDEO_DATA
STREAM_STOP
stream_id
monotonic timestamp / wall clock timestamp
payload length
```

远端 recorder 负责：

- 按 `stream_id` 建目录
- 按 10 分钟切片
- 写 DB 索引
- 清理过期文件
- 记录断流/重连状态

DB 只存索引，不存视频二进制：

```text
stream_id
start_time
end_time
file_path
codec
width
height
fps
bitrate_estimate
uploaded/synced status
```

### 阶段 4：本地缓存 + 远端同步

更稳的生产形态：

```text
31 本地保留最近 1-2 小时
远端保留 1-7 天
```

同步策略：

- 只同步已经关闭的切片
- 不同步正在写的文件
- 成功后标记 synced
- 本地清理只删 synced 且超过保留时间的文件
- 远端不可用时，本地最多积压到保留上限，然后丢旧录像

## 5. 旧项目/旧口子问题清单

### 5.1 `h264_tap` 不适合直接用于当前 20 路 HEVC 录像

现有位置：

```text
stream_server/src/h264_tap.c
stream_server/include/h264_tap.h
```

问题：

- 名字和注释都是 H264 场景
- 当前主 20 路是 `HEVC444`
- `TAP_MAX_CLIENTS=8`，不够 20 路
- 只输出 `VIDEO_DATA` payload，不输出完整 `STREAM_START` 元信息
- IDR/SPS/PPS 识别按 H264 NAL type 写，对 HEVC 不正确
- 连接端按 `SUB <stream_id>` 单路订阅，更像调试预览，不像录像汇聚
- 当前 31 的 `stream_server` 启动命令没有启用 `--h264-tap-port`

处理建议：

- 不直接复用为生产录像
- 可以借鉴非阻塞发送、阻塞丢帧、防止主链路卡住的思路
- 新建 `compressed_tap` 或 `recording_tap`，明确支持 HEVC/H264 和 20 路

### 5.2 `strem_agent_server` 是旧 H264 代理链，不是当前主链路录像方案

当前 31 上有进程：

```text
/app/strem_agent_server --in-port 19000 --video-port 31234 --ctrl-port 31235
```

问题：

- 文档和代码定位是 H264 远程代理
- 不是当前 `HEVC444 -> stream_server -> ZMQ BGR` 主链路的等价组件
- 更适合单路/预览/控制代理，不适合直接作为 20 路 HEVC 录像中心
- 容易和 `stream_server` 内置 `h264_tap` 概念混淆

处理建议：

- 保留旧代理用途
- 不把它作为本次 20 路录像的核心依赖
- 文档里标注当前主链路与旧代理链路的边界

### 5.3 `GET_LATEST_BGR` 不适合做长期录像

当前业务取图走：

```text
ipc:///tmp/stream_server_bgr.sock
GET_LATEST_BGR
```

问题：

- 它是按需取最新帧，不是历史视频流
- 会触发硬件帧下载到 CPU
- 会做 YUV/VUYX -> BGR24 转换
- 全帧 BGR 数据量大：`1024*768*3 ~= 2.36 MB/帧`
- 20 路 5fps 全帧 BGR 约 `236 MB/s` 本机 IPC 数据搬运
- 再做 JPEG/MJPEG/H264 会产生额外编码成本

处理建议：

- 业务取图继续用 BGR
- 录像不要走 BGR
- 录像直接保存压缩 `VIDEO_DATA`

### 5.4 ZMQ ROI 只能减少返回 payload，不能降低解码/转换成本

当前 ROI 是：

```text
先生成整帧 BGR
再裁剪 ROI 返回
```

问题：

- ROI 能省 IPC payload
- 不能省完整解码
- 不能省整帧 BGR 转换
- 不能做服务端 resize

处理建议：

- ROI 继续用于业务裁图
- 不作为录像降成本方案
- 如果未来要低分辨率预览录像，再考虑新增服务端 `GET_LATEST_JPEG` 或 `GET_LATEST_RESIZED`

### 5.5 当前文档/命名有历史包袱

问题：

- `h264_tap` 命名和当前 HEVC 主链路不一致
- `strem_agent` 拼写和用途都容易误解
- 部分文档还围绕 H264/代理链，和当前 20 路 HEVC 主链路不是一回事
- `protocol.h` 注释里仍有 H264 表述，但实际 `STREAM_START` 已经带 codec/chroma/bitdepth

处理建议：

- 新功能命名避开 `h264_tap`
- 新文档明确 `compressed recording tap`
- 不急着删除旧代码，先隔离边界
- 后续如果确认旧代理不用，再单独清理

### 5.6 当前 REQ_IDR 恢复逻辑缺少“中途停滞”检测

当前 `stream_server` 已有 `REQ_IDR` 能力：

```text
stream_server
  -> UDP ML_CTRL_CMD_REQ_IDR
  -> ml_worker
  -> LiRequestIdrFrame()
```

现有自动触发主要覆盖：

```text
STREAM_START 时请求一次
frames_received >= 30 且 frames_decoded == 0 时限频请求
```

问题：

- 它能处理“刚开始一直解不出第一帧”
- 但不完整覆盖“已经成功解码过，后来中途 decoder 停滞”
- 如果后续出现 `frames_received` 持续增长，但 `frames_decoded` 长时间不增长，当前逻辑不一定主动请求 IDR
- 业务取图可能继续拿到旧 `last_frame`，表现为画面卡住，而不是立即报错

处理建议：

```text
每路记录 last_frames_received、last_frames_decoded、last_decoded_change_time

如果：
  stream active
  decoder initialized
  frames_received 持续增长
  frames_decoded 超过 N 秒不增长

则：
  对该 stream_id 发送 REQ_IDR
  按 2s/5s/10s 退避限频
  记录 reason=decode_stalled
```

这个 watchdog 正常情况下不触发，只做异常恢复兜底。

## 6. 实现要点

### 6.1 不能阻塞主解码线程

`handle_packet` 收到 `VIDEO_DATA` 后可以快速复制 payload 到队列，但不能在主线程里：

- 写慢磁盘
- 等远端 ACK
- 调 ffmpeg 阻塞
- 做网络重连

建议：

```text
handle_packet
  -> recorder_enqueue(stream_id, payload, len, metadata)
  -> stream_decode_video(...)

recorder worker thread
  -> write/send/mux
```

队列满时：

```text
丢录像数据
记录 dropped counter
不阻塞业务
```

### 6.2 切片和关键帧

直接按时间切 raw HEVC 可能出现切片开头不能立刻解码。

优先级：

1. 先补正确的 HEVC NAL 识别，避免把 HEVC 误判成“没有关键帧”
2. 第一版先按时间切，验证可用性
3. 如果回放开头黑屏，记录最近 VPS/SPS/PPS，切片开头补参数集
4. 再进一步按 IDR/IRAP 切片

HEVC NAL type 与 H264 不同，不能复用旧 H264 的 `nal_type = byte & 0x1f` 判断。

当前老代码里至少有两处关键帧/参数集识别是 H264 口径：

```text
src/video_callbacks.c
stream_server/src/decoder.c
stream_server/src/h264_tap.c
```

这些逻辑里 `nal_type = byte & 0x1f`、`nal_type == 5` 是 H264 IDR 判断。HEVC 应该从 NAL header 取：

```text
hevc_nal_type = (nal_header_byte0 >> 1) & 0x3f
```

常用 HEVC 类型：

```text
VPS: 32
SPS: 33
PPS: 34
IDR_W_RADL: 19
IDR_N_LP: 20
CRA_NUT: 21
BLA: 16-18
```

录像切片策略建议：

```text
recorder 启动某路录像
  -> 立刻 REQ_IDR
  -> 缓存 VPS/SPS/PPS
  -> 等到 IDR/CRA 后开始写第一个独立切片

准备 10 分钟切片
  -> 提前或到点 REQ_IDR
  -> 等下一个 IDR/CRA
  -> 新文件开头写 VPS/SPS/PPS
  -> 从 IDR/CRA 开始写新切片
```

不要 20 路同一毫秒一起 `REQ_IDR`，建议错峰：

```text
stream_id 1..20 每路错开 100-300ms
```

如果请求后长期没有 IDR：

- 继续写连续文件，但 DB 标记 `starts_with_keyframe=false`
- 定时重试 `REQ_IDR`，例如 2s/5s/10s 退避
- 记录告警，说明 Sunshine/编码器可能忽略了 `LiRequestIdrFrame()`
- 后续检查 Sunshine/编码器是否开启了超长 GOP、intra refresh、或禁用了周期 IDR

### 6.3 远端同步

远端方案不建议同步正在写的文件。

建议状态：

```text
writing
closed
syncing
synced
failed
expired
```

本地清理规则：

```text
synced 且超过本地保留时间 -> 删除
not synced 但超过最大本地空间 -> 删除最老，并记录告警
```

## 7. 验收指标

上线前至少看：

- 20 路 `docker logs` 仍然稳定 `state=connected`
- `stream_server` CPU 不明显上涨
- `intel_gpu_top` Video 不明显上涨
- com.jun OCR/取帧延迟没有明显恶化
- 本地磁盘增长符合 `30-40GB/小时` 量级
- 远端同步带宽稳定在 `60-100Mbps`
- 远端文件能按 stream_id 和时间检索
- 任意抽样文件可播放或可被 ffmpeg 解封装

## 8. 推荐下一步

1. 先做本地 `compressed_recorder`，只支持 raw `.hevc`，每 10 分钟切片。
2. 跑单路 30 分钟，验证文件可播放和切片大小。
3. 跑 4 路 30 分钟，看 `stream_server` 是否被影响。
4. 跑 20 路 1 小时，看磁盘增长和业务稳定性。
5. 再做远端同步/远端 recorder。
6. 最后补 DB 索引和清理策略。

本次不要先做 BGR/JPEG/MJPEG 录像。那条链路适合低清预览或问题截图，不适合最低成本保存 20 路原始现场。

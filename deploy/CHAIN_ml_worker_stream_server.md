# ml_worker -> stream_server 链路说明

本文只描述当前有效、已经跑通的主链路：

- `ml_worker` 连接 Sunshine
- `ml_worker` 把编码视频通过 TCP(TLV) 推给 `stream_server`
- `stream_server` 解码并通过内置 ZMQ bridge 对外返回 `BGR24`

不包含已移除的 `shm` 路径。

## 1. 主链路

```text
Sunshine Host
  -> ml_worker
  -> TCP TLV (stream_start + video_data)
  -> stream_server
  -> decode
  -> last_frame (CPU frame or HW frame ref)
  -> ZMQ GET_LATEST_BGR
  -> client (cv2.imshow / 其他消费者)
```

## 2. ml_worker 做的事

`ml_worker` 是 Moonlight/Limelight 客户端。

它负责：

- 和 Sunshine 建立串流会话
- 协商编码格式
- 从视频回调里拿到编码后的 AnnexB bytestream
- 通过 TCP TLV 推给 `stream_server`
- 可选：开启本地 UDP 控制口，接收 `REQ_IDR`

当前已经生效的协商策略：

- 默认 `420 -> h264`
- 默认 `444 -> hevc`
- 颜色参数会跟着流一起带下去
  - `colorspace`
  - `range`

## 3. TCP TLV 协议

包头：

```text
[u32_be length][u8 type][u16_be stream_id][payload...]
```

当前主链路实际用到的消息：

- `STREAM_START (0x03)`
- `VIDEO_DATA (0x01)`
- `HEARTBEAT (0x02)`
- `STREAM_STOP (0x04)`

### 3.1 STREAM_START payload

当前使用 40 字节版本：

```text
width
height
fps
bitrate
codec
chroma
bitdepth
video_format
color_space
color_range
```

全部都是 `u32_be`。

兼容性：

- `stream_server` 仍兼容旧的 `16` / `32` 字节版本

## 4. stream_server 做的事

`stream_server` 负责：

- 接收 `ml_worker` 的 TCP TLV 视频流
- 按 `stream_id` 管理多路流
- 根据 `STREAM_START` 里的元信息选择解码后端
- 解码后只保留每路 `last_frame`
- 默认优先保留硬件帧引用，不在每帧 decode 后立刻下载
- 在收到 ZMQ 请求时，如果 `last_frame` 还在 GPU 上，则先按需下载到 CPU，再转成 `BGR24`

## 5. 解码路由与回退顺序

当前固定策略：

- `HEVC 4:4:4 -> NVIDIA`
- 其他全部先走 `Intel`

当前回退顺序：

- `Intel -> NVIDIA -> CPU`
- `NVIDIA -> CPU`

也就是：

- `HEVC444` 优先 `NVIDIA`
- 其他流优先 `Intel`
- 如果 `Intel` 初始化失败，自动改试 `NVIDIA`
- 如果还不行，再退到 `CPU`

## 6. 自动 REQ_IDR

当前主链路已经加上自动 `REQ_IDR`：

- `ml_worker` 启动时可开启 UDP 控制口
- `stream_server` 如果看到：
  - 某路流已经开始收包
  - 但 `Decoded == 0`
- 会限频向 `ml_worker` 发一次 `REQ_IDR`

这用于解决：

- `HEVC` 晚加入
- `PPS/SPS/IDR` 没赶上导致一直出不了第一帧

## 7. ZMQ 返回格式

内置 ZMQ bridge 使用：

- bind：`tcp://0.0.0.0:5566`
- 命令：`GET_LATEST_BGR`
- 行为：请求时从对应流的 `last_frame` 生成一帧 `BGR24`
  - `last_frame` 是 CPU 帧时，直接做 libyuv 转换
  - `last_frame` 是硬件帧时，先 `av_hwframe_transfer_data()` 下载，再做 libyuv 转换

返回 multipart：

```text
[status][meta_json][bgr24]
```

`meta_json` 当前包含：

- `stream_id`
- `width`
- `height`
- `stride`
- `pts`
- `mono_ns`
- `key_frame`
- `pixfmt`
- `color_space`
- `color_range`

其中颜色字段是：

- `"color_space":"bt709#1"`
- `"color_range":"full#1"`

## 8. 颜色转换

当前颜色转换策略：

- 不再猜测固定矩阵
- 按 `STREAM_START` 里传下来的
  - `color_space`
  - `color_range`
  选择 libyuv 矩阵
- 对外统一输出 `BGR24`

## 9. 与启动参数相关的新默认值

当前默认启用：

- `STREAM_MAX_STREAMS=20`
- `STREAM_DEFER_HW_DOWNLOAD=on`
- `STREAM_NVDEC_EXTRA_HW_FRAMES=24`

含义：

- 硬解路径默认不再“每解一帧就下载一帧”
- 改为只保留最后一帧的硬件引用
- 只有真正有人取图时才发生 `GPU -> CPU` 下载
- 同时给 NVDEC/VAAPI 增加 surface 余量，避免因为保留 `last_frame` 而卡住帧池
- 路数扩容优先改 `--max-streams` 或 `STREAM_MAX_STREAMS`，不需要每次改代码重编

如需恢复旧行为，可设置：

- `STREAM_DEFER_HW_DOWNLOAD=off`

## 10. 当前验证过的运行形态

已验证的一条工作链路：

- Sunshine 编码：`HEVC 4:4:4`
- 分辨率：`1024x768`
- 色彩：`BT.709 + full`
- 解码：`NVIDIA`
- 输出：ZMQ `BGR24`

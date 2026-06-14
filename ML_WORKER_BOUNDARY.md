# ml_worker 职责边界

本文只描述 `ml_worker` 本体，不描述 `deploy/mlctl.sh`、Docker compose、`stream_server`、`agent_link_service` 等运维或消费侧组件。

## 1. 定位

`ml_worker` 是当前主链路里的 Sunshine 适配器和压缩视频生产者。

```text
Sunshine
  -> Moonlight/Limelight session
  -> ml_worker
  -> TCP TLV encoded video
  -> stream_server
```

它的核心职责是：

- 作为 Moonlight/Limelight 客户端连接 Sunshine
- 使用指定 key 目录完成配对状态读取和会话建立
- 启动指定 Sunshine app，例如 `Desktop`
- 请求并协商视频格式，例如 `HEVC / 4:4:4 / 8bit`
- 从 Limelight 视频回调拿到编码后的压缩视频 payload
- 把流元信息和压缩视频包通过项目自定义 TCP TLV 协议推给下游
- 可选开启本地 UDP control socket，把外部控制命令转成 Limelight 输入/控制调用

一句话：

```text
ml_worker = Sunshine client adapter + encoded stream producer
```

## 2. 边界

`ml_worker` 的输入侧边界是 Sunshine/Moonlight 协议。

它从 Sunshine 获得：

- app 列表和 app id
- 配对状态
- 串流会话
- 编码视频数据
- Sunshine 对键鼠/文本/IDR 请求等控制事件的处理结果

`ml_worker` 的输出侧边界是 TCP TLV。

它向下游输出：

- `STREAM_START`
- `VIDEO_DATA`
- `HEARTBEAT`
- `STREAM_STOP`

`ml_worker` 不负责：

- 解码 H264/HEVC/AV1
- 选择 Intel/NVIDIA/CPU 解码后端
- 生成 BGR/RGB 图像
- 保存 `last_frame`
- 提供 ZMQ `GET_LATEST_BGR`
- 做 OCR/业务取图
- 做录像、切片、远端同步
- 理解下游业务语义
- 管理多路流的汇聚调度

一个 `ml_worker` 进程通常对应一个 Sunshine host/app/stream_id。20 路主链路就是 20 个 worker 进程或容器分别推到同一个 `stream_server`。

## 3. CLI 接口

`ml_worker` 暴露三个主要命令。

### 3.1 pair

```bash
ml_worker pair <host> [pin] [--key-dir <path>]
```

作用：

- 和指定 Sunshine 主机完成配对
- 把证书/key 写入 `--key-dir`
- 如果没有传 PIN，会自动生成 4 位 PIN 并等待 Sunshine 端确认

边界：

- 只处理 Sunshine 配对
- 不启动视频流
- 不连接 `stream_server`

### 3.2 list

```bash
ml_worker list <host> [--key-dir <path>]
```

作用：

- 使用指定 key 目录连接 Sunshine
- 输出 Sunshine app 列表和 app id

边界：

- 只做 app 查询
- 不启动视频流
- 不产生 TCP TLV 输出

### 3.3 stream

```bash
ml_worker stream --host <ip> --app <name_or_index> [options]
```

核心参数：

- `--host <ip>`：Sunshine 主机
- `--app <name_or_index>`：启动的 Sunshine app
- `--key-dir <path>`：配对证书目录
- `--tcp-host <ip>`：下游 TCP 接收端，当前主链路是 `stream_server`
- `--tcp-port <port>`：下游 TCP 端口，当前主链路默认 `9000`
- `--stream-id <id>`：下游识别这一路流的 ID
- `--width / --height / --fps / --bitrate`：请求的串流参数
- `--codec <h264|hevc|av1>`：请求编码格式
- `--chroma <420|444>`：请求色度格式
- `--bitdepth <8|10>`：请求位深
- `--colorspace <601|709>`：请求颜色空间
- `--range <limited|full>`：请求颜色范围
- `--control-bind <ip>`：UDP control socket 绑定地址
- `--control-port <port>`：UDP control socket 端口，`0` 表示关闭

当前主链路常用形态：

```text
1024x768 @ 30fps
HEVC 4:4:4 8bit
BT.709 full range
TCP -> stream_server:9000
```

## 4. TCP TLV 输出接口

`ml_worker` 把压缩视频推给下游时使用项目自定义 TLV。

包头：

```text
[u32_be length][u8 type][u16_be stream_id][payload...]
```

消息类型：

```text
0x01 VIDEO_DATA
0x02 HEARTBEAT
0x03 STREAM_START
0x04 STREAM_STOP
```

### 4.1 STREAM_START

`STREAM_START` 在流开始时发送，payload 当前为 40 字节，全部是 `u32_be`：

```text
width
height
fps
bitrate
codec        # 0=h264, 1=hevc, 2=av1
chroma       # 0=420, 1=444
bitdepth     # 8 / 10
video_format # Limelight negotiated video format mask
color_space  # Limelight color space enum
color_range  # Limelight color range enum
```

这是下游选择解码器和颜色转换路径的主要依据。

### 4.2 VIDEO_DATA

`VIDEO_DATA` 的 payload 是 Sunshine/Limelight 回调给到的压缩视频数据。

边界说明：

- `ml_worker` 不解码这个 payload
- `ml_worker` 不把它转换成 BGR/RGB
- `ml_worker` 不保证下一个包一定是 IDR
- `ml_worker` 只保证按当前连接和 `stream_id` 推给 TCP 下游

如果要从主链路桥出视频流，优先应该在 `stream_server` 收到 `VIDEO_DATA` 后、解码前旁路复制这一份压缩 payload，而不是让 `ml_worker` 同时承担多路分发、录像或远端同步。

### 4.3 HEARTBEAT / STREAM_STOP

`HEARTBEAT` 用于保持下游连接状态。

`STREAM_STOP` 用于表示该 worker 的视频流结束。

## 5. UDP control 输入接口

`ml_worker` 可选开启 UDP control socket：

```bash
--control-bind <ip> --control-port <port>
```

控制包头是 `MlControlCmd`：

```text
magic   = "MLCT"
version = 1
type
a
b
c
d
seq
```

当前命令类型：

```text
1  MOUSE_ABS
2  MOUSE_REL
3  MOUSE_BUTTON
4  MOUSE_CLICK
5  MOUSE_SCROLL
6  MOUSE_HSCROLL
7  KEYBOARD
8  KEY_PRESS
9  TEXT
10 REQ_IDR
```

这些命令的作用不是在 `ml_worker` 内部处理业务，而是转换成 Limelight 调用发回 Sunshine。

其中 `REQ_IDR` 的链路是：

```text
stream_server or other controller
  -> UDP ML_CTRL_CMD_REQ_IDR
  -> ml_worker
  -> LiRequestIdrFrame()
  -> Sunshine encoder
```

注意：

- `REQ_IDR` 是 best-effort 请求，不保证 Sunshine 下一帧一定立刻是 IDR
- 什么时候需要 IDR 通常由下游判断，例如解码停滞、录像切片、新 tap 订阅
- 真正调用 `LiRequestIdrFrame()` 的执行者是 `ml_worker`

## 6. 和 deploy/mlctl.sh 的关系

`deploy/mlctl.sh` 是运维封装，不是 `ml_worker` 本体接口。

它负责：

- 生成 worker 配置
- 管理 key/data 目录
- 创建或重启 Docker 容器
- 拼出 `ml_worker stream ...` 参数
- 配置 Docker 日志轮转
- 可选做启动验活和日志诊断

但运行时边界仍然是：

```text
ml_worker process
  <- Sunshine/Limelight
  -> TCP TLV
  <- UDP control
```

## 7. 对后续视频旁路的架构含义

因为 `ml_worker` 的边界已经清晰，后续“从主链路桥一个视频流出来”不建议优先改 `ml_worker`。

推荐位置：

```text
stream_server handle VIDEO_DATA
  -> compressed tap / recorder queue
  -> stream_decode_video
```

这样可以保持：

- `ml_worker` 仍然只做 Sunshine adapter
- 主业务取帧链路继续走 `stream_server -> ZMQ BGR`
- 录像/桥接失败不影响 worker 和解码主链路
- 旁路模块可以独立处理队列、丢包、切片、远端断线和 `REQ_IDR`

---

Doc-Version: 0.1.0
Repo-Rev: b2957cb

# Architecture Flow: ml_worker / stream_server / strem_agent_server / strem_agent_client

本文件给出本仓库主要组件的交互流程（视频/控制）、以及常用启动参数与意图。

> 注意：不同部署场景可能只使用其中一部分组件。

## 0. 组件概览

- **Sunshine Host**（外部组件，GameStream 协议服务端）
  - 输出编码后的视频（H.264 bytestream）
  - 接受控制输入（鼠标/键盘/文本等）

- **`ml_worker`**（本仓库）
  - 作为 Moonlight/Limelight 客户端连接 Sunshine
  - 从回调中拿到编码后 H.264 bytestream
  - 通过 **TCP(TLV)** 推送给下游（`stream_server` 或 `strem_agent_server`）
  - 可选：监听 **UDP 控制端口**，把外部输入注入 Sunshine 会话

- **`strem_agent_server`**（本仓库，远程代理 server）
  - Ingest：接收 `ml_worker` 推来的 **TCP(TLV/H264)**
  - Video out：对外提供 **TCP(AnnexB H264)** 给远端 client（需 SUB）
  - Control in：对外接收 **TCP 控制**，转发为 **UDP** 到 `ml_worker`
  - Late-join：当有新 video SUB 时，自动向 `ml_worker` 发送 **REQ_IDR**

- **`strem_agent_client`**（本仓库，远程代理 client）
  - 连接 `strem_agent_server` 的 video 端口，发送 `SUB <stream_id>`
  - 解码 H.264 并显示（PyAV；无 PyAV 时可用 ffmpeg fallback）
  - 连接 ctrl 端口，把鼠标/键盘事件发回 server

- **`stream_server`**（本仓库，接收/解码/桥接 server）
  - 接收 `ml_worker` 推来的 **TCP(TLV/H264)**
  - 可选：解码并写入 SHM / ZMQ bridge（见 `deploy/SERVICES_RUNBOOK.md`）
  - 可选：开启 H264 tap（TCP 输出 AnnexB H264）
  - Late-join（tap 可选）：新订阅者连接时可向 `ml_worker` 发送 **REQ_IDR**（需配置环境变量）

## 1. 视频/控制交互流程图（ASCII）

### 1.1 远程代理（推荐：strem_agent_server + strem_agent_client）

```text
               (GameStream)
   +-------------------------------+
   |        Sunshine Host          |
   +-------------------------------+
                 ^        |
                 |control |
                 |(GS)    | video (H.264)
                 |        v
   +-------------------------------+
   |            ml_worker          |
   | - connects Sunshine           |
   | - outputs TLV/H264 over TCP   |---- TCP(TLV/H264) ---> (in-port=19000)
   | - listens UDP control :50001  |<--- UDP(MlControlCmd) ---+
   +-------------------------------+                          |
                                                               |
                                     +-------------------------v--------------------+
                                     |               strem_agent_server             |
                                     | ingest :19000  (TLV/H264)                    |
                                     | video  :31234  (AnnexB H264, SUB required)   |---- TCP(H264) ---> strem_agent_client
                                     | ctrl   :31235  (TCP control in)              |<--- TCP(ctrl) ---- strem_agent_client
                                     | late-join: on SUB -> send REQ_IDR(type=10)   |
                                     +----------------------------------------------+
```

### 1.2 解码/桥接服务（stream_server + shm/zmq bridge）

```text
Sunshine Host -> ml_worker -> TCP(TLV/H264) -> stream_server(:19000)
                                            |
                                            | (optional) decode -> /dev/shm/...
                                            | (optional) ZMQ bridge -> tcp://*:5566
                                            |
                                            | (optional) H264_TAP_PORT -> TCP(AnnexB H264)
                                            |            on SUB -> optional REQ_IDR(type=10)
```

## 2. 协议速查

### 2.1 `ml_worker` -> (`strem_agent_server` / `stream_server`)：TCP TLV

TLV 头部（大端）：

```
[u32_be length][u8 type][u16_be stream_id][payload...]
```

常用 type：

- `0x03 STREAM_START`：payload=16B (width/height/fps/bitrate, u32_be)
- `0x01 VIDEO_DATA`：payload=H.264 bytestream (通常为 AnnexB)

### 2.2 client -> strem_agent_server：video SUB

连接到 `video-port` 后发送：

```
SUB <stream_id>\n
```

如果 server 启用了 token，则先：

```
AUTH <token>\n
SUB <stream_id>\n
```

### 2.3 控制协议：MlControlCmd（MLCT）

`strem_agent_client` -> `strem_agent_server`：TCP framing：

```
[u32_be length][MlControlCmd + optional payload]
```

`strem_agent_server` -> `ml_worker`：UDP 直接转发 `MlControlCmd`。

关键字段：

- `magic = 0x4d4c4354` ("MLCT")
- `version = 1`
- `type = ...`

新增 Late-join 相关命令：

- `type = 10`：`REQ_IDR`（请求主机尽快发送 IDR；`ml_worker` 调用 `LiRequestIdrFrame()`）

## 3. 常用启动命令与意图

### 3.1 `strem_agent_server`

**意图**：提供远程代理 server：接入 TLV/H264、对外输出 H264、并转发控制。

```bash
./strem_agent_server/build/strem_agent_server \
  --in-host 0.0.0.0 --in-port 19000 \
  --video-bind 0.0.0.0 --video-port 31234 \
  --ctrl-bind 0.0.0.0 --ctrl-port 31235 \
  --worker-ctrl-ip 127.0.0.1 --worker-ctrl-port 50001
```

- `--in-*`：接收 `ml_worker` 推来的 TLV/H264
- `--video-*`：给远端 client 输出 H264（AnnexB）
- `--ctrl-*`：接收远端 client 控制（TCP），并转发到 `--worker-ctrl-ip:port`
- `--worker-ctrl-*`：`ml_worker` 监听的 UDP 控制端口

### 3.2 `strem_agent_client`（Python）

**意图**：连接 agent_server，展示画面并回传鼠标控制。

```bash
python strem_agent_client/strem_agent_client.py \
  --host <agent_server_ip> \
  --video-port 31234 \
  --ctrl-port 31235 \
  --stream-id 1
```

IDE 快速运行（配置写死在文件顶部，适合 Windows IDE 点 Run）：

```bash
python strem_agent_client/ide_demo_client.py
```

### 3.3 `ml_worker stream`

**意图**：连接 Sunshine 获取 H264，并推送给下游（agent 或 stream_server）。

```bash
./build/ml_worker stream \
  --host 192.168.11.50 --app Desktop \
  --tcp-host 127.0.0.1 --tcp-port 19000 --stream-id 1 \
  --control-bind 127.0.0.1 --control-port 50001
```

- `--tcp-host/--tcp-port`：推流目标（TLV/H264）
- `--control-port`：开启 UDP 控制端口，供 agent/server 请求 IDR、转发输入等

Docker 镜像（推荐脚本）：

```bash
cmake --build build -j
./deploy/build_image.sh
./deploy/mlctl.sh restart worker_local_agent
./deploy/mlctl.sh logs worker_local_agent
```

### 3.4 `stream_server`

**意图**：接收 TLV/H264，可选解码/共享内存/桥接；也可开启 tap 输出给外部。

常用：

```bash
DECODE_BACKEND=intel ENABLE_SHM=1 \
  ./stream_server/build/stream_server -h 0.0.0.0 -p 19000 -c 20 -s 5
```

参数（以 `--help` 为准）：

- `-h/--host`：监听地址
- `-p/--port`：监听端口
- `-c/--connections`：最大连接数
- `-s/--stats-interval`：统计打印周期

可选：开启 H264 tap + 晚加入请求 IDR：

```bash
export H264_TAP_PORT=19090
export H264_TAP_BIND=0.0.0.0
export ML_WORKER_CTRL_IP=127.0.0.1
export ML_WORKER_CTRL_PORT=50001
```

## 4. 典型问题

### 4.1 晚加入黑屏

根因通常是上游长时间没有新的 IDR，解码器无法从 P 帧中途起播。

解决：使用本文档描述的 REQ_IDR（type=10）机制，或重启推流会话。

---

Doc-Version: 0.1.0
Repo-Rev: 90b0776


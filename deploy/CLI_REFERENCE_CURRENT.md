# 当前有效命令参数说明

本文只保留当前还有效、且符合当前实现的参数。

不再列出：

- 已移除的 `shm` 相关参数
- 文档里还存在但当前主链路不用的旧说明

## 1. ml_worker

二进制：

```bash
./build-ninja/ml_worker
```

### 1.1 pair

```bash
./build-ninja/ml_worker pair <host> [pin] [--key-dir <path>]
```

参数：

- `<host>`：Sunshine 主机 IP
- `[pin]`：4 位 PIN，可省略；省略时自动生成
- `--key-dir <path>`：证书目录

### 1.2 list

```bash
./build-ninja/ml_worker list <host> [--key-dir <path>]
```

参数：

- `<host>`：Sunshine 主机 IP
- `--key-dir <path>`：证书目录

### 1.3 stream

```bash
./build-ninja/ml_worker stream --host <ip> --app <name_or_index> [options]
```

有效参数：

- `--host <ip>`：Sunshine 主机 IP
- `--app <name_or_index>`：应用名或应用索引
- `--key-dir <path>`：证书目录
- `--tcp-host <ip>`：下游 TCP 接收端地址
- `--tcp-port <port>`：下游 TCP 接收端端口，默认 `9000`
- `--stream-id <id>`：流 ID，默认 `1`
- `--width <n>`：宽度，默认 `1280`
- `--height <n>`：高度，默认 `720`
- `--fps <n>`：帧率，默认 `60`
- `--bitrate <n>`：码率 kbps，默认 `10000`
- `--packet-size <n>`：包大小，默认 `1024`
- `--colorspace <601|709>`：颜色空间，默认 `709`
- `--range <limited|full>`：颜色范围，默认 `limited`
- `--codec <h264|hevc|av1>`：请求编码，默认 `h264`
- `--chroma <420|444>`：色度格式，默认 `420`
- `--bitdepth <8|10>`：位深，默认 `8`
- `--skip-mode-check`：跳过 Sunshine mode 检查，默认开启
- `--enforce-mode-check`：强制做 mode 检查
- `--control-bind <ip>`：本地 UDP 控制监听地址，默认 `127.0.0.1`
- `--control-port <port>`：本地 UDP 控制监听端口，默认 `0`（关闭）

当前行为说明：

- 如果没显式指定 `--codec`
- 且 `--chroma 444`
- 会自动切到 `hevc`

### 1.4 当前推荐启动参数

```bash
./build-ninja/ml_worker stream \
  --host 192.168.11.170 \
  --app Desktop \
  --key-dir /home/gejun/work/my_ml_work/deploy/data/keys_170 \
  --tcp-host 192.168.11.31 \
  --tcp-port 9000 \
  --stream-id 1 \
  --width 1024 \
  --height 768 \
  --chroma 444 \
  --colorspace 709 \
  --range full \
  --control-bind 127.0.0.1 \
  --control-port 19000
```

## 2. stream_server

二进制：

```bash
./stream_server/build/stream_server
```

有效参数：

- `-h, --host <HOST>`：监听地址
- `-p, --port <PORT>`：监听端口，默认 `9000`
- `-c, --connections <N>`：最大连接数，默认 `20`
- `-s, --stats-interval <sec>`：统计输出间隔，默认 `10`
- `-d, --daemon`：守护进程
- `-v, --verbose`：更详细日志
- `--decode-backend <auto|intel|nvidia|cpu>`：默认后端偏好，默认 `auto`
- `--zmq-bridge-bind <addr>`：启用内置 ZMQ BGR bridge
- `--stress-test`：压测模式
- `--stress-copies <n>`：压测副本数
- `--h264-tap-port <p>`：开启 H264 tap
- `--h264-tap-bind <ip>`：tap 监听地址
- `--h264-tap-stall-ms <ms>`：tap 堵塞阈值
- `--h264-tap-drop-idr <0|1>`：tap 恢复策略
- `--ml-worker-ctrl-ip <ip>`：向上游 `ml_worker` 发控制命令
- `--ml-worker-ctrl-port <p>`：上游 `ml_worker` 控制端口

### 2.1 当前推荐启动参数

```bash
./stream_server/build/stream_server \
  -h 0.0.0.0 \
  -p 9000 \
  -c 30 \
  --zmq-bridge-bind tcp://0.0.0.0:5566 \
  --ml-worker-ctrl-ip 127.0.0.1 \
  --ml-worker-ctrl-port 19000 \
  -v
```

### 2.2 当前主链路真正使用的参数

当前 `ml_worker -> stream_server -> ZMQ` 主链路里，核心只需要：

- `-h`
- `-p`
- `-c`
- `--zmq-bridge-bind`
- `--ml-worker-ctrl-ip`
- `--ml-worker-ctrl-port`
- `-v`

## 3. strem_agent_server

二进制：

```bash
./build-ninja/strem_agent_server
```

有效参数：

- `--in-host <ip>`：接收 TLV 输入的监听地址
- `--in-port <port>`：接收 TLV 输入的监听端口，默认 `19000`
- `--video-bind <ip>`：视频输出监听地址
- `--video-port <port>`：视频输出端口，默认 `31234`
- `--ctrl-bind <ip>`：控制输入监听地址
- `--ctrl-port <port>`：控制输入端口，默认 `31235`
- `--worker-ctrl-ip <ip>`：下游转发到 `ml_worker` 的控制地址，默认 `127.0.0.1`
- `--worker-ctrl-port <p>`：下游转发到 `ml_worker` 的控制端口，默认 `50001`
- `--token <token>`：可选认证 token

兼容性说明：

- 当前 `strem_agent_server` / `strem_agent_client` 仍以 `H264` 代理链为主
- 不能视为当前 `HEVC444` 主链路的完整替代

## 4. deploy/mlctl.sh

常用命令：

- `./deploy/mlctl.sh up <worker>`
- `./deploy/mlctl.sh restart <worker>`
- `./deploy/mlctl.sh logs <worker>`

当前有用的 worker 配置字段：

- `HOST`
- `APP`
- `IMAGE`
- `WORKER_BIN`
- `TCP_HOST`
- `TCP_PORT`
- `STREAM_ID`
- `CONTROL_BIND`
- `CONTROL_PORT`
- `WIDTH`
- `HEIGHT`
- `FPS`
- `BITRATE`
- `PACKET_SIZE`
- `COLORSPACE`
- `RANGE`
- `CODEC`
- `CHROMA`
- `BITDEPTH`
- `SKIP_MODE_CHECK`

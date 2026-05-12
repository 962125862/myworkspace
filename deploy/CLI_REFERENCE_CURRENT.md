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
- `--width <n>`：宽度，默认 `1024`
- `--height <n>`：高度，默认 `768`
- `--fps <n>`：帧率，默认 `30`
- `--bitrate <n>`：码率 kbps，默认 `10000`
- `--packet-size <n>`：包大小，默认 `1024`
- `--colorspace <601|709>`：颜色空间，默认 `709`
- `--range <limited|full>`：颜色范围，默认 `full`
- `--codec <h264|hevc|av1>`：请求编码，默认 `hevc`
- `--chroma <420|444>`：色度格式，默认 `444`
- `--bitdepth <8|10>`：位深，默认 `8`
- `--skip-mode-check`：跳过 Sunshine mode 检查，默认开启
- `--enforce-mode-check`：强制做 mode 检查
- `--control-bind <ip>`：本地 UDP 控制监听地址，默认 `0.0.0.0`
- `--control-port <port>`：本地 UDP 控制监听端口，默认 `0`（关闭）

当前行为说明：

- 当前默认参数就是 `1024x768 / 30fps / hevc / 444 / full / skip-mode-check`
- `mlctl.sh` 当前模板也与这里保持一致
- 启动时会先探测 Sunshine 主机是否可达；如果探测不到，会在容器内等待 30 秒再重试，避免 `restart: unless-stopped` 形成快速重启循环

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
  --fps 30 \
  --codec hevc \
  --chroma 444 \
  --colorspace 709 \
  --range full \
  --control-bind 0.0.0.0 \
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
- `--max-streams <N>`：运行时启用的最大流数，默认 `20`，编译期硬上限 `256`
- `-s, --stats-interval <sec>`：统计输出间隔，默认 `10`
- `-d, --daemon`：守护进程
- `-v, --verbose`：更详细日志
- `--decode-backend <auto|intel|nvidia|cpu>`：默认后端偏好，默认 `auto`
  - `auto`：保留为真正的自动模式，按流元信息走 `stream_server` 路由表；当前主机默认优先 `Intel`
  - `intel/nvidia/cpu`：显式指定后端，直接跳过路由表
- `--zmq-bridge-bind <addr>`：启用内置 ZMQ BGR bridge；当前只要启用 bridge，就会默认再监听 `ipc:///tmp/stream_server_bgr.sock`
- `--zmq-bridge-ipc-bind <addr>`：覆盖默认 IPC 地址
- `--stress-test`：压测模式
- `--stress-copies <n>`：压测副本数
- `--h264-tap-port <p>`：开启 H264 tap
- `--h264-tap-bind <ip>`：tap 监听地址
- `--h264-tap-stall-ms <ms>`：tap 堵塞阈值
- `--h264-tap-drop-idr <0|1>`：tap 恢复策略
- `--ml-worker-ctrl-map <map>`：按 `stream_id` 路由上游 `REQ_IDR`，格式如 `1:127.0.0.1:30001,2:127.0.0.1:30002`
- `--ml-worker-ctrl-map-file <path>`：按文件配置 `stream_id -> ip:port` 映射，每行格式 `<stream_id> <ip> <port>`

### 2.1 当前推荐启动参数

```bash
./stream_server/build/stream_server \
  -h 0.0.0.0 \
  -p 9000 \
  --max-streams 20 \
  -c 30 \
  --zmq-bridge-bind tcp://0.0.0.0:5566 \
  --ml-worker-ctrl-map-file /home/gejun/work/my_ml_work/deploy/stream_server_ctrl_map.txt \
  -v
```

默认同时监听：

- `tcp://0.0.0.0:5566`
- `ipc:///tmp/stream_server_bgr.sock`
- 当前主机 `192.168.11.31` 上，`HEVC444 + intel` 下载到 CPU 后通常落到 `VUYX`，再走专门的 `VUYX -> BGR24` 快路径

### 2.2 启动环境变量

当前和路数规模相关的推荐做法是：

- 用 `--max-streams <N>` 控制实际启用的流槽位
- 或用 `STREAM_MAX_STREAMS=<N>` 通过环境变量控制
- 用 `-c <N>` 控制允许的 TCP 连接数

两者区别：

- `--max-streams` 决定可接受的 `stream_id` 范围，以及 `StreamManager` 实际启用多少路
- `STREAM_MAX_STREAMS` 是 `--max-streams` 的环境变量等价入口，适合 `systemd`
- `-c` 只决定同时允许多少 TCP 连接，通常可以比 `--max-streams` 略大，给重连抖动留余量
- 程序会自动保证 `connections >= max-streams`，避免只改了路数却忘了改连接数

兼容性说明：

- 编译期硬上限已经放宽到 `256`
- 以后 `20 -> 35 -> 48` 这类扩容，优先改启动参数，不需要每次改代码重编
- 如果要超过 `256`，才需要再次改代码

和硬解保帧逻辑相关的环境变量是：

- `STREAM_DEFER_HW_DOWNLOAD`
  - 默认 `on`
  - 作用：硬解时 `last_frame` 默认保留为硬件帧引用，不在每次 decode 后立刻 `GPU -> CPU` 下载
  - 取图时（当前主要是 ZMQ `GET_LATEST_BGR`）再按需 `av_hwframe_transfer_data()` 下载并转 `BGR24`
  - `GET_LATEST_BGR` 可带 `roi:{"x","y","w","h"}` 只返回 ROI payload；这是输出裁剪，用于节省 ZMQ/IPC 带宽，不改变解码和整帧转换路径
- `STREAM_NVDEC_EXTRA_HW_FRAMES`
  - 默认：`STREAM_DEFER_HW_DOWNLOAD=on` 时为 `24`，关闭延迟下载时为 `8`
  - 作用：给 NVDEC/VAAPI 多留一些硬件 surface 余量，避免 `last_frame` 仍持有硬件帧引用时把帧池顶满

- 如果你不显式设置这两个环境变量，默认就已经按新逻辑运行
- 如果想恢复旧行为，可以启动前设置 `STREAM_DEFER_HW_DOWNLOAD=off`
- 当前 20 路真实流、`30 fps/路` 的 `IPC BGR` benchmark 结果，直接参考 `deploy/BENCHMARK_stream_server_2026-04-13.md`

### 2.3 当前主链路真正使用的参数

当前 `ml_worker -> stream_server -> ZMQ` 主链路里，核心只需要：

- `-h`
- `-p`
- `--max-streams`
- `-c`
- `--zmq-bridge-bind`
- `--ml-worker-ctrl-map` 或 `--ml-worker-ctrl-map-file`
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
- `--worker-ctrl-port <p>`：下游转发到 `ml_worker` 的控制端口，默认 `30001`
- `--token <token>`：可选认证 token

兼容性说明：

- 当前 `strem_agent_server` / `strem_agent_client` 仍以 `H264` 代理链为主
- 不能视为当前 `HEVC444` 主链路的完整替代

## 4. deploy/mlctl.sh

常用命令：

- `./deploy/mlctl.sh add <worker> <host>`
- `./deploy/mlctl.sh batch-add <host> [start] [end]`
- `./deploy/mlctl.sh batch-add-ips <ip1> [ip2] [ip3] ...`
- `./deploy/mlctl.sh pair <worker> [pin]`
- `./deploy/mlctl.sh batch-pair <start> <end> [pin|pin1,pin2,...]`
- `./deploy/mlctl.sh up <worker>`
- `./deploy/mlctl.sh restart <worker>`
- `./deploy/mlctl.sh logs <worker>`

worker Docker 日志轮转：

- `mlctl.sh up/restart` 创建容器时默认使用 Docker `json-file` 日志轮转
- 默认 `ML_WORKER_LOG_MAX_SIZE=20m`
- 默认 `ML_WORKER_LOG_MAX_FILE=2`
- 单个 `mlw-worker_*` 容器最多约保留 `40MB` Docker 日志
- 已存在容器需要 `./deploy/mlctl.sh restart <worker>` 重建后才会应用新的 LogConfig

当前批量模式：

- `batch-add`：生成 `worker_sN`，默认 `STREAM_ID=N`、`CONTROL_PORT=30000+N`
- `batch-add-ips`：按输入顺序生成，命名规则 `worker_<ip最后一段>`
- `batch-add-ips` 会自动写出 `deploy/stream_server_ctrl_map.txt`
- `batch-pair`：按 `worker_sN` 串行配对；可传一个固定 PIN，也可传逗号分隔的 PIN 列表
- 生成的 ctrl map 可直接给 `stream_server --ml-worker-ctrl-map-file` 使用

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

当前默认模板：

- `APP="Desktop"`
- `IMAGE="ml-worker:latest"`
- `TCP_HOST="127.0.0.1"`
- `TCP_PORT="9000"`
- `CONTROL_BIND="0.0.0.0"`
- `WIDTH="1024"`
- `HEIGHT="768"`
- `FPS="30"`
- `BITRATE="10000"`
- `PACKET_SIZE="1024"`
- `COLORSPACE="709"`
- `RANGE="full"`
- `CODEC="hevc"`
- `CHROMA="444"`
- `BITDEPTH="8"`
- `SKIP_MODE_CHECK="1"`

## 5. 相关文档

- 链路说明：`deploy/CHAIN_ml_worker_stream_server.md`
- 架构说明：`deploy/ARCHITECTURE_CURRENT.md`
- benchmark：`deploy/BENCHMARK_stream_server_2026-04-13.md`

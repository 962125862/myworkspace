# Runbook: ml_worker → stream_server → shm_zmq_bridge (ZMQ 拉 NV12 帧)

本文把当前这套链路的**启动/常驻/检查/端口/客户端取帧（含 Windows 多端）**整理成一份可复制的运行手册。

- Producer：`ml_worker`（Docker，通过 `deploy/mlctl.sh`）把 H.264 bytestream 推到 31
- Receiver/Decoder：`stream_server`（运行在 `192.168.11.31`，Intel iGPU 解码）
- Frame Bridge：`python_dir/shm_zmq_bridge.py`（读 `/dev/shm/stream_server_stream_XX`，通过 ZMQ 返回 NV12）
- Consumer：Linux/Windows 客户端通过 ZMQ 获取最新帧（建议走 cache：`GET_LATEST_NV12`）

默认端口/路径约定（本 repo 现状）：

- `stream_server`: `0.0.0.0:19000`
- `shm_zmq_bridge`: `tcp://*:5566`
- SHM：`/dev/shm/stream_server_stream_01`, `/02`, ...（stream_id → `%02d`）

---

## 0. Quick Start（最短路径）

在 31 上启动（前台跑，用于验证链路）：

```bash
# 31: stream_server
cd /home/gejun/work/my_ml_work/stream_server
make -j
  ./build/stream_server \
    -h 0.0.0.0 -p 19000 -c 20 -s 5 \
    --decode-backend intel \
    --enable-shm
```

另一个 31 终端启动 bridge（推荐 pump+cache，适合多客户端）：

```bash
cd /home/gejun/work/my_ml_work
source /home/my_server/bin/activate

python python_dir/shm_zmq_bridge.py \
  --bind tcp://*:5566 \
  --workers 1 \
  --pump-streams 1
```

本机启动推流（producer）：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh up worker00
./deploy/mlctl.sh logs worker00
```

任意机器取一帧（Linux / Windows 都可，只要能访问 31:5566）：

```bash
python3 python_dir/shm_zmq_bridge_client.py \
  --addr tcp://192.168.11.31:5566 \
  --stream-id 1 \
  --out-prefix /tmp/frame
```

---

## 1. 31 上：编译与运行 stream_server

### 1.1 编译

```bash
cd /home/gejun/work/my_ml_work/stream_server
make -j
ls -l /home/gejun/work/my_ml_work/stream_server/build/stream_server
```

### 1.2 前台运行（Intel iGPU + SHM）

```bash
cd /home/gejun/work/my_ml_work/stream_server
  ./build/stream_server \
    -h 0.0.0.0 -p 19000 -c 20 -s 5 \
    --decode-backend intel \
    --enable-shm
```

### 1.2.1 stream_server 全参数速查（推荐看 --help）

基础参数：

- `-h/--host <HOST>`：监听地址
- `-p/--port <PORT>`：监听端口
- `-c/--connections <N>`：最大连接数
- `-s/--stats-interval <sec>`：统计打印周期
- `-d/--daemon`：守护进程

可选功能参数：

- `--decode-backend <auto|intel|nvidia|cpu>`
- `--enable-shm` / `--shm-always`
- `--zmq-bridge-bind <addr>`
- `--stress-test` / `--stress-copies <n>`
- `--h264-tap-port <p>` / `--h264-tap-bind <ip>`
- `--h264-tap-stall-ms <ms>` / `--h264-tap-drop-idr <0|1>`
- `--ml-worker-ctrl-ip <ip>` / `--ml-worker-ctrl-port <p>`

### 1.3 （可选）启用内置 ZMQ bridge（并入 stream_server）

如果你的目标是减少 Python bridge 进程与 SHM 二次拷贝带来的 CPU 开销，可以启用
`stream_server` 内置的 ROUTER/DEALER ZMQ 服务端。

说明：

- 该功能需要编译时检测到 `libzmq`（CMake/Makefile 都是“检测到才启用”）
- 通过参数开启：`--zmq-bridge-bind tcp://0.0.0.0:5566`
- 目前实现仅提供“取最新帧”：`GET_LATEST_NV12`（`GET_SHM_NV12` 兼容为同义）
- 内置 bridge 采用“解码时缓存紧凑 NV12 + ZMQ 发送零拷贝”的方式：
  - 解码线程更新 per-stream latest cache
  - 客户端请求只读 cache（不会触发 request_seq / 不会等待下一帧）
  - 为避免服务刚启动时返回空，内置 bridge 会在首次请求时用 `last_frame` 立即“种子”一帧到 cache（若存在）。

运行示例：

```bash
cd /home/gejun/work/my_ml_work/stream_server
  ./build/stream_server \
    -h 0.0.0.0 -p 19000 -c 20 -s 5 \
    --decode-backend intel \
    --enable-shm \
    --zmq-bridge-bind tcp://0.0.0.0:5566
```

检查：

```bash
ss -lntp | grep ':19000'
```

SHM 只有在该 `stream_id` 有数据（producer 正在推）后才会出现/更新：

```bash
ls -l /dev/shm/stream_server_stream_01
```

---

## 2. 31 上：systemd（--user）常驻 stream_server

创建用户级 service：`~/.config/systemd/user/stream_server_19000.service`

```ini
[Unit]
Description=my_ml_work stream_server (19000)
After=network.target

[Service]
Type=simple
WorkingDirectory=/home/gejun/work/my_ml_work/stream_server
ExecStart=/home/gejun/work/my_ml_work/stream_server/build/stream_server \
  -h 0.0.0.0 -p 19000 -c 20 -s 5 \
  --decode-backend intel \
  --enable-shm
Restart=always
RestartSec=1

[Install]
WantedBy=default.target
```

启用并启动：

```bash
systemctl --user daemon-reload
systemctl --user enable --now stream_server_19000.service
systemctl --user status stream_server_19000.service --no-pager -l
```

查看日志：

```bash
journalctl --user -u stream_server_19000.service -n 200 --no-pager
```

---

## 3. 本机：ml_worker（Docker）推流到 31

### 3.1 检查 worker 配置

编辑 `deploy/workers/worker00.conf`，确保至少这些字段正确：

- `TCP_HOST="192.168.11.31"`
- `TCP_PORT="19000"`
- `STREAM_ID="1"`（多路时每路不同）

### 3.2 构建镜像（首次/有依赖变化时）

```bash
cd /home/gejun/work/my_ml_work
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./deploy/build_image.sh
```

### 3.3 启动与查看日志

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh up worker00
./deploy/mlctl.sh logs worker00
```

---

## 4. 31 上：启动 shm_zmq_bridge（ZMQ 桥）

### 4.1 Python 环境

在 31 上使用已有 venv（示例路径以当前机器为准）：

```bash
source /home/my_server/bin/activate
python -c 'import zmq, numpy, cv2; print("ok")'
```

> 如果缺少依赖：`pip install pyzmq numpy opencv-python`

### 4.2 运行 bridge（推荐：pump + cache）

**推荐原因**：多客户端并发时，避免多个 reader 同时竞争同一 shm 的 `request_seq++`。

```bash
cd /home/gejun/work/my_ml_work
source /home/my_server/bin/activate

python python_dir/shm_zmq_bridge.py \
  --bind tcp://*:5566 \
  --workers 1 \
  --pump-streams 1
```

检查端口：

```bash
ss -lntp | grep ':5566'
```

### 4.3 systemd（--user）常驻 bridge

创建：`~/.config/systemd/user/shm_zmq_bridge_5566.service`

```ini
[Unit]
Description=my_ml_work shm_zmq_bridge (5566)
After=network.target

[Service]
Type=simple
WorkingDirectory=/home/gejun/work/my_ml_work
Environment=PYTHONUNBUFFERED=1
ExecStart=/bin/bash -lc "source /home/my_server/bin/activate && exec python /home/gejun/work/my_ml_work/python_dir/shm_zmq_bridge.py --bind tcp://*:5566 --workers 1 --pump-streams 1"
Restart=always
RestartSec=1

[Install]
WantedBy=default.target
```

启用并启动：

```bash
systemctl --user daemon-reload
systemctl --user enable --now shm_zmq_bridge_5566.service
systemctl --user status shm_zmq_bridge_5566.service --no-pager -l
```

---

## 5. 客户端取帧

### 5.1 Linux：拉取 raw（meta/y/uv/nv12）

```bash
python3 python_dir/shm_zmq_bridge_client.py \
  --addr tcp://192.168.11.31:5566 \
  --stream-id 1 \
  --out-prefix /tmp/frame
```

如需保存 PNG（需要 OpenCV）：

```bash
python3 python_dir/shm_zmq_bridge_client_save_png.py \
  --addr tcp://192.168.11.31:5566 \
  --stream-id 1 \
  --out /tmp/frame.png
```

### 5.2 Windows：多端/多客户端建议与常见坑

结论：**Windows 端只走 ZMQ**，不要试图访问 `/dev/shm`。

#### A) 安装依赖（PowerShell）

```powershell
py -m pip install --upgrade pip
py -m pip install pyzmq numpy opencv-python
```

#### B) 拉取一帧（raw）

在 repo 根目录执行（确保 `python_dir\` 可见）：

```powershell
py python_dir\shm_zmq_bridge_client.py --addr tcp://192.168.11.31:5566 --stream-id 1 --out-prefix C:\Temp\frame
```

#### C) 保存 PNG（OpenCV）

```powershell
py python_dir\shm_zmq_bridge_client_save_png.py --addr tcp://192.168.11.31:5566 --stream-id 1 --out C:\Temp\frame.png
```

#### D) 多端并发的关键点（必须读）

- **推荐在 31 上启动 bridge 时使用 `--pump-streams ...`**，客户端默认会优先走 `GET_LATEST_NV12`（缓存读取，不会写 shm 的 `request_seq`）。
- 如果你不 pump，而让每个客户端都走 `GET_SHM_NV12`（会触发 `request_seq++`），则多客户端同时拉同一路可能出现竞争/抖动。
- Windows 上如果你开了多进程/多机同时拉帧：保持 bridge `--workers 1` + `--pump-streams`，是当前最稳配置。

---

## 6. 端口与防火墙

需要连通：

- Producer → 31: `19000/tcp`（ml_worker → stream_server）
- Consumer → 31: `5566/tcp`（client → shm_zmq_bridge）

31 上检查监听：

```bash
ss -lntp | egrep ':(19000|5566)'
```

防火墙放通（按系统二选一）：

**ufw（Ubuntu 常见）**：

```bash
sudo ufw allow 19000/tcp
sudo ufw allow 5566/tcp
sudo ufw status
```

**firewalld（RHEL/CentOS 常见）**：

```bash
sudo firewall-cmd --permanent --add-port=19000/tcp
sudo firewall-cmd --permanent --add-port=5566/tcp
sudo firewall-cmd --reload
sudo firewall-cmd --list-ports
```

---

## 7. 多路 stream_id 管理（推荐模式）

### 7.1 约定

- 每路 producer 使用独立的 `STREAM_ID`（例如 1..N）
- `stream_server` 将对应发布：`/dev/shm/stream_server_stream_%02d`

### 7.2 推荐：bridge pump 多路，然后客户端 GET_LATEST_NV12

31 上（例：pump 1/2/3 三路）：

```bash
python python_dir/shm_zmq_bridge.py \
  --bind tcp://*:5566 \
  --workers 1 \
  --pump-streams 1,2,3
```

客户端只需指定 `--stream-id`：

```bash
python3 python_dir/shm_zmq_bridge_client.py --addr tcp://192.168.11.31:5566 --stream-id 2 --out-prefix /tmp/stream2
```

---

## 8. 常见问题（Troubleshooting）

### 8.1 client 连接不上 5566

- 31 上确认 bridge 在监听：`ss -lntp | grep ':5566'`
- 31 上确认防火墙放通 5566
- 确认客户端与 31 在同一网络可达

### 8.2 /dev/shm 没有 stream_server_stream_01

- 确认 `stream_server` 启动时带 `--enable-shm`（或兼容模式下 `ENABLE_SHM=1`）
- 确认 producer 已经开始推流到 `19000`
- 确认 `STREAM_ID` 和你检查的 `01` 一致（例如 stream_id=2 对应 `_02`）

### 8.3 多客户端拉帧抖动/超时

- 推荐用 `--pump-streams`，让 bridge 负责“唯一 request_seq 写入者”
- 客户端优先用 `GET_LATEST_NV12`（本 repo client 已默认优先）

### 8.4 性能/资源展示（常用命令）

```bash
# 31 上查看进程 CPU/MEM
ps -C stream_server -o pid,%cpu,%mem,rss,etime,cmd
ps -C python -o pid,%cpu,%mem,rss,etime,cmd | head

# 查看 systemd 日志
journalctl --user -u stream_server_19000.service -n 100 --no-pager
journalctl --user -u shm_zmq_bridge_5566.service -n 100 --no-pager
```

---

Doc-Version: 0.2.1
Repo-Rev: 797c719

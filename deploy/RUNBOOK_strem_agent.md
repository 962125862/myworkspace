# Runbook: strem_agent_server / strem_agent_client

本 runbook 介绍如何部署一个“远程代理”：

- 服务器侧 `strem_agent_server`：接收 `ml_worker` 推来的 H264(TLV/TCP)，对外输出 H264(TCP)，并代理键鼠控制（TCP→UDP）。
- 客户端侧 `strem_agent_client`：接收 H264 解码展示，并采集键鼠事件回传。

## 端口（默认）

- agent ingest（接 ml_worker）：`19000/tcp`
- agent video out（给 client）：`31234/tcp`
- agent control in（给 client）：`31235/tcp`
- worker control（ml_worker 监听）：`50001/udp`（仅 server 侧到 worker 侧，不跨公网）

## Late-join（晚加入）与 IDR

Sunshine 在某些设置/场景下可能不会周期性发送 IDR（关键帧）。如果客户端在推流开始较久后才连接，可能出现：

- 一直黑屏/无可解码帧
- PyAV 报 `Invalid data found when processing input`

为改善晚加入体验，本 repo 增加了 **REQ_IDR（请求关键帧）** 机制：

- `ml_worker` 新增控制命令：`ML_CTRL_CMD_REQ_IDR = 10`，收到后调用 `LiRequestIdrFrame()` 请求主机尽快发送 IDR。
- `strem_agent_server`：当有新的 video 订阅者（SUB）连接成功时，会自动向 `ml_worker` 的 UDP 控制端口发送一次 REQ_IDR。
- `strem_agent_client`：连接 ctrl 后也会主动发送一次 REQ_IDR（兜底）。

这意味着：即使只连接 video 端口（31234），server 也会尽力触发主机补一个 IDR，使晚加入能在几秒内起播。

## 1) Server 部署（Linux/31）

### 1.1 编译

```bash
cd /home/gejun/work/my_ml_work/strem_agent_server
make -j
ls -l build/strem_agent_server
```

### 1.2 运行

```bash
./build/strem_agent_server \
  --in-host 0.0.0.0 --in-port 19000 \
  --video-bind 0.0.0.0 --video-port 31234 \
  --ctrl-bind 0.0.0.0 --ctrl-port 31235 \
  --worker-ctrl-ip 127.0.0.1 --worker-ctrl-port 50001
```

如果需要 token（可选）：

```bash
./build/strem_agent_server --token your_token ...
```

### 1.3 防火墙

对公网开放：

- `31234/tcp`
- `31235/tcp`

（`19000/tcp` 是否开放取决于 ml_worker 是否在公网/内网）

## 2) worker（ml_worker）控制端口

`ml_worker` 侧需要启用 `--control-port`。

如果 agent_server 与 ml_worker 同机：默认 `--control-bind 127.0.0.1` 即可。

如果 agent_server 与 ml_worker 跨机：需要 `--control-bind 0.0.0.0` 并确保 50001/udp 可达。

## 3) Client（Windows/macOS/Linux）

### 3.1 安装

```bash
pip install -r strem_agent_client/requirements.txt
```

Windows:

```powershell
py -m pip install -r strem_agent_client\requirements.txt
```

### 3.2 运行

```bash
python strem_agent_client/strem_agent_client.py \
  --host <server_ip> --video-port 31234 --ctrl-port 31235 --stream-id 1
```

IDE 方式（Windows 直接点 Run，配置写死在文件顶部）：

```bash
python strem_agent_client/ide_demo_client.py
```

启用 token：

```bash
python strem_agent_client/strem_agent_client.py --host <server_ip> --token your_token
```

## 4) 快速排查

## 5) 本地一机自测（不依赖 GUI）

### 5.1 启动 strem_agent_server（本机）

```bash
make -C strem_agent_server -j

./strem_agent_server/build/strem_agent_server \
  --in-host 127.0.0.1 --in-port 19000 \
  --video-bind 127.0.0.1 --video-port 31234 \
  --ctrl-bind 127.0.0.1 --ctrl-port 31235 \
  --worker-ctrl-ip 127.0.0.1 --worker-ctrl-port 50001
```

### 5.2 配置 ml_worker 推到本机 agent

复制示例配置并修改 worker 名称：

```bash
cp -v deploy/workers/worker_local_agent.conf.example deploy/workers/worker_local_agent.conf
```

确保 `deploy/workers/worker_local_agent.conf`：

- `TCP_HOST=127.0.0.1`
- `TCP_PORT=19000`

启动：

```bash
./deploy/mlctl.sh up worker_local_agent
./deploy/mlctl.sh logs worker_local_agent
```

如果你修改了 `ml_worker` 源码（例如 REQ_IDR），需要重新构建镜像：

```bash
cmake --build build -j
./deploy/build_image.sh
./deploy/mlctl.sh restart worker_local_agent
```

### 5.3 headless 拉一帧保存 PNG（无窗口）

```bash
python3 strem_agent_client/strem_agent_client_save_png.py \
  --host 127.0.0.1 --video-port 31234 --stream-id 1 \
  --out /tmp/agent_frame.png
ls -l /tmp/agent_frame.png
```

### 4.1 只验证视频输出（不用 client）

说明：video 端口要求先发送 `SUB <stream_id>\n`（可选先 AUTH），所以 ffplay 不能直接连。
可以用本 repo 提供的 python client 保存 .h264 后再用 ffplay 播放。

```bash
python3 stream_server/tests/mock_h264_tap_client.py --host <server_ip> --port 31234 --stream 1 --out /tmp/agent_out.h264
ffplay -fflags nobuffer -flags low_delay -f h264 -i /tmp/agent_out.h264
```

---

Doc-Version: 0.2.0
Repo-Rev: 4e07aa7

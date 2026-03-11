# Runbook: strem_agent_server / strem_agent_client

本 runbook 介绍如何部署一个“远程代理”：

- 服务器侧 `strem_agent_server`：接收 `ml_worker` 推来的 H264(TLV/TCP)，对外输出 H264(TCP)，并代理键鼠控制（TCP→UDP）。
- 客户端侧 `strem_agent_client`：接收 H264 解码展示，并采集键鼠事件回传。

## 端口（默认）

- agent ingest（接 ml_worker）：`19000/tcp`
- agent video out（给 client）：`31234/tcp`
- agent control in（给 client）：`31235/tcp`
- worker control（ml_worker 监听）：`50001/udp`（仅 server 侧到 worker 侧，不跨公网）

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

启用 token：

```bash
python strem_agent_client/strem_agent_client.py --host <server_ip> --token your_token
```

## 4) 快速排查

### 4.1 只验证视频输出（不用 client）

```bash
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
  -f h264 tcp://<server_ip>:31234
```


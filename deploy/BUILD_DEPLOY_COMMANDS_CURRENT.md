# 当前有效的打包、构建、部署命令

本文只保留当前仓库里已经验证过、仍然有效的命令。

## 1. 本地构建

### 1.1 根项目

用于构建：

- `ml_worker`
- `strem_agent_server`

```bash
cd /home/gejun/work/my_ml_work
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja -j
```

只编 `ml_worker`：

```bash
cd /home/gejun/work/my_ml_work
cmake --build build-ninja -j --target ml_worker
```

### 1.2 stream_server

```bash
cd /home/gejun/work/my_ml_work/stream_server
cmake -S . -B build
cmake --build build -j
```

只编 `stream_server`：

```bash
cd /home/gejun/work/my_ml_work/stream_server
cmake --build build -j --target stream_server
```

## 2. 打包 Docker 镜像

当前脚本：

```bash
cd /home/gejun/work/my_ml_work
./deploy/build_image.sh
```

前置条件：

- 根项目已经编出 `build/ml_worker`

如果只编了 `build-ninja/ml_worker`，要先再补一次传统目录构建，或者调整脚本输入。

## 3. mlctl 部署命令

按 IP 列表批量生成 worker 和 `stream_server` 控制映射文件：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh batch-add-ips 192.168.11.150 192.168.11.151 192.168.11.152
```

从指定起始 `stream_id` 开始：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh batch-add-ips --start 5 192.168.11.150,192.168.11.151
```

启动 worker：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh up worker_150
```

重启 worker：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh restart worker_150
```

查看日志：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh logs worker_150
```

## 4. 当前直接运行命令

### 4.1 查看 192.168.11.31 上的正式 stream_server 服务

```bash
ssh 192.168.11.31 'systemctl cat stream_server_9000.service'
ssh 192.168.11.31 'systemctl show -p MainPID,SubState,ExecMainStartTimestamp stream_server_9000.service'
```

### 4.2 在 192.168.11.31 上重编并替换 `stream_server`

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work/stream_server && cmake --build build -j --target stream_server'
ssh 192.168.11.31 'pid=$(systemctl show -p MainPID --value stream_server_9000.service); kill -TERM "$pid"'
```

说明：

- 当前正式服务启用了 `Restart=always`
- 普通用户没有 `systemctl restart` 权限时，可用上面的 `SIGTERM` 方式让 systemd 自动拉起新进程

### 4.3 在 192.168.11.31 上前台启动等价命令

```bash
cd /home/gejun/work/my_ml_work/stream_server
./build/stream_server \
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

当前这条启动命令默认已经启用新逻辑，不需要额外加 CLI：

- `STREAM_DEFER_HW_DOWNLOAD=on`
- `STREAM_NVDEC_EXTRA_HW_FRAMES=24`

这里补充一条：

- `--max-streams 20`：实际启用 20 路流槽位
- `-c 30`：允许 30 个 TCP 连接，给重连抖动留余量
- 如果 `-c` 小于 `--max-streams`，程序会自动把连接数抬到 `--max-streams`

以后如果只是从 20 路扩到 35 路，优先改成：

```bash
--max-streams 35 -c 48
```

不需要再去改代码里的 `MAX_STREAMS` 重编；当前编译期硬上限已经预留到 `256`。

如果是 `systemd` 启动，想显式覆盖这两个值，可以在 unit 或 drop-in 里加：

```ini
Environment=STREAM_MAX_STREAMS=20
Environment=STREAM_DEFER_HW_DOWNLOAD=on
Environment=STREAM_NVDEC_EXTRA_HW_FRAMES=24
```

含义：

- `STREAM_DEFER_HW_DOWNLOAD=on`：硬解后默认保留 `last_frame` 的硬件帧引用，取图时再下载
- `STREAM_NVDEC_EXTRA_HW_FRAMES`：给 NVDEC/VAAPI 多留硬件 surface，避免保留 `last_frame` 时帧池被占满
- `STREAM_MAX_STREAMS`：运行时启用的流槽位数量；扩容时优先改它，而不是改代码重编
- 如果想恢复旧行为，再单独改成 `STREAM_DEFER_HW_DOWNLOAD=off` 和较小的 `STREAM_NVDEC_EXTRA_HW_FRAMES`

### 4.4 在 192.168.11.31 上启动 `ml_worker`

```bash
cd /home/gejun/work/my_ml_work
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
  --control-port 30001
```

## 4.5 当前代理链启动命令

如果你使用 `strem_agent_server`，当前推荐仍然走 `H264/420` 场景：

```bash
cd /home/gejun/work/my_ml_work
./build-ninja/ml_worker stream \
  --host 192.168.11.170 \
  --app Desktop \
  --key-dir /home/gejun/work/my_ml_work/deploy/data/keys_170 \
  --tcp-host 127.0.0.1 \
  --tcp-port 19000 \
  --stream-id 1 \
  --codec h264 \
  --chroma 420 \
  --control-bind 127.0.0.1 \
  --control-port 30001
```

```bash
cd /home/gejun/work/my_ml_work
./build-ninja/strem_agent_server \
  --in-host 0.0.0.0 \
  --in-port 19000 \
  --video-bind 0.0.0.0 \
  --video-port 31234 \
  --ctrl-bind 0.0.0.0 \
  --ctrl-port 31235 \
  --worker-ctrl-ip 127.0.0.1 \
  --worker-ctrl-port 30001
```

### 4.6 在 192.168.11.31 上保留 ml_worker 控制端口段

当前默认约定改为 `CONTROL_PORT=30000+STREAM_ID`，这样可以避开当前主机默认的临时端口范围 `32768-60999`。

如果你希望这段端口不再被内核自动分配为临时源端口，建议在 `192.168.11.31` 上执行：

```bash
ssh 192.168.11.31 'echo "net.ipv4.ip_local_reserved_ports = 30001-30256" | sudo tee /etc/sysctl.d/99-ml-worker-ports.conf'
ssh 192.168.11.31 'sudo sysctl --system'
```

说明：

- `30001-30256` 覆盖当前 `stream_id <= 256` 的默认控制端口范围
- 这不会阻止别的程序显式 `bind()` 到同一端口，但会阻止内核把它们当作临时端口自动分配

## 5. 当前远端同步命令

如果是从当前机器同步代码到 `192.168.11.31`：

按本次改动列出文件：

```bash
printf '%s\n' \
  README.md \
  PROGRESS.md \
  AGENTS.md \
  deploy/CHAIN_ml_worker_stream_server.md \
  deploy/ARCHITECTURE_CURRENT.md \
  deploy/CLI_REFERENCE_CURRENT.md \
  deploy/BUILD_DEPLOY_COMMANDS_CURRENT.md \
  deploy/BENCHMARK_stream_server_2026-04-13.md \
  stream_server/CODE_REVIEW.md \
  python_dir/zmq_multi_stream_bgr_perf.py \
| rsync -az --files-from=- /home/gejun/work/my_ml_work/ 192.168.11.31:/home/gejun/work/my_ml_work/
```

## 6. 当前排障命令

检查进程：

```bash
ssh 192.168.11.31 'pgrep -af "stream_server|ml_worker stream"'
```

检查端口：

```bash
ssh 192.168.11.31 'ss -lntp | egrep ":(9000|5566)\b"; ss -lunp | egrep ":19000\b"'
```

看日志：

```bash
ssh 192.168.11.31 'journalctl -u stream_server_9000.service -n 120 --no-pager'
ssh 192.168.11.31 'tail -n 120 /tmp/ml_worker_170_remote.log'
```

20 路 IPC BGR benchmark：

```bash
ssh 192.168.11.31 'source /home/my_server/bin/activate && cd /home/gejun/work/my_ml_work && python python_dir/zmq_multi_stream_bgr_perf.py --addr ipc:///tmp/stream_server_bgr.sock --streams 1-20 --fps 30 --duration-sec 30'
```

20 路离线硬解 benchmark：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work/stream_server && ./build/test_20streams_hw intel /home/gejun/work/my_ml_work/test_optimized.h264'
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work/stream_server && ./build/test_20streams_hw nvidia /home/gejun/work/my_ml_work/test_optimized.h264'
```

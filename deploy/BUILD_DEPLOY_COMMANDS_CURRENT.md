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

### 4.1 在 192.168.11.31 上启动 stream_server

```bash
cd /home/gejun/work/my_ml_work/stream_server
./build/stream_server \
  -h 0.0.0.0 \
  -p 9000 \
  -c 30 \
  --zmq-bridge-bind tcp://0.0.0.0:5566 \
  --ml-worker-ctrl-map-file /home/gejun/work/my_ml_work/deploy/stream_server_ctrl_map.txt \
  -v
```

```angular2html
for f in workers/worker_*.conf; do     ./mlctl.sh up "$(basename "$f" .conf)" 0000;   done

```




### 4.2 在 192.168.11.31 上启动 ml_worker

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
  --control-port 50001
```

## 4.3 当前代理链启动命令

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
  --control-port 50001
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
  --worker-ctrl-port 50001
```

## 5. 当前远端同步命令

如果是从当前机器同步代码到 `192.168.11.31`：

同步少量改动文件：

```bash
printf '%s\n' \
  deploy/mlctl.sh \
  deploy/CLI_REFERENCE_CURRENT.md \
  deploy/BUILD_DEPLOY_COMMANDS_CURRENT.md \
  src/main.c \
  src/tcp_sender.c \
  src/video_callbacks.c \
  include/tcp_sender.h \
  include/video_callbacks.h \
  stream_server/include/protocol.h \
  stream_server/include/stream.h \
  stream_server/include/mlctl_cmd.h \
  stream_server/src/protocol.c \
  stream_server/src/server.c \
  stream_server/src/stream.c \
  stream_server/src/zmq_bridge.c \
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
ssh 192.168.11.31 'tail -n 120 /tmp/my_ml_work_stream_server.log'
ssh 192.168.11.31 'tail -n 120 /tmp/ml_worker_170_remote.log'
```

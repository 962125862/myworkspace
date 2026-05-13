# 当前主链路运维手册

本文是当前正式主链路的统一入口，合并原来的使用、维护、构建和部署命令说明。

不覆盖：

- `strem_agent_server/client` 代理链路
- `agent_link_service` 按需启动链路
- `web_webrtc` 实验链路

这些链路见本文末尾的关联文档。

## 1. 当前正式链路

```text
Sunshine Host
  -> ml_worker (20 路 Docker 容器)
  -> 192.168.11.31:9000 stream_server
  -> ZMQ bridge
  -> tcp://192.168.11.31:5566 / ipc:///tmp/stream_server_bgr.sock
```

正式主机：

- `192.168.11.31`

关键进程：

- `stream_server_9000.service`
- 20 个 `mlw-worker_<ip最后一段>` Docker 容器

关键路径：

- 仓库目录：`/home/gejun/work/my_ml_work`
- worker 配置：`deploy/workers/*.conf`
- worker 数据和 key：`deploy/data/`
- 控制映射：`deploy/stream_server_ctrl_map.txt`
- 巡检脚本：`deploy/check_stream_workers_health.sh`

端口：

- `9000/tcp`：`ml_worker -> stream_server` TLV 推流
- `5566/tcp`：ZMQ BGR bridge
- `ipc:///tmp/stream_server_bgr.sock`：本机 ZMQ BGR bridge
- `30001-30020/udp`：主链路 worker 控制口

正常判定口径：

- 20 个主链路容器都处于 `running`
- 最近窗口内持续出现 `[video] fps=... state=CONNECTED`
- `received_fps > 0`
- `sent_fps > 0`

不要只看 Docker `Up`，`Up` 只代表容器主进程还活着。

## 2. 日常巡检

一键巡检：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work && ./deploy/check_stream_workers_health.sh'
```

常用放宽参数：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work && ML_HEALTH_LOG_WINDOW=30s ML_HEALTH_MIN_SAMPLES=2 ./deploy/check_stream_workers_health.sh'
```

检查 `stream_server`：

```bash
ssh 192.168.11.31 'systemctl show -p MainPID,SubState,ExecMainStartTimestamp stream_server_9000.service'
```

查看单路日志：

```bash
ssh 192.168.11.31 'docker logs --since 60s mlw-worker_170 2>&1 | tail -n 120'
```

查看 20 路容器：

```bash
ssh 192.168.11.31 'docker ps -a --format "{{.Names}}\t{{.Status}}" | grep "^mlw-worker_" | sort'
```

验证 GHCR 当前镜像：

```bash
ssh 192.168.11.31 'docker manifest inspect ghcr.io/962125862/myworkspace/ml-worker:latest | grep -m1 "sha256:db3eb406"'
```

## 3. Worker 操作

进入仓库：

```bash
cd /home/gejun/work/my_ml_work
```

启动单路：

```bash
./deploy/mlctl.sh up worker_170
```

重启单路：

```bash
./deploy/mlctl.sh restart worker_170
```

查看单路日志：

```bash
./deploy/mlctl.sh logs worker_170
```

重新配对：

```bash
./deploy/mlctl.sh pair worker_170
```

重新生成控制映射：

```bash
./deploy/mlctl.sh gen-ctrl-map
```

批量生成 worker：

```bash
./deploy/mlctl.sh batch-add-ips 192.168.11.150 192.168.11.151 192.168.11.152
```

从指定 `stream_id` 开始：

```bash
./deploy/mlctl.sh batch-add-ips --start 5 192.168.11.150,192.168.11.151
```

### 3.1 Docker 日志轮转

`mlctl.sh up/restart` 创建 worker 容器时默认启用 Docker `json-file` 日志轮转：

- `max-size=20m`
- `max-file=2`
- 单个 `mlw-worker_*` 容器最多约保留 `40MB`

临时调小：

```bash
ML_WORKER_LOG_MAX_SIZE=10m ML_WORKER_LOG_MAX_FILE=2 ./deploy/mlctl.sh restart worker_170
```

注意：

- Docker LogConfig 只在创建容器时生效
- 单纯 `docker restart` 不会更新日志轮转参数
- 需要用 `./deploy/mlctl.sh restart <worker>` 删除旧容器并重建

## 4. 构建

根项目：

```bash
cd /home/gejun/work/my_ml_work
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja -j
```

只编 `ml_worker`：

```bash
cmake --build build-ninja -j --target ml_worker
```

`stream_server`：

```bash
cd /home/gejun/work/my_ml_work/stream_server
cmake -S . -B build
cmake --build build -j --target stream_server
```

Docker 镜像：

```bash
cd /home/gejun/work/my_ml_work
cmake --build build -j --target ml_worker
./deploy/build_image.sh
```

`deploy/build_image.sh` 默认读取 `build/ml_worker`，不是 `build-ninja/ml_worker`。

## 5. 发布 `ml_worker` 镜像

当前 GHCR 镜像：

- `ghcr.io/962125862/myworkspace/ml-worker:latest`

发布命令：

```bash
docker login ghcr.io
docker tag ml-worker:latest ghcr.io/962125862/myworkspace/ml-worker:latest
docker push ghcr.io/962125862/myworkspace/ml-worker:latest
docker logout ghcr.io
```

登录 token 需要 `write:packages` 权限。

推送后验证：

```bash
docker manifest inspect ghcr.io/962125862/myworkspace/ml-worker:latest | grep -m1 'sha256:'
```

当前已发布版本：

- push digest：`sha256:76f388cddc9a4d549cdc77a041a285cd7c3d20f0298809d820a5464c23c6cf9e`
- manifest config digest：`sha256:db3eb40696018b906ff6f2f38ef565d11656bc3cafa6212c09c436664437ee5e`

## 6. 正式机更新

正式机现实约束：

- `/home/gejun/work/my_ml_work` 不是 git 工作树
- 普通用户不一定有 `systemctl restart` 权限
- 当前更新仍主要靠 `rsync`、远端编译和重建 Docker 容器

### 6.1 更新 stream_server

同步改动文件后：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work/stream_server && cmake --build build -j --target stream_server'
ssh 192.168.11.31 'pid=$(systemctl show -p MainPID --value stream_server_9000.service); kill -TERM "$pid"'
```

依赖：

- `stream_server_9000.service` 配了 `Restart=always`

### 6.2 更新 ml_worker

如果正式机本地编译：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work && cmake --build build -j --target ml_worker && ./deploy/build_image.sh'
```

如果需要避免 Dockerfile 拉基础镜像，可以基于现有镜像热替换 `/app/ml_worker`：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work && \
  cmake --build build -j --target ml_worker && \
  rm -f deploy/image/ml_worker && rm -rf deploy/image/lib && mkdir -p deploy/image/lib && \
  cp -av build/ml_worker deploy/image/ml_worker && chmod +x deploy/image/ml_worker && \
  ldd build/ml_worker | awk -v root="$PWD/" '"'"'$3 ~ "^" root { print $3 }'"'"' | sort -u | while read -r lib; do cp -Lv "$lib" deploy/image/lib/; done && \
  cid=$(docker create ml-worker:latest) && \
  docker cp deploy/image/ml_worker "$cid":/app/ml_worker && \
  docker cp deploy/image/lib/. "$cid":/app/lib/ && \
  docker commit "$cid" ml-worker:latest >/tmp/ml_worker_commit_id && \
  docker rm "$cid" >/dev/null && \
  cat /tmp/ml_worker_commit_id'
```

重建 20 个 worker 容器：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work && for w in $(ls deploy/workers/*.conf | xargs -n1 basename | sed "s/\\.conf$//" | sort); do ./deploy/mlctl.sh restart "$w" || true; done'
```

重建后检查：

```bash
ssh 192.168.11.31 'for n in $(docker ps -a --format "{{.Names}}" | grep -E "^mlw-worker_[0-9]+$" | sort); do docker logs --since 2m "$n" 2>&1 | grep -E "fatal_code=|negotiated video format mismatch|video worker ready" | tail -2 | sed "s/^/$n: /"; done'
```

## 7. 常见故障

### 7.1 `container_count_mismatch`

含义：

- 主链路容器数不是 `20`

先查：

```bash
ssh 192.168.11.31 'docker ps -a --format "{{.Names}}\t{{.Status}}" | grep "^mlw-worker" | sort'
```

### 7.2 `video_ok=0/N`

含义：

- 容器活着，但最近窗口里没有稳定 `[video]` 输出

先看：

```bash
ssh 192.168.11.31 'docker logs --since 60s <container> 2>&1 | tail -n 120'
```

重点失败词：

- `host ... offline`
- `gs_init failed`
- `host is not paired for this key directory`
- `could not resolve app:`
- `gs_start_app failed`
- `LiStartConnection failed:`
- `control socket open failed`
- `fatal_code=`
- `tcp_sender: connect failed`
- `tcp_sender: attempting to reconnect`

`fatal_code=8` 表示请求的视频格式和 Sunshine/Limelight 实际协商结果不一致，例如配置要求 `HEVC 4:4:4` 但实际退成 `HEVC 4:2:0`。`ml_worker` 会先等待 `60s` 再退出，让 Docker 自动重启最多按分钟级重试。

### 7.3 `control socket open failed`

通常是 UDP 控制口冲突。当前主链路使用：

- `30001-30020`

优先检查：

- worker 配置是否改回旧端口
- 是否有额外进程显式绑定同一 UDP 口

### 7.4 `tcp_sender: attempting to reconnect`

含义：

- `ml_worker` 到 `stream_server:9000` 的下游推流连接断开或发送失败

先查：

```bash
ssh 192.168.11.31 'ss -ltnp | grep ":9000 "'
ssh 192.168.11.31 'systemctl show -p MainPID,SubState stream_server_9000.service'
```

## 8. ZMQ BGR 请求

`GET_LATEST_BGR` 当前返回 `BGR24`。

完整帧请求：

```json
{"stream_id":1,"timeout_ms":1000}
```

ROI 请求：

```json
{"stream_id":1,"timeout_ms":1000,"roi":{"x":100,"y":100,"w":320,"h":240}}
```

说明：

- ROI 是输出裁剪，解码仍按整帧进行
- ROI payload 是 tight `BGR24`，可以显著降低 ZMQ/IPC 带宽
- `request_new` 是 legacy ignored 字段，客户端不需要再传

## 9. 关联文档

当前主链路：

- `deploy/CHAIN_ml_worker_stream_server.md`
- `deploy/ARCHITECTURE_CURRENT.md`
- `deploy/CLI_REFERENCE_CURRENT.md`
- `deploy/BENCHMARK_stream_server_2026-04-13.md`
- `PROGRESS.md`

代理/按需启动链路：

- `deploy/RUNBOOK_strem_agent.md`
- `deploy/RUNBOOK_agent_link_service.zh-CN.md`
- `deploy/RUNBOOK_stream_service_tcp_gate.zh-CN.md`
- `deploy/agent_stack_runtime/README.md`

子模块：

- `stream_server/BUILD_GUIDE.md`
- `stream_server/CODE_REVIEW.md`
- `strem_agent_client/README.md`
- `web_webrtc/README.md`

---

Doc-Version: 1.0.0
Repo-Rev: f3e2e97

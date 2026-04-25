# 当前项目使用手册

本文面向当前正式使用中的主链路：

```text
Sunshine Host
  -> ml_worker (20 路 Docker 容器)
  -> 192.168.11.31:9000 stream_server
  -> ZMQ bridge
  -> tcp://192.168.11.31:5566 / ipc:///tmp/stream_server_bgr.sock
```

不包含 `strem_agent_server` 的代理链路使用说明。

## 1. 当前正式环境

- 主机：`192.168.11.31`
- 主服务：`stream_server_9000.service`
- 上游推流端口：`9000/tcp`
- ZMQ TCP：`5566/tcp`
- 上游控制口：`30001-30020/udp`
- 20 路容器命名：`mlw-worker_<ip最后一段>`

当前这 20 路的正常判定口径：

- 容器处于 `running`
- 最近窗口内持续出现 `[video] fps=... state=CONNECTED`
- `received_fps > 0`
- `sent_fps > 0`

## 2. 日常巡检

### 2.1 一键检查 20 路容器

仓库内脚本：

```bash
cd /home/gejun/work/my_ml_work
./deploy/check_stream_workers_health.sh
```

远端常用放置路径：

```bash
~/check/check_stream_workers_health.sh
```

输出规则：

- 全部正常：`ok`
- 任意一路异常：`error`，并逐行列出异常容器和原因

常用参数：

```bash
ML_HEALTH_LOG_WINDOW=15s ML_HEALTH_MIN_SAMPLES=3 ~/check/check_stream_workers_health.sh
```

说明：

- 默认只检查 `^mlw-worker_[0-9]+$`
- 默认要求命中 `20` 个容器
- 如果后续命名规则或路数变了，再用环境变量覆盖

### 2.2 检查 stream_server 服务

```bash
ssh 192.168.11.31 'systemctl show -p MainPID,SubState,ExecMainStartTimestamp stream_server_9000.service'
```

正常预期：

- `SubState=running`
- `MainPID` 非空

### 2.3 看单路日志

例如查看 `192.168.11.170` 那一路：

```bash
ssh 192.168.11.31 'docker logs --since 30s mlw-worker_170 2>&1 | tail -n 80'
```

正常日志特征：

- `tcp_sender: connected to 127.0.0.1:9000`
- `tcp_sender: stream_start sent`
- `streaming started, press Ctrl+C to stop`
- 持续的 `[video] fps=... state=CONNECTED`

### 2.4 看全部容器状态

```bash
ssh 192.168.11.31 'docker ps -a --format "{{.Names}}\t{{.Status}}" | grep "^mlw-worker_" | sort'
```

## 3. 常见日常操作

### 3.1 新增或修改 worker 配置

配置文件在：

- `deploy/workers/*.conf`

批量生成示例：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh batch-add-ips 192.168.11.150 192.168.11.151 192.168.11.152
```

### 3.2 启动或重启单路

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh up worker_170
./deploy/mlctl.sh restart worker_170
```

补充说明：

- 当前线上容器长期运行后，不依赖 `mlctl` 常驻
- `mlctl` 主要用于生成配置、配对、启动和重启

### 3.3 重新配对

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh pair worker_170
```

### 3.4 重新生成控制映射

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh gen-ctrl-map
```

生成文件：

- `deploy/stream_server_ctrl_map.txt`

## 4. 正常与异常的快速判断

可以直接按下面口径判断：

- `ok`
  - 20 路容器都在
  - 巡检脚本输出 `ok`
- `error`
  - 少容器
  - 某一路不是 `running`
  - 最近 `15s` 内没有足够的 `[video]` 统计
  - 出现重连或失败日志

重点不是看容器是不是 `Up`，而是看有没有持续的 `[video] fps=... state=CONNECTED`。

## 5. 进一步参考

- 主链路说明：`deploy/CHAIN_ml_worker_stream_server.md`
- 架构说明：`deploy/ARCHITECTURE_CURRENT.md`
- 参数说明：`deploy/CLI_REFERENCE_CURRENT.md`
- 构建/部署命令：`deploy/BUILD_DEPLOY_COMMANDS_CURRENT.md`
- 项目状态：`PROGRESS.md`

---

Doc-Version: 1.0.0
Repo-Rev: fcc4282

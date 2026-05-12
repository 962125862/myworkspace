# 当前项目维护手册

本文面向当前正式运行的 20 路主链路维护工作，重点覆盖：

- `ml_worker` 容器健康判断
- `stream_server` 服务维护
- 配置变更
- 常见故障排查

不覆盖 `strem_agent_server` 代理链的日常运维。

## 1. 当前维护对象

### 1.1 正式主机

- `192.168.11.31`

### 1.2 关键进程

- `stream_server_9000.service`
- 20 个 `mlw-worker_<ip最后一段>` Docker 容器

### 1.3 关键路径

- 仓库目录：`/home/gejun/work/my_ml_work`
- worker 配置：`/home/gejun/work/my_ml_work/deploy/workers`
- 控制映射：`/home/gejun/work/my_ml_work/deploy/stream_server_ctrl_map.txt`
- 巡检脚本建议放置：`~/check/check_stream_workers_health.sh`

### 1.4 当前端口分工

- `9000/tcp`
  - `ml_worker -> stream_server` TLV 推流
- `5566/tcp`
  - ZMQ bridge
- `30001-30020/udp`
  - 主链路 20 路 `ml_worker` 控制口
- `40120-40122/tcp`
  - 这是 `agent_link_service` 外网入口，不属于本手册的主维护对象

## 2. 标准巡检动作

### 2.1 先跑一键巡检

```bash
ssh 192.168.11.31 '~/check/check_stream_workers_health.sh'
```

输出解释：

- `ok`
  - 当前 20 路容器都符合稳定出流条件
- `error`
  - 后续每一行就是需要处理的对象

### 2.2 再看 stream_server 服务

```bash
ssh 192.168.11.31 'systemctl show -p MainPID,SubState,ExecMainStartTimestamp stream_server_9000.service'
```

### 2.3 最后看异常容器日志

```bash
ssh 192.168.11.31 'docker logs --since 60s mlw-worker_170 2>&1 | tail -n 120'
```

### 2.4 Docker 日志保留策略

主链路 worker 容器由 `deploy/mlctl.sh` 创建时默认启用 Docker `json-file` 日志轮转：

- `max-size=20m`
- `max-file=2`
- 单个 `mlw-worker_*` 容器最多约保留 `40MB`

如果要调小：

```bash
ML_WORKER_LOG_MAX_SIZE=10m ML_WORKER_LOG_MAX_FILE=2 ./deploy/mlctl.sh restart worker_170
```

注意：

- Docker LogConfig 只在创建容器时生效
- 单纯 `docker restart` 不会更新日志轮转参数
- 需要用 `./deploy/mlctl.sh restart <worker>` 删除旧容器并重建

## 3. 巡检脚本判定逻辑

当前脚本的默认规则是：

- 容器名匹配 `^mlw-worker_[0-9]+$`
- 命中数量必须等于 `20`
- 容器状态必须是 `running`
- 最近 `15s` 内至少有 `3` 条 `[video] fps=... state=CONNECTED`
- 每条样本都要求 `received_fps > 0` 且 `sent_fps > 0`

如果要临时放宽或调整：

```bash
ML_HEALTH_LOG_WINDOW=30s \
ML_HEALTH_MIN_SAMPLES=2 \
ML_HEALTH_EXPECTED_COUNT=20 \
~/check/check_stream_workers_health.sh
```

如果以后命名规则改了：

```bash
ML_HEALTH_NAME_REGEX='^mlw-worker_.+$' ~/check/check_stream_workers_health.sh
```

## 4. 常见故障与处理口径

### 4.1 `container_count_mismatch`

含义：

- 当前宿主机上匹配到的主链路容器数不是 `20`

先查：

```bash
ssh 192.168.11.31 'docker ps -a --format "{{.Names}}\t{{.Status}}" | grep "^mlw-worker" | sort'
```

常见原因：

- 某个容器被删了
- 命名规则变了
- 多了不属于 20 路主链路的测试容器

### 4.2 `video_ok=0/N`

含义：

- 容器活着，但最近窗口里没有形成稳定的 `[video]` 输出

先看：

```bash
ssh 192.168.11.31 'docker logs --since 60s <container> 2>&1 | tail -n 120'
```

重点看这些失败词：

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

### 4.3 `control socket open failed`

当前这类问题最常见来源是控制口冲突。

当前主链路已经统一使用低位端口：

- `30001-30020`

如果再次出现，优先检查：

- 某个配置是否被改回了旧端口
- 是否有额外进程显式绑定了同一个 UDP 口

### 4.4 `tcp_sender: attempting to reconnect`

含义：

- `ml_worker` 到 `stream_server:9000` 的下游推流连接出现断开或发送错误

先查：

```bash
ssh 192.168.11.31 'ss -ltnp | grep ":9000 "'
ssh 192.168.11.31 'systemctl show -p MainPID,SubState stream_server_9000.service'
```

### 4.5 只有 `Up`，但没有 `[video] fps`

这不算正常。

原因是：

- 容器 `Up` 只代表主进程还活着
- `ml_worker` 可能正在容器里等待主机恢复
- 也可能已经会话起来，但下游推流没有稳定

所以一律以 `[video] fps=... state=CONNECTED` 为准。

## 5. 配置变更流程

### 5.1 修改 worker 配置

改动位置：

- `deploy/workers/*.conf`

典型字段：

- `HOST`
- `APP`
- `TCP_HOST`
- `TCP_PORT`
- `STREAM_ID`
- `CONTROL_BIND`
- `CONTROL_PORT`

### 5.2 修改控制映射

如果有新增、删减或 `CONTROL_PORT` 变化，需要同步更新：

- `deploy/stream_server_ctrl_map.txt`

可直接重生：

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh gen-ctrl-map
```

### 5.3 修改镜像或代码

推荐顺序：

1. 本地改仓库
2. 本地构建/语法检查
3. 提交并推送 git
4. 构建并推送镜像
5. 再同步到正式机

## 6. 正式机更新注意事项

当前正式机有两个现实约束：

- `/home/gejun/work/my_ml_work` 不是 git 工作树
- 普通用户不一定有 `systemctl restart` 权限

所以 `stream_server` 当前常用更新方式是：

```bash
ssh 192.168.11.31 'cd /home/gejun/work/my_ml_work/stream_server && cmake --build build -j --target stream_server'
ssh 192.168.11.31 'pid=$(systemctl show -p MainPID --value stream_server_9000.service); kill -TERM "$pid"'
```

依赖点：

- service 配了 `Restart=always`

## 7. 建议的维护顺序

出现异常时，建议固定按这个顺序处理：

1. 跑巡检脚本，确认异常范围
2. 看 `stream_server_9000.service`
3. 看对应容器最近 `60s` 日志
4. 判断是主机侧问题、配对问题、控制口问题，还是下游 `9000` 问题
5. 单路修复后再回跑巡检脚本

---

Doc-Version: 1.0.1
Repo-Rev: be36af2

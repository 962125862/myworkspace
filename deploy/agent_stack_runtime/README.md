# agent_stack_runtime（无需源码）

这个目录是一个最小运行包：用户不需要 clone 全仓库源码。

它会拉起两个服务（直接 pull GHCR 镜像）：

- `ghcr.io/962125862/myworkspace/strem-agent-server:latest`
- `ghcr.io/962125862/myworkspace/agent-link-service:latest`

## Quick Start

1) 复制示例配置：

```bash
cp -v config/strem_agent_server.env.example config/strem_agent_server.env
cp -v config/agent_link_service.env.example config/agent_link_service.env
```

2) 编辑两个配置文件，确保 token 一致：

- `config/strem_agent_server.env`: `AGENT_TOKEN=...`
- `config/agent_link_service.env`: `AGENT_LINK_TOKEN=...`

3) 启动：

```bash
./oneclick_up.sh
docker compose logs -f --tail 200 agent_link_service
```

4) 第一次需要配对（可选一键脚本）：

```bash
# 自动 PIN（命令行会打印 PIN，你去 Sunshine 主机端输入）
./oneclick_pair.sh <SUNSHINE_IP> 1

# 或指定 PIN=1234
./oneclick_pair.sh <SUNSHINE_IP> 1 1234
```

停止：

```bash
./oneclick_down.sh
```

## 第一次配对（ml_worker）

`ml_worker` 的配对无法完全自动化，因为 PIN 必须去 Sunshine 主机端手动输入。

在 stack 启动后，在 `agent_link_service` 容器里执行：

```bash
# 创建一个 worker 配置（示例：stream=1 推到本机 strem_agent_server 的 19000）
docker exec -it agent_link_service bash -lc '/app/mlctl.sh add worker_s1 <SUNSHINE_IP> Desktop "" "" 127.0.0.1 19000 1'

# 配对：终端会打印 PIN，你需要去 Sunshine 主机端输入这个 PIN
docker exec -it agent_link_service bash -lc '/app/mlctl.sh pair worker_s1'

# 可选：自定义 PIN（必须 4 位数字）。仍然需要去 Sunshine 主机端输入同样的 PIN
docker exec -it agent_link_service bash -lc '/app/mlctl.sh pair worker_s1 1234'
```

配对产生的 keys 以及 worker 配置会保存在 docker volumes（不会因为容器重启丢失）：

- `agent_stack_workers` -> `/app/workers`
- `agent_stack_data` -> `/app/data`

约定（建议遵守，避免混乱）：

- `worker_sN.conf` 里配置 `STREAM_ID="N"`（文件名和 STREAM_ID 一致）
- `TCP_PORT` 推荐固定为 `19000`（推流到 strem_agent_server ingest）
- `CONTROL_PORT` 推荐为 `50000+STREAM_ID`（例如 stream 1 -> 50001）

## 串流分辨率/帧率/码率怎么配？

这些参数是 **ml_worker 的视频编码参数**，推荐按 worker 配置（按 stream）设置。

另外也支持在 `agent_link_service` 容器的 env 里配置“全局默认值”（仅当 worker 配置没写对应字段时才生效）：

- `ML_WORKER_DEFAULT_WIDTH/HEIGHT/FPS/BITRATE/...`（见 `config/agent_link_service.env.example`）

worker 配置文件在容器内：

- `/app/workers/<worker>.conf`（持久化在 volume `agent_stack_workers`）

常用字段（示例）：

- `WIDTH="1920"`
- `HEIGHT="1080"`
- `FPS="60"`
- `BITRATE="20000"`  (kbps)
- `PACKET_SIZE="1024"`

查看当前 worker 配置：

```bash
docker exec -it agent_link_service bash -lc 'ls -la /app/workers; echo; sed -n "1,120p" /app/workers/worker_s1.conf'
```

修改后重启对应 worker（让 ml_worker 重新按新参数启动）：

```bash
docker exec -it agent_link_service bash -lc '/app/mlctl.sh restart worker_s1'
```

## startLink

配对成功后，再调用：

- `POST http://127.0.0.1:40120/startLink?...`（带 HMAC 签名参数；签名规则见 `run_docker_config.md`）

然后客户端连接到：

- video gate: `40121`
- ctrl gate: `40122`

## 一键脚本参数

- `./oneclick_up.sh --fresh`：等价于先 `docker compose down -v`，再启动（清空 workers/keys）
- `./oneclick_up.sh --no-pull`：启动时不 `docker compose pull`（离线/不想拉镜像时用）
- `./oneclick_down.sh -v`：down 时同时删除 volumes（清空 workers/keys）

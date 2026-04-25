# agent_stack_runtime（无需源码）

这个目录是一个最小运行包：用户不需要 clone 全仓库源码。

默认会拉起两个服务（直接 pull GHCR 镜像）：

- `ghcr.io/962125862/myworkspace/strem-agent-server:latest`
- `ghcr.io/962125862/myworkspace/agent-link-service:latest`

## Quick Start

1) 准备配置文件（第一次运行时 `./oneclick_up.sh` 也会自动创建）：

```bash
cp -v config/strem_agent_server.env.example config/strem_agent_server.env
cp -v config/agent_link_service.env.example config/agent_link_service.env
```

2) 编辑两个配置文件，确保 token 一致，并且不要用默认值：

- `config/strem_agent_server.env`: `AGENT_TOKEN=...`
- `config/agent_link_service.env`: `AGENT_LINK_TOKEN=...`
- `config/agent_link_service.env`: `AGENT_LINK_SK=...`（/startLink HMAC 签名用，推荐开启；脚本默认会拒绝 `change-me-sk`）

3) 启动：

```bash
./oneclick_up.sh
docker compose logs -f --tail 200 agent_link_service
```

如果本机 docker socket 需要 root 权限，请用：

```bash
sudo ./oneclick_up.sh
```

如果你要跑本地 `agent-link-service:local` 覆盖版，显式加 `--local`，或者设置 `AGENT_STACK_USE_LOCAL=1`。

4) 第一次需要配对（可选一键脚本）：

```bash
# 自动 PIN（命令行会打印 PIN，你去 Sunshine 主机端输入）
./oneclick_pair.sh <SUNSHINE_IP> --stream-id 1

# 也可以继续用老的 positional 写法
./oneclick_pair.sh <SUNSHINE_IP> 1

# 或指定 PIN=1234
./oneclick_pair.sh <SUNSHINE_IP> --stream-id 1 --pin 1234

# 如果同机上还有另一套 ml_worker 已经占用了 stream 1 的默认控制端口，可显式改到其他低位端口（例如 30100）
./oneclick_pair.sh <SUNSHINE_IP> --stream-id 1 --control-port 30100
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
- `CONTROL_PORT` 推荐为 `30000+STREAM_ID`（例如 stream 1 -> 30001）；如果你在 `worker_sN.conf` 里手写了 `CONTROL_PORT`，`oneclick_pair.sh` 会优先沿用它
- 如果同机上已经有另一套 `stream_id=1` 的 worker 在用 `30001`，请显式改成别的低位专用端口，例如 `30100`
- 目前 `strem_agent_server` 只接受 `stream_id=1..20`，更大的编号会在配对前直接报错；如果要扩展上限，需要先改 server 端协议限制

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

例如，把现有 `worker_s21` 改成 `2560x1440` 并立即重启：

```bash
ssh 192.168.11.31 'docker exec agent_link_service bash -lc "sed -i -e \"s/^WIDTH=.*/WIDTH=\\\"2560\\\"/\" -e \"s/^HEIGHT=.*/HEIGHT=\\\"1440\\\"/\" /app/workers/worker_s21.conf && /app/mlctl.sh restart worker_s21"'
```

## startLink

配对成功后，客户端侧需要先调用 `startLink`（它会按需启动对应 `stream_id` 的 `ml_worker` 容器推流）：

- 同机调用可用 `127.0.0.1`
- 其他机器调用请替换成运行 stack 的机器 IP（例如 `<31_ip>`）

请求参数（query）：

- `stream`: stream id（例如 `1`）
- `call_name`: 固定为 `startLink`
- `ts`: Unix epoch seconds（例如 `date -u +%s`）
- `nonce`: 随机字符串
- `sig`: `HMAC_SHA256(AGENT_LINK_SK, "call_name=startLink&stream=<stream>&ts=<ts>&nonce=<nonce>")` 的 hex（小写）

示例：

- `POST http://<31_ip>:40120/startLink?stream=1&call_name=startLink&ts=...&nonce=...&sig=...`

然后客户端连接到：

- video gate: `40121`
- ctrl gate: `40122`

## 一键脚本参数

- `./oneclick_up.sh --local`：改用本地 `docker-compose.local.yml` 覆盖
- `./oneclick_up.sh --fresh`：等价于先 `docker compose down -v`，再启动（清空 workers/keys）
- `./oneclick_up.sh --no-pull`：启动时不 `docker compose pull`（离线/不想拉镜像时用）
- `./oneclick_up.sh --dry-run`：只打印本次启动计划，不真正 `pull/up`
- `./oneclick_down.sh --local`：down 时同样使用本地覆盖文件
- `./oneclick_down.sh -v`：down 时同时删除 volumes（清空 workers/keys）

## 关于 GHCR 加速

GHCR 没有 Docker Hub 那种统一的 `registry-mirrors` 机制可直接套用。
如果你在网络较差的环境里需要加速，通常只有两种实用做法：

- 用你自己的 registry proxy / mirror，把镜像重定向到那个仓库，再改 `image:` 前缀
- 先在能联网的机器 `docker pull`，再 `docker save` / `docker load`

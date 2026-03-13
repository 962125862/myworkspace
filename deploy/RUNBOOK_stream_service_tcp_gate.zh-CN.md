# Runbook: stream_service TCP Gate (Token + On-demand ml_worker by stream_id)

目标：

- 提供一个独立的 Python TCP 入口（gate/proxy），客户端连接时必须带 token。
- token 通过 HTTP `/token` 触发生成，并通过 webhook 推送到钉钉（或任意 HTTP endpoint）。
- TCP 连接通过后，根据 `SUB <stream_id>` 自动判断对应 `ml_worker` 容器是否已启动：已启动不动；未启动则启动。
- TCP 断开后，若该 `stream_id` 无任何活跃连接，则延迟一段时间自动 stop（保留容器，下次快速 start）。

本组件与 `web_webrtc` 无耦合，可单独部署。

## 0) 前提

- 本机可执行 `docker`，并且已配置好 `deploy/workers/*.conf`（每个文件里有 `STREAM_ID="1"` 这类字段）。
- `strem_agent_server` 已运行并监听 video 端口（默认 `31234`）。
- `deploy/mlctl.sh` 已包含 `ensure-up` / `stop-soft`（本 repo 已补齐）。

## 1) 启动服务

建议先用环境变量配置（例子）：

```bash
cd /home/gejun/work/my_ml_work

export STREAM_GATE_TCP_BIND=0.0.0.0
export STREAM_GATE_TCP_PORT=40100

export STREAM_GATE_HTTP_BIND=127.0.0.1
export STREAM_GATE_HTTP_PORT=40101
export STREAM_GATE_TOKEN_HTTP_SECRET="change-me"

# 上游 agent video 端口（gate 作为客户端连进去）
export STREAM_GATE_AGENT_HOST=127.0.0.1
export STREAM_GATE_AGENT_VIDEO_PORT=31234

# 如果 strem_agent_server 开了 --token，需要 gate 连接上游时带这个 token（与对外动态 token 不同）
export STREAM_GATE_AGENT_TOKEN=""

# worker 配置目录 + mlctl
export STREAM_GATE_WORKERS_DIR=/home/gejun/work/my_ml_work/deploy/workers
export STREAM_GATE_MLCTL=/home/gejun/work/my_ml_work/deploy/mlctl.sh

# 断开后多少秒 stop-soft（0 表示不自动 stop）
export STREAM_GATE_IDLE_STOP_SEC=15

# token 策略
export STREAM_GATE_TOKEN_TTL_SEC=300
export STREAM_GATE_TOKEN_PER_IP_WINDOW_SEC=300
export STREAM_GATE_TOKEN_SINGLE_USE=1

# 钉钉/ webhook 推送
export STREAM_GATE_TOKEN_WEBHOOK_URL="https://oapi.dingtalk.com/robot/send?access_token=..."
export STREAM_GATE_TOKEN_WEBHOOK_FORMAT=dingTalk
export STREAM_GATE_TOKEN_WEBHOOK_AUTH=""
```

启动：

```bash
python3 python_dir/stream_service_tcp_gate.py
```

看到类似日志：

```text
[stream_gate] tcp=0.0.0.0:40100 http=127.0.0.1:40101 agent_video=127.0.0.1:31234 token_webhook=yes
```

## 2) 申请 token（触发钉钉推送）

```bash
curl -X POST \
  -H "X-Token-Secret: change-me" \
  "http://127.0.0.1:40101/token?stream=1"
```

返回里会包含 `token`，同时也会通过 webhook 推送到钉钉（以 `token=xxxxxx stream=1 ...` 的文本形式）。

## 3) 客户端 TCP 连接方式

客户端连 gate 的 TCP（不是直接连 `strem_agent_server:31234`）：

- 先发 `AUTH <token>\n`
- 再发 `SUB <stream_id>\n`

示例（用 `nc` 仅演示握手，真实视频会持续输出二进制 H264）：

```bash
{ printf "AUTH 123456\nSUB 1\n"; cat; } | nc -v 127.0.0.1 40100 > /tmp/out.h264
```

## 4) stream_id -> worker 的映射

默认逻辑：扫描 `deploy/workers/*.conf`，找第一个 `STREAM_ID="<id>"` 匹配的 worker 作为启动对象。

如果你想显式指定映射（不扫描文件），用：

```bash
export STREAM_GATE_STREAM_MAP="1=worker_s1,2=worker_s2"
```

## 5) 关闭/停止

本服务直接 `Ctrl+C` 结束即可（不会自动 down worker）。

手动停某个 stream 对应容器（保留容器，下次快速 start）：

```bash
./deploy/mlctl.sh stop-soft <worker_name>
```

彻底停并删除容器：

```bash
./deploy/mlctl.sh down <worker_name>
```


# Runbook: agent_link_service (startLink + TCP proxy + idle stop)

目标：

- 启动一个常驻 Python 服务：
  - HTTP 接口 `POST /startLink` 用于触发启动对应 `stream_id` 的 `ml_worker` 容器
  - 提供 video/ctrl 两个 TCP proxy 入口，客户端通过 proxy 连接到 `strem_agent_server`
  - 断开连接后可感知，若某个 `stream_id` 无任何活跃连接，则等待 5 分钟 `stop-soft` 容器

> 说明：只有当客户端走 proxy（而不是直连 `strem_agent_server`）时，服务才能可靠感知“断开连接”。

## 1) strem_agent_server token/配置方式（现状）

- token：只支持 CLI 参数 `--token <token>`（空表示禁用鉴权）
- env：仅用于 tap runtime tuning（如 `H264_TAP_STALL_MS` / `H264_TAP_DROP_IDR`）
- 不支持配置文件读取（现版本）

## 2) 准备 worker 配置

确保 `deploy/workers/*.conf` 中存在对应 stream：

```bash
cd /home/gejun/work/my_ml_work
rg -n 'STREAM_ID=' deploy/workers/*.conf
```

例如 `STREAM_ID="1"`。

## 3) 启动 agent_link_service

示例（等待 300 秒后 stop-soft）：

```bash
cd /home/gejun/work/my_ml_work

export AGENT_LINK_TOKEN="your-static-token"
export AGENT_LINK_IDLE_STOP_SEC=300

# API 只监听本机
export AGENT_LINK_API_BIND=127.0.0.1
export AGENT_LINK_API_PORT=40120
export AGENT_LINK_SK="change-me-sk"        # 推荐：用于 /startLink HMAC-SHA256 签名鉴权
export AGENT_LINK_SIG_SKEW_SEC=60          # 可选：时间戳允许偏差（秒）
export AGENT_LINK_NONCE_TTL_SEC=300        # 可选：nonce 去重窗口（秒）
export AGENT_LINK_API_SECRET="change-me"   # 可选：legacy header 鉴权（未配置 AGENT_LINK_SK 时才生效）

# 对外的 TCP proxy
export AGENT_LINK_VIDEO_BIND=0.0.0.0
export AGENT_LINK_VIDEO_PORT=40121
export AGENT_LINK_CTRL_BIND=0.0.0.0
export AGENT_LINK_CTRL_PORT=40122
export AGENT_LINK_PUBLIC_HOST="127.0.0.1" # 返回给调用方的 host

# 上游 strem_agent_server 地址
export AGENT_LINK_AGENT_HOST=127.0.0.1
export AGENT_LINK_AGENT_VIDEO_PORT=31234
export AGENT_LINK_AGENT_CTRL_PORT=31235

python3 python_dir/agent_link_service.py
```

## 4) 调用 startLink

签名模式（推荐）：客户端提交 `call_name/ts/nonce/sig`。

- `call_name`: 固定 `startLink`
- `ts`: unix 秒级时间戳（UTC epoch 秒；不要用本地时间字符串去算，推荐 `date -u +%s`）
- `nonce`: 随机字符串（同一 nonce 只能用一次，防重放）
- `sig`: `HMAC_SHA256_HEX(sk, "call_name=startLink&stream=<id>&ts=<ts>&nonce=<nonce>")`

```bash
ts="$(date -u +%s)"
curl -X POST "http://127.0.0.1:40120/startLink?stream=1&call_name=startLink&ts=${ts}&nonce=abc123&sig=<hex>"
```

返回：

- `token`：客户端连 TCP proxy 时要用的 token
- `video_port` / `ctrl_port`：客户端应连接的 proxy 端口

## 5) 客户端连接协议

Video proxy：

- `AUTH <token>\n`
- `SUB <stream_id>\n`

Ctrl proxy：

- `AUTH <token>\n`
- `SUB <stream_id>\n`

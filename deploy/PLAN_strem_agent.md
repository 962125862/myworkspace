# Plan: strem_agent_server / strem_agent_client

本文件记录 `strem_agent_server` / `strem_agent_client` 的技术方案与落地步骤。

## 目标

- **视频代理（不解码）**：接收 `ml_worker` 推送的 TCP TLV(H.264 bytestream) 数据，并将原始 H.264 AnnexB bytestream 通过 TCP 推给远端客户端。
- **控制代理（跨公网）**：客户端采集鼠标/键盘事件，通过 **TCP** 上行到 server；server 再转发到 `ml_worker` 的控制端口。
- **Token 鉴权**：预留 `--token` 入参；当 token 非空时强制 AUTH。
- 客户端在 Windows/macOS 可启动并展示画面；解码尽量启用硬解（best-effort，失败回退软解）。

## 架构

### 视频数据面

```
ml_worker  -- TCP(TLV/H264) -->  strem_agent_server  -- TCP(AnnexB H264) -->  strem_agent_client
```

- TLV 协议复用 `stream_server/include/protocol.h`
- 输出协议：裸 AnnexB H264 bytestream；客户端连接后可选发送 `SUB <stream_id>\n`，默认 stream_id=1。
- 服务端采用“追最新丢旧”策略，避免 TCP 堆积导致延迟无限增长。

### Late-join: REQ_IDR（请求关键帧）

在某些 Sunshine 场景下，推流过程中 IDR（关键帧）可能不周期性出现，导致客户端晚加入时无法起播。

解决：引入 `REQ_IDR` 控制命令（`ML_CTRL_CMD_REQ_IDR = 10`）：

- `ml_worker`：收到 REQ_IDR 后调用 `LiRequestIdrFrame()` 请求主机尽快发送 IDR。
- `strem_agent_server`：在 video SUB 成功后自动向 `ml_worker` UDP 控制端口发送一次 REQ_IDR。
- `strem_agent_client`：连接 ctrl 后也会发送一次 REQ_IDR（兜底）。

### 控制面（必须 TCP）

```
strem_agent_client -- TCP(control) --> strem_agent_server -- UDP(to ml_worker) --> ml_worker(control_socket)
```

- client→server：TCP，带 framing
- server→ml_worker：v1 复用现有 UDP `MlControlCmd`（只在 server 所在侧/内网，不跨公网）

### 控制消息 framing

- TCP(control) 上行：`[u32_be length][payload...]`
- payload：`MlControlCmd` + 可选 TEXT payload（与 `python_dir/input_client.py` 一致）

## 端口（默认，可配置）

- `--in-port`：接收 ml_worker TLV（默认 19000）
- `--video-port`：对外 H264 输出（默认 **31234**）
- `--ctrl-port`：对外控制输入（默认 31235）
- `--worker-ctrl-target`：转发到 ml_worker 控制端口（默认 127.0.0.1:50001）

## 鉴权（v1）

当 `--token` 非空时，要求连接后先发送：

```
AUTH <token>\n
```

- video 连接：AUTH 成功后可选 `SUB <stream_id>\n`，然后开始收 H264
- control 连接：AUTH 成功后开始收 length-prefixed 控制包

后续可选：使用 `ssh -L` 做端口转发，由 SSH 完成加密/鉴权。

## 落地步骤（分阶段提交）

1. `strem_agent_server/`：C 实现（TLV ingest + H264 输出 + control TCP→UDP 代理 + token）
2. `strem_agent_client/`：Python 实现（H264 解码展示 + 键鼠采集 + control TCP 上行 + token）
3. `deploy/RUNBOOK_strem_agent.md`：部署与公网用法（含 ssh -L 推荐）

---

Doc-Version: 0.2.0
Repo-Rev: 4e07aa7

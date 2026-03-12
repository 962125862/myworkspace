# my_ml_work

本仓库包含三块能力：

1. `ml_worker`：Moonlight(Limelight) 客户端，连接 Sunshine 主机并获取编码后的 H.264 bytestream，然后通过 TCP(TLV) 推送给下游。
2. `strem_agent_server/` + `strem_agent_client/`：远程代理（视频代理 + 键鼠控制代理）。
3. `stream_server/`：多路 TLV 接收、统计、（可选）解码/共享内存/桥接等能力。

## 快速入口

- 远程代理：见 `deploy/RUNBOOK_strem_agent.md`
- stream_server + shm/zmq：见 `deploy/SERVICES_RUNBOOK.md` 与 `deploy/RUNBOOK_stream_server_zmq.md`

## 版本号

文档末尾会标注：

- `Doc-Version`: 文档版本（手工维护，语义化）
- `Repo-Rev`: 对应仓库提交号（`git rev-parse --short HEAD`）


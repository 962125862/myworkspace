# my_ml_work

本仓库包含三块能力：

1. `ml_worker`：Moonlight(Limelight) 客户端，连接 Sunshine 主机并获取编码视频流，然后通过 TCP(TLV) 推送给下游。
2. `strem_agent_server/` + `strem_agent_client/`：远程代理（视频代理 + 键鼠控制代理）。
3. `stream_server/`：多路 TLV 接收、统计、解码和 ZMQ bridge 能力。

## 快速入口

当前主链路先读：

- 统一运维入口：`deploy/OPERATIONS_CURRENT.md`
- 当前项目状态和修改记录：`PROGRESS.md`

主链路参考：

- 链路说明：`deploy/CHAIN_ml_worker_stream_server.md`
- 架构设计：`deploy/ARCHITECTURE_CURRENT.md`
- 参数说明：`deploy/CLI_REFERENCE_CURRENT.md`
- benchmark：`deploy/BENCHMARK_stream_server_2026-04-13.md`

代理/按需启动链路：

- 远程代理：`deploy/RUNBOOK_strem_agent.md`
- agent_link_service：`deploy/RUNBOOK_agent_link_service.zh-CN.md`
- TCP gate：`deploy/RUNBOOK_stream_service_tcp_gate.zh-CN.md`
- runtime bundle：`deploy/agent_stack_runtime/README.md`

## 当前结论

- 主链路推荐：`ml_worker -> stream_server -> ZMQ BGR`
- 当前正式主链路默认是 `HEVC444 -> stream_server -> BGR24`
- `strem_agent_server` 仍可用于代理 `H264` 场景
- 当前 `HEVC444` 主链路能力以 `stream_server` 为准
- `deploy/USAGE_MANUAL_CURRENT.md`、`deploy/MAINTENANCE_MANUAL_CURRENT.md`、`deploy/BUILD_DEPLOY_COMMANDS_CURRENT.md` 已收敛为 `deploy/OPERATIONS_CURRENT.md` 的跳转入口

## 版本号

文档末尾会标注：

- `Doc-Version`: 文档版本（手工维护，语义化）
- `Repo-Rev`: 对应仓库提交号（`git rev-parse --short HEAD`）

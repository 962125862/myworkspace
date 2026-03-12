# Env Reference (Deprecated)

本文件列出当前仓库仍然支持、但不推荐继续使用的环境变量开关。

> 推荐做法：优先使用对应的命令行参数（CLI），便于统一管理与在 systemd/service 中显式配置。
> 兼容性说明：当前实现中，`stream_server` 的 CLI 参数会在启动时写回 env（setenv）以兼容旧模块。

## stream_server

### 解码/共享内存/桥接

- `DECODE_BACKEND`: `auto|intel|nvidia|cpu`（建议改用 `--decode-backend`）
- `ENABLE_SHM=1`（建议改用 `--enable-shm`）
- `SHM_ALWAYS=1`（建议改用 `--shm-always`）
- `ZMQ_BRIDGE_BIND=tcp://0.0.0.0:5566`（建议改用 `--zmq-bridge-bind`）

### 压测

- `STRESS_TEST=1`（建议改用 `--stress-test`）
- `STRESS_COPIES=<n>`（建议改用 `--stress-copies <n>`）

### H264 tap（旁路输出 H264）

- `H264_TAP_PORT=<p>`（建议改用 `--h264-tap-port <p>`）
- `H264_TAP_BIND=<ip>`（建议改用 `--h264-tap-bind <ip>`）
- `H264_TAP_STALL_MS=<ms>`（建议改用 `--h264-tap-stall-ms <ms>`）
- `H264_TAP_DROP_IDR=<0|1>`（建议改用 `--h264-tap-drop-idr <0|1>`）
- `ML_WORKER_CTRL_IP=<ip>`（建议改用 `--ml-worker-ctrl-ip <ip>`）
- `ML_WORKER_CTRL_PORT=<p>`（建议改用 `--ml-worker-ctrl-port <p>`）

## strem_agent_server

### H264 tap runtime tuning

`strem_agent_server` 的 tap 行为仍通过 env 调整：

- `H264_TAP_STALL_MS=<ms>`
- `H264_TAP_DROP_IDR=<0|1>`

（后续如果需要，也可以再把这两项做成 `strem_agent_server` 的 CLI 参数。）

---

Doc-Version: 0.1.0
Repo-Rev: 90b0776


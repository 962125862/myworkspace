# Agent Instructions (my_ml_work)

This repo contains three main components:

- `ml_worker` (root `src/`): Moonlight/Limelight client that receives encoded H.264 and forwards it downstream via TCP (TLV).
- `strem_agent_server/` + `strem_agent_client/`: remote agent (video proxy + keyboard/mouse control).
- `stream_server/`: multi-stream TLV receiver + stats + optional decode/shm/bridge.

Primary runbooks live in `deploy/` (see below). Prefer following those docs over guessing.

## Key Docs

- Remote agent quickstart: `deploy/RUNBOOK_strem_agent.md`
- `stream_server` services: `deploy/SERVICES_RUNBOOK.md`
- `stream_server` + shm/zmq: `deploy/RUNBOOK_stream_server_zmq.md`
- Environment variables reference: `deploy/ENV_REFERENCE.md`
- Architecture diagrams/flow: `deploy/ARCHITECTURE_FLOW.md`
- Current project status/issues: `PROGRESS.md`

## Build (CMake)

Project requires CMake >= 3.28 and uses C11.

Common build (Ninja):

```bash
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja -j
```

Notes:

- Root builds `ml_worker` and (by default) `strem_agent_server` via `-DBUILD_STREM_AGENT_SERVER=ON|OFF`.
- `stream_server/` is its own CMake subproject; it is also built when included as a subdir in some setups, but treat it as independently buildable when debugging.

## Run/Operate

- Prefer `deploy/mlctl.sh` for Docker/script orchestration; worker config is under `deploy/workers/`.
- When diagnosing decode behavior, capture exact env vars used (especially `DECODE_BACKEND=...`) and the command line.
- If a change impacts the on-wire TLV protocol, update both producer (`ml_worker`) and consumers (`stream_server`, agent components) together, and note the compatibility implications in the relevant runbook.

## Debugging Expectations

- When a performance issue is reported (e.g. decode FPS), reproduce with a single minimal executable first (often under `stream_server/` like `test_*` or `stream_receiver_decode`) before touching the end-to-end stack.
- In `decoder_decode` style loops, be careful to drain frames (`avcodec_receive_frame` loop until `EAGAIN`) to avoid silently dropping buffered frames.
- Keep logs actionable: include stream id, timestamps, FPS counters, and backend selected.

## Contribution Conventions

- Prefer small, surgical changes with a clear reproduction + verification path.
- Avoid committing large binary artifacts (e.g. `.h264` samples) unless explicitly requested; re-use existing `test_stream.h264` / `test_optimized.h264` for local testing.
- Keep documentation version markers (e.g. `Doc-Version`, `Repo-Rev`) consistent with existing docs when editing runbooks.


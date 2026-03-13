# Run With Docker (strem_agent_server + agent_link_service + on-demand ml_worker)

This repo supports a one-command docker-compose stack that runs:

- `strem_agent_server` (Docker): provides H264 video TCP + control TCP (both token-gated).
- `agent_link_service` (Docker): resident service that:
  - exposes `POST /startLink` (HMAC signature auth)
  - starts `ml_worker` containers on-demand via `deploy/mlctl.sh ensure-up <worker>`
  - provides TCP gates (video+ctrl) so it can observe disconnects and `stop-soft` after idle

## 0) Files

Stack:

- `deploy/agent_stack/docker-compose.yml`
- `deploy/agent_stack/docker-compose.pull.yml` (pull prebuilt images)
- `deploy/agent_stack/agent_link_service.Dockerfile`
- `deploy/agent_stack/oneclick_up.sh`
- `deploy/agent_stack/oneclick_down.sh`
- `deploy/agent_stack/export_images.sh`
- `deploy/agent_stack_runtime/` (minimal runtime bundle, no full repo needed)

Configs (copy `*.example` to the real filename):

- `deploy/agent_stack/config/strem_agent_server.env.example` -> `deploy/agent_stack/config/strem_agent_server.env`
- `deploy/agent_stack/config/agent_link_service.env.example` -> `deploy/agent_stack/config/agent_link_service.env`

Worker configs (used by `mlctl.sh`):

- `deploy/workers/*.conf` (must include `STREAM_ID="<id>"` for each stream you want)
- `deploy/data/` (persistent worker data, keys, etc.)

## 1) One-Command Start

1) Create config files:

```bash
cd /home/gejun/work/my_ml_work
cp -v deploy/agent_stack/config/strem_agent_server.env.example deploy/agent_stack/config/strem_agent_server.env
cp -v deploy/agent_stack/config/agent_link_service.env.example deploy/agent_stack/config/agent_link_service.env
```

2) Edit both configs and set the same static token:

- `deploy/agent_stack/config/strem_agent_server.env`: `AGENT_TOKEN=...`
- `deploy/agent_stack/config/agent_link_service.env`: `AGENT_LINK_TOKEN=...`

3) Start:

```bash
cd /home/gejun/work/my_ml_work/deploy/agent_stack
./oneclick_up.sh
docker compose logs -f --tail 100 agent_link_service
```

Stop:

```bash
cd /home/gejun/work/my_ml_work/deploy/agent_stack
./oneclick_down.sh
```

### Use Prebuilt Images (No Local Build)

If you have prebuilt images in a registry or loaded via `docker load`, run:

```bash
cd /home/gejun/work/my_ml_work/deploy/agent_stack
AGENT_STACK_MODE=pull ./oneclick_up.sh
```

Default registry image names (override via env if needed):

- `ghcr.io/962125862/myworkspace/strem-agent-server:latest`
- `ghcr.io/962125862/myworkspace/agent-link-service:latest`

To export images (for offline import on another machine):

```bash
cd /home/gejun/work/my_ml_work/deploy/agent_stack
./export_images.sh /tmp/agent_stack_images_latest.tar.gz
```

### Publish to GHCR (Maintainer)

From a machine with GitHub permissions:

```bash
docker login ghcr.io

docker tag strem-agent-server:latest ghcr.io/962125862/myworkspace/strem-agent-server:latest
docker tag agent-link-service:latest ghcr.io/962125862/myworkspace/agent-link-service:latest

docker push ghcr.io/962125862/myworkspace/strem-agent-server:latest
docker push ghcr.io/962125862/myworkspace/agent-link-service:latest
```

## Runtime Bundle (No Source Required)

If you want users to run without cloning the full repo, give them the directory:

- `deploy/agent_stack_runtime/`

They only need:

1) Fill two env files under `deploy/agent_stack_runtime/config/`
2) Do `ml_worker` pair once (interactive PIN, via `docker exec` into `agent_link_service`)
3) Then `startLink` + client connect to gate ports

## 2) How startLink Auth Works (HMAC)

`agent_link_service` verifies an HMAC-SHA256 signature if `AGENT_LINK_SK` is set in
`deploy/agent_stack/config/agent_link_service.env`.

Required query parameters:

- `stream`: stream id (e.g. `1`)
- `call_name`: must be `startLink`
- `ts`: unix timestamp (seconds, UTC epoch). Do not use local time strings; use epoch seconds (e.g. `date -u +%s`).
- `nonce`: random string (replay-protected)
- `sig`: lowercase hex of `HMAC_SHA256(sk, canonical_string)`

Canonical string (must match exactly):

```text
call_name=startLink&stream=<stream>&ts=<ts>&nonce=<nonce>
```

Return JSON:

- `token`: static token for TCP gate
- `video_port`: TCP port for video gate (default 40121)
- `ctrl_port`: TCP port for ctrl gate (default 40122)

## 3) Client Connection Flow

1) Call `startLink` (HTTP) to ensure the stream container is running.
2) Client connects to gate TCP ports (not directly to `strem_agent_server`):

Video gate:

- send `AUTH <token>\n`
- send `SUB <stream_id>\n`
- then receive raw AnnexB H264 bytes

Ctrl gate:

- send `AUTH <token>\n`
- send `SUB <stream_id>\n`
- then send framed control packets

When both video+ctrl connections for a stream go to 0, the service waits
`AGENT_LINK_IDLE_STOP_SEC` (default 300s) and then calls `mlctl stop-soft` for that stream.

## 4) Config Reference

### A) strem_agent_server config (`deploy/agent_stack/config/strem_agent_server.env`)

Main settings:

- `AGENT_TOKEN`: enable AUTH token on video+ctrl (must match gate token)
- `IN_HOST` / `IN_PORT`: ingest bind for `ml_worker` (default `0.0.0.0:19000`)
- `VIDEO_BIND` / `VIDEO_PORT`: video out (default `0.0.0.0:31234`)
- `CTRL_BIND` / `CTRL_PORT`: control in (default `0.0.0.0:31235`)
- `WORKER_CTRL_IP` / `WORKER_CTRL_PORT`: UDP forward target for control packets (default `127.0.0.1:50001`)

Optional tuning (read by the binary itself):

- `H264_TAP_STALL_MS`
- `H264_TAP_DROP_IDR`

### B) agent_link_service config (`deploy/agent_stack/config/agent_link_service.env`)

Security:

- `AGENT_LINK_TOKEN`: static token used by gate (and upstream AUTH)
- `AGENT_LINK_SK`: HMAC shared secret for `/startLink` signature auth
- `AGENT_LINK_SIG_SKEW_SEC`: timestamp skew allowed (seconds)
- `AGENT_LINK_NONCE_TTL_SEC`: nonce replay window (seconds)

Ports:

- `AGENT_LINK_API_BIND` / `AGENT_LINK_API_PORT`: HTTP API
- `AGENT_LINK_VIDEO_BIND` / `AGENT_LINK_VIDEO_PORT`: video gate TCP
- `AGENT_LINK_CTRL_BIND` / `AGENT_LINK_CTRL_PORT`: ctrl gate TCP

Upstream:

- `AGENT_LINK_AGENT_HOST` / `AGENT_LINK_AGENT_VIDEO_PORT` / `AGENT_LINK_AGENT_CTRL_PORT`

On-demand:

- `AGENT_LINK_IDLE_STOP_SEC`: idle stop delay after disconnect (300 = 5 minutes)

mlctl integration (inside the container, repo is mounted to `/repo`):

- `AGENT_LINK_MLCTL=/repo/deploy/mlctl.sh`
- `AGENT_LINK_WORKERS_DIR=/repo/deploy/workers`
- `ML_WORKERS_DIR=/repo/deploy/workers`
- `ML_DATA_DIR=/repo/deploy/data`

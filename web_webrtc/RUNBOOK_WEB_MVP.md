# Web WebRTC + WS Control (MVP) Runbook

This document summarizes the issues encountered and provides a repeatable startup / rebuild workflow.

Repo root: `/home/gejun/work/my_ml_work`  
Stack dir: `/home/gejun/work/my_ml_work/web_webrtc`

## Architecture (What Talks to What)

- **Video path**
  1. `strem_agent_server` exposes H264 AnnexB over TCP (default `31234`) and accepts `SUB <stream_id>\n`.
  2. `republisher` reads that H264 stream and publishes to MediaMTX via RTSP (host `34568`).
  3. `mediamtx` serves WebRTC playback over HTTP (host `34569`) and ICE on `34570` (UDP/TCP).

- **Control path (mouse/keyboard)**
  1. Browser connects to `ws_ctrl_bridge` WebSocket: `ws://<host>:34567/ws`.
  2. `ws_ctrl_bridge` forwards MLCT-framed control packets over TCP to `strem_agent_server` ctrl port (default `31235`).

## Problems Encountered (Root Causes)

1. **Black screen / no video in browser**
   - Symptom: MediaMTX logs show:
     - `closed: deadline exceeded while waiting connection`
   - Root cause: MediaMTX was advertising ICE candidates like `127.0.0.1` or LAN IP (`192.168.x.x`), which is unreachable from the Internet client.
   - Fix: Set `PUBLIC_HOST` so MediaMTX includes the public IP/DNS in candidates.
     - Persisted in `.env` to avoid forgetting it.

2. **`docker-compose` v1 `'ContainerConfig'` error**
   - Symptom: `docker-compose up ...` fails with `'ContainerConfig'`.
   - Root cause: `docker-compose` v1.29.2 is too old and can break with newer Docker Engine (this machine: Docker 28.x).
   - Fix: Install and use **Compose v2** (`docker compose ...`).

3. **macOS `Cmd+C/V/...` not working**
   - Symptom: Web page captures keyboard but shortcuts don't work as expected.
   - Root cause:
     - The page `preventDefault()`'d all keydown events without forwarding modifiers.
     - Server side was sending `b=0` (no modifiers) for key presses.
   - Fix:
     - Frontend now sends `modifiers` bitmask (shift/ctrl/alt/meta).
     - Server now forwards modifiers, and by default maps macOS `META(Command)` to Windows `CTRL` so `Cmd+C` becomes `Ctrl+C` on the host.

4. **Late join / long time to recover after issues because IDR is rare**
   - Symptom: If upstream doesn't emit keyframes for minutes, join/recovery can be very slow.
   - Fix:
     - `ws_ctrl_bridge` sends a burst of `REQ_IDR` on viewer connect.
     - Optional periodic `REQ_IDR` can be enabled via env var.

5. **Mouse "not responsive" / feels laggy while video is smooth**
   - Symptom: Video looks OK but pointer control feels behind.
   - Root cause (common): perceived input lag is dominated by **video E2E latency** (your action is applied quickly, but you only see it when the next delayed video frame arrives).
   - Fix implemented:
     - Mouse moves are now **coalesced** (at most once per animation frame).
     - If WebSocket send buffer backs up, intermediate moves are dropped (no "queue buildup" causing worsening lag).

6. **Jitter buffer metrics look "increasing"**
   - Symptom: `jitterBufferDelay`, `jitterBufferMinimumDelay`, `jitterBufferTargetDelay` look like they "keep growing".
   - Note: These are often **cumulative** counters; to understand current buffering, compute:
     - average jitter buffer (seconds) = `jitterBufferDelay / jitterBufferEmittedCount`
   - If `jitter` itself is large (e.g. ~0.159s), the receiver will increase buffer to avoid stalling. Fix is usually network/bitrate/encoder settings, not WS.

## Files Changed (What Was Optimized / Fixed)

- `web_webrtc/.env`
  - `PUBLIC_HOST=124.90.118.111` (your current public IP)

- `web_webrtc/docker-compose.yml`
  - Added env knobs for IDR burst/periodic IDR and macOS modifier mapping.

- `web_webrtc/ws_ctrl_bridge/server.js`
  - IDR request burst + optional periodic IDR.
  - Keyboard modifiers forwarding; optional macOS `META -> CTRL` mapping (`MAP_MAC_META_TO_CTRL`).

- `web_webrtc/ws_ctrl_bridge/web/index.html`
  - Keyboard: send modifiers.
  - Mouse: move coalescing + WS backpressure drop.

- `web_webrtc/republisher/republish.py`
  - `ffmpeg` flags to generate/attach timestamps and reduce mux buffering (helps downstream jitter buffering).

## One-Time Setup

### 1) Ensure public host is configured

Edit `.env` (already created in this repo):

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
cat .env
```

Expect:

```text
PUBLIC_HOST=124.90.118.111
```

If your public IP changes, update this value (or use a DNS name).

### 2) (Optional) HTTP proxy for downloads/build

If you have an HTTP proxy on `127.0.0.1:1095`:

```bash
export HTTP_PROXY=http://127.0.0.1:1095
export HTTPS_PROXY=http://127.0.0.1:1095
export NO_PROXY=localhost,127.0.0.1,::1,192.168.0.0/16
```

Verify:

```bash
curl -I -x http://127.0.0.1:1095 https://github.com
```

### 3) Install Docker Compose v2 (if not present)

If `docker compose` is missing and `apt` doesn't provide `docker-compose-plugin`, install via CLI plugin:

```bash
sudo mkdir -p /usr/local/lib/docker/cli-plugins
sudo -E curl -L https://github.com/docker/compose/releases/download/v2.27.1/docker-compose-linux-x86_64 \
  -o /usr/local/lib/docker/cli-plugins/docker-compose
sudo chmod +x /usr/local/lib/docker/cli-plugins/docker-compose
docker compose version
```

## Start / Stop (Normal Operation)

### Start everything (build images if needed)

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build
```

### Check status

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose ps
```

### Follow logs

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose logs -f --tail 100 mediamtx
docker compose logs -f --tail 100 republisher
docker compose logs -f --tail 100 ws_ctrl_bridge
```

### Stop everything

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose down
```

## Rebuild / Recreate Specific Services (Packaging Images)

This is the standard workflow after code changes.

### Rebuild + recreate `ws_ctrl_bridge` only (fast)

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build --no-deps --force-recreate ws_ctrl_bridge
```

### Rebuild + recreate `republisher` only

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build --no-deps --force-recreate republisher
```

### Recreate `mediamtx` (when `PUBLIC_HOST` changed)

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --no-deps --force-recreate mediamtx
```

### Build images without starting (optional)

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose build
```

## Key Runtime Knobs (Environment Variables)

### MediaMTX public candidate

- `.env`: `PUBLIC_HOST=<public_ip_or_dns>`
- Used by compose as `MTX_WEBRTCADDITIONALHOSTS`.

### IDR request strategy (ws_ctrl_bridge)

- `REQ_IDR_ON_CONNECT_BURST` (default `3`)
- `REQ_IDR_BURST_INTERVAL_MS` (default `500`)
- `REQ_IDR_PERIOD_SEC` (default `0`, set e.g. `2` to request every 2 seconds)

Example:

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
REQ_IDR_PERIOD_SEC=2 docker compose up -d --no-deps --force-recreate ws_ctrl_bridge
```

### macOS shortcut mapping

- `MAP_MAC_META_TO_CTRL` (default `1`)
  - `1`: map macOS `Command` to Windows `Ctrl` when ctrl isn't pressed
  - `0`: keep `Command` as `META` modifier

Example:

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
MAP_MAC_META_TO_CTRL=0 docker compose up -d --no-deps --force-recreate ws_ctrl_bridge
```

## Troubleshooting Checklist

### A) Black screen

1. Confirm MediaMTX is up:
   - `docker compose ps`
2. Check MediaMTX logs for ICE failure:
   - If you see `deadline exceeded while waiting connection`, verify `.env` has correct `PUBLIC_HOST` and recreate `mediamtx`.
3. Ensure ports are reachable from the client network:
   - `34569/tcp` (WebRTC HTTP page)
   - `34570/udp` (ICE)
   - `34570/tcp` (ICE TCP fallback)

### B) Video present but latency increases / control feels laggy

1. Check `chrome://webrtc-internals/`:
   - Look at `jitter` and average jitter buffer: `jitterBufferDelay / jitterBufferEmittedCount`.
2. If `jitter` is high:
   - Reduce bitrate / resolution at the source encoder first (most effective).
3. If keyframes are rare:
   - Enable periodic `REQ_IDR_PERIOD_SEC` (1-2 seconds for testing, larger for production).

### C) Control works but `Cmd+C/V` doesn't

1. Ensure you clicked inside the video area (overlay must have focus).
2. Ensure `MAP_MAC_META_TO_CTRL=1` (default) if controlling a Windows host.


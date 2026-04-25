# Web WebRTC MVP (No HTTPS)

This directory is an MVP to view the stream in a browser with low latency video (WebRTC) and basic input (mouse/keyboard) without requiring HTTPS.

Components

- `strem_agent_server` (already in this repo): ingest TLV/H264, expose H264 tap (`video-port`, default `31234`) and ctrl tcp (`ctrl-port`, default `31235`).
- MediaMTX (docker): RTSP ingest + WebRTC playback.
- `republisher` (docker): pulls H264 from `strem_agent_server` and publishes to MediaMTX via RTSP.
- `ws_ctrl_bridge` (docker): serves a small web page and forwards WebSocket input events to `strem_agent_server` ctrl tcp.

Ports (public server)

Default port plan (customized):

- `34567/tcp`: web page + WS control bridge
- `34568/tcp`: RTSP ingest (republisher -> MediaMTX)
- `34569/tcp`: MediaMTX WebRTC pages / WHEP
- `34570/udp` (+ `34570/tcp` optional fallback): WebRTC ICE/DTLS-SRTP (MediaMTX)

Quick Start

1) Ensure `strem_agent_server` is running on the same host (or set `AGENT_HOST`):

```bash
./strem_agent_server/build/strem_agent_server \\
  --in-host 0.0.0.0 --in-port 19000 \\
  --video-bind 0.0.0.0 --video-port 31234 \\
  --ctrl-bind 0.0.0.0 --ctrl-port 31235 \\
  --worker-ctrl-ip 127.0.0.1 --worker-ctrl-port 30001
```

2) Start the stack:

```bash
cd web_webrtc

# Set to the public IP or DNS of the server for ICE candidate generation.
PUBLIC_HOST=YOUR_PUBLIC_IP_OR_DNS \\
  docker compose up -d --build
```

If image builds are slow, you can pass proxy variables to the build (Compose forwards them as build args):

```bash
HTTP_PROXY=http://127.0.0.1:1095 \\
HTTPS_PROXY=http://127.0.0.1:1095 \\
PUBLIC_HOST=YOUR_PUBLIC_IP_OR_DNS \\
  docker compose up -d --build
```

Note (Linux): if your proxy only listens on host loopback (`127.0.0.1`), build containers can't reach it unless
the build uses host networking. This repo's `docker-compose.yml` sets `build.network: host` for the images that
need `apt`/`npm` to make loopback proxies work.

If you are in CN and want to speed up apt without relying on proxy, set an apt mirror (example):

```bash
APT_MIRROR=mirrors.tuna.tsinghua.edu.cn \\
PUBLIC_HOST=YOUR_PUBLIC_IP_OR_DNS \\
  docker compose up -d --build
```

3) Open in browser (Chrome/Edge recommended for MVP):

```text
http://YOUR_PUBLIC_IP_OR_DNS:34567/
```

Notes

- This MVP uses absolute mouse mapping based on the overlay size. It does not use Pointer Lock.
- Without TURN, some client networks may fail to connect. Add TURN later if needed.
- If you enabled token auth on `strem_agent_server`, set `AGENT_TOKEN` for both republisher and ws bridge:

```bash
AGENT_TOKEN=xxx PUBLIC_HOST=... docker compose up -d --build
```

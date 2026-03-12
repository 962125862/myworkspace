# Plan: Web (WebRTC Video + WS Control) MVP

Goal (MVP)

- Public server (Linux, Docker available).
- 1 user / 1 stream.
- Low latency video via WebRTC.
- Mouse/keyboard interaction from browser.
- No HTTPS for the first iteration.

Constraints / Reality Checks

- True end-to-end 100ms depends on RTT. If client<->server RTT is e.g. 60-80ms, 100ms E2E is not realistic.
- Without HTTPS, browser features like Pointer Lock (relative mouse) and some fullscreen behaviors may be restricted. MVP will use absolute mouse coordinates mapped from the rendered video.
- To make public Internet connectivity reliable, TURN is usually required. MVP can start without TURN (works in many networks but not all).

Existing Assets We Will Reuse

- Video source: `strem_agent_server` already exposes H264 (AnnexB) over TCP on the video port (default 31234) with `SUB <stream_id>`.
- Control injection: `strem_agent_server` already exposes a TCP control port (default 31235) that forwards `MLCT` (MlControlCmd) packets to `ml_worker` UDP control socket.
- Late-join: `REQ_IDR` exists (MLCT type=10). We can request an IDR on viewer connect.

High-Level Architecture (MVP)

- Browser:
  - Plays WebRTC video from a WebRTC gateway.
  - Sends input events over WebSocket to a small `ws_ctrl_bridge` service.

- Server components:
  1) WebRTC gateway for video
     - Fastest path to MVP: run an existing gateway/SFU and feed it H264.
     - Recommendation for MVP: MediaMTX (Docker) because it has built-in WebRTC playback and is easy to operate.

  2) Republisher (H264 -> gateway)
     - Reads H264 from `strem_agent_server` video port.
     - Publishes to the gateway (likely via RTSP or RTP).
     - Implementation options:
       - `ffmpeg` container: `tcp://...` input, push to `rtsp://mediamtx/...` output.
       - GStreamer container: more control/less buffering, better for ultra low latency.

  3) `ws_ctrl_bridge`
     - WebSocket server that accepts JSON input events from the browser.
     - Translates them to `MLCT` and forwards to `strem_agent_server` ctrl TCP port.
     - On websocket connect, optionally send `REQ_IDR` once.

Deliverables

- `web_webrtc/docker-compose.yml`
  - `mediamtx` service
  - `ffmpeg` (or gstreamer) republisher service
  - `ws_ctrl_bridge` service

- `web_webrtc/web/`
  - `index.html` + JS
  - Shows the video (WebRTC page from gateway or embedded player if available)
  - Captures mouse move/click/wheel and keyboard press
  - Sends events to `ws_ctrl_bridge`

- `web_webrtc/ws_ctrl_bridge/`
  - Minimal server (Node or Python)
  - Protocol:
    - Client -> server: JSON
      - `{type:"mouse", kind:"move", x, y, ref_w, ref_h}`
      - `{type:"mouse", kind:"down|up", button:"left|right|middle"}`
      - `{type:"wheel", dx, dy}`
      - `{type:"key", kind:"press", key:"a"}`
      - `{type:"text", text:"..."}`
    - Server -> strem_agent_server ctrl: MLCT framing `[u32_be len][MlControlCmd(+payload)]`

MVP Implementation Steps

1) Video gateway (MediaMTX) baseline
- Bring up MediaMTX container.
- Validate that MediaMTX WebRTC playback works from a LAN client.

2) H264 republisher
- Consume H264 from `strem_agent_server` video TCP:
  - Connect, send optional `AUTH <token>\n`, then `SUB <stream_id>\n`.
- Push to MediaMTX (RTSP or RTP) with minimal buffering.
- Ensure periodic IDR (or request IDR on startup).

3) WS control bridge
- Implement `ws_ctrl_bridge`.
- Forward MLCT packets to `strem_agent_server` ctrl TCP (31235).
- Add a `REQ_IDR` on viewer connect (best-effort).

4) Web UI
- Simple page that:
  - Opens video (WebRTC) using MediaMTX web endpoint.
  - Opens WS to `ws_ctrl_bridge`.
  - Maps input coordinates:
    - Use current video element bounding rect.
    - Map pointer `clientX/clientY` to `ref_w/ref_h`.
    - Default `ref_w/ref_h` = current video resolution.
    - Optional override `ctrl_ref_w/ctrl_ref_h` (host desktop resolution).

5) Latency tuning
- Reduce buffering in republisher.
- Ensure encoder settings: no B-frames, short GOP, keyint tuned.
- Wire PLI->REQ_IDR later (WebRTC gateways can expose this; for MVP we do join-triggered IDR).

What We Will Not Do In MVP

- TURN/coturn integration (unless you want "public works everywhere").
- Pointer Lock / relative mouse (likely needs HTTPS).
- Multi-user / SFU scaling.

Estimated Timeline

- MVP in LAN / friendly networks: ~1-2 days.
- MVP with TURN (public works in most networks): ~2-4 days.

Open Questions (Answer When Ready)

- Which video gateway do you want for MVP?
  - MediaMTX (recommended for speed)
  - Janus
  - mediasoup
- Will the viewer be on the same network as the server or arbitrary networks?
  - If arbitrary, TURN is strongly recommended.
- Source video profile and keyframe policy (Sunshine settings / encoder).

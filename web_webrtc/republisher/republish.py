from __future__ import annotations

import os
import socket
import subprocess
import time


def env_int(name: str, default: int) -> int:
    v = os.getenv(name, "")
    if not v:
        return default
    try:
        return int(v)
    except Exception:
        return default


def log(msg: str) -> None:
    print(f"[republisher] {msg}", flush=True)


def connect_and_stream() -> int:
    agent_host = os.getenv("AGENT_HOST", "127.0.0.1")
    agent_port = env_int("AGENT_VIDEO_PORT", 31234)
    token = os.getenv("AGENT_TOKEN", "")
    stream_id = env_int("STREAM_ID", 1)

    mtx_rtsp = os.getenv("MTX_RTSP_URL", "rtsp://127.0.0.1:8554/mystream")
    ff_loglevel = os.getenv("FFMPEG_LOGLEVEL", "warning")

    log(f"connect agent video: {agent_host}:{agent_port} (stream_id={stream_id})")
    s = socket.create_connection((agent_host, agent_port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Send handshake (AUTH optional, then SUB).
    if token:
        s.sendall(f"AUTH {token}\n".encode("utf-8"))
    s.sendall(f"SUB {stream_id}\n".encode("utf-8"))

    # Pipe H264 AnnexB -> ffmpeg -> RTSP publish.
    # NOTE: use minimal buffering flags to reduce end-to-end latency.
    cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        ff_loglevel,
        "-fflags",
        "+nobuffer+genpts",
        "-flags",
        "low_delay",
        # Raw H264 has no timestamps; use wallclock to avoid "unset timestamps" behavior that can
        # amplify jitter buffering downstream (mediamtx/webrtc).
        "-use_wallclock_as_timestamps",
        "1",
        "-probesize",
        "32",
        "-analyzeduration",
        "0",
        "-f",
        "h264",
        "-i",
        "pipe:0",
        "-an",
        "-c:v",
        "copy",
        # Reduce mux latency on the publishing side.
        "-muxdelay",
        "0",
        "-muxpreload",
        "0",
        "-flush_packets",
        "1",
        "-f",
        "rtsp",
        "-rtsp_transport",
        "tcp",
        mtx_rtsp,
    ]
    log("spawn ffmpeg: " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    assert proc.stdin is not None

    buf = bytearray(1024 * 1024)
    try:
        while True:
            n = s.recv_into(buf)
            if n <= 0:
                break
            try:
                proc.stdin.write(memoryview(buf)[:n])
            except BrokenPipeError:
                break
    finally:
        try:
            s.close()
        except Exception:
            pass
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    return proc.returncode or 0


def main() -> int:
    backoff = 1.0
    while True:
        try:
            rc = connect_and_stream()
            log(f"session ended (rc={rc}), restarting...")
        except Exception as e:
            log(f"error: {e!r}, restarting...")

        time.sleep(backoff)
        backoff = min(10.0, backoff * 1.5)


if __name__ == "__main__":
    raise SystemExit(main())

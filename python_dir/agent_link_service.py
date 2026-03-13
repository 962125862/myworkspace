#!/usr/bin/env python3
"""
agent_link_service.py

Resident service for:
- Auth/token distribution (static token for now).
- On-demand start/stop ml_worker containers by stream_id.
- TCP proxy for strem_agent_server (video + ctrl) so we can observe disconnects.

HTTP:
  POST /startLink?stream=<id>
  Response includes token + gate endpoints.

TCP Gate:
  Video: expects "AUTH <token>\n" then "SUB <stream_id>\n"
  Ctrl : expects "AUTH <token>\n"

After successful auth, the gate:
- ensures ml_worker container for stream_id is running (via deploy/mlctl.sh ensure-up <worker>)
- proxies the TCP stream to strem_agent_server video/ctrl port.

Disconnect handling:
- when both video+ctrl connections for a stream_id go to zero, schedule stop-soft in 5 minutes.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import socket
import socketserver
import subprocess
import threading
import time
import urllib.parse
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def now_ms() -> int:
    return int(time.time() * 1000)


def env_int(name: str, default: int) -> int:
    v = os.getenv(name, "").strip()
    if not v:
        return default
    try:
        return int(v)
    except ValueError:
        return default


def consteq(a: str, b: str) -> bool:
    try:
        return hmac.compare_digest(a.encode("utf-8"), b.encode("utf-8"))
    except Exception:
        return False


def recv_line(sock: socket.socket, timeout_sec: float, max_len: int = 512) -> str:
    sock.settimeout(timeout_sec)
    buf = bytearray()
    while True:
        if len(buf) >= max_len:
            raise ValueError("line_too_long")
        b = sock.recv(1)
        if not b:
            raise EOFError()
        if b == b"\n":
            break
        buf += b
    return buf.decode("utf-8", errors="replace").rstrip("\r")


def read_stream_id_from_conf(path: str) -> int | None:
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("STREAM_ID="):
                    rhs = line.split("=", 1)[1].strip().strip('"').strip("'")
                    try:
                        return int(rhs)
                    except ValueError:
                        return None
    except OSError:
        return None
    return None


def find_worker_for_stream_id(workers_dir: str, stream_id: int) -> str | None:
    try:
        entries = sorted([p for p in os.listdir(workers_dir) if p.endswith(".conf")])
    except OSError:
        return None
    for fn in entries:
        p = os.path.join(workers_dir, fn)
        sid = read_stream_id_from_conf(p)
        if sid == int(stream_id):
            return fn[:-5]
    return None


class OnDemandManager:
    def __init__(
        self,
        *,
        mlctl_path: str,
        workers_dir: str,
        idle_stop_sec: int,
    ):
        self.mlctl_path = mlctl_path
        self.workers_dir = workers_dir
        self.idle_stop_sec = max(0, int(idle_stop_sec))

        self._lock = threading.Lock()
        self._active_video: dict[int, int] = {}
        self._active_ctrl: dict[int, int] = {}
        self._stop_timer_by_stream: dict[int, threading.Timer] = {}

    def _run_mlctl(self, args: list[str]) -> None:
        subprocess.run(
            [self.mlctl_path, *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def ensure_up(self, stream_id: int) -> str:
        worker = find_worker_for_stream_id(self.workers_dir, int(stream_id))
        if not worker:
            raise RuntimeError(f"no_worker_for_stream_id:{stream_id}")
        self._run_mlctl(["ensure-up", worker])
        return worker

    def stop_soft(self, stream_id: int) -> str:
        worker = find_worker_for_stream_id(self.workers_dir, int(stream_id))
        if not worker:
            raise RuntimeError(f"no_worker_for_stream_id:{stream_id}")
        self._run_mlctl(["stop-soft", worker])
        return worker

    def _cancel_timer_locked(self, stream_id: int) -> None:
        t = self._stop_timer_by_stream.pop(int(stream_id), None)
        if t:
            try:
                t.cancel()
            except Exception:
                pass

    def on_video_connect(self, stream_id: int) -> None:
        with self._lock:
            self._cancel_timer_locked(stream_id)
            self._active_video[int(stream_id)] = self._active_video.get(int(stream_id), 0) + 1

    def on_video_disconnect(self, stream_id: int) -> None:
        with self._lock:
            cur = max(0, self._active_video.get(int(stream_id), 0) - 1)
            if cur == 0:
                self._active_video.pop(int(stream_id), None)
            else:
                self._active_video[int(stream_id)] = cur
            self._maybe_schedule_stop_locked(int(stream_id))

    def on_ctrl_connect(self, stream_id: int) -> None:
        with self._lock:
            self._cancel_timer_locked(stream_id)
            self._active_ctrl[int(stream_id)] = self._active_ctrl.get(int(stream_id), 0) + 1

    def on_ctrl_disconnect(self, stream_id: int) -> None:
        with self._lock:
            cur = max(0, self._active_ctrl.get(int(stream_id), 0) - 1)
            if cur == 0:
                self._active_ctrl.pop(int(stream_id), None)
            else:
                self._active_ctrl[int(stream_id)] = cur
            self._maybe_schedule_stop_locked(int(stream_id))

    def _maybe_schedule_stop_locked(self, stream_id: int) -> None:
        if self.idle_stop_sec <= 0:
            return
        if self._active_video.get(stream_id, 0) != 0:
            return
        if self._active_ctrl.get(stream_id, 0) != 0:
            return
        if stream_id in self._stop_timer_by_stream:
            return

        def fire() -> None:
            # Re-check counts when timer fires.
            with self._lock:
                if self._active_video.get(stream_id, 0) != 0:
                    return
                if self._active_ctrl.get(stream_id, 0) != 0:
                    return
                self._stop_timer_by_stream.pop(stream_id, None)
            try:
                self.stop_soft(stream_id)
            except Exception:
                pass

        tm = threading.Timer(self.idle_stop_sec, fire)
        tm.daemon = True
        self._stop_timer_by_stream[stream_id] = tm
        tm.start()


@dataclass(frozen=True)
class GateConfig:
    token: str
    handshake_timeout_sec: float
    agent_host: str
    agent_video_port: int
    agent_ctrl_port: int
    mlctl_path: str
    workers_dir: str


class GateState:
    def __init__(self, cfg: GateConfig, ondemand: OnDemandManager):
        self.cfg = cfg
        self.ondemand = ondemand


class VideoGateHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        st: GateState = self.server.state  # type: ignore[attr-defined]
        ip = self.client_address[0]
        s = self.request
        assert isinstance(s, socket.socket)

        stream_id = 0
        try:
            l1 = recv_line(s, st.cfg.handshake_timeout_sec)
            l2 = recv_line(s, st.cfg.handshake_timeout_sec)
            tok = ""
            for ln in (l1, l2):
                if ln.startswith("AUTH "):
                    tok = ln[5:].strip()
                if ln.startswith("SUB "):
                    try:
                        stream_id = int(ln[4:].strip())
                    except ValueError:
                        stream_id = 0

            if tok != st.cfg.token or stream_id <= 0:
                s.sendall(b"ERR bad_auth\n")
                return

            st.ondemand.on_video_connect(stream_id)
            try:
                st.ondemand.ensure_up(stream_id)
            except Exception:
                s.sendall(b"ERR start_failed\n")
                return

            upstream = socket.create_connection((st.cfg.agent_host, st.cfg.agent_video_port), timeout=3.0)
            try:
                upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass

            # Forward to strem_agent_server (it expects AUTH then SUB when token enabled).
            upstream.sendall(f"AUTH {st.cfg.token}\n".encode("utf-8"))
            upstream.sendall(f"SUB {stream_id}\n".encode("utf-8"))

            upstream.settimeout(10.0)
            s.settimeout(10.0)

            while True:
                try:
                    data = upstream.recv(64 * 1024)
                except socket.timeout:
                    continue
                if not data:
                    break
                s.sendall(data)
        except Exception:
            return
        finally:
            if stream_id > 0:
                st.ondemand.on_video_disconnect(stream_id)


class CtrlGateHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        st: GateState = self.server.state  # type: ignore[attr-defined]
        s = self.request
        assert isinstance(s, socket.socket)

        stream_id = 0
        upstream: socket.socket | None = None
        try:
            # Ctrl handshake:
            #   AUTH <token>\n
            #   SUB <stream_id>\n
            l1 = recv_line(s, st.cfg.handshake_timeout_sec)
            l2 = recv_line(s, st.cfg.handshake_timeout_sec)

            tok = ""
            for ln in (l1, l2):
                if ln.startswith("AUTH "):
                    tok = ln[5:].strip()
                elif ln.startswith("SUB "):
                    try:
                        stream_id = int(ln[4:].strip())
                    except ValueError:
                        stream_id = 0

            if not tok:
                s.sendall(b"ERR bad_auth\n")
                return
            if tok != st.cfg.token:
                s.sendall(b"ERR bad_auth\n")
                return
            if stream_id <= 0:
                s.sendall(b"ERR bad_stream\n")
                return

            st.ondemand.on_ctrl_connect(stream_id)
            try:
                st.ondemand.ensure_up(stream_id)
            except Exception:
                s.sendall(b"ERR start_failed\n")
                return

            upstream = socket.create_connection((st.cfg.agent_host, st.cfg.agent_ctrl_port), timeout=3.0)
            try:
                upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass

            upstream.sendall(f"AUTH {st.cfg.token}\n".encode("utf-8"))
            upstream.sendall(f"SUB {stream_id}\n".encode("utf-8"))

            # Bi-directional proxy (ctrl is client->server heavy).
            #
            # Important: if the client disconnects, we must also close the upstream
            # socket; otherwise the upstream->client pump can sit in recv() timeouts
            # forever and keep the client socket stuck in CLOSE-WAIT, preventing
            # idle-stop from triggering.
            upstream.settimeout(1.0)
            s.settimeout(1.0)

            stop = threading.Event()

            def pump(src: socket.socket, dst: socket.socket) -> None:
                while not stop.is_set():
                    try:
                        b = src.recv(64 * 1024)
                    except socket.timeout:
                        continue
                    except Exception:
                        break
                    if not b:
                        break
                    try:
                        dst.sendall(b)
                    except Exception:
                        break
                stop.set()
                # Best-effort to wake the other thread promptly.
                try:
                    src.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    dst.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass

            t1 = threading.Thread(target=pump, args=(s, upstream), daemon=True)
            t2 = threading.Thread(target=pump, args=(upstream, s), daemon=True)
            t1.start()
            t2.start()
            t1.join()
            t2.join()
        except Exception:
            return
        finally:
            try:
                s.close()
            except Exception:
                pass
            if upstream is not None:
                try:
                    upstream.close()
                except Exception:
                    pass
            if stream_id > 0:
                st.ondemand.on_ctrl_disconnect(stream_id)


class _ThreadingTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True

    def __init__(self, addr, handler, state: GateState):
        super().__init__(addr, handler)
        self.state = state


class ApiHandler(BaseHTTPRequestHandler):
    server_version = "agent-link/0.1"

    def do_POST(self) -> None:
        srv: "ApiServer" = self.server  # type: ignore[assignment]
        u = urllib.parse.urlparse(self.path)
        if u.path != "/startLink":
            self._json(404, {"ok": False, "error": "not_found"})
            return

        qs = urllib.parse.parse_qs(u.query or "")
        try:
            stream_id = int((qs.get("stream", [""])[0] or "").strip() or "1")
        except ValueError:
            stream_id = 0
        if stream_id <= 0:
            self._json(400, {"ok": False, "error": "bad_stream"})
            return

        call_name = (qs.get("call_name", ["startLink"])[0] or "startLink").strip()
        try:
            ts = int((qs.get("ts", ["0"])[0] or "0").strip())
        except ValueError:
            ts = 0
        nonce = (qs.get("nonce", [""])[0] or "").strip()
        sig = (qs.get("sig", [""])[0] or "").strip()

        # Auth priority:
        # - If AGENT_LINK_SK is set: require signature.
        # - Else fallback to X-Api-Secret if configured (legacy).
        if srv.sk:
            if not srv.check_sig(call_name=call_name, stream_id=stream_id, ts=ts, nonce=nonce, sig_hex=sig):
                self._json(403, {"ok": False, "error": "bad_signature"})
                return
        elif srv.api_secret:
            got = (self.headers.get("x-api-secret") or "").strip()
            if got != srv.api_secret:
                self._json(403, {"ok": False, "error": "forbidden"})
                return

        # Start container early (before client connects to TCP gate).
        try:
            worker = srv.state.ondemand.ensure_up(stream_id)
        except Exception as e:
            self._json(500, {"ok": False, "error": "ensure_up_failed", "detail": str(e)})
            return

        self._json(
            200,
            {
                "ok": True,
                "t": "startLink",
                "token": srv.state.cfg.token,
                "video_port": srv.video_gate_port,
                "ctrl_port": srv.ctrl_gate_port,
                "ts_ms": now_ms(),
            },
        )

    def log_message(self, fmt: str, *args) -> None:
        if os.getenv("AGENT_LINK_HTTP_LOG", "0").strip() == "1":
            super().log_message(fmt, *args)

    def _json(self, code: int, obj: dict) -> None:
        b = json.dumps(obj, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class ApiServer(ThreadingHTTPServer):
    def __init__(
        self,
        addr: tuple[str, int],
        handler,
        *,
        state: GateState,
        api_secret: str,
        public_host: str,
        video_gate_port: int,
        ctrl_gate_port: int,
    ):
        super().__init__(addr, handler)
        self.state = state
        self.api_secret = api_secret or ""
        self.public_host = public_host
        self.video_gate_port = int(video_gate_port)
        self.ctrl_gate_port = int(ctrl_gate_port)

        # HMAC auth for /startLink (optional, but recommended). If set, signature is required.
        self.sk = (os.getenv("AGENT_LINK_SK", "") or "").strip()
        self.sig_skew_sec = env_int("AGENT_LINK_SIG_SKEW_SEC", 60)
        self.nonce_ttl_sec = env_int("AGENT_LINK_NONCE_TTL_SEC", 300)
        self._nonce_lock = threading.Lock()
        self._nonce_exp_ms: dict[str, int] = {}

    def _gc_nonces_locked(self) -> None:
        t = now_ms()
        # Best-effort GC.
        for k, exp in list(self._nonce_exp_ms.items())[:256]:
            if exp <= t:
                self._nonce_exp_ms.pop(k, None)

    def check_sig(self, *, call_name: str, stream_id: int, ts: int, nonce: str, sig_hex: str) -> bool:
        if not self.sk:
            return True  # signature disabled

        if call_name != "startLink":
            return False
        if stream_id <= 0:
            return False
        if not nonce:
            return False
        if not sig_hex:
            return False

        tnow = int(time.time())
        if ts <= 0 or abs(tnow - ts) > int(self.sig_skew_sec):
            return False

        with self._nonce_lock:
            self._gc_nonces_locked()
            if nonce in self._nonce_exp_ms:
                return False  # replay
            self._nonce_exp_ms[nonce] = now_ms() + max(1, int(self.nonce_ttl_sec)) * 1000

        # Canonical string. Keep stable; clients must follow exactly.
        msg = f"call_name={call_name}&stream={int(stream_id)}&ts={int(ts)}&nonce={nonce}"
        mac = hmac.new(self.sk.encode("utf-8"), msg.encode("utf-8"), hashlib.sha256).hexdigest()
        return consteq(mac, sig_hex.lower())


def main() -> int:
    ap = argparse.ArgumentParser()

    ap.add_argument("--api-bind", default=os.getenv("AGENT_LINK_API_BIND", "127.0.0.1"))
    ap.add_argument("--api-port", type=int, default=env_int("AGENT_LINK_API_PORT", 40120))
    ap.add_argument("--api-secret", default=os.getenv("AGENT_LINK_API_SECRET", ""))

    ap.add_argument("--public-host", default=os.getenv("AGENT_LINK_PUBLIC_HOST", "127.0.0.1"))

    ap.add_argument("--video-gate-bind", default=os.getenv("AGENT_LINK_VIDEO_BIND", "0.0.0.0"))
    ap.add_argument("--video-gate-port", type=int, default=env_int("AGENT_LINK_VIDEO_PORT", 40121))
    ap.add_argument("--ctrl-gate-bind", default=os.getenv("AGENT_LINK_CTRL_BIND", "0.0.0.0"))
    ap.add_argument("--ctrl-gate-port", type=int, default=env_int("AGENT_LINK_CTRL_PORT", 40122))

    ap.add_argument("--token", default=os.getenv("AGENT_LINK_TOKEN", "change-me-token"))
    ap.add_argument("--handshake-timeout-sec", type=float, default=float(os.getenv("AGENT_LINK_HANDSHAKE_TIMEOUT_SEC", "3.0")))

    ap.add_argument("--agent-host", default=os.getenv("AGENT_LINK_AGENT_HOST", "127.0.0.1"))
    ap.add_argument("--agent-video-port", type=int, default=env_int("AGENT_LINK_AGENT_VIDEO_PORT", 31234))
    ap.add_argument("--agent-ctrl-port", type=int, default=env_int("AGENT_LINK_AGENT_CTRL_PORT", 31235))

    # Defaults target the "no-source runtime" container image which bundles mlctl.sh.
    ap.add_argument("--mlctl", default=os.getenv("AGENT_LINK_MLCTL", "/app/mlctl.sh"))
    ap.add_argument("--workers-dir", default=os.getenv("AGENT_LINK_WORKERS_DIR", "/app/workers"))
    ap.add_argument("--idle-stop-sec", type=int, default=env_int("AGENT_LINK_IDLE_STOP_SEC", 300))

    args = ap.parse_args()

    cfg = GateConfig(
        token=args.token,
        handshake_timeout_sec=args.handshake_timeout_sec,
        agent_host=args.agent_host,
        agent_video_port=args.agent_video_port,
        agent_ctrl_port=args.agent_ctrl_port,
        mlctl_path=args.mlctl,
        workers_dir=args.workers_dir,
    )

    ondemand = OnDemandManager(mlctl_path=cfg.mlctl_path, workers_dir=cfg.workers_dir, idle_stop_sec=args.idle_stop_sec)
    st = GateState(cfg, ondemand)

    video_srv = _ThreadingTCPServer((args.video_gate_bind, args.video_gate_port), VideoGateHandler, state=st)
    ctrl_srv = _ThreadingTCPServer((args.ctrl_gate_bind, args.ctrl_gate_port), CtrlGateHandler, state=st)
    api_srv = ApiServer(
        (args.api_bind, args.api_port),
        ApiHandler,
        state=st,
        api_secret=args.api_secret,
        public_host=args.public_host,
        video_gate_port=args.video_gate_port,
        ctrl_gate_port=args.ctrl_gate_port,
    )

    th_api = threading.Thread(target=api_srv.serve_forever, daemon=True)
    th_vid = threading.Thread(target=video_srv.serve_forever, daemon=True)
    th_ctl = threading.Thread(target=ctrl_srv.serve_forever, daemon=True)
    th_api.start()
    th_vid.start()
    th_ctl.start()

    print(
        f"[agent_link] api={args.api_bind}:{args.api_port} "
        f"video_gate={args.video_gate_bind}:{args.video_gate_port} ctrl_gate={args.ctrl_gate_bind}:{args.ctrl_gate_port} "
        f"agent={cfg.agent_host}:{cfg.agent_video_port}/{cfg.agent_ctrl_port} idle_stop_sec={ondemand.idle_stop_sec}"
    )

    # Block forever.
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    raise SystemExit(main())

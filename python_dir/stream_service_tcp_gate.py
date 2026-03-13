#!/usr/bin/env python3
"""
stream_service_tcp_gate.py

Goal:
- Provide a small TCP "gate/proxy" in front of strem_agent_server video port.
- Require short-lived tokens (issued via HTTP endpoint) before allowing TCP connections.
- On successful auth, start/stop the corresponding ml_worker container based on stream_id.

TCP protocol (client -> gate):
  AUTH <token>\n
  SUB <stream_id>\n

The gate then connects to strem_agent_server video TCP and forwards:
  (optional) AUTH <AGENT_TOKEN>\n
  SUB <stream_id>\n
and proxies bytes from agent->client.

This is intentionally standalone from web_webrtc.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import socket
import socketserver
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def env_int(name: str, default: int) -> int:
    v = os.getenv(name, "").strip()
    if not v:
        return default
    try:
        return int(v)
    except ValueError:
        return default


def now_ms() -> int:
    return int(time.time() * 1000)


class TokenStore:
    def __init__(
        self,
        ttl_sec: int,
        per_ip_window_sec: int,
        bind_to_ip: bool,
        single_use: bool,
    ):
        self.ttl_ms = max(1, ttl_sec) * 1000
        self.per_ip_window_ms = max(0, per_ip_window_sec) * 1000
        self.bind_to_ip = bind_to_ip
        self.single_use = single_use
        self._lock = threading.Lock()
        self._by_token: dict[str, dict] = {}
        self._by_ip: dict[str, str] = {}  # ip -> token

    def _gc_locked(self, n: int = 64) -> None:
        # Best-effort GC to keep the map bounded.
        t = now_ms()
        killed = 0
        for tok, rec in list(self._by_token.items()):
            if rec["exp_ms"] <= t:
                self._by_token.pop(tok, None)
                if rec.get("ip"):
                    if self._by_ip.get(rec["ip"]) == tok:
                        self._by_ip.pop(rec["ip"], None)
                killed += 1
                if killed >= n:
                    break

    def issue(self, stream_id: int, ip: str, reason: str) -> dict:
        with self._lock:
            self._gc_locked()
            t = now_ms()

            if self.per_ip_window_ms > 0:
                existing_tok = self._by_ip.get(ip, "")
                if existing_tok:
                    rec = self._by_token.get(existing_tok)
                    if rec and rec["exp_ms"] > t and rec["stream_id"] == stream_id:
                        return {
                            "ok": True,
                            "status": "already_issued",
                            "token": existing_tok,
                            "exp_ms": rec["exp_ms"],
                            "reason": reason,
                        }

            # 6 digits numeric token is enough for "human-entered", but still short.
            token = f"{secrets.randbelow(1_000_000):06d}"
            exp_ms = t + self.ttl_ms
            rec = {
                "token": token,
                "stream_id": int(stream_id),
                "ip": ip if self.bind_to_ip else "",
                "issued_ms": t,
                "exp_ms": exp_ms,
            }
            self._by_token[token] = rec
            if self.per_ip_window_ms > 0:
                self._by_ip[ip] = token

            return {"ok": True, "status": "issued", "token": token, "exp_ms": exp_ms, "reason": reason}

    def validate(self, token: str, ip: str, stream_id: int) -> bool:
        with self._lock:
            self._gc_locked()
            rec = self._by_token.get(token)
            if not rec:
                return False
            if rec["exp_ms"] <= now_ms():
                return False
            if int(rec["stream_id"]) != int(stream_id):
                return False
            if self.bind_to_ip and rec.get("ip") and rec["ip"] != ip:
                return False
            if self.single_use:
                self._by_token.pop(token, None)
                if rec.get("ip") and self._by_ip.get(rec["ip"]) == token:
                    self._by_ip.pop(rec["ip"], None)
            return True


def webhook_push(url: str, auth: str, fmt: str, payload: dict) -> dict:
    if not url:
        return {"ok": False, "error": "webhook_not_configured"}

    headers = {"content-type": "application/json"}
    if auth:
        headers["authorization"] = auth

    if fmt == "dingTalk":
        # DingTalk robot webhook: {"msgtype":"text","text":{"content":"..."}}
        if payload.get("t") == "token":
            msg = f"token={payload.get('token','')} stream={payload.get('stream_id','')} exp={payload.get('exp_ms','')}"
        else:
            msg = json.dumps(payload, ensure_ascii=True)
        body_obj = {"msgtype": "text", "text": {"content": msg}}
    else:
        body_obj = payload

    body = json.dumps(body_obj, separators=(",", ":"), ensure_ascii=True).encode("utf-8")

    try:
        req = urllib.request.Request(url, data=body, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=4) as resp:
            txt = resp.read(200).decode("utf-8", errors="replace")
            return {"ok": True, "status": resp.status, "body": txt}
    except urllib.error.URLError as e:
        return {"ok": False, "error": "webhook_request_failed", "detail": str(e)}


def parse_kv_map(s: str) -> dict[int, str]:
    # "1=worker_s1,2=worker_s2"
    out: dict[int, str] = {}
    for part in (s or "").split(","):
        part = part.strip()
        if not part:
            continue
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        k = k.strip()
        v = v.strip()
        if not k or not v:
            continue
        try:
            out[int(k)] = v
        except ValueError:
            continue
    return out


def read_stream_id_from_conf(path: str) -> int | None:
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("STREAM_ID="):
                    # STREAM_ID="1" or STREAM_ID=1
                    rhs = line.split("=", 1)[1].strip().strip('"').strip("'")
                    try:
                        return int(rhs)
                    except ValueError:
                        return None
    except OSError:
        return None
    return None


def find_worker_for_stream_id(workers_dir: str, stream_id: int) -> str | None:
    # Deterministic: sort by filename and pick first match.
    try:
        entries = sorted([p for p in os.listdir(workers_dir) if p.endswith(".conf")])
    except OSError:
        return None
    for fn in entries:
        p = os.path.join(workers_dir, fn)
        sid = read_stream_id_from_conf(p)
        if sid == int(stream_id):
            return fn[:-5]  # strip .conf
    return None


class OnDemandManager:
    def __init__(self, mlctl_path: str, workers_dir: str, stream_to_worker: dict[int, str], idle_stop_sec: int):
        self.mlctl_path = mlctl_path
        self.workers_dir = workers_dir
        self.stream_to_worker = stream_to_worker
        self.idle_stop_sec = max(0, int(idle_stop_sec))
        self._lock = threading.Lock()
        self._active_by_stream: dict[int, int] = {}
        self._stop_timer_by_stream: dict[int, threading.Timer] = {}

    def worker_for_stream(self, stream_id: int) -> str | None:
        w = self.stream_to_worker.get(int(stream_id))
        if w:
            return w
        return find_worker_for_stream_id(self.workers_dir, int(stream_id))

    def _run_mlctl(self, args: list[str]) -> None:
        # Keep stdout/stderr for debugging in logs if needed.
        subprocess.run([self.mlctl_path, *args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def ensure_up(self, stream_id: int) -> str:
        worker = self.worker_for_stream(stream_id)
        if not worker:
            raise RuntimeError(f"no_worker_for_stream_id:{stream_id}")
        self._run_mlctl(["ensure-up", worker])
        return worker

    def stop_soft(self, stream_id: int) -> str:
        worker = self.worker_for_stream(stream_id)
        if not worker:
            raise RuntimeError(f"no_worker_for_stream_id:{stream_id}")
        self._run_mlctl(["stop-soft", worker])
        return worker

    def on_connect(self, stream_id: int) -> None:
        with self._lock:
            t = self._stop_timer_by_stream.pop(int(stream_id), None)
            if t:
                try:
                    t.cancel()
                except Exception:
                    pass
            self._active_by_stream[int(stream_id)] = self._active_by_stream.get(int(stream_id), 0) + 1

    def on_disconnect(self, stream_id: int) -> None:
        stream_id = int(stream_id)
        with self._lock:
            cur = self._active_by_stream.get(stream_id, 0)
            cur = max(0, cur - 1)
            if cur == 0:
                self._active_by_stream.pop(stream_id, None)
            else:
                self._active_by_stream[stream_id] = cur

            if cur != 0 or self.idle_stop_sec <= 0:
                return

            def fire() -> None:
                with self._lock:
                    if self._active_by_stream.get(stream_id, 0) != 0:
                        return
                try:
                    self.stop_soft(stream_id)
                except Exception:
                    # best-effort
                    pass

            tm = threading.Timer(self.idle_stop_sec, fire)
            tm.daemon = True
            self._stop_timer_by_stream[stream_id] = tm
            tm.start()


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


def parse_handshake(lines: list[str]) -> tuple[str, int]:
    token = ""
    stream_id = 0
    for ln in lines:
        if ln.startswith("AUTH "):
            token = ln[5:].strip()
        elif ln.startswith("SUB "):
            v = ln[4:].strip()
            try:
                stream_id = int(v)
            except ValueError:
                stream_id = 0
    return token, stream_id


class GateTCPHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        srv: "GateTCPServer" = self.server  # type: ignore[assignment]
        ip = self.client_address[0]
        s = self.request
        assert isinstance(s, socket.socket)

        try:
            # Read up to 2 lines (AUTH + SUB), tolerate them arriving in any order.
            l1 = recv_line(s, srv.handshake_timeout_sec)
            l2 = recv_line(s, srv.handshake_timeout_sec)
            token, stream_id = parse_handshake([l1, l2])

            if not token or stream_id <= 0:
                s.sendall(b"ERR need_auth_or_stream\n")
                return

            if not srv.tokens.validate(token, ip, stream_id):
                s.sendall(b"ERR bad_token\n")
                return

            # On-demand start/track
            srv.ondemand.on_connect(stream_id)
            try:
                srv.ondemand.ensure_up(stream_id)
            except Exception:
                s.sendall(b"ERR start_stream_failed\n")
                return

            # Connect to strem_agent_server video port and proxy agent->client.
            upstream = socket.create_connection((srv.agent_host, srv.agent_video_port), timeout=3.0)
            try:
                upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass

            if srv.agent_token:
                upstream.sendall(f"AUTH {srv.agent_token}\n".encode("utf-8"))
            upstream.sendall(f"SUB {stream_id}\n".encode("utf-8"))

            s.settimeout(10.0)
            upstream.settimeout(10.0)

            while True:
                data = upstream.recv(64 * 1024)
                if not data:
                    break
                s.sendall(data)
        except Exception:
            return
        finally:
            try:
                token = ""
                stream_id = 0
                # Best-effort: the local vars may not exist if handshake failed early.
                token = locals().get("token", "")
                stream_id = int(locals().get("stream_id", 0) or 0)
                if stream_id > 0:
                    srv.ondemand.on_disconnect(stream_id)
            except Exception:
                pass


class GateTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True

    def __init__(
        self,
        addr: tuple[str, int],
        handler,
        *,
        agent_host: str,
        agent_video_port: int,
        agent_token: str,
        tokens: TokenStore,
        ondemand: OnDemandManager,
        handshake_timeout_sec: float,
    ):
        super().__init__(addr, handler)
        self.agent_host = agent_host
        self.agent_video_port = int(agent_video_port)
        self.agent_token = agent_token or ""
        self.tokens = tokens
        self.ondemand = ondemand
        self.handshake_timeout_sec = float(handshake_timeout_sec)


class TokenHTTPHandler(BaseHTTPRequestHandler):
    server_version = "stream-gate/0.1"

    def do_POST(self) -> None:
        srv: "TokenHTTPServer" = self.server  # type: ignore[assignment]
        url = urllib.parse.urlparse(self.path)
        if url.path != "/token":
            self._json(404, {"ok": False, "error": "not_found"})
            return

        if srv.http_secret:
            got = (self.headers.get("x-token-secret") or "").strip()
            if got != srv.http_secret:
                self._json(403, {"ok": False, "error": "forbidden"})
                return

        qs = urllib.parse.parse_qs(url.query or "")
        try:
            stream_id = int((qs.get("stream", [""])[0] or "").strip())
        except ValueError:
            stream_id = 0
        if stream_id <= 0:
            self._json(400, {"ok": False, "error": "bad_stream"})
            return

        ip = self.client_address[0]
        issued = srv.tokens.issue(stream_id=stream_id, ip=ip, reason="http_token")

        push = webhook_push(
            srv.webhook_url,
            srv.webhook_auth,
            srv.webhook_format,
            {"t": "token", "token": issued.get("token", ""), "stream_id": stream_id, "ip": ip, "exp_ms": issued.get("exp_ms", 0)},
        )

        self._json(200, {"ok": True, "issued": issued, "webhook": push})

    def log_message(self, fmt: str, *args) -> None:
        # Keep stdout clean by default; enable by setting STREAM_GATE_HTTP_LOG=1.
        if os.getenv("STREAM_GATE_HTTP_LOG", "0").strip() == "1":
            super().log_message(fmt, *args)

    def _json(self, code: int, obj: dict) -> None:
        b = json.dumps(obj, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class TokenHTTPServer(ThreadingHTTPServer):
    def __init__(
        self,
        addr: tuple[str, int],
        handler,
        *,
        tokens: TokenStore,
        http_secret: str,
        webhook_url: str,
        webhook_auth: str,
        webhook_format: str,
    ):
        super().__init__(addr, handler)
        self.tokens = tokens
        self.http_secret = http_secret or ""
        self.webhook_url = webhook_url or ""
        self.webhook_auth = webhook_auth or ""
        self.webhook_format = (webhook_format or "raw").strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp-bind", default=os.getenv("STREAM_GATE_TCP_BIND", "0.0.0.0"))
    ap.add_argument("--tcp-port", type=int, default=env_int("STREAM_GATE_TCP_PORT", 40100))
    ap.add_argument("--http-bind", default=os.getenv("STREAM_GATE_HTTP_BIND", "127.0.0.1"))
    ap.add_argument("--http-port", type=int, default=env_int("STREAM_GATE_HTTP_PORT", 40101))
    ap.add_argument("--handshake-timeout-sec", type=float, default=float(os.getenv("STREAM_GATE_HANDSHAKE_TIMEOUT_SEC", "3.0")))

    ap.add_argument("--agent-host", default=os.getenv("STREAM_GATE_AGENT_HOST", "127.0.0.1"))
    ap.add_argument("--agent-video-port", type=int, default=env_int("STREAM_GATE_AGENT_VIDEO_PORT", 31234))
    ap.add_argument("--agent-token", default=os.getenv("STREAM_GATE_AGENT_TOKEN", ""))

    ap.add_argument("--mlctl", default=os.getenv("STREAM_GATE_MLCTL", "/home/gejun/work/my_ml_work/deploy/mlctl.sh"))
    ap.add_argument("--workers-dir", default=os.getenv("STREAM_GATE_WORKERS_DIR", "/home/gejun/work/my_ml_work/deploy/workers"))
    ap.add_argument("--stream-map", default=os.getenv("STREAM_GATE_STREAM_MAP", ""))
    ap.add_argument("--idle-stop-sec", type=int, default=env_int("STREAM_GATE_IDLE_STOP_SEC", 15))

    ap.add_argument("--token-ttl-sec", type=int, default=env_int("STREAM_GATE_TOKEN_TTL_SEC", 300))
    ap.add_argument("--token-per-ip-window-sec", type=int, default=env_int("STREAM_GATE_TOKEN_PER_IP_WINDOW_SEC", 300))
    ap.add_argument("--token-bind-ip", type=int, default=env_int("STREAM_GATE_TOKEN_BIND_IP", 0))
    ap.add_argument("--token-single-use", type=int, default=env_int("STREAM_GATE_TOKEN_SINGLE_USE", 1))
    ap.add_argument("--token-http-secret", default=os.getenv("STREAM_GATE_TOKEN_HTTP_SECRET", ""))

    ap.add_argument("--token-webhook-url", default=os.getenv("STREAM_GATE_TOKEN_WEBHOOK_URL", ""))
    ap.add_argument("--token-webhook-auth", default=os.getenv("STREAM_GATE_TOKEN_WEBHOOK_AUTH", ""))
    ap.add_argument("--token-webhook-format", default=os.getenv("STREAM_GATE_TOKEN_WEBHOOK_FORMAT", "dingTalk"))

    args = ap.parse_args()

    tokens = TokenStore(
        ttl_sec=args.token_ttl_sec,
        per_ip_window_sec=args.token_per_ip_window_sec,
        bind_to_ip=bool(args.token_bind_ip),
        single_use=bool(args.token_single_use),
    )

    ondemand = OnDemandManager(
        mlctl_path=args.mlctl,
        workers_dir=args.workers_dir,
        stream_to_worker=parse_kv_map(args.stream_map),
        idle_stop_sec=args.idle_stop_sec,
    )

    tcp_srv = GateTCPServer(
        (args.tcp_bind, args.tcp_port),
        GateTCPHandler,
        agent_host=args.agent_host,
        agent_video_port=args.agent_video_port,
        agent_token=args.agent_token,
        tokens=tokens,
        ondemand=ondemand,
        handshake_timeout_sec=args.handshake_timeout_sec,
    )

    http_srv = TokenHTTPServer(
        (args.http_bind, args.http_port),
        TokenHTTPHandler,
        tokens=tokens,
        http_secret=args.token_http_secret,
        webhook_url=args.token_webhook_url,
        webhook_auth=args.token_webhook_auth,
        webhook_format=args.token_webhook_format,
    )

    th = threading.Thread(target=http_srv.serve_forever, daemon=True)
    th.start()

    print(
        f"[stream_gate] tcp={args.tcp_bind}:{args.tcp_port} http={args.http_bind}:{args.http_port} "
        f"agent_video={args.agent_host}:{args.agent_video_port} token_webhook={'yes' if args.token_webhook_url else 'no'}"
    )
    tcp_srv.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


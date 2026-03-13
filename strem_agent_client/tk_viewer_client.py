#!/usr/bin/env python3
"""
Tk viewer + input client.

Goals:
- Show remote video without cv2 window.
- Only send keyboard/mouse when the Tk window is focused (no global shortcuts leakage).

Dependencies:
- PyAV (av), numpy
- Pillow (PIL) for fast Tk image updates
"""

from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request

import numpy as np  # type: ignore

try:
    from PIL import Image, ImageTk  # type: ignore
except Exception as e:  # pragma: no cover
    raise SystemExit("Missing Pillow. Install with: pip install Pillow") from e

import tkinter as tk


# =====================
# Config (edit here)
# =====================
SERVER_HOST = "192.168.11.31"  # agent_link_service host
STREAM_ID = 1

ENABLE_STARTLINK = True
STARTLINK_URL = f"http://{SERVER_HOST}:40120/startLink"
STARTLINK_SK = "change-me-sk"  # must match AGENT_LINK_SK on server

VIDEO_PORT = 40121
CTRL_PORT = 40122
TOKEN = "your-static-token"


# =====================
# Ctrl protocol
# =====================
ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1
ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_MOUSE_BUTTON = 3
ML_CTRL_CMD_MOUSE_SCROLL = 5
ML_CTRL_CMD_MOUSE_HSCROLL = 6
ML_CTRL_CMD_KEY_PRESS = 8
ML_CTRL_CMD_TEXT = 9
ML_CTRL_CMD_REQ_IDR = 10

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08
BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03


def be32(n: int) -> bytes:
    return struct.pack(">I", n)


def _hmac_sha256_hex(sk: str, msg: str) -> str:
    return hmac.new(sk.encode("utf-8"), msg.encode("utf-8"), hashlib.sha256).hexdigest()


def start_link(stream_id: int) -> dict:
    ts = int(time.time())  # UTC epoch seconds
    nonce = secrets.token_hex(8)
    call_name = "startLink"
    msg = f"call_name={call_name}&stream={int(stream_id)}&ts={ts}&nonce={nonce}"
    sig = _hmac_sha256_hex(STARTLINK_SK, msg)

    q = urllib.parse.urlencode(
        {
            "stream": str(int(stream_id)),
            "call_name": call_name,
            "ts": str(ts),
            "nonce": nonce,
            "sig": sig,
        }
    )
    url = f"{STARTLINK_URL}?{q}"
    req = urllib.request.Request(url, data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=5) as resp:
        body = resp.read()
    obj = json.loads(body.decode("utf-8", errors="replace"))
    if not isinstance(obj, dict) or not obj.get("ok"):
        raise RuntimeError(f"startLink failed: {obj!r}")
    return obj


def vk_from_ascii(ch: str) -> int:
    if not ch:
        return 0
    c = ch
    if "a" <= c <= "z":
        c = c.upper()
    o = ord(c)
    if ord("A") <= o <= ord("Z"):
        return o
    if ord("0") <= o <= ord("9"):
        return o
    if c == " ":
        return 0x20  # VK_SPACE
    if c == "\t":
        return 0x09  # VK_TAB
    if c == "\r" or c == "\n":
        return 0x0D  # VK_RETURN
    oem = {
        "-": 0xBD,
        "=": 0xBB,
        "[": 0xDB,
        "]": 0xDD,
        "\\": 0xDC,
        ";": 0xBA,
        "'": 0xDE,
        ",": 0xBC,
        ".": 0xBE,
        "/": 0xBF,
        "`": 0xC0,
    }
    return oem.get(c, 0)


def vk_from_tk_keysym(keysym: str) -> int:
    special = {
        "Return": 0x0D,
        "Tab": 0x09,
        "BackSpace": 0x08,
        "Escape": 0x1B,
        "Delete": 0x2E,
        "Insert": 0x2D,
        "Home": 0x24,
        "End": 0x23,
        "Prior": 0x21,  # PageUp
        "Next": 0x22,  # PageDown
        "Left": 0x25,
        "Up": 0x26,
        "Right": 0x27,
        "Down": 0x28,
    }
    if keysym in special:
        return special[keysym]
    if keysym.startswith("F"):
        try:
            n = int(keysym[1:])
        except ValueError:
            return 0
        if 1 <= n <= 12:
            return 0x70 + (n - 1)
    return 0


class CtrlSender:
    def __init__(self, host: str, port: int, token: str, stream_id: int):
        self.addr = (host, int(port))
        self.token = token
        self.stream_id = int(stream_id)
        self.sock: socket.socket | None = None
        self.seq = 0
        self.lock = threading.Lock()
        self._last_connect_attempt = 0.0

    def connect_best_effort(self) -> None:
        try:
            s = socket.create_connection(self.addr, timeout=3)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.token:
                s.sendall(f"AUTH {self.token}\n".encode("utf-8"))
            s.sendall(f"SUB {self.stream_id}\n".encode("utf-8"))
            self.sock = s
            print(f"[ctrl] connected: {self.addr}")
        except Exception as e:
            self.sock = None
            print(f"[ctrl] connect failed: {e}")

    def _ensure_connected(self) -> bool:
        if self.sock:
            return True
        now = time.time()
        if now - float(self._last_connect_attempt) < 1.0:
            return False
        self._last_connect_attempt = now
        self.connect_best_effort()
        return self.sock is not None

    def _send(self, cmd_type: int, a=0, b=0, c=0, d=0, payload: bytes = b"") -> None:
        if not self._ensure_connected():
            return

        pkt = b""
        with self.lock:
            self.seq += 1
            hdr = _CMD_STRUCT.pack(
                ML_CTRL_MAGIC,
                ML_CTRL_VERSION,
                int(cmd_type),
                int(a),
                int(b),
                int(c),
                int(d),
                int(self.seq),
            )
            pkt = hdr + payload
            sock = self.sock

        def _try_send() -> bool:
            nonlocal sock, pkt
            if not sock:
                return False
            try:
                sock.sendall(be32(len(pkt)) + pkt)
                return True
            except Exception:
                try:
                    sock.close()
                except Exception:
                    pass
                return False

        if _try_send():
            return

        # Ctrl gate may drop idle connections; reconnect once and retry.
        with self.lock:
            self.sock = None
        if not self._ensure_connected():
            return
        with self.lock:
            sock = self.sock
        _try_send()

    def mouse_abs(self, x: int, y: int, ref_w: int, ref_h: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_button(self, action: int, button: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button, 0, 0)

    def mouse_scroll(self, clicks: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, int(clicks), 0, 0, 0)

    def mouse_hscroll(self, clicks: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, int(clicks), 0, 0, 0)

    def key_press(self, vk: int) -> None:
        self._send(ML_CTRL_CMD_KEY_PRESS, int(vk), 0, 0, 0)

    def text(self, s: str) -> None:
        payload = s.encode("utf-8", errors="ignore")
        self._send(ML_CTRL_CMD_TEXT, len(payload), 0, 0, 0, payload=payload)

    def request_idr(self) -> None:
        self._send(ML_CTRL_CMD_REQ_IDR, 0, 0, 0, 0)


def video_recv_thread(host: str, port: int, stream_id: int, token: str, frame_cb) -> None:
    try:
        import av  # type: ignore
    except Exception as e:
        raise RuntimeError("PyAV is required (pip install av)") from e

    s = socket.create_connection((host, int(port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if token:
        s.sendall(f"AUTH {token}\n".encode("utf-8"))
    s.sendall(f"SUB {int(stream_id)}\n".encode("utf-8"))
    s.settimeout(1.0)

    codec = av.CodecContext.create("h264", "r")
    buf = bytearray(1024 * 1024)
    while True:
        try:
            n = s.recv_into(buf)
        except (TimeoutError, socket.timeout):
            continue
        if n <= 0:
            break
        data = bytes(memoryview(buf)[:n])
        if data.startswith(b"ERR "):
            try:
                msg = data.decode("utf-8", errors="replace").strip()
            except Exception:
                msg = repr(data[:64])
            raise RuntimeError(f"video gate rejected: {msg}")

        for packet in codec.parse(data):
            try:
                frames = codec.decode(packet)
            except Exception:
                frames = []
            for fr in frames:
                img = fr.to_ndarray(format="bgr24")
                frame_cb(img)


class TkViewer:
    def __init__(self, host: str, video_port: int, ctrl_port: int, stream_id: int, token: str):
        self.host = host
        self.video_port = int(video_port)
        self.ctrl = CtrlSender(host, int(ctrl_port), token=token, stream_id=int(stream_id))
        self.stream_id = int(stream_id)
        self.token = token

        self.root = tk.Tk()
        self.root.title("tk_viewer_client")
        self.root.geometry("1280x720")

        self.focused = False
        self.root.bind("<FocusIn>", self._on_focus_in)
        self.root.bind("<FocusOut>", self._on_focus_out)

        self.canvas = tk.Canvas(self.root, bg="#111", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.focus_set()

        self._img_lock = threading.Lock()
        self._latest_bgr: np.ndarray | None = None
        self._photo = None
        self._disp = (0, 0, 1, 1)  # x0,y0,w,h in canvas

        # input bindings (only fire when window focused)
        self.root.bind("<Key>", self._on_key)
        self.canvas.bind("<Motion>", self._on_mouse_move)
        self.canvas.bind("<ButtonPress-1>", self._on_mouse_down_left)
        self.canvas.bind("<ButtonRelease-1>", self._on_mouse_up_left)
        self.canvas.bind("<ButtonPress-3>", self._on_mouse_down_right)
        self.canvas.bind("<ButtonRelease-3>", self._on_mouse_up_right)
        self.canvas.bind("<MouseWheel>", self._on_mouse_wheel)  # Windows/macOS
        self.canvas.bind("<Button-4>", self._on_mouse_wheel_linux_up)  # X11
        self.canvas.bind("<Button-5>", self._on_mouse_wheel_linux_down)  # X11

        self.ctrl.connect_best_effort()
        self.ctrl.request_idr()

        self._start_video_thread()
        self._tick()

    def _on_focus_in(self, _e=None):
        if not self.focused:
            self.focused = True
            print("[input] focused: ON")

    def _on_focus_out(self, _e=None):
        if self.focused:
            self.focused = False
            print("[input] focused: OFF")

    def _start_video_thread(self) -> None:
        def on_frame(bgr: np.ndarray) -> None:
            with self._img_lock:
                self._latest_bgr = bgr

        t = threading.Thread(
            target=video_recv_thread,
            args=(self.host, self.video_port, self.stream_id, self.token, on_frame),
            daemon=True,
        )
        t.start()

    def _tick(self) -> None:
        bgr = None
        with self._img_lock:
            if self._latest_bgr is not None:
                bgr = self._latest_bgr.copy()

        if bgr is not None:
            h, w = bgr.shape[:2]
            cw = max(1, int(self.canvas.winfo_width()))
            ch = max(1, int(self.canvas.winfo_height()))

            # fit image into canvas keeping aspect ratio
            scale = min(cw / float(w), ch / float(h))
            dw = max(1, int(round(w * scale)))
            dh = max(1, int(round(h * scale)))
            x0 = (cw - dw) // 2
            y0 = (ch - dh) // 2
            self._disp = (x0, y0, dw, dh)

            rgb = bgr[:, :, ::-1]
            pil = Image.fromarray(rgb)
            if dw != w or dh != h:
                pil = pil.resize((dw, dh), resample=Image.BILINEAR)
            self._photo = ImageTk.PhotoImage(pil)
            self.canvas.delete("all")
            self.canvas.create_image(x0, y0, anchor="nw", image=self._photo)
            self.canvas.create_text(
                10,
                10,
                anchor="nw",
                fill="#ddd",
                text=f"stream={self.stream_id} focused={'ON' if self.focused else 'OFF'}  (click window to focus)",
            )

        self.root.after(15, self._tick)

    def _map_xy(self, x: int, y: int) -> tuple[int, int, int, int] | None:
        with self._img_lock:
            img = self._latest_bgr
        if img is None:
            return None
        ih, iw = img.shape[:2]
        x0, y0, dw, dh = self._disp
        if dw <= 0 or dh <= 0:
            return None
        if x < x0 or y < y0 or x >= x0 + dw or y >= y0 + dh:
            return None
        ix = int((x - x0) * iw / float(dw))
        iy = int((y - y0) * ih / float(dh))
        ix = max(0, min(iw - 1, ix))
        iy = max(0, min(ih - 1, iy))
        return ix, iy, iw, ih

    def _on_key(self, e) -> None:
        if not self.focused:
            return
        keysym = getattr(e, "keysym", "") or ""
        ch = getattr(e, "char", "") or ""

        # Prefer sending text for printable characters. This avoids platform-specific VK mapping.
        if ch and 32 <= ord(ch) <= 126:
            self.ctrl.text(ch)
            return

        vk = vk_from_tk_keysym(keysym)
        if vk:
            self.ctrl.key_press(vk)

    def _on_mouse_move(self, e) -> None:
        if not self.focused:
            return
        m = self._map_xy(int(e.x), int(e.y))
        if not m:
            return
        x, y, w, h = m
        self.ctrl.mouse_abs(x, y, w, h)

    def _on_mouse_down_left(self, e) -> None:
        if not self.focused:
            return
        self._on_mouse_move(e)
        self.ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_LEFT)

    def _on_mouse_up_left(self, e) -> None:
        if not self.focused:
            return
        self._on_mouse_move(e)
        self.ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT)

    def _on_mouse_down_right(self, e) -> None:
        if not self.focused:
            return
        self._on_mouse_move(e)
        self.ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_RIGHT)

    def _on_mouse_up_right(self, e) -> None:
        if not self.focused:
            return
        self._on_mouse_move(e)
        self.ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT)

    def _on_mouse_wheel(self, e) -> None:
        if not self.focused:
            return
        delta = int(getattr(e, "delta", 0) or 0)
        if delta:
            clicks = 1 if delta > 0 else -1
            self.ctrl.mouse_scroll(clicks)

    def _on_mouse_wheel_linux_up(self, _e) -> None:
        if not self.focused:
            return
        self.ctrl.mouse_scroll(1)

    def _on_mouse_wheel_linux_down(self, _e) -> None:
        if not self.focused:
            return
        self.ctrl.mouse_scroll(-1)

    def run(self) -> int:
        self.root.mainloop()
        return 0


def main() -> int:
    global VIDEO_PORT, CTRL_PORT, TOKEN

    if ENABLE_STARTLINK:
        r = start_link(STREAM_ID)
        TOKEN = str(r.get("token") or TOKEN or "")
        VIDEO_PORT = int(r.get("video_port") or VIDEO_PORT)
        CTRL_PORT = int(r.get("ctrl_port") or CTRL_PORT)
        print(f"[startLink] ok token={'yes' if TOKEN else 'no'} video_port={VIDEO_PORT} ctrl_port={CTRL_PORT}")

    app = TkViewer(SERVER_HOST, VIDEO_PORT, CTRL_PORT, STREAM_ID, TOKEN)
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())


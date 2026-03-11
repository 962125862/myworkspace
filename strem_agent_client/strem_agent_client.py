"""strem_agent_client (Python)

Video:
- Connect to agent_server video TCP, send optional AUTH/token and SUB stream_id
- Receive AnnexB H.264 bytestream, decode with PyAV, display via OpenCV

Control:
- Connect to agent_server control TCP, send optional AUTH/token
- Capture keyboard (cv2.waitKey) and mouse (cv2.setMouseCallback)
- Send control messages with framing: [u32_be length][MlControlCmd+payload]

Notes:
- PyAV wheels may or may not include hw decode backends; we do best-effort by selecting codec name.
"""

from __future__ import annotations

import argparse
import socket
import struct
import threading
import time
from dataclasses import dataclass

import av  # type: ignore
import cv2  # type: ignore
import numpy as np  # type: ignore


ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1

ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_MOUSE_REL = 2
ML_CTRL_CMD_MOUSE_BUTTON = 3
ML_CTRL_CMD_MOUSE_CLICK = 4
ML_CTRL_CMD_MOUSE_SCROLL = 5
ML_CTRL_CMD_MOUSE_HSCROLL = 6
ML_CTRL_CMD_KEYBOARD = 7
ML_CTRL_CMD_KEY_PRESS = 8
ML_CTRL_CMD_TEXT = 9

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08

BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03

KEY_ACTION_DOWN = 0x03
KEY_ACTION_UP = 0x04

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")


def be32(n: int) -> bytes:
    return struct.pack(">I", n)


class CtrlSender:
    def __init__(self, host: str, port: int, token: str = ""):
        self.addr = (host, int(port))
        self.token = token
        self.sock: socket.socket | None = None
        self.seq = 0
        self.lock = threading.Lock()

    def connect(self):
        s = socket.create_connection(self.addr, timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.token:
            s.sendall(f"AUTH {self.token}\n".encode("utf-8"))
        self.sock = s

    def _send(self, cmd_type: int, a=0, b=0, c=0, d=0, payload: bytes = b""):
        if not self.sock:
            return
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
            self.sock.sendall(be32(len(pkt)) + pkt)

    # ---------- mouse ----------
    def mouse_abs(self, x, y, ref_w=0, ref_h=0):
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_rel(self, dx, dy):
        self._send(ML_CTRL_CMD_MOUSE_REL, dx, dy)

    def button(self, button=BUTTON_LEFT, pressed=True):
        action = BUTTON_ACTION_PRESS if pressed else BUTTON_ACTION_RELEASE
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button)

    def click(self, button=BUTTON_LEFT):
        self._send(ML_CTRL_CMD_MOUSE_CLICK, button)

    def scroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, int(clicks))

    # ---------- keyboard ----------
    def key(self, key_code: int, action: int, modifiers: int = 0):
        self._send(ML_CTRL_CMD_KEYBOARD, key_code, action, modifiers)

    def key_press(self, key_code: int, modifiers: int = 0):
        self._send(ML_CTRL_CMD_KEY_PRESS, key_code, modifiers)

    # ---------- text ----------
    def text(self, s: str):
        payload = s.encode("utf-8")
        self._send(ML_CTRL_CMD_TEXT, len(payload), payload=payload)


@dataclass
class VideoCfg:
    host: str
    port: int
    stream_id: int
    token: str


def video_recv_thread(cfg: VideoCfg, frame_cb):
    s = socket.create_connection((cfg.host, int(cfg.port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if cfg.token:
        s.sendall(f"AUTH {cfg.token}\n".encode("utf-8"))
    s.sendall(f"SUB {cfg.stream_id}\n".encode("utf-8"))

    # Best-effort hw decode: pick codec name based on OS; fallback to h264
    # PyAV uses FFmpeg; availability depends on build.
    import platform

    codec_candidates = ["h264"]
    sys = platform.system().lower()
    if "darwin" in sys or "mac" in sys:
        codec_candidates = ["h264_videotoolbox", "h264"]
    elif "windows" in sys:
        codec_candidates = ["h264_d3d11va", "h264_dxva2", "h264"]

    codec = None
    last_err = None
    for name in codec_candidates:
        try:
            codec = av.CodecContext.create(name, "r")
            break
        except Exception as e:
            last_err = e
            codec = None
    if codec is None:
        raise RuntimeError(f"failed to create decoder codec: {last_err}")

    parser = av.Parser.create(codec.name)

    buf = bytearray(1024 * 1024)
    while True:
        n = s.recv_into(buf)
        if n <= 0:
            break
        data = bytes(memoryview(buf)[:n])
        for packet in parser.parse(data):
            try:
                frames = codec.decode(packet)
            except Exception:
                frames = []
            for fr in frames:
                # convert to BGR for OpenCV
                img = fr.to_ndarray(format="bgr24")
                frame_cb(img)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--video-port", type=int, default=31234)
    ap.add_argument("--ctrl-port", type=int, default=31235)
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--token", default="")
    args = ap.parse_args()

    ctrl = CtrlSender(args.host, args.ctrl_port, token=args.token)
    ctrl.connect()

    latest = {"img": None}
    lock = threading.Lock()

    def on_frame(img):
        with lock:
            latest["img"] = img

    vt = threading.Thread(
        target=video_recv_thread,
        args=(VideoCfg(args.host, args.video_port, args.stream_id, args.token), on_frame),
        daemon=True,
    )
    vt.start()

    win = "strem_agent_client"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    # mouse callback
    state = {"down": False}

    def mouse_cb(event, x, y, flags, param):
        if event == cv2.EVENT_LBUTTONDOWN:
            state["down"] = True
            ctrl.button(BUTTON_LEFT, pressed=True)
        elif event == cv2.EVENT_LBUTTONUP:
            state["down"] = False
            ctrl.button(BUTTON_LEFT, pressed=False)
        elif event == cv2.EVENT_MOUSEMOVE:
            # send abs in current window coord system; ref_w/ref_h filled by client-side image size
            with lock:
                img = latest["img"]
            if img is not None:
                h, w = img.shape[:2]
                ctrl.mouse_abs(x, y, w, h)
        elif event == cv2.EVENT_MOUSEWHEEL:
            # flags contains wheel delta in high 16 bits on some platforms; OpenCV behavior varies.
            delta = (flags >> 16) & 0xFFFF
            if delta & 0x8000:
                delta = delta - 0x10000
            if delta:
                ctrl.scroll(1 if delta > 0 else -1)

    cv2.setMouseCallback(win, mouse_cb)

    last_show = time.time()
    while True:
        with lock:
            img = latest["img"]
        if img is not None:
            cv2.imshow(win, img)
        k = cv2.waitKey(1) & 0xFF
        if k == 27:  # ESC
            break
        # very simple keyboard mapping: send ASCII as TEXT
        if 32 <= k <= 126:
            ctrl.text(chr(k))

        # avoid tight loop when no frames
        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())


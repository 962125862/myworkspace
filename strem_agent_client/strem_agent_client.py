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
import subprocess
import threading
import time
from dataclasses import dataclass

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
ML_CTRL_CMD_REQ_IDR = 10

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

    def mouse_button(self, action: int, button: int):
        # a=action, b=button
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button, 0, 0)

    def mouse_click(self, button: int):
        # a=button
        self._send(ML_CTRL_CMD_MOUSE_CLICK, button, 0, 0, 0)

    def mouse_scroll(self, clicks: int):
        # a=clicks (signed char on receiver)
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, clicks, 0, 0, 0)

    def mouse_hscroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, clicks, 0, 0, 0)

    # ---------- keyboard ----------
    def key_press(self, vk: int, modifiers: int = 0):
        # a=keyCode(VK), b=modifiers
        self._send(ML_CTRL_CMD_KEY_PRESS, vk, modifiers, 0, 0)

    def text(self, s: str):
        payload = s.encode("utf-8", errors="ignore")
        # a = payload length
        self._send(ML_CTRL_CMD_TEXT, len(payload), 0, 0, 0, payload=payload)

    def request_idr(self):
        self._send(ML_CTRL_CMD_REQ_IDR)


def clamp_i8(v: int) -> int:
    if v < -127:
        return -127
    if v > 127:
        return 127
    return int(v)


def vk_from_ascii(ch: str) -> int:
    """Best-effort mapping from ASCII char to Win32 VK code.

    Limelight expects VK codes and interprets them as keys on a US English layout.
    This mapping is not exhaustive but covers common keys.
    """
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

    # Common US keyboard OEM keys
    oem = {
        "-": 0xBD,  # VK_OEM_MINUS
        "=": 0xBB,  # VK_OEM_PLUS
        "[": 0xDB,  # VK_OEM_4
        "]": 0xDD,  # VK_OEM_6
        "\\": 0xDC,  # VK_OEM_5
        ";": 0xBA,  # VK_OEM_1
        "'": 0xDE,  # VK_OEM_7
        ",": 0xBC,  # VK_OEM_COMMA
        ".": 0xBE,  # VK_OEM_PERIOD
        "/": 0xBF,  # VK_OEM_2
        "`": 0xC0,  # VK_OEM_3
    }
    return oem.get(c, 0)


@dataclass
class VideoCfg:
    host: str
    port: int
    stream_id: int
    token: str


def video_recv_thread(cfg: VideoCfg, frame_cb):
    # We prefer PyAV for lowest latency, but provide a ffmpeg(1) fallback so the
    # client can run even when PyAV wheels aren't available.
    av = None
    try:  # pragma: no cover
        import av as _av  # type: ignore

        av = _av
    except Exception:
        av = None

    s = socket.create_connection((cfg.host, int(cfg.port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if cfg.token:
        s.sendall(f"AUTH {cfg.token}\n".encode("utf-8"))
    s.sendall(f"SUB {cfg.stream_id}\n".encode("utf-8"))

    # create_connection(timeout=...) sets socket recv timeout too.
    # If upstream hasn't started streaming yet, recv() may raise timeout.
    # We want to keep waiting.
    s.settimeout(1.0)

    if av is not None:
        # ---------- PyAV path ----------
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

        # PyAV API compatibility:
        # - Newer versions: CodecContext.parse(data) -> [Packet, ...]
        # - Older versions: av.Parser.create(codec_name).parse(data)
        use_codec_parse = hasattr(codec, "parse")
        parser = None
        if not use_codec_parse:
            # Older PyAV
            if not hasattr(av, "Parser"):
                raise RuntimeError(
                    "This PyAV build has neither CodecContext.parse nor av.Parser. "
                    "Please upgrade PyAV."
                )
            parser = av.Parser.create(codec.name)

        buf = bytearray(1024 * 1024)
        while True:
            try:
                n = s.recv_into(buf)
            except (TimeoutError, socket.timeout):
                continue
            if n <= 0:
                break
            data = bytes(memoryview(buf)[:n])

            packets = codec.parse(data) if use_codec_parse else parser.parse(data)  # type: ignore[union-attr]
            for packet in packets:
                try:
                    frames = codec.decode(packet)
                except Exception:
                    frames = []
                for fr in frames:
                    img = fr.to_ndarray(format="bgr24")
                    frame_cb(img)
    else:
        # ---------- ffmpeg(1) fallback path ----------
        # Decode H264 AnnexB -> PNG frames (image2pipe), then cv2.imdecode.
        # This avoids PyAV dependency, but is heavier than the PyAV path.

        PNG_SIG = b"\x89PNG\r\n\x1a\n"

        def extract_pngs(buf2: bytearray):
            """Yield complete PNG files from an in-memory byte buffer."""
            out = []
            while True:
                sig_i = buf2.find(PNG_SIG)
                if sig_i < 0:
                    # keep tail in case signature is split
                    if len(buf2) > len(PNG_SIG):
                        del buf2[:-len(PNG_SIG)]
                    break
                if sig_i > 0:
                    del buf2[:sig_i]
                # Now buf2 starts with signature
                i = len(PNG_SIG)
                while True:
                    if len(buf2) < i + 8:
                        return out
                    ln = int.from_bytes(buf2[i : i + 4], "big")
                    typ = bytes(buf2[i + 4 : i + 8])
                    chunk_total = 4 + 4 + ln + 4
                    if len(buf2) < i + chunk_total:
                        return out
                    i += chunk_total
                    if typ == b"IEND":
                        out.append(bytes(buf2[:i]))
                        del buf2[:i]
                        break
            return out
        cmd = [
            "ffmpeg",
            "-loglevel",
            "error",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-probesize",
            "32",
            "-analyzeduration",
            "0",
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-f",
            "image2pipe",
            "-vcodec",
            "png",
            "pipe:1",
        ]
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )
        assert proc.stdin is not None
        assert proc.stdout is not None

        stop = threading.Event()

        def png_reader():
            buf2 = bytearray()
            try:
                while not stop.is_set():
                    chunk = proc.stdout.read(65536)
                    if not chunk:
                        break
                    buf2 += chunk
                    for png in extract_pngs(buf2):
                        arr = np.frombuffer(png, dtype=np.uint8)
                        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                        if img is not None:
                            frame_cb(img)
                    # avoid unbounded memory if stream is corrupted
                    if len(buf2) > 50 * 1024 * 1024:
                        del buf2[:-5 * 1024 * 1024]
            finally:
                stop.set()

        rt = threading.Thread(target=png_reader, daemon=True)
        rt.start()

        buf = bytearray(1024 * 1024)
        try:
            while not stop.is_set():
                try:
                    n = s.recv_into(buf)
                except (TimeoutError, socket.timeout):
                    continue
                if n <= 0:
                    break
                try:
                    proc.stdin.write(memoryview(buf)[:n])
                    proc.stdin.flush()
                except BrokenPipeError:
                    break
        finally:
            stop.set()
            try:
                proc.stdin.close()
            except Exception:
                pass
            try:
                proc.terminate()
            except Exception:
                pass
            rt.join(timeout=1.0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--video-port", type=int, default=31234)
    ap.add_argument("--ctrl-port", type=int, default=31235)
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--token", default="")
    ap.add_argument(
        "--ctrl-ref-w",
        type=int,
        default=0,
        help=(
            "Reference width for ML_CTRL_CMD_MOUSE_ABS (optional). "
            "If set (>0), mouse coords will be mapped from decoded frame (video) size "
            "to this reference size before sending. If unset (0), use decoded frame width."
        ),
    )
    ap.add_argument(
        "--ctrl-ref-h",
        type=int,
        default=0,
        help=(
            "Reference height for ML_CTRL_CMD_MOUSE_ABS (optional). "
            "If set (>0), mouse coords will be mapped from decoded frame (video) size "
            "to this reference size before sending. If unset (0), use decoded frame height."
        ),
    )
    args = ap.parse_args()

    ctrl = CtrlSender(args.host, args.ctrl_port, token=args.token)
    ctrl.connect()
    # Improve late-join experience: request an IDR soon.
    ctrl.request_idr()

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

    def aspect_snap_size(win_w: int, win_h: int, img_w: int, img_h: int) -> tuple[int, int]:
        """Snap window size to keep aspect ratio == (img_w/img_h).

        OpenCV doesn't provide a native "lock aspect" window mode, so we keep the window
        resizable but continuously snap it back to the nearest size with the correct aspect.

        Returns (snap_w, snap_h).
        """
        if win_w <= 0 or win_h <= 0 or img_w <= 0 or img_h <= 0:
            return img_w, img_h

        # Candidate 1: keep width, adjust height
        c1_w = win_w
        c1_h = max(1, int(round(win_w * (img_h / float(img_w)))))

        # Candidate 2: keep height, adjust width
        c2_h = win_h
        c2_w = max(1, int(round(win_h * (img_w / float(img_h)))))

        d1 = (c1_w - win_w) * (c1_w - win_w) + (c1_h - win_h) * (c1_h - win_h)
        d2 = (c2_w - win_w) * (c2_w - win_w) + (c2_h - win_h) * (c2_h - win_h)
        return (c1_w, c1_h) if d1 <= d2 else (c2_w, c2_h)

    # Throttle window snapping to reduce jitter while user drags the window.
    snap_state = {"t": 0.0, "w": 0, "h": 0}
    SNAP_MIN_INTERVAL_SEC = 0.05

    def ensure_window_aspect(img_w: int, img_h: int) -> tuple[int, int]:
        """Ensure window keeps aspect ratio, return the (target_w, target_h) used for display."""
        try:
            _x, _y, win_w, win_h = cv2.getWindowImageRect(win)
        except Exception:
            win_w, win_h = img_w, img_h

        target_w, target_h = aspect_snap_size(win_w, win_h, img_w, img_h)

        now = time.time()
        need_resize = abs(target_w - win_w) >= 3 or abs(target_h - win_h) >= 3
        if need_resize:
            if now - float(snap_state["t"]) >= SNAP_MIN_INTERVAL_SEC:
                # Also avoid repeating identical sizes
                if int(target_w) != int(snap_state["w"]) or int(target_h) != int(snap_state["h"]):
                    try:
                        cv2.resizeWindow(win, int(target_w), int(target_h))
                        snap_state.update({"t": now, "w": int(target_w), "h": int(target_h)})
                    except Exception:
                        pass

        return int(target_w), int(target_h)

    # mouse callback
    state = {"down": False}

    def mouse_cb(event, x, y, flags, param):
        with lock:
            img = latest["img"]
        if img is None:
            return

        img_h, img_w = img.shape[:2]
        try:
            _x, _y, win_w, win_h = cv2.getWindowImageRect(win)
        except Exception:
            win_w, win_h = img_w, img_h

        snap_w, snap_h = aspect_snap_size(int(win_w), int(win_h), int(img_w), int(img_h))
        if snap_w <= 0 or snap_h <= 0:
            return

        # Map window coords -> decoded frame coords
        ix = float(x) * float(img_w) / float(snap_w)
        iy = float(y) * float(img_h) / float(snap_h)
        if ix < 0 or iy < 0 or ix >= img_w or iy >= img_h:
            return

        # Optional: map to a custom reference size (e.g. host desktop resolution)
        ref_w = int(args.ctrl_ref_w) if int(args.ctrl_ref_w) > 0 else img_w
        ref_h = int(args.ctrl_ref_h) if int(args.ctrl_ref_h) > 0 else img_h
        sx = int(round(ix * ref_w / float(img_w)))
        sy = int(round(iy * ref_h / float(img_h)))

        if event == cv2.EVENT_MOUSEMOVE:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
        elif event == cv2.EVENT_LBUTTONDOWN:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_LEFT)
        elif event == cv2.EVENT_LBUTTONUP:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT)
        elif event == cv2.EVENT_RBUTTONDOWN:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_RIGHT)
        elif event == cv2.EVENT_RBUTTONUP:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT)
        elif event == cv2.EVENT_MBUTTONDOWN:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_MIDDLE)
        elif event == cv2.EVENT_MBUTTONUP:
            ctrl.mouse_abs(sx, sy, ref_w, ref_h)
            ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE)
        elif event == cv2.EVENT_MOUSEWHEEL:
            try:
                delta = cv2.getMouseWheelDelta(flags)
            except Exception:
                delta = 0
            clicks = int(delta / 120) if delta else 0
            if clicks == 0 and delta:
                clicks = 1 if delta > 0 else -1
            if clicks:
                ctrl.mouse_scroll(clamp_i8(clicks))
        elif event == cv2.EVENT_MOUSEHWHEEL:
            try:
                delta = cv2.getMouseWheelDelta(flags)
            except Exception:
                delta = 0
            clicks = int(delta / 120) if delta else 0
            if clicks == 0 and delta:
                clicks = 1 if delta > 0 else -1
            if clicks:
                ctrl.mouse_hscroll(clamp_i8(clicks))

    cv2.setMouseCallback(win, mouse_cb)

    last_show = time.time()
    while True:
        with lock:
            img = latest["img"]
        if img is not None:
            ih, iw = img.shape[:2]
            target_w, target_h = ensure_window_aspect(iw, ih)
            # Display exactly fills the window (no black bars), because we keep the window aspect.
            show = cv2.resize(img, (target_w, target_h), interpolation=cv2.INTER_AREA)
            cv2.imshow(win, show)
        k = cv2.waitKey(1) & 0xFF
        if k == 27:  # ESC
            break

        # Basic keyboard integration (best-effort): send VK key press for common ASCII keys.
        # Note: OpenCV doesn't expose key-up events, so this is "press" (down+up) behavior.
        if k != 255:
            if k == 8:
                ctrl.key_press(0x08)  # VK_BACK
            elif k == 13:
                ctrl.key_press(0x0D)  # VK_RETURN
            elif k == 9:
                ctrl.key_press(0x09)  # VK_TAB
            elif 32 <= k <= 126:
                ch = chr(k)
                vk = vk_from_ascii(ch)
                if vk:
                    ctrl.key_press(vk)
                else:
                    # Fallback: text input
                    ctrl.text(ch)
        # avoid tight loop when no frames
        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

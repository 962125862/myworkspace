"""IDE demo client (Windows-friendly)

把配置写死在代码里，便于你直接在 Windows 的 IDE 里点 Run。

依赖：
  pip install -r strem_agent_client/requirements.txt

说明：
  - 优先使用 PyAV 解码（低延迟）。
  - 如果 PyAV 不可用，会 fallback 到调用系统 ffmpeg 解码（需要 ffmpeg 在 PATH）。
  - 控制通道（鼠标 ABS）是可选的：连不上也不影响只看画面。

按键：ESC 退出。
"""

from __future__ import annotations

import socket
import struct
import subprocess
import threading
import time

import cv2  # type: ignore
import numpy as np  # type: ignore


# =====================
# 写死配置：改这里就行
# =====================
SERVER_HOST = "192.168.11.43"  # strem_agent_server 所在机器 IP
VIDEO_PORT = 31234
CTRL_PORT = 31235
STREAM_ID = 1
TOKEN = ""  # 如果 server 启用了 --token，这里填同样的 token

# Optional: mouse ABS reference size.
# - If 0: use decoded video frame size (1:1)
# - If set (e.g. 2560x1440): map mouse coords from video frame -> this reference before sending
CTRL_REF_W = 0
CTRL_REF_H = 0


# =====================
# 控制协议（复用原项目定义）
# =====================
ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1
ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_MOUSE_BUTTON = 3
ML_CTRL_CMD_MOUSE_CLICK = 4
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


class CtrlSender:
    """Control sender (optional)."""

    def __init__(self, host: str, port: int, token: str = ""):
        self.addr = (host, int(port))
        self.token = token
        self.sock: socket.socket | None = None
        self.seq = 0
        self.lock = threading.Lock()

    def connect_best_effort(self) -> None:
        try:
            s = socket.create_connection(self.addr, timeout=2)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.token:
                s.sendall(f"AUTH {self.token}\n".encode("utf-8"))
            self.sock = s
            print(f"[ctrl] connected: {self.addr}")
        except Exception as e:
            self.sock = None
            print(f"[ctrl] disabled (connect failed): {e}")

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
            try:
                self.sock.sendall(be32(len(pkt)) + pkt)
            except Exception:
                # 控制通道挂了也不影响看视频
                try:
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None

    def mouse_abs(self, x, y, ref_w=0, ref_h=0):
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_button(self, action: int, button: int):
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button, 0, 0)

    def mouse_scroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, clicks, 0, 0, 0)

    def mouse_hscroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, clicks, 0, 0, 0)

    def key_press(self, vk: int, modifiers: int = 0):
        self._send(ML_CTRL_CMD_KEY_PRESS, vk, modifiers, 0, 0)

    def text(self, s: str):
        payload = s.encode("utf-8", errors="ignore")
        self._send(ML_CTRL_CMD_TEXT, len(payload), 0, 0, 0, payload=payload)

    def request_idr(self):
        # a/b/c/d unused
        self._send(ML_CTRL_CMD_REQ_IDR, 0, 0, 0, 0)


def clamp_i8(v: int) -> int:
    if v < -127:
        return -127
    if v > 127:
        return 127
    return int(v)


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


def video_recv_thread(host: str, port: int, stream_id: int, token: str, frame_cb):
    # Try PyAV first.
    av = None
    try:
        import av as _av  # type: ignore

        av = _av
    except Exception:
        av = None

    # NOTE: create_connection(timeout=...) sets the *socket recv timeout* too.
    # If the upstream doesn't send immediately (e.g. stream not started yet),
    # Windows will raise TimeoutError on recv(). We want to keep waiting.
    s = socket.create_connection((host, int(port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if token:
        s.sendall(f"AUTH {token}\n".encode("utf-8"))
    s.sendall(f"SUB {int(stream_id)}\n".encode("utf-8"))

    # After handshake, switch to a short timeout and keep polling.
    # (Blocking mode is also OK, but short timeout lets us exit cleanly.)
    s.settimeout(1.0)

    if av is not None:
        print(f"[video] decode backend: PyAV {getattr(av, '__version__', '?')}")
        codec = av.CodecContext.create("h264", "r")
        use_codec_parse = hasattr(codec, "parse")
        parser = None
        if not use_codec_parse:
            # 极老版本 PyAV 才会走到这里
            if not hasattr(av, "Parser"):
                raise RuntimeError("PyAV has neither CodecContext.parse nor av.Parser")
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
        return

    # ffmpeg fallback
    print("[video] decode backend: ffmpeg (PyAV not available)")
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

    PNG_SIG = b"\x89PNG\r\n\x1a\n"

    def extract_pngs(buf2: bytearray):
        out = []
        while True:
            sig_i = buf2.find(PNG_SIG)
            if sig_i < 0:
                if len(buf2) > len(PNG_SIG):
                    del buf2[:-len(PNG_SIG)]
                break
            if sig_i > 0:
                del buf2[:sig_i]
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
        finally:
            stop.set()

    threading.Thread(target=png_reader, daemon=True).start()

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


def main() -> int:
    # control (optional)
    ctrl = CtrlSender(SERVER_HOST, CTRL_PORT, token=TOKEN)
    ctrl.connect_best_effort()

    # Ask for an IDR on connect to improve late-join startup.
    ctrl.request_idr()

    latest = {"img": None}
    lock = threading.Lock()

    def on_frame(img):
        with lock:
            latest["img"] = img

    threading.Thread(
        target=video_recv_thread,
        args=(SERVER_HOST, VIDEO_PORT, STREAM_ID, TOKEN, on_frame),
        daemon=True,
    ).start()

    win = "strem_agent_client (IDE demo)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    def aspect_snap_size(win_w: int, win_h: int, img_w: int, img_h: int) -> tuple[int, int]:
        """Snap window size to keep aspect ratio == (img_w/img_h)."""
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

    snap_state = {"t": 0.0, "w": 0, "h": 0}
    SNAP_MIN_INTERVAL_SEC = 0.05

    def ensure_window_aspect(img_w: int, img_h: int) -> tuple[int, int]:
        try:
            _x, _y, win_w, win_h = cv2.getWindowImageRect(win)
        except Exception:
            win_w, win_h = img_w, img_h

        target_w, target_h = aspect_snap_size(int(win_w), int(win_h), int(img_w), int(img_h))

        now = time.time()
        need_resize = abs(target_w - win_w) >= 3 or abs(target_h - win_h) >= 3
        if need_resize:
            if now - float(snap_state["t"]) >= SNAP_MIN_INTERVAL_SEC:
                if int(target_w) != int(snap_state["w"]) or int(target_h) != int(snap_state["h"]):
                    try:
                        cv2.resizeWindow(win, int(target_w), int(target_h))
                        snap_state.update({"t": now, "w": int(target_w), "h": int(target_h)})
                    except Exception:
                        pass

        return int(target_w), int(target_h)

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

        ix = float(x) * float(img_w) / float(snap_w)
        iy = float(y) * float(img_h) / float(snap_h)
        if ix < 0 or iy < 0 or ix >= img_w or iy >= img_h:
            return

        ref_w = CTRL_REF_W if CTRL_REF_W > 0 else img_w
        ref_h = CTRL_REF_H if CTRL_REF_H > 0 else img_h
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

    # UI loop
    while True:
        with lock:
            img = latest["img"]
        if img is not None:
            ih, iw = img.shape[:2]
            target_w, target_h = ensure_window_aspect(iw, ih)
            show = cv2.resize(img, (target_w, target_h), interpolation=cv2.INTER_AREA)
            cv2.imshow(win, show)
        k = cv2.waitKey(1) & 0xFF
        if k == 27:  # ESC
            break
        if k != 255:
            if k == 8:
                ctrl.key_press(0x08)
            elif k == 13:
                ctrl.key_press(0x0D)
            elif k == 9:
                ctrl.key_press(0x09)
            elif 32 <= k <= 126:
                ch = chr(k)
                vk = vk_from_ascii(ch)
                if vk:
                    ctrl.key_press(vk)
                else:
                    ctrl.text(ch)
        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

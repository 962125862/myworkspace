"""IDE demo client v2 (input-focused)

目标：演示“不新增依赖”的前提下，如何在 OpenCV(cv2) 窗口里获取输入事件并发回远端。

能做到：
  - 鼠标：move / 左右中键按下抬起 / 滚轮（取决于 OpenCV HighGUI 后端是否支持）
  - 键盘：通过 cv2.waitKey() 获取按键（只能拿到"按下"，没有 key-up / modifiers）
  - 窗口：保持与视频相同的宽高比（无黑边、无拉伸），拖拽时会自动 snap 回正确比例

做不到（仅靠 cv2）：
  - 可靠的 key-up
  - 可靠的 Ctrl/Alt/Shift 修饰键状态

依赖：
  pip install -r strem_agent_client/requirements.txt

按键：
  ESC 退出

提示：
  - 如果你只想验证输入映射，不需要看真实视频，可以把 USE_FAKE_VIDEO=True。
"""

from __future__ import annotations

import socket
import struct
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

# If True, show a synthetic frame instead of receiving real video.
USE_FAKE_VIDEO = False


# =====================
# 控制协议（复用原项目定义）
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

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08
BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")


def be32(n: int) -> bytes:
    return struct.pack(">I", n)


def clamp_i8(v: int) -> int:
    if v < -127:
        return -127
    if v > 127:
        return 127
    return int(v)


def vk_from_ascii(ch: str) -> int:
    """Best-effort mapping from ASCII char to Win32 VK code."""
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
                try:
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None

    # mouse
    def mouse_abs(self, x, y, ref_w=0, ref_h=0):
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_button(self, action: int, button: int):
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button, 0, 0)

    def mouse_scroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, clicks, 0, 0, 0)

    def mouse_hscroll(self, clicks: int):
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, clicks, 0, 0, 0)

    # keyboard
    def key_press(self, vk: int, modifiers: int = 0):
        self._send(ML_CTRL_CMD_KEY_PRESS, vk, modifiers, 0, 0)

    def text(self, s: str):
        payload = s.encode("utf-8", errors="ignore")
        self._send(ML_CTRL_CMD_TEXT, len(payload), 0, 0, 0, payload=payload)

    def request_idr(self):
        self._send(ML_CTRL_CMD_REQ_IDR, 0, 0, 0, 0)


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


def main() -> int:
    # control
    ctrl = CtrlSender(SERVER_HOST, CTRL_PORT, token=TOKEN)
    ctrl.connect_best_effort()
    ctrl.request_idr()

    latest = {"img": None}
    lock = threading.Lock()

    def fake_video_thread():
        t0 = time.time()
        while True:
            # 720p test pattern with moving dot
            w, h = 1280, 720
            img = np.zeros((h, w, 3), dtype=np.uint8)
            # grid
            for x in range(0, w, 80):
                cv2.line(img, (x, 0), (x, h - 1), (50, 50, 50), 1)
            for y in range(0, h, 80):
                cv2.line(img, (0, y), (w - 1, y), (50, 50, 50), 1)
            # moving point
            dt = time.time() - t0
            px = int((0.5 + 0.45 * np.sin(dt)) * (w - 1))
            py = int((0.5 + 0.45 * np.cos(dt * 0.7)) * (h - 1))
            cv2.circle(img, (px, py), 8, (0, 255, 0), -1)
            cv2.putText(img, "FAKE VIDEO", (30, 60), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (255, 255, 255), 2)
            with lock:
                latest["img"] = img
            time.sleep(1 / 60.0)

    if USE_FAKE_VIDEO:
        threading.Thread(target=fake_video_thread, daemon=True).start()
    else:
        # Import from the main client to avoid duplicating the decoder implementation.
        from strem_agent_client import VideoCfg, video_recv_thread  # type: ignore

        def on_frame(img):
            with lock:
                latest["img"] = img

        threading.Thread(
            target=video_recv_thread,
            args=(VideoCfg(SERVER_HOST, VIDEO_PORT, STREAM_ID, TOKEN), on_frame),
            daemon=True,
        ).start()

    win = "strem_agent_client demo2 (cv2 input)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    # throttle window snapping
    snap_state = {"t": 0.0, "w": 0, "h": 0}
    SNAP_MIN_INTERVAL_SEC = 0.05

    dbg = {"last": "(none)"}

    def ensure_window_aspect(img_w: int, img_h: int) -> tuple[int, int, int, int]:
        try:
            _x, _y, win_w, win_h = cv2.getWindowImageRect(win)
        except Exception:
            win_w, win_h = img_w, img_h
        target_w, target_h = aspect_snap_size(int(win_w), int(win_h), int(img_w), int(img_h))

        now = time.time()
        need_resize = abs(target_w - win_w) >= 3 or abs(target_h - win_h) >= 3
        if need_resize and now - float(snap_state["t"]) >= SNAP_MIN_INTERVAL_SEC:
            if int(target_w) != int(snap_state["w"]) or int(target_h) != int(snap_state["h"]):
                try:
                    cv2.resizeWindow(win, int(target_w), int(target_h))
                    snap_state.update({"t": now, "w": int(target_w), "h": int(target_h)})
                except Exception:
                    pass
        return int(win_w), int(win_h), int(target_w), int(target_h)

    def send_mouse(event_name: str, event, x, y, flags):
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

        dbg["last"] = f"mouse {event_name} win=({x},{y}) img=({sx},{sy}) ref={ref_w}x{ref_h}"

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

    def mouse_cb(event, x, y, flags, param):
        # cv2 只能在窗口里做这些事件；这里就是“嵌入在 cv2 窗口里”的典型用法。
        send_mouse(str(event), event, x, y, flags)

    cv2.setMouseCallback(win, mouse_cb)

    while True:
        with lock:
            img = latest["img"]

        if img is not None:
            ih, iw = img.shape[:2]
            win_w, win_h, snap_w, snap_h = ensure_window_aspect(iw, ih)
            show = cv2.resize(img, (snap_w, snap_h), interpolation=cv2.INTER_AREA)
            cv2.putText(
                show,
                f"win={win_w}x{win_h} snap={snap_w}x{snap_h} img={iw}x{ih}",
                (10, 25),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (255, 255, 0),
                2,
            )
            cv2.putText(show, dbg["last"], (10, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
            cv2.imshow(win, show)

        k = cv2.waitKey(1) & 0xFF
        if k == 27:
            break
        if k != 255:
            if k == 8:
                dbg["last"] = "key VK_BACK"
                ctrl.key_press(0x08)
            elif k == 13:
                dbg["last"] = "key VK_RETURN"
                ctrl.key_press(0x0D)
            elif k == 9:
                dbg["last"] = "key VK_TAB"
                ctrl.key_press(0x09)
            elif 32 <= k <= 126:
                ch = chr(k)
                vk = vk_from_ascii(ch)
                if vk:
                    dbg["last"] = f"key VK 0x{vk:02x} '{ch}'"
                    ctrl.key_press(vk)
                else:
                    dbg["last"] = f"text '{ch}'"
                    ctrl.text(ch)

        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())


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

import hashlib
import hmac
import json
import platform
import socket
import struct
import secrets
import threading
import time
import urllib.parse
import urllib.request

import cv2  # type: ignore
import numpy as np  # type: ignore


# =====================
# 写死配置：改这里就行
# =====================
# 运行 agent_link_service / strem_agent_server 的那台机器 IP（也就是你部署 docker 栈的机器）
SERVER_HOST = "192.168.11.43"

# 如果你用 agent_link_service 的 gate（推荐），这两个端口会在 startLink 后返回并自动覆盖
VIDEO_PORT = 40121
CTRL_PORT = 40122
STREAM_ID = 1

# 方式 A（推荐）：启动时自动调用 startLink，拿到 token + gate 端口
ENABLE_STARTLINK = True
STARTLINK_URL = f"http://{SERVER_HOST}:40120/startLink"  # agent_link_service API
STARTLINK_SK = "change-me-sk"  # 必须和服务端 AGENT_LINK_SK 一致（HMAC-SHA256）

# 方式 B：不调用 startLink，直接手填 token/端口（仅用于调试）
TOKEN = "your-static-token"

# 键盘采集后端：
# - "cv2": 只能拿到按下事件，没有修饰键/按键抬起，适合简单输入
# - "pynput": 全局键盘 hook，支持 key down/up + modifiers（更好用）
KEYBOARD_BACKEND = "pynput"  # "cv2" | "pynput"
# None=auto (macOS 开启，其它关闭)
MAP_MAC_META_TO_CTRL: bool | None = None
KEY_DEBUG = False

# When using a global keyboard hook (pynput), require explicit "arming" so we don't
# send shortcuts while the OpenCV window isn't active.
REQUIRE_ARM = True

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

MODIFIER_SHIFT = 0x01
MODIFIER_CTRL = 0x02
MODIFIER_ALT = 0x04
MODIFIER_META = 0x08


def be32(n: int) -> bytes:
    return struct.pack(">I", n)


def _hmac_sha256_hex(sk: str, msg: str) -> str:
    return hmac.new(sk.encode("utf-8"), msg.encode("utf-8"), hashlib.sha256).hexdigest()


def start_link(stream_id: int) -> dict:
    ts = int(time.time())  # UTC epoch seconds; do not use local time strings here.
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


def vk_from_pynput_key(key) -> int:
    try:
        from pynput.keyboard import Key, KeyCode  # type: ignore
    except Exception:
        return 0

    sys = platform.system().lower()
    is_darwin = ("darwin" in sys) or ("mac" in sys)

    if isinstance(key, KeyCode):
        ch = getattr(key, "char", None)
        if isinstance(ch, str) and ch:
            shift_map = {
                "!": "1",
                "@": "2",
                "#": "3",
                "$": "4",
                "%": "5",
                "^": "6",
                "&": "7",
                "*": "8",
                "(": "9",
                ")": "0",
                "_": "-",
                "+": "=",
                "{": "[",
                "}": "]",
                "|": "\\",
                ":": ";",
                "\"": "'",
                "<": ",",
                ">": ".",
                "?": "/",
                "~": "`",
            }
            base = shift_map.get(ch, ch)
            return vk_from_ascii(base)
        if not is_darwin:
            vk = getattr(key, "vk", None)
            if isinstance(vk, int) and 0 < vk <= 0xFF:
                return int(vk)
        return 0

    auto_map = is_darwin
    map_meta = auto_map if MAP_MAC_META_TO_CTRL is None else bool(MAP_MAC_META_TO_CTRL)

    # pynput Key enum differs across platforms (e.g. Key.insert may not exist on macOS).
    special = {}

    def _add(key_name: str, vk: int) -> None:
        k = getattr(Key, key_name, None)
        if k is not None:
            special[k] = vk

    _add("enter", 0x0D)
    _add("tab", 0x09)
    _add("space", 0x20)
    _add("backspace", 0x08)
    _add("esc", 0x1B)
    _add("delete", 0x2E)
    _add("insert", 0x2D)
    _add("home", 0x24)
    _add("end", 0x23)
    _add("page_up", 0x21)
    _add("page_down", 0x22)
    _add("up", 0x26)
    _add("down", 0x28)
    _add("left", 0x25)
    _add("right", 0x27)
    _add("caps_lock", 0x14)
    _add("shift", 0x10)
    _add("shift_l", 0xA0)
    _add("shift_r", 0xA1)
    _add("ctrl", 0x11)
    _add("ctrl_l", 0xA2)
    _add("ctrl_r", 0xA3)
    _add("alt", 0x12)
    _add("alt_l", 0xA4)
    _add("alt_r", 0xA5)
    _add("cmd", 0xA2 if map_meta else 0x5B)
    _add("cmd_l", 0xA2 if map_meta else 0x5B)
    _add("cmd_r", 0xA3 if map_meta else 0x5C)
    if key in (Key.f1, Key.f2, Key.f3, Key.f4, Key.f5, Key.f6, Key.f7, Key.f8, Key.f9, Key.f10, Key.f11, Key.f12):
        base = [Key.f1, Key.f2, Key.f3, Key.f4, Key.f5, Key.f6, Key.f7, Key.f8, Key.f9, Key.f10, Key.f11, Key.f12].index(key)
        return 0x70 + base
    return special.get(key, 0)


def modifiers_from_state(*, shift: bool, ctrl: bool, alt: bool, meta: bool) -> int:
    m = 0
    if shift:
        m |= MODIFIER_SHIFT
    if ctrl:
        m |= MODIFIER_CTRL
    if alt:
        m |= MODIFIER_ALT
    if meta:
        m |= MODIFIER_META
    sys = platform.system().lower()
    auto_map = ("darwin" in sys) or ("mac" in sys)
    map_meta = auto_map if MAP_MAC_META_TO_CTRL is None else bool(MAP_MAC_META_TO_CTRL)
    if map_meta and (m & MODIFIER_META) and not (m & MODIFIER_CTRL):
        m = (m | MODIFIER_CTRL) & ~MODIFIER_META
    return m


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

    def __init__(self, host: str, port: int, token: str = "", stream_id: int = 1):
        self.addr = (host, int(port))
        self.token = token
        self.stream_id = int(stream_id)
        self.sock: socket.socket | None = None
        self.seq = 0
        self.lock = threading.Lock()
        self._last_connect_attempt = 0.0

    def connect_best_effort(self) -> None:
        try:
            s = socket.create_connection(self.addr, timeout=2)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.token:
                s.sendall(f"AUTH {self.token}\n".encode("utf-8"))
            s.sendall(f"SUB {self.stream_id}\n".encode("utf-8"))
            self.sock = s
            print(f"[ctrl] connected: {self.addr}")
        except Exception as e:
            self.sock = None
            print(f"[ctrl] disabled (connect failed): {e}")

    def _ensure_connected(self) -> bool:
        if self.sock:
            return True
        now = time.time()
        if now - float(self._last_connect_attempt) < 1.0:
            return False
        self._last_connect_attempt = now
        self.connect_best_effort()
        return self.sock is not None

    def _send(self, cmd_type: int, a=0, b=0, c=0, d=0, payload: bytes = b""):
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

    def key(self, vk: int, action: int, modifiers: int = 0):
        self._send(7, vk, action, modifiers, 0)

    def key_down(self, vk: int, modifiers: int = 0):
        self.key(vk, 0x03, modifiers)

    def key_up(self, vk: int, modifiers: int = 0):
        self.key(vk, 0x04, modifiers)

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
    global VIDEO_PORT, CTRL_PORT, TOKEN

    if ENABLE_STARTLINK:
        try:
            r = start_link(STREAM_ID)
            TOKEN = str(r.get("token") or TOKEN or "")
            VIDEO_PORT = int(r.get("video_port") or VIDEO_PORT)
            CTRL_PORT = int(r.get("ctrl_port") or CTRL_PORT)
            print(f"[startLink] ok token={'yes' if TOKEN else 'no'} video_port={VIDEO_PORT} ctrl_port={CTRL_PORT}")
        except Exception as e:
            print(f"[startLink] failed: {e}")
            return 2

    # control
    ctrl = CtrlSender(SERVER_HOST, CTRL_PORT, token=TOKEN, stream_id=STREAM_ID)
    ctrl.connect_best_effort()
    ctrl.request_idr()

    stop = threading.Event()
    input_armed = threading.Event()
    if REQUIRE_ARM:
        print("[input] disarmed by default. Press F8 or left-click the video window to arm; middle-click to disarm.")
    if KEYBOARD_BACKEND == "pynput":
        try:
            from pynput import keyboard as _kb  # type: ignore

            mod = {"shift": False, "ctrl": False, "alt": False, "meta": False}

            def _set_mod(k, down: bool):
                try:
                    if k in (_kb.Key.shift, _kb.Key.shift_l, _kb.Key.shift_r):
                        mod["shift"] = down
                    elif k in (_kb.Key.ctrl, _kb.Key.ctrl_l, _kb.Key.ctrl_r):
                        mod["ctrl"] = down
                    elif k in (_kb.Key.alt, _kb.Key.alt_l, _kb.Key.alt_r):
                        mod["alt"] = down
                    elif k in (_kb.Key.cmd, _kb.Key.cmd_l, _kb.Key.cmd_r):
                        mod["meta"] = down
                except Exception:
                    pass

            def on_press(k):
                if k == getattr(_kb.Key, "f8", None):
                    if input_armed.is_set():
                        input_armed.clear()
                        print("[input] disarmed (F8)")
                    else:
                        input_armed.set()
                        print("[input] armed (F8)")
                    return
                _set_mod(k, True)
                vk = vk_from_pynput_key(k)
                if not vk:
                    ch = getattr(k, "char", None)
                    if isinstance(ch, str) and ch:
                        if (not REQUIRE_ARM) or input_armed.is_set():
                            ctrl.text(ch)
                    return
                if vk == 0x1B:  # ESC
                    stop.set()
                    return
                if REQUIRE_ARM and not input_armed.is_set():
                    return
                m = modifiers_from_state(shift=mod["shift"], ctrl=mod["ctrl"], alt=mod["alt"], meta=mod["meta"])
                ch = getattr(k, "char", None)
                if isinstance(ch, str) and ch and (ch.isupper() or ch in "!@#$%^&*()_+{}|:\\\"<>?~"):
                    m |= MODIFIER_SHIFT
                if KEY_DEBUG:
                    print(f"[kbd] down k={k!r} vk=0x{vk:02X} m=0x{m:02X}")
                ctrl.key_down(vk, m)

            def on_release(k):
                _set_mod(k, False)
                vk = vk_from_pynput_key(k)
                if not vk:
                    return
                if REQUIRE_ARM and not input_armed.is_set():
                    return
                m = modifiers_from_state(shift=mod["shift"], ctrl=mod["ctrl"], alt=mod["alt"], meta=mod["meta"])
                if KEY_DEBUG:
                    print(f"[kbd] up   k={k!r} vk=0x{vk:02X} m=0x{m:02X}")
                ctrl.key_up(vk, m)

            listener = _kb.Listener(on_press=on_press, on_release=on_release)
            listener.daemon = True
            listener.start()
            print("[kbd] pynput enabled (ESC to quit)")
        except Exception as e:
            print(f"[kbd] pynput disabled: {e}")

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

        if REQUIRE_ARM:
            if event == cv2.EVENT_LBUTTONDOWN and not input_armed.is_set():
                input_armed.set()
                print("[input] armed (left click)")
            elif event == cv2.EVENT_MBUTTONDOWN and input_armed.is_set():
                input_armed.clear()
                print("[input] disarmed (middle click)")
                return
            if not input_armed.is_set() and event != cv2.EVENT_LBUTTONDOWN:
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
        if stop.is_set():
            break
        if KEYBOARD_BACKEND == "cv2" and k == 27:
            break
        if KEYBOARD_BACKEND == "cv2" and k != 255:
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

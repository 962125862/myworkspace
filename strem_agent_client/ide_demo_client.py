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

import hashlib
import hmac
import json
import platform
import socket
import struct
import subprocess
import secrets
import threading
import time
import urllib.parse
import urllib.request

import cv2
import numpy as np


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

# Video debugging:
# - If True, prints connection state, first bytes, and periodic rx stats.
# - If FORCE_FFMPEG is True, skip PyAV and use ffmpeg subprocess for decode.
VIDEO_DEBUG = False
FORCE_FFMPEG = False

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

MODIFIER_SHIFT = 0x01
MODIFIER_CTRL = 0x02
MODIFIER_ALT = 0x04
MODIFIER_META = 0x08

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
    """Call agent_link_service /startLink and return JSON.

    Signature:
      msg = "call_name=startLink&stream=<id>&ts=<ts>&nonce=<nonce>"
      sig = HMAC-SHA256-hex(sk, msg)
    """
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

    def key(self, vk: int, action: int, modifiers: int = 0):
        # a=vk, b=action, c=modifiers
        self._send(7, vk, action, modifiers, 0)

    def key_down(self, vk: int, modifiers: int = 0):
        self.key(vk, 0x03, modifiers)

    def key_up(self, vk: int, modifiers: int = 0):
        self.key(vk, 0x04, modifiers)

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
        # On macOS, KeyCode.vk is an Apple virtual keycode (not Windows VK).
        # For non-printable KeyCode keys without a char, we can't reliably map here.
        if not is_darwin:
            vk = getattr(key, "vk", None)
            if isinstance(vk, int) and 0 < vk <= 0xFF:
                return int(vk)
        return 0

    auto_map = is_darwin
    map_meta = auto_map if MAP_MAC_META_TO_CTRL is None else bool(MAP_MAC_META_TO_CTRL)

    # pynput Key enum differs across platforms (e.g. Key.insert may not exist on macOS).
    # Build the mapping with getattr() to avoid AttributeError in listener callbacks.
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


def video_recv_thread(host: str, port: int, stream_id: int, token: str, frame_cb):
    # Try PyAV first.
    av = None
    try:
        import av as _av  # type: ignore

        av = _av
    except Exception:
        av = None
    if FORCE_FFMPEG:
        av = None

    # NOTE: create_connection(timeout=...) sets the *socket recv timeout* too.
    # If the upstream doesn't send immediately (e.g. stream not started yet),
    # Windows will raise TimeoutError on recv(). We want to keep waiting.
    s = socket.create_connection((host, int(port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if VIDEO_DEBUG:
        print(f"[video] connected: {(host, int(port))} stream_id={int(stream_id)} token={'yes' if token else 'no'}")
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
        rx_bytes = 0
        frames_ok = 0
        first_chunk = True
        last_stat_t = time.time()
        while True:
            try:
                n = s.recv_into(buf)
            except (TimeoutError, socket.timeout):
                if VIDEO_DEBUG:
                    now = time.time()
                    if now - last_stat_t >= 2.0:
                        print(f"[video] rx={rx_bytes}B frames={frames_ok} (waiting...)")
                        last_stat_t = now
                continue
            if n <= 0:
                break
            data = bytes(memoryview(buf)[:n])
            rx_bytes += len(data)
            if first_chunk:
                first_chunk = False
                if data.startswith(b"ERR "):
                    # Gate rejected the handshake (bad_auth/start_failed/etc).
                    try:
                        msg = data.decode("utf-8", errors="replace").strip()
                    except Exception:
                        msg = repr(data[:64])
                    print(f"[video][ERR] gate: {msg}")
                    return
                if VIDEO_DEBUG:
                    import binascii

                    print(f"[video] first bytes: {binascii.hexlify(data[:16]).decode()}")
            packets = codec.parse(data) if use_codec_parse else parser.parse(data)  # type: ignore[union-attr]
            for packet in packets:
                try:
                    frames = codec.decode(packet)
                except Exception:
                    frames = []
                for fr in frames:
                    img = fr.to_ndarray(format="bgr24")
                    frame_cb(img)
                    frames_ok += 1
            if VIDEO_DEBUG:
                now = time.time()
                if now - last_stat_t >= 2.0:
                    print(f"[video] rx={rx_bytes}B frames={frames_ok}")
                    last_stat_t = now
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

    # control (optional)
    ctrl = CtrlSender(SERVER_HOST, CTRL_PORT, token=TOKEN, stream_id=STREAM_ID)
    ctrl.connect_best_effort()

    # Ask for an IDR on connect to improve late-join startup.
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
    t0 = time.time()
    printed_wait = False

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
        if img is None and not printed_wait and time.time() - t0 >= 3.0:
            printed_wait = True
            print("[ui] waiting for first frame... (check video gate / token / ml_worker ingest)")
        if img is not None:
            ih, iw = img.shape[:2]
            target_w, target_h = ensure_window_aspect(iw, ih)
            show = cv2.resize(img, (target_w, target_h), interpolation=cv2.INTER_AREA)
            cv2.imshow(win, show)
        k = cv2.waitKey(1) & 0xFF
        if stop.is_set():
            break
        if KEYBOARD_BACKEND == "cv2" and k == 27:  # ESC
            break
        if KEYBOARD_BACKEND == "cv2" and k != 255:
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

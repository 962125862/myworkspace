#!/usr/bin/env python3
"""
Qt viewer + input client (no global keyboard hook).

Why this exists:
- cv2.imshow has great video performance, but cv2.waitKey() is weak for modifiers.
- pynput provides modifiers but is a global hook (steals shortcuts when window not focused).
- Qt gives focused-window input events (keydown/up + modifiers) without global hooks.

This client:
- Calls /startLink (optional) to get token + gate ports.
- Connects to video gate, decodes H264 via PyAV into BGR frames.
- Renders frames in a QWidget.
- Sends mouse/keyboard to ctrl gate only when this window is focused (Qt guarantees this).
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
    import av  # type: ignore
except Exception as e:  # pragma: no cover
    raise SystemExit("Missing PyAV. Install with: pip install av") from e

try:
    from PySide6 import QtCore, QtGui, QtWidgets  # type: ignore
except Exception as e:  # pragma: no cover
    raise SystemExit("Missing PySide6. Install with: pip install PySide6") from e


# =====================
# Config (edit here)
# =====================
SERVER_HOST = "192.168.11.31"  # agent_link_service host (public IP or LAN IP)
STREAM_ID = 1

ENABLE_STARTLINK = True
STARTLINK_URL = f"http://{SERVER_HOST}:40120/startLink"
STARTLINK_SK = "change-me-sk"  # must match AGENT_LINK_SK on server

VIDEO_PORT = 40121
CTRL_PORT = 40122
TOKEN = "your-static-token"

# Optional: mouse ABS reference size (0 means use decoded frame size)
CTRL_REF_W = 0
CTRL_REF_H = 0


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

# Extended key event (down/up) used by newer clients.
ML_CTRL_CMD_KEY = 7
KEY_ACTION_DOWN = 0x03
KEY_ACTION_UP = 0x04

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08
BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03

MOD_SHIFT = 0x01
MOD_CTRL = 0x02
MOD_ALT = 0x04
MOD_META = 0x08


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


def _vk_and_mods_from_text(text: str, mods: int) -> tuple[int, int]:
    """Map a printable ASCII char to a Win32 VK + modifiers.

    This avoids TEXT injection so IME and non-letter keys behave more like real key events.
    """
    if not text or len(text) != 1:
        return 0, mods
    ch = text
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
    need_shift = (ch != base) or ("A" <= ch <= "Z")
    vk = vk_from_ascii(base)
    m = mods
    if need_shift:
        m |= MOD_SHIFT
    return vk, m


def vk_from_qt_key(key: int) -> int:
    # Minimal mapping (extend as needed).
    K = QtCore.Qt.Key
    special = {
        int(K.Key_Shift): 0x10,  # VK_SHIFT
        int(K.Key_Control): 0x11,  # VK_CONTROL
        int(K.Key_Alt): 0x12,  # VK_MENU
        # macOS Command key shows up as Meta in Qt. This may not map 1:1 to remote OS,
        # but sending VK_LWIN is still better than dropping it entirely for hotkeys.
        int(K.Key_Meta): 0x5B,  # VK_LWIN
        int(K.Key_Return): 0x0D,
        int(K.Key_Enter): 0x0D,
        int(K.Key_Tab): 0x09,
        int(K.Key_Backspace): 0x08,
        int(K.Key_Escape): 0x1B,
        int(K.Key_Delete): 0x2E,
        int(K.Key_Insert): 0x2D,
        int(K.Key_Home): 0x24,
        int(K.Key_End): 0x23,
        int(K.Key_PageUp): 0x21,
        int(K.Key_PageDown): 0x22,
        int(K.Key_Left): 0x25,
        int(K.Key_Up): 0x26,
        int(K.Key_Right): 0x27,
        int(K.Key_Down): 0x28,
        int(K.Key_F1): 0x70,
        int(K.Key_F2): 0x71,
        int(K.Key_F3): 0x72,
        int(K.Key_F4): 0x73,
        int(K.Key_F5): 0x74,
        int(K.Key_F6): 0x75,
        int(K.Key_F7): 0x76,
        int(K.Key_F8): 0x77,
        int(K.Key_F9): 0x78,
        int(K.Key_F10): 0x79,
        int(K.Key_F11): 0x7A,
        int(K.Key_F12): 0x7B,
    }
    return special.get(int(key), 0)


def modifiers_from_qt(mods: QtCore.Qt.KeyboardModifiers) -> int:
    m = 0
    if mods & QtCore.Qt.ShiftModifier:
        m |= MOD_SHIFT
    if mods & QtCore.Qt.ControlModifier:
        m |= MOD_CTRL
    if mods & QtCore.Qt.AltModifier:
        m |= MOD_ALT
    if mods & QtCore.Qt.MetaModifier:
        m |= MOD_META
    return m


class CtrlSender:
    def __init__(self, host: str, port: int, token: str, stream_id: int):
        self.addr = (host, int(port))
        self.token = token
        self.stream_id = int(stream_id)
        self.sock: socket.socket | None = None
        self.seq = 0
        self.lock = threading.Lock()

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

    def _send(self, cmd_type: int, a=0, b=0, c=0, d=0, payload: bytes = b"") -> None:
        if not self.sock:
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
            try:
                self.sock.sendall(be32(len(pkt)) + pkt)
            except Exception:
                try:
                    self.sock.close()
                except Exception:
                    pass
                self.sock = None

    def mouse_abs(self, x: int, y: int, ref_w: int, ref_h: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_button(self, action: int, button: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button, 0, 0)

    def mouse_scroll(self, clicks: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, int(clicks), 0, 0, 0)

    def mouse_hscroll(self, clicks: int) -> None:
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, int(clicks), 0, 0, 0)

    def key_press(self, vk: int, modifiers: int = 0) -> None:
        self._send(ML_CTRL_CMD_KEY_PRESS, int(vk), int(modifiers), 0, 0)

    def key_down(self, vk: int, modifiers: int = 0) -> None:
        # a=vk, b=action, c=modifiers
        self._send(ML_CTRL_CMD_KEY, int(vk), KEY_ACTION_DOWN, int(modifiers), 0)

    def key_up(self, vk: int, modifiers: int = 0) -> None:
        self._send(ML_CTRL_CMD_KEY, int(vk), KEY_ACTION_UP, int(modifiers), 0)

    def text(self, s: str) -> None:
        payload = s.encode("utf-8", errors="ignore")
        self._send(ML_CTRL_CMD_TEXT, len(payload), 0, 0, 0, payload=payload)

    def request_idr(self) -> None:
        self._send(ML_CTRL_CMD_REQ_IDR, 0, 0, 0, 0)


class VideoWorker(QtCore.QObject):
    frame = QtCore.Signal(object)  # np.ndarray (BGR)
    error = QtCore.Signal(str)

    def __init__(self, host: str, port: int, stream_id: int, token: str):
        super().__init__()
        self.host = host
        self.port = int(port)
        self.stream_id = int(stream_id)
        self.token = token
        self._stop = threading.Event()

    def stop(self) -> None:
        self._stop.set()

    @QtCore.Slot()
    def run(self) -> None:
        try:
            s = socket.create_connection((self.host, self.port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.token:
                s.sendall(f"AUTH {self.token}\n".encode("utf-8"))
            s.sendall(f"SUB {self.stream_id}\n".encode("utf-8"))
            s.settimeout(1.0)

            codec = av.CodecContext.create("h264", "r")
            buf = bytearray(1024 * 1024)
            while not self._stop.is_set():
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
                        self.frame.emit(img)
        except Exception as e:
            self.error.emit(str(e))


class VideoWidget(QtWidgets.QWidget):
    mouseMoved = QtCore.Signal(int, int)
    mousePressed = QtCore.Signal(int)
    mouseReleased = QtCore.Signal(int)
    wheelScrolled = QtCore.Signal(int, int)

    def __init__(self):
        super().__init__()
        self.setMouseTracking(True)
        self.setFocusPolicy(QtCore.Qt.StrongFocus)
        self._lock = threading.Lock()
        self._bgr: np.ndarray | None = None
        self._disp = (0, 0, 1, 1)  # x0,y0,w,h in widget

    def set_frame(self, bgr: np.ndarray) -> None:
        with self._lock:
            self._bgr = bgr
        self.update()

    def paintEvent(self, _e) -> None:  # noqa: N802
        painter = QtGui.QPainter(self)
        try:
            painter.fillRect(self.rect(), QtGui.QColor(17, 17, 17))

            with self._lock:
                bgr = None if self._bgr is None else self._bgr
            if bgr is None:
                painter.setPen(QtGui.QColor(220, 220, 220))
                painter.drawText(self.rect(), QtCore.Qt.AlignCenter, "Waiting for video...")
                return

            h, w = bgr.shape[:2]
            ww = max(1, int(self.width()))
            wh = max(1, int(self.height()))

            scale = min(ww / float(w), wh / float(h))
            dw = max(1, int(round(w * scale)))
            dh = max(1, int(round(h * scale)))
            x0 = (ww - dw) // 2
            y0 = (wh - dh) // 2
            self._disp = (x0, y0, dw, dh)

            # bgr[:, :, ::-1] is usually non-contiguous; QImage requires a contiguous buffer.
            rgb_c = np.ascontiguousarray(bgr[:, :, ::-1])
            qimg = QtGui.QImage(
                rgb_c.data,
                w,
                h,
                int(rgb_c.strides[0]),
                QtGui.QImage.Format_RGB888,
            ).copy()
            pix = QtGui.QPixmap.fromImage(qimg).scaled(dw, dh, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
            painter.drawPixmap(x0, y0, pix)
        finally:
            painter.end()

    def map_xy(self, x: int, y: int) -> tuple[int, int, int, int] | None:
        with self._lock:
            img = self._bgr
        if img is None:
            return None
        ih, iw = img.shape[:2]
        x0, y0, dw, dh = self._disp
        if dw <= 0 or dh <= 0:
            return None
        # Even if the cursor is in the letterboxed area, clamp to the nearest edge
        # so users can keep moving the remote cursor.
        cx = min(max(x, x0), x0 + dw - 1)
        cy = min(max(y, y0), y0 + dh - 1)
        ix = int((cx - x0) * iw / float(dw))
        iy = int((cy - y0) * ih / float(dh))
        ix = max(0, min(iw - 1, ix))
        iy = max(0, min(ih - 1, iy))
        return ix, iy, iw, ih

    def mouseMoveEvent(self, e: QtGui.QMouseEvent) -> None:  # noqa: N802
        self.mouseMoved.emit(int(e.position().x()), int(e.position().y()))

    def mousePressEvent(self, e: QtGui.QMouseEvent) -> None:  # noqa: N802
        b = e.button()
        bi = int(getattr(b, "value", b))
        self.mousePressed.emit(bi)

    def mouseReleaseEvent(self, e: QtGui.QMouseEvent) -> None:  # noqa: N802
        b = e.button()
        bi = int(getattr(b, "value", b))
        self.mouseReleased.emit(bi)

    def wheelEvent(self, e: QtGui.QWheelEvent) -> None:  # noqa: N802
        d = e.angleDelta()
        self.wheelScrolled.emit(int(d.x()), int(d.y()))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, host: str, video_port: int, ctrl_port: int, stream_id: int, token: str):
        super().__init__()
        self.host = host
        self.video_port = int(video_port)
        self.ctrl = CtrlSender(host, int(ctrl_port), token=token, stream_id=int(stream_id))
        self.stream_id = int(stream_id)
        self.token = token

        self.setWindowTitle("strem_agent_client (Qt)")
        self.resize(1280, 720)

        self.vw = VideoWidget()
        self.setCentralWidget(self.vw)
        # Handle mouse input on the widget that actually receives hover events.
        # QMainWindow won't get mouseMoveEvent() unless a button is pressed.
        self.vw.mouseMoved.connect(self._on_mouse_moved)
        self.vw.mousePressed.connect(self._on_mouse_pressed)
        self.vw.mouseReleased.connect(self._on_mouse_released)
        self.vw.wheelScrolled.connect(self._on_wheel_scrolled)

        self.ctrl.connect_best_effort()
        self.ctrl.request_idr()

        self._video_thread = QtCore.QThread(self)
        self._video_worker = VideoWorker(self.host, self.video_port, self.stream_id, self.token)
        self._video_worker.moveToThread(self._video_thread)
        self._video_thread.started.connect(self._video_worker.run)
        self._video_worker.frame.connect(self.vw.set_frame)
        self._video_worker.error.connect(self._on_video_error)
        self._video_thread.start()
        self._pressed_vk: set[int] = set()

    def closeEvent(self, e) -> None:  # noqa: N802
        try:
            self._video_worker.stop()
        except Exception:
            pass
        try:
            self._video_thread.quit()
            self._video_thread.wait(1000)
        except Exception:
            pass
        super().closeEvent(e)

    def _on_video_error(self, msg: str) -> None:
        print(f"[video][ERR] {msg}")

    def keyPressEvent(self, e: QtGui.QKeyEvent) -> None:  # noqa: N802
        if not self.isActiveWindow():
            return
        text = e.text() or ""
        mods = modifiers_from_qt(e.modifiers())
        vk = 0
        if text and len(text) == 1 and 32 <= ord(text) <= 126:
            vk, mods = _vk_and_mods_from_text(text, mods)
        if not vk:
            vk = vk_from_qt_key(int(e.key()))
        if not vk:
            return

        # Use key_down/key_up so long-press works (remote OS will auto-repeat).
        if e.isAutoRepeat():
            return
        if vk in self._pressed_vk:
            return
        self._pressed_vk.add(vk)
        self.ctrl.key_down(vk, mods)

    def keyReleaseEvent(self, e: QtGui.QKeyEvent) -> None:  # noqa: N802
        if not self.isActiveWindow():
            return
        if e.isAutoRepeat():
            return
        text = e.text() or ""
        mods = modifiers_from_qt(e.modifiers())
        vk = 0
        if text and len(text) == 1 and 32 <= ord(text) <= 126:
            vk, mods = _vk_and_mods_from_text(text, mods)
        if not vk:
            vk = vk_from_qt_key(int(e.key()))
        if not vk:
            return
        if vk in self._pressed_vk:
            self._pressed_vk.discard(vk)
            self.ctrl.key_up(vk, mods)

    @QtCore.Slot(int, int)
    def _on_mouse_moved(self, x: int, y: int) -> None:
        if not self.isActiveWindow():
            return
        m = self.vw.map_xy(int(x), int(y))
        if not m:
            return
        x, y, w, h = m
        ref_w = CTRL_REF_W if CTRL_REF_W > 0 else w
        ref_h = CTRL_REF_H if CTRL_REF_H > 0 else h
        sx = int(round(x * ref_w / float(w)))
        sy = int(round(y * ref_h / float(h)))
        self.ctrl.mouse_abs(sx, sy, ref_w, ref_h)

    @QtCore.Slot(int)
    def _on_mouse_pressed(self, btn: int) -> None:
        if not self.isActiveWindow():
            return
        left = int(getattr(QtCore.Qt.LeftButton, "value", QtCore.Qt.LeftButton))
        right = int(getattr(QtCore.Qt.RightButton, "value", QtCore.Qt.RightButton))
        mid = int(getattr(QtCore.Qt.MiddleButton, "value", QtCore.Qt.MiddleButton))
        if btn == left:
            self.ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_LEFT)
        elif btn == right:
            self.ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_RIGHT)
        elif btn == mid:
            self.ctrl.mouse_button(BUTTON_ACTION_PRESS, BUTTON_MIDDLE)

    @QtCore.Slot(int)
    def _on_mouse_released(self, btn: int) -> None:
        if not self.isActiveWindow():
            return
        left = int(getattr(QtCore.Qt.LeftButton, "value", QtCore.Qt.LeftButton))
        right = int(getattr(QtCore.Qt.RightButton, "value", QtCore.Qt.RightButton))
        mid = int(getattr(QtCore.Qt.MiddleButton, "value", QtCore.Qt.MiddleButton))
        if btn == left:
            self.ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT)
        elif btn == right:
            self.ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT)
        elif btn == mid:
            self.ctrl.mouse_button(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE)

    @QtCore.Slot(int, int)
    def _on_wheel_scrolled(self, dx: int, dy: int) -> None:
        if not self.isActiveWindow():
            return
        if dy:
            self.ctrl.mouse_scroll(1 if dy > 0 else -1)
        if dx:
            self.ctrl.mouse_hscroll(1 if dx > 0 else -1)


def main() -> int:
    global VIDEO_PORT, CTRL_PORT, TOKEN

    if ENABLE_STARTLINK:
        r = start_link(STREAM_ID)
        TOKEN = str(r.get("token") or TOKEN or "")
        VIDEO_PORT = int(r.get("video_port") or VIDEO_PORT)
        CTRL_PORT = int(r.get("ctrl_port") or CTRL_PORT)
        print(f"[startLink] ok token={'yes' if TOKEN else 'no'} video_port={VIDEO_PORT} ctrl_port={CTRL_PORT}")

    app = QtWidgets.QApplication([])
    w = MainWindow(SERVER_HOST, VIDEO_PORT, CTRL_PORT, STREAM_ID, TOKEN)
    w.show()
    return int(app.exec())


if __name__ == "__main__":
    raise SystemExit(main())

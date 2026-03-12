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


# =====================
# 控制协议（复用原项目定义）
# =====================
ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1
ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_REQ_IDR = 10

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")


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

    def request_idr(self):
        # a/b/c/d unused
        self._send(ML_CTRL_CMD_REQ_IDR, 0, 0, 0, 0)


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

    def mouse_cb(event, x, y, flags, param):
        # 只在有图像尺寸时发送 ABS
        if event == cv2.EVENT_MOUSEMOVE:
            with lock:
                img = latest["img"]
            if img is not None:
                h, w = img.shape[:2]
                ctrl.mouse_abs(x, y, w, h)

    cv2.setMouseCallback(win, mouse_cb)

    # UI loop
    while True:
        with lock:
            img = latest["img"]
        if img is not None:
            cv2.imshow(win, img)
        k = cv2.waitKey(1) & 0xFF
        if k == 27:  # ESC
            break
        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

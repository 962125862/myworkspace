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

    def request_idr(self):
        self._send(ML_CTRL_CMD_REQ_IDR)

    # Note: v1 only supports mouse move (ABS). No click/keyboard/text.


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

    # mouse callback
    state = {"down": False}

    def mouse_cb(event, x, y, flags, param):
        if event == cv2.EVENT_MOUSEMOVE:
            # send abs in current window coord system; ref_w/ref_h filled by client-side image size
            with lock:
                img = latest["img"]
            if img is not None:
                h, w = img.shape[:2]
                ctrl.mouse_abs(x, y, w, h)

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
        # avoid tight loop when no frames
        if img is None:
            time.sleep(0.01)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

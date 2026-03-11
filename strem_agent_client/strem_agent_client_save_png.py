"""Headless client: connect to agent video TCP, decode first frame, save PNG, exit.

Useful for testing without a GUI window.
"""

from __future__ import annotations

import argparse
import socket

import av  # type: ignore
import cv2  # type: ignore


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--video-port", type=int, default=31234)
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--token", default="")
    ap.add_argument("--out", default="/tmp/agent_frame.png")
    ap.add_argument("--timeout-sec", type=float, default=10.0)
    args = ap.parse_args()

    s = socket.create_connection((args.host, int(args.video_port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if args.token:
        s.sendall(f"AUTH {args.token}\n".encode("utf-8"))
    s.sendall(f"SUB {args.stream_id}\n".encode("utf-8"))

    codec = av.CodecContext.create("h264", "r")
    parser = av.Parser.create(codec.name)

    s.settimeout(1.0)
    deadline = __import__("time").time() + float(args.timeout_sec)
    buf = bytearray(1024 * 1024)

    while __import__("time").time() < deadline:
        try:
            n = s.recv_into(buf)
        except socket.timeout:
            continue
        if n <= 0:
            break
        data = bytes(memoryview(buf)[:n])
        for pkt in parser.parse(data):
            frames = codec.decode(pkt)
            for fr in frames:
                img = fr.to_ndarray(format="bgr24")
                ok = cv2.imwrite(args.out, img)
                if not ok:
                    raise SystemExit(f"cv2.imwrite failed: {args.out}")
                print(f"OK saved {args.out} {img.shape[1]}x{img.shape[0]}")
                return 0

    raise SystemExit("timeout: no frame decoded")


if __name__ == "__main__":
    raise SystemExit(main())


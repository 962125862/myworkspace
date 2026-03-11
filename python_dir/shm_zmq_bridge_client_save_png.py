"""Fetch one NV12 frame via ZMQ DEALER and save to PNG using OpenCV.

This runs on the machine that has access to the bridge endpoint.

Example (on 192.168.11.31):
  source /home/my_server/bin/activate
  python python_dir/shm_zmq_bridge_client_save_png.py --addr tcp://127.0.0.1:5566 --stream-id 1 --out /tmp/frame_cv2.png
"""

from __future__ import annotations

import argparse
import json


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="tcp://127.0.0.1:5566")
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument("--no-request-new", action="store_true")
    ap.add_argument("--out", default="/tmp/frame_cv2.png")
    args = ap.parse_args()

    import zmq  # type: ignore
    import numpy as np  # type: ignore
    import cv2  # type: ignore

    ctx = zmq.Context.instance()
    s = ctx.socket(zmq.DEALER)
    s.connect(args.addr)

    req = {
        "stream_id": args.stream_id,
        "timeout_ms": args.timeout_ms,
        "request_new": (not args.no_request_new),
    }

    # Prefer cached path when bridge is running with --pump-streams.
    # Fallback to on-demand path if cache isn't enabled.
    s.send_multipart([b"GET_LATEST_NV12", json.dumps(req).encode("utf-8")])
    resp = s.recv_multipart()
    if resp and resp[0] != b"OK":
        s.send_multipart([b"GET_SHM_NV12", json.dumps(req).encode("utf-8")])
        resp = s.recv_multipart()
    if len(resp) < 2:
        raise SystemExit(f"bad response frames={len(resp)}")

    if resp[0] != b"OK":
        raise SystemExit(b" ".join(resp).decode("utf-8", errors="replace"))

    meta = json.loads(resp[1].decode("utf-8"))
    w = int(meta["width"])
    h = int(meta["height"])
    y = resp[2]
    uv = resp[3]

    # NV12 -> BGR
    y_arr = np.frombuffer(y, dtype=np.uint8).reshape((h, w))
    uv_arr = np.frombuffer(uv, dtype=np.uint8).reshape((h // 2, w))
    nv12 = np.vstack([y_arr, uv_arr])
    bgr = cv2.cvtColor(nv12, cv2.COLOR_YUV2BGR_NV12)

    ok = cv2.imwrite(args.out, bgr)
    if not ok:
        raise SystemExit(f"cv2.imwrite failed: {args.out}")

    print(f"OK saved {args.out} ({w}x{h})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

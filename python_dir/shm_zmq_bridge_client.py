"""Example DEALER client for python_dir/shm_zmq_bridge.py.

Usage:
  python3 shm_zmq_bridge_client.py --addr tcp://127.0.0.1:5555 --stream-id 1 --out-prefix /tmp/frame

It requests one NV12 frame and saves:
  /tmp/frame.meta.json
  /tmp/frame.y
  /tmp/frame.uv

You can preview NV12 with ffplay (width/height from meta):
  ffplay -f rawvideo -pixel_format nv12 -video_size {W}x{H} -i /tmp/frame.nv12

This script also writes /tmp/frame.nv12 (Y followed by UV).
"""

from __future__ import annotations

import argparse
import json
import os


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="tcp://127.0.0.1:5555")
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument(
        "--no-request-new",
        action="store_true",
        help="do not bump request_seq; just read the latest already-published frame",
    )
    ap.add_argument("--out-prefix", default="/tmp/frame")
    args = ap.parse_args()

    try:
        import zmq  # type: ignore
    except ModuleNotFoundError:
        raise SystemExit("pyzmq is required: pip install pyzmq")

    ctx = zmq.Context.instance()
    s = ctx.socket(zmq.DEALER)
    # optional: set identity for debugging
    s.setsockopt(zmq.IDENTITY, b"client-1")
    s.connect(args.addr)

    req = {
        "stream_id": args.stream_id,
        "timeout_ms": args.timeout_ms,
        "request_new": (not bool(args.no_request_new)),
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

    status = resp[0]
    if status != b"OK":
        raise SystemExit(b" ".join(resp).decode("utf-8", errors="replace"))

    meta = json.loads(resp[1].decode("utf-8"))
    y = resp[2]
    uv = resp[3]

    out_prefix = args.out_prefix
    os.makedirs(os.path.dirname(out_prefix) or ".", exist_ok=True)
    with open(out_prefix + ".meta.json", "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    with open(out_prefix + ".y", "wb") as f:
        f.write(y)
    with open(out_prefix + ".uv", "wb") as f:
        f.write(uv)
    with open(out_prefix + ".nv12", "wb") as f:
        f.write(y)
        f.write(uv)

    w = meta.get("width")
    h = meta.get("height")
    print(f"OK stream={args.stream_id} {w}x{h} y={len(y)} uv={len(uv)} -> {out_prefix}.*")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

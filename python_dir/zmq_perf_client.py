"""Simple perf client for ZMQ NV12 endpoints.

Works with:
- python_dir/shm_zmq_bridge.py (GET_LATEST_NV12 / GET_SHM_NV12)
- stream_server embedded ZMQ bridge (GET_LATEST_NV12)

It measures:
- avg request/response wall time per frame
- avg meta.mono_ns age (if provided)

Example:
  source /home/my_server/bin/activate
  python python_dir/zmq_perf_client.py --addr tcp://192.168.11.31:5566 --stream-id 1 --frames 200
"""

from __future__ import annotations

import argparse
import json
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="tcp://127.0.0.1:5566")
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument(
        "--cmd",
        choices=["GET_LATEST_NV12", "GET_SHM_NV12"],
        default="GET_LATEST_NV12",
        help="which cmd to use",
    )
    args = ap.parse_args()

    import zmq  # type: ignore

    ctx = zmq.Context.instance()
    s = ctx.socket(zmq.DEALER)
    s.setsockopt(zmq.RCVTIMEO, args.timeout_ms)
    s.setsockopt(zmq.SNDTIMEO, args.timeout_ms)
    s.connect(args.addr)

    req = {
        "stream_id": args.stream_id,
        "timeout_ms": args.timeout_ms,
        "request_new": False,
    }
    payload = json.dumps(req).encode("utf-8")
    cmd_b = args.cmd.encode("ascii")

    n = int(args.frames)
    dt_sum = 0.0
    dt_max = 0.0
    age_sum_ms = 0.0
    age_cnt = 0
    ok = 0

    for i in range(n):
        t0 = time.perf_counter()
        s.send_multipart([cmd_b, payload])
        resp = s.recv_multipart()
        t1 = time.perf_counter()
        dt = (t1 - t0) * 1000.0
        dt_sum += dt
        dt_max = max(dt_max, dt)

        if not resp or resp[0] != b"OK":
            continue
        ok += 1
        if len(resp) >= 2:
            try:
                meta = json.loads(resp[1].decode("utf-8"))
                mono_ns = meta.get("mono_ns")
                if isinstance(mono_ns, int):
                    age_ms = (time.monotonic_ns() - mono_ns) / 1e6
                    age_sum_ms += age_ms
                    age_cnt += 1
            except Exception:
                pass

        if (i + 1) % 50 == 0:
            print(f"{i+1}/{n} avg_ms={dt_sum/(i+1):.3f} max_ms={dt_max:.3f} ok={ok}")

    avg_ms = dt_sum / max(1, n)
    avg_age = (age_sum_ms / age_cnt) if age_cnt else None
    print("\n=== summary ===")
    print(f"addr={args.addr} stream={args.stream_id} cmd={args.cmd}")
    print(f"frames={n} ok={ok}")
    print(f"avg_rtt_ms={avg_ms:.3f} max_rtt_ms={dt_max:.3f}")
    if avg_age is not None:
        print(f"avg_frame_age_ms={avg_age:.3f} (from meta.mono_ns)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


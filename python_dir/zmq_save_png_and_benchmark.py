"""DEALER client: fetch BGR24 over ZMQ, save one PNG, then benchmark for N seconds.

Works with:
- stream_server embedded ZMQ bridge (GET_LATEST_BGR)

It will:
1) fetch one frame and save PNG (OpenCV)
2) run for --duration-sec, printing FPS and latency stats

Example:
  source /home/my_server/bin/activate
  python python_dir/zmq_save_png_and_benchmark.py --addr tcp://192.168.11.31:5566 --stream-id 1 --out /tmp/frame.png --duration-sec 60
"""

from __future__ import annotations

import argparse
import json
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="tcp://127.0.0.1:5566")
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument("--cmd", choices=["GET_LATEST_BGR"], default="GET_LATEST_BGR")
    ap.add_argument("--out", default="/tmp/frame.png")
    ap.add_argument("--duration-sec", type=float, default=60.0)
    ap.add_argument("--report-every", type=int, default=5)
    args = ap.parse_args()

    import zmq  # type: ignore
    import cv2  # type: ignore

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

    def fetch_one():
        t0 = time.perf_counter()
        s.send_multipart([cmd_b, payload])
        resp = s.recv_multipart()
        t1 = time.perf_counter()
        if not resp or resp[0] != b"OK":
            raise RuntimeError(b" ".join(resp).decode("utf-8", errors="replace") if resp else "no resp")
        meta = json.loads(resp[1].decode("utf-8"))
        bgr = resp[2]
        return (t1 - t0) * 1000.0, meta, bgr

    # 1) save png (warm up: tolerate initial "no cached frame yet")
    last_err = None
    for _ in range(50):
        try:
            rtt_ms, meta, bgr = fetch_one()
            break
        except Exception as e:
            last_err = e
            time.sleep(0.05)
    else:
        raise SystemExit(f"failed to fetch first frame: {last_err}")
    w = int(meta["width"])
    h = int(meta["height"])
    import numpy as np  # type: ignore
    bgr_img = np.frombuffer(bgr, dtype=np.uint8).reshape((h, w, 3))
    ok = cv2.imwrite(args.out, bgr_img)
    if not ok:
        raise SystemExit(f"cv2.imwrite failed: {args.out}")
    print(f"saved png: {args.out} ({w}x{h}) first_rtt_ms={rtt_ms:.3f}")

    # 2) benchmark for duration
    end = time.time() + float(args.duration_sec)
    n = 0
    ok_n = 0
    rtt_sum = 0.0
    rtt_max = 0.0
    age_sum = 0.0
    age_cnt = 0
    last_report = time.time()

    while time.time() < end:
        n += 1
        try:
            rtt_ms, meta, _ = fetch_one()
        except Exception:
            continue
        ok_n += 1
        rtt_sum += rtt_ms
        rtt_max = max(rtt_max, rtt_ms)
        mono_ns = meta.get("mono_ns")
        if isinstance(mono_ns, int):
            age_ms = (time.monotonic_ns() - mono_ns) / 1e6
            age_sum += age_ms
            age_cnt += 1

        now = time.time()
        if now - last_report >= args.report_every:
            fps = ok_n / max(1e-6, (now - (end - args.duration_sec)))
            avg_rtt = rtt_sum / max(1, ok_n)
            avg_age = (age_sum / age_cnt) if age_cnt else None
            msg = f"t+{now-(end-args.duration_sec):.1f}s ok={ok_n} fps={fps:.2f} avg_rtt_ms={avg_rtt:.3f} max_rtt_ms={rtt_max:.3f}"
            if avg_age is not None:
                msg += f" avg_age_ms={avg_age:.3f}"
            print(msg)
            last_report = now

    avg_rtt = rtt_sum / max(1, ok_n)
    avg_age = (age_sum / age_cnt) if age_cnt else None
    print("\n=== summary ===")
    print(f"addr={args.addr} stream={args.stream_id} cmd={args.cmd}")
    print(f"duration_sec={args.duration_sec} ok={ok_n}")
    print(f"avg_rtt_ms={avg_rtt:.3f} max_rtt_ms={rtt_max:.3f}")
    if avg_age is not None:
        print(f"avg_frame_age_ms={avg_age:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

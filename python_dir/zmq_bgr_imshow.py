"""Display BGR24 frames from stream_server built-in ZMQ bridge via cv2.imshow().

Protocol:
  request:  [b"GET_LATEST_BGR", json]
  response: [b"OK", meta_json, bgr24]

Example:
  python3 python_dir/zmq_bgr_imshow.py --addr tcp://192.168.11.31:5566 --stream-id 1
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
    ap.add_argument("--title", default="stream_server_bgr")
    ap.add_argument("--report-every", type=float, default=2.0)
    args = ap.parse_args()

    import cv2  # type: ignore
    import numpy as np  # type: ignore
    import zmq  # type: ignore

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.DEALER)
    sock.setsockopt(zmq.RCVTIMEO, args.timeout_ms)
    sock.setsockopt(zmq.SNDTIMEO, args.timeout_ms)
    sock.connect(args.addr)

    req = {
        "stream_id": args.stream_id,
        "timeout_ms": args.timeout_ms,
        "request_new": False,
    }
    payload = json.dumps(req).encode("utf-8")

    frames = 0
    last_report_t = time.time()
    start_t = last_report_t

    while True:
        t0 = time.perf_counter()
        sock.send_multipart([b"GET_LATEST_BGR", payload])
        resp = sock.recv_multipart()
        t1 = time.perf_counter()

        if not resp or resp[0] != b"OK":
            msg = b" ".join(resp).decode("utf-8", errors="replace") if resp else "no response"
            print(f"[ERR] {msg}")
            time.sleep(0.05)
            continue

        if len(resp) < 3:
            print(f"[ERR] bad response frames={len(resp)}")
            time.sleep(0.05)
            continue

        meta = json.loads(resp[1].decode("utf-8"))
        w = int(meta["width"])
        h = int(meta["height"])
        stride = int(meta.get("stride", w * 3))
        buf = resp[2]

        img = np.frombuffer(buf, dtype=np.uint8).reshape((h, stride))
        img = img[:, : w * 3].reshape((h, w, 3))

        cv2.imshow(args.title, img)
        key = cv2.waitKey(1) & 0xFF
        if key in (27, ord("q")):
            break

        frames += 1
        now = time.time()
        if now - last_report_t >= args.report_every:
            fps = frames / max(1e-6, now - start_t)
            rtt_ms = (t1 - t0) * 1000.0
            age_ms = None
            mono_ns = meta.get("mono_ns")
            if isinstance(mono_ns, int):
                age_ms = (time.monotonic_ns() - mono_ns) / 1e6

            line = f"fps={fps:.2f} rtt_ms={rtt_ms:.2f} size={w}x{h}"
            if age_ms is not None:
                line += f" age_ms={age_ms:.2f}"
            print(line)
            last_report_t = now

    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

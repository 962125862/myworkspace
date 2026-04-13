"""Multi-stream ZMQ BGR24 perf client.

Drives GET_LATEST_BGR against stream_server's built-in ZMQ bridge using one
DEALER socket per stream. Requests are paced to a target FPS so the benchmark
can approximate one BGR pull per decoded frame.

Example:
  source /home/my_server/bin/activate
  python python_dir/zmq_multi_stream_bgr_perf.py \
      --addr ipc:///tmp/stream_server_bgr.sock \
      --streams 1-20 \
      --fps 30 \
      --duration-sec 30
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import threading
import time
from dataclasses import dataclass, field


def parse_streams(spec: str) -> list[int]:
    result: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            start_s, end_s = part.split("-", 1)
            start = int(start_s)
            end = int(end_s)
            if end < start:
                start, end = end, start
            result.extend(range(start, end + 1))
        else:
            result.append(int(part))
    seen: set[int] = set()
    deduped: list[int] = []
    for stream_id in result:
        if stream_id in seen:
            continue
        seen.add(stream_id)
        deduped.append(stream_id)
    return deduped


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * q
    lower = math.floor(pos)
    upper = math.ceil(pos)
    if lower == upper:
        return ordered[lower]
    frac = pos - lower
    return ordered[lower] * (1.0 - frac) + ordered[upper] * frac


@dataclass
class StreamStats:
    stream_id: int
    requests: int = 0
    ok: int = 0
    errors: int = 0
    rtts_ms: list[float] = field(default_factory=list)
    ages_ms: list[float] = field(default_factory=list)


def worker(
    *,
    addr: str,
    stream_id: int,
    fps: float,
    duration_sec: float,
    timeout_ms: int,
    request_new: bool,
    start_event: threading.Event,
    done: dict[int, StreamStats],
    done_lock: threading.Lock,
) -> None:
    import zmq  # type: ignore

    stats = StreamStats(stream_id=stream_id)
    payload = json.dumps(
        {
            "stream_id": stream_id,
            "timeout_ms": timeout_ms,
            "request_new": request_new,
        }
    ).encode("utf-8")

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.DEALER)
    sock.setsockopt(zmq.RCVTIMEO, timeout_ms)
    sock.setsockopt(zmq.SNDTIMEO, timeout_ms)
    sock.connect(addr)

    interval = 1.0 / fps
    start_event.wait()
    t0 = time.perf_counter()
    deadline = t0 + duration_sec
    next_tick = t0

    try:
        while True:
            now = time.perf_counter()
            if now >= deadline:
                break
            if now < next_tick:
                time.sleep(next_tick - now)
                now = time.perf_counter()

            req_start = time.perf_counter()
            stats.requests += 1
            try:
                sock.send_multipart([b"GET_LATEST_BGR", payload])
                resp = sock.recv_multipart()
                req_end = time.perf_counter()
                rtt_ms = (req_end - req_start) * 1000.0
                stats.rtts_ms.append(rtt_ms)
                if resp and resp[0] == b"OK":
                    stats.ok += 1
                    if len(resp) >= 2:
                        try:
                            meta = json.loads(resp[1].decode("utf-8"))
                            mono_ns = meta.get("mono_ns")
                            if isinstance(mono_ns, int):
                                stats.ages_ms.append(
                                    (time.monotonic_ns() - mono_ns) / 1e6
                                )
                        except Exception:
                            pass
                else:
                    stats.errors += 1
            except Exception:
                stats.errors += 1
            next_tick += interval
    finally:
        sock.close(linger=0)
        with done_lock:
            done[stream_id] = stats


def summarize(streams: list[StreamStats], duration_sec: float) -> int:
    print("=== per-stream ===")
    all_rtts: list[float] = []
    all_ages: list[float] = []
    total_ok = 0
    total_requests = 0
    total_errors = 0

    for stats in sorted(streams, key=lambda s: s.stream_id):
        all_rtts.extend(stats.rtts_ms)
        all_ages.extend(stats.ages_ms)
        total_ok += stats.ok
        total_requests += stats.requests
        total_errors += stats.errors
        avg_rtt = statistics.fmean(stats.rtts_ms) if stats.rtts_ms else 0.0
        p95_rtt = percentile(stats.rtts_ms, 0.95) if stats.rtts_ms else 0.0
        max_rtt = max(stats.rtts_ms) if stats.rtts_ms else 0.0
        avg_age = statistics.fmean(stats.ages_ms) if stats.ages_ms else 0.0
        achieved_fps = stats.ok / duration_sec if duration_sec > 0 else 0.0
        print(
            f"stream={stats.stream_id:02d} req={stats.requests} ok={stats.ok} "
            f"err={stats.errors} fps={achieved_fps:.2f} "
            f"avg_rtt_ms={avg_rtt:.3f} p95_rtt_ms={p95_rtt:.3f} "
            f"max_rtt_ms={max_rtt:.3f} avg_age_ms={avg_age:.3f}"
        )

    print("\n=== summary ===")
    overall_avg_rtt = statistics.fmean(all_rtts) if all_rtts else 0.0
    overall_p95_rtt = percentile(all_rtts, 0.95) if all_rtts else 0.0
    overall_max_rtt = max(all_rtts) if all_rtts else 0.0
    overall_avg_age = statistics.fmean(all_ages) if all_ages else 0.0
    overall_fps = total_ok / duration_sec if duration_sec > 0 else 0.0
    print(f"streams={len(streams)} duration_sec={duration_sec:.1f}")
    print(
        f"requests={total_requests} ok={total_ok} err={total_errors} "
        f"overall_ok_fps={overall_fps:.2f}"
    )
    print(
        f"avg_rtt_ms={overall_avg_rtt:.3f} p95_rtt_ms={overall_p95_rtt:.3f} "
        f"max_rtt_ms={overall_max_rtt:.3f}"
    )
    if all_ages:
        print(f"avg_frame_age_ms={overall_avg_age:.3f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="ipc:///tmp/stream_server_bgr.sock")
    ap.add_argument("--streams", default="1-20")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--duration-sec", type=float, default=30.0)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument("--request-new", action="store_true")
    args = ap.parse_args()

    stream_ids = parse_streams(args.streams)
    if not stream_ids:
        raise SystemExit("no stream ids parsed")

    start_event = threading.Event()
    done: dict[int, StreamStats] = {}
    done_lock = threading.Lock()
    threads = [
        threading.Thread(
            target=worker,
            kwargs={
                "addr": args.addr,
                "stream_id": stream_id,
                "fps": args.fps,
                "duration_sec": args.duration_sec,
                "timeout_ms": args.timeout_ms,
                "request_new": args.request_new,
                "start_event": start_event,
                "done": done,
                "done_lock": done_lock,
            },
            daemon=True,
        )
        for stream_id in stream_ids
    ]

    for thread in threads:
        thread.start()
    start_event.set()
    for thread in threads:
        thread.join()

    stats = [done[stream_id] for stream_id in stream_ids if stream_id in done]
    return summarize(stats, args.duration_sec)


if __name__ == "__main__":
    raise SystemExit(main())

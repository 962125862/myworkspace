"""Monitor per-process CPU usage using /proc.

Works without pidstat/top.

CPU% is reported in a "per-core" convention (like top):
  100% ~= fully occupying 1 CPU core.

Example:
  python3 proc_cpu_monitor.py --pids 123,456 --duration-sec 60
"""

from __future__ import annotations

import argparse
import os
import time
from dataclasses import dataclass


@dataclass
class Sample:
    t: float
    total_jiffies: int
    proc_jiffies: dict[int, int]


def read_total_jiffies() -> int:
    with open("/proc/stat", "r", encoding="utf-8") as f:
        line = f.readline()
    # cpu  user nice system idle iowait irq softirq steal guest guest_nice
    parts = line.strip().split()
    vals = [int(x) for x in parts[1:]]
    return sum(vals)


def read_proc_jiffies(pid: int) -> int:
    # /proc/<pid>/stat: ... utime stime ... (fields 14,15; 1-based)
    with open(f"/proc/{pid}/stat", "r", encoding="utf-8") as f:
        s = f.read()
    parts = s.split()
    utime = int(parts[13])
    stime = int(parts[14])
    return utime + stime


def take_sample(pids: list[int]) -> Sample:
    total = read_total_jiffies()
    proc = {}
    for pid in pids:
        try:
            proc[pid] = read_proc_jiffies(pid)
        except FileNotFoundError:
            proc[pid] = 0
    return Sample(t=time.time(), total_jiffies=total, proc_jiffies=proc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pids", required=True, help="comma-separated pids")
    ap.add_argument("--duration-sec", type=float, default=60.0)
    ap.add_argument("--interval-sec", type=float, default=1.0)
    args = ap.parse_args()

    pids = [int(x) for x in args.pids.split(",") if x.strip()]
    ncpu = os.cpu_count() or 1

    end = time.time() + float(args.duration_sec)
    prev = take_sample(pids)
    per_pid_sum = {pid: 0.0 for pid in pids}
    per_pid_max = {pid: 0.0 for pid in pids}
    ticks = 0

    while time.time() < end:
        time.sleep(float(args.interval_sec))
        cur = take_sample(pids)
        dt_total = cur.total_jiffies - prev.total_jiffies
        if dt_total <= 0:
            prev = cur
            continue

        for pid in pids:
            dp = cur.proc_jiffies.get(pid, 0) - prev.proc_jiffies.get(pid, 0)
            # scale to "per-core" percent (top style)
            cpu_pct = (dp / dt_total) * 100.0 * ncpu
            per_pid_sum[pid] += cpu_pct
            per_pid_max[pid] = max(per_pid_max[pid], cpu_pct)

        ticks += 1
        prev = cur

    print("=== cpu summary (per-core %) ===")
    print(f"duration_sec={args.duration_sec} interval_sec={args.interval_sec} ticks={ticks} ncpu={ncpu}")
    for pid in pids:
        avg = per_pid_sum[pid] / max(1, ticks)
        mx = per_pid_max[pid]
        print(f"pid={pid} avg_cpu%={avg:.2f} max_cpu%={mx:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


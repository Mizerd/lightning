#!/usr/bin/env python3
"""Sample an existing Lightning process without reading stores or credentials.

Run after sync warm-up, with no UI interaction, for at least 300 seconds.
Output is JSONL: cumulative /proc counters, interval rates, SQLite file sizes
and thread activity. File labels hash parent paths; no account paths are saved.
write_bytes is kernel-accounted process I/O, not WAL growth or device-wide I/O.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import time


KEYS = ("read_bytes", "write_bytes", "syscr", "syscw", "rchar", "wchar",
        "cancelled_write_bytes")


def counters(path):
    return {k: int(v) for k, v in
            (line.split(":", 1) for line in path.read_text().splitlines())}


def identity(proc):
    # Start time prevents accidentally following a reused PID.
    return proc.joinpath("stat").read_text().rsplit(")", 1)[1].split()[19]


def database_sizes(proc):
    result = {}
    for fd in proc.joinpath("fd").iterdir():
        try:
            path = Path(os.readlink(fd))
            # Only names with a fixed database extension; never inspect contents.
            if not path.name.endswith((".sqlite", ".sqlite3", ".sqlite-wal",
                                       ".sqlite3-wal", ".sqlite-shm", ".sqlite3-shm")):
                continue
            base = Path(str(path).removesuffix("-wal").removesuffix("-shm"))
            label = hashlib.sha256(str(base.parent).encode()).hexdigest()[:12]
            for suffix in ("", "-wal", "-shm"):
                file = Path(str(base) + suffix)
                try:
                    stat = file.stat()
                    result[label + "/" + file.name] = {
                        "size": stat.st_size, "allocated_bytes": stat.st_blocks * 512,
                        "mtime_ns": stat.st_mtime_ns,
                    }
                except FileNotFoundError:
                    pass
        except (FileNotFoundError, PermissionError, OSError):
            continue
    return result


def threads(proc):
    result = {}
    for task in proc.joinpath("task").iterdir():
        try:
            stat = task.joinpath("stat").read_text().rsplit(")", 1)[1].split()
            result[task.name] = {
                "name": task.joinpath("comm").read_text().strip(),
                "cpu_ticks": int(stat[11]) + int(stat[12]),
                "io": counters(task / "io"),
            }
        except (FileNotFoundError, ProcessLookupError):
            continue
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--seconds", type=float, default=300)
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        parser.error("seconds and interval must be positive")
    proc = Path("/proc") / str(args.pid)
    executable = Path(os.readlink(proc / "exe")).name
    if executable != "lightning-matrix":
        parser.error("PID is not the lightning-matrix executable")
    started = identity(proc)
    # Refuse overwriting logs and restrict permissions at creation.
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as output:
        first = previous = counters(proc / "io")
        begin = last = time.monotonic()
        rates = []
        burst_times = []
        in_burst = False
        output.write(json.dumps({"type": "start", "pid": args.pid,
                                 "start_ticks": started, "io": first}) + "\n")
        while last - begin < args.seconds:
            time.sleep(min(args.interval, args.seconds - (last - begin)))
            if identity(proc) != started:
                raise RuntimeError("PID identity changed; measurement incomplete")
            current = counters(proc / "io")
            now = time.monotonic()
            elapsed = now - last
            rate = {k: (current[k] - previous[k]) / elapsed for k in KEYS}
            mib = rate["write_bytes"] / 1048576
            rates.append(mib)
            # A stated threshold, not an inferred root cause. Group adjacent samples.
            if mib >= 10 and not in_burst:
                burst_times.append(now - begin)
            in_burst = mib >= 10
            record = {"type": "sample", "utc": datetime.datetime.now(
                datetime.timezone.utc).isoformat(), "elapsed": now - begin,
                "interval": elapsed, "io": current, "per_second": rate,
                "write_mib_s": mib, "read_mib_s": rate["read_bytes"] / 1048576,
                "databases": database_sizes(proc), "threads": threads(proc)}
            output.write(json.dumps(record) + "\n")
            output.flush()
            previous, last = current, now
        duration = last - begin
        summary = {"type": "summary", "seconds": duration,
                   "total_write_bytes": current["write_bytes"] - first["write_bytes"],
                   "total_read_bytes": current["read_bytes"] - first["read_bytes"],
                   "average_write_mib_s": (current["write_bytes"] - first["write_bytes"])
                   / duration / 1048576, "minimum_write_mib_s": min(rates),
                   "peak_write_mib_s": max(rates), "bursts_at_least_10_mib_s": burst_times,
                   "burst_intervals": [b - a for a, b in zip(burst_times, burst_times[1:])]}
        output.write(json.dumps(summary) + "\n")
        print(json.dumps(summary))


if __name__ == "__main__":
    main()

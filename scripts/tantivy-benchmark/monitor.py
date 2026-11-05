#!/usr/bin/env python3
"""Sample selected Linux processes; CPU 100% means one logical CPU core.

Use the same process scope for each backend. Include graphd, metad, all three
storaged, and (for ES) Listener plus Elasticsearch. RSS sums double-count shared
pages; PSS is collected when /proc/PID/smaps_rollup is readable.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import time


def read_process(pid):
    root = Path("/proc") / str(pid)
    stat = (root / "stat").read_text()
    # A process name may contain spaces and parentheses.
    fields = stat[stat.rfind(")") + 2:].split()
    status = {}
    for line in (root / "status").read_text().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            status[key] = value.strip()
    io = {}
    try:
        io = {key: int(value) for key, value in
              (line.split(":") for line in (root / "io").read_text().splitlines())}
    except PermissionError:
        pass
    pss = None
    try:
        for line in (root / "smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                pss = int(line.split()[1]) * 1024
    except (PermissionError, FileNotFoundError):
        pass
    return {"cpu_ticks": int(fields[11]) + int(fields[12]),
            "start_ticks": int(fields[19]),
            "rss_bytes": int(status.get("VmRSS", "0 kB").split()[0]) * 1024,
            "pss_bytes": pss, "threads": int(status.get("Threads", "0")),
            "read_bytes": io.get("read_bytes"), "write_bytes": io.get("write_bytes")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, action="append", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--duration", type=float, default=0, help="0 runs until interrupted")
    args = parser.parse_args()
    if args.interval <= 0 or args.duration < 0:
        parser.error("interval must be positive and duration nonnegative")
    target = Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    ticks = os.sysconf("SC_CLK_TCK")
    previous = {}
    start = previous_time = time.monotonic()
    alive = True
    def stop(*unused):
        nonlocal alive
        alive = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    max_rss = max_pss = max_cpu = count = 0
    with target.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=["utc", "elapsed_s", "pid", "alive",
            "cpu_percent_one_core", "rss_bytes", "pss_bytes", "threads", "read_bytes", "write_bytes"])
        writer.writeheader()
        while alive:
            now = time.monotonic()
            elapsed = now - start
            rss_sum = pss_sum = cpu_sum = 0
            for pid in dict.fromkeys(args.pid):
                row = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                       "elapsed_s": round(elapsed, 3), "pid": pid, "alive": True}
                try:
                    current = read_process(pid)
                    old = previous.get(pid)
                    cpu = None
                    if old and old["start_ticks"] == current["start_ticks"]:
                        cpu = 100 * (current["cpu_ticks"] - old["cpu_ticks"]) / ticks / (now - previous_time)
                    previous[pid] = current
                    row.update({key: current[key] for key in
                                ("rss_bytes", "pss_bytes", "threads", "read_bytes", "write_bytes")})
                    row["cpu_percent_one_core"] = round(cpu, 3) if cpu is not None else None
                    rss_sum += current["rss_bytes"]
                    pss_sum += current["pss_bytes"] or 0
                    cpu_sum += cpu or 0
                except (FileNotFoundError, ProcessLookupError):
                    row["alive"] = False
                writer.writerow(row)
            output.flush()
            count += 1
            max_rss, max_pss, max_cpu = max(max_rss, rss_sum), max(max_pss, pss_sum), max(max_cpu, cpu_sum)
            previous_time = now
            if args.duration and elapsed >= args.duration:
                break
            time.sleep(args.interval)
    summary = {"pids": args.pid, "sample_count": count, "elapsed_s": time.monotonic() - start,
               "max_sum_rss_bytes": max_rss, "max_sum_available_pss_bytes": max_pss,
               "max_sum_cpu_percent_one_core": max_cpu,
               "rss_note": "RSS sums double-count shared pages; PSS unavailable values are omitted",
               "disk_note": "read_bytes/write_bytes are cumulative process block-I/O counters, not data size"}
    Path(str(target) + ".summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()

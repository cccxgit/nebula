#!/usr/bin/env python3
"""Record only this experiment's daemon counters; no process control."""
import argparse
import datetime
import json
from pathlib import Path
import time

p = argparse.ArgumentParser()
p.add_argument("--runtime", default="/home/sch/nebula-run/nebula-index")
p.add_argument("--seconds", type=int, default=1200)
args = p.parse_args()
root = Path(args.runtime)
start = time.monotonic()
while time.monotonic() - start < args.seconds:
    record = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "elapsed": round(time.monotonic() - start, 3), "processes": {}}
    for daemon in ("metad", "graphd", "storaged"):
        try:
            pid = int((root / f"pids/nebula-{daemon}.pid").read_text().strip())
            proc = Path(f"/proc/{pid}")
            executable = (proc / "exe").resolve()
            if executable != root / f"bin/nebula-{daemon}":
                raise ValueError("PID is not this experiment's executable")
            status = dict(line.split(":", 1) for line in (proc / "status").read_text().splitlines()
                          if ":" in line)
            io = dict(line.split(":", 1) for line in (proc / "io").read_text().splitlines()
                      if ":" in line)
            stat = (proc / "stat").read_text().split(") ", 1)[1].split()
            record["processes"][daemon] = dict(pid=pid, rss=status.get("VmRSS", "").strip(),
                    threads=status.get("Threads", "").strip(), user_ticks=int(stat[11]),
                    system_ticks=int(stat[12]), io={k: int(v) for k, v in io.items()})
        except (OSError, ValueError) as e:
            record["processes"][daemon] = {"unavailable": str(e)}
    record["loadavg"] = Path("/proc/loadavg").read_text().strip()
    print(json.dumps(record), flush=True)
    if all("unavailable" in value for value in record["processes"].values()):
        break
    time.sleep(2)

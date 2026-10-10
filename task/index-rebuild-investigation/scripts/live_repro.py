#!/usr/bin/env python3
"""Live nGQL reproduction; no fault injection and no production connections.

Requires the diagnostic-only RebuildIndexTask.cpp patch. Logs complete nGQL,
responses, effective flags, wall times, and newly emitted storage diagnostics.
Each invocation creates a fresh space and never drops existing data.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import time
import urllib.request

from nebula3.Config import Config
from nebula3.gclient.net import ConnectionPool


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--kind", choices=["TAG", "EDGE"], required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--backlog", type=int, default=96)
    p.add_argument("--seed", type=int, default=2048)
    p.add_argument("--batch", type=int, default=131072)
    p.add_argument("--rate", type=int, default=524288)
    p.add_argument("--port", type=int, default=21531)
    p.add_argument("--http-port", type=int, default=22631)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--runtime", default="/home/sch/nebula-run/nebula-index")
    p.add_argument("--output", required=True)
    args = p.parse_args()
    if not re.fullmatch(r"ir_[a-z0-9_]+", args.name):
        raise ValueError("Use a fresh experiment space name beginning ir_")
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    events = (out / "events.jsonl").open("w", buffering=1)
    queries = (out / "queries.ngql").open("w", buffering=1)
    diag = (out / "storage-observation.log").open("w", buffering=1)
    experiment_diag = (out / "experiment-storage.log").open("w", buffering=1)
    begin = time.monotonic()

    def event(kind, **fields):
        value = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                     elapsed=round(time.monotonic() - begin, 3), event=kind, **fields)
        line = json.dumps(value, ensure_ascii=False, default=str)
        events.write(line + "\n")
        print(line, flush=True)

    def flags(method, payload=None):
        url = f"http://127.0.0.1:{args.http_port}/flags"
        if method == "GET":
            url += "?flags=rebuild_index_batch_size,rebuild_index_part_rate_limit,max_concurrent_subtasks&format=json"
        req = urllib.request.Request(url,
                data=None if payload is None else json.dumps(payload).encode(),
                method=method, headers={"Content-Type": "application/json"})
        # This experiment must connect directly to loopback, not the host's HTTP proxy.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=10) as response:
            result = response.read().decode()
        event("flags_" + method.lower(), response=result, payload=payload)
        parsed = json.loads(result)
        if method == "PUT" and parsed.get("errCode") != 0:
            raise RuntimeError(f"Failed to set experiment flags: {parsed}")
        return parsed

    cfg = Config()
    cfg.max_connection_pool_size = 3
    cfg.timeout = 120000
    pool = ConnectionPool()
    if not pool.init([("127.0.0.1", args.port)], cfg):
        raise RuntimeError("Cannot connect to local experiment graphd")
    session = pool.get_session("root", "nebula")
    storage_log = Path(args.runtime) / "logs" / "nebula-storaged.INFO"
    reader = storage_log.open(errors="replace")
    reader.seek(0, 2)
    capture_experiment = False
    experiment_logs = []

    def read_storage():
        content = reader.read()
        if content:
            diag.write(content)
            if capture_experiment:
                experiment_logs.append(content)
                experiment_diag.write(content)
        return content

    def execute(query, label="query"):
        queries.write(query.rstrip(";") + ";\n")
        start = time.monotonic()
        res = session.execute(query)
        event(label, ok=res.is_succeeded(), seconds=round(time.monotonic()-start, 6),
              error=res.error_msg(), result=res.as_primitive())
        if not res.is_succeeded():
            raise RuntimeError(f"{label}: {res.error_msg()}")
        return res

    def insert(start, count, marker):
        cols = ",".join(f"c{i}" for i in range(16))
        values = ",".join('"' + marker + '"' for _ in range(16))
        for lo in range(start, start + count, 128):
            hi = min(start + count, lo + 128)
            if args.kind == "TAG":
                keys = [f"{i}:({values})" for i in range(lo, hi)]
                query = f"INSERT VERTEX sample({cols}) VALUES " + ",".join(keys)
            else:
                keys = [f"{i}->{i+1000000}@0:({values})" for i in range(lo, hi)]
                query = f"INSERT EDGE sample({cols}) VALUES " + ",".join(keys)
            execute(query, "insert")

    def job_id(result):
        return result.row_values(0)[0].as_int()

    def poll_job(job, started):
        while time.monotonic() - started < args.timeout:
            read_storage()
            res = execute(f"SHOW JOB {job}", "show_job")
            text = str(res.as_primitive())
            # First row is job status, following rows are individual host tasks.
            status = res.row_values(0)[2].as_string()
            if status in ("FINISHED", "FAILED", "STOPPED"):
                event("job_terminal", job=job, status=status,
                      job_seconds=round(time.monotonic()-started, 3))
                if status != "FINISHED":
                    raise RuntimeError(text)
                return
            time.sleep(2)
        raise TimeoutError(f"Job {job} exceeded observation timeout; no STOP sent")

    try:
        event("parameters", parameters=vars(args))
        flags("GET")
        flags("PUT", {"rebuild_index_batch_size": str(args.batch),
                      "rebuild_index_part_rate_limit": str(args.rate)})
        effective = flags("GET")
        actual = {item["name"]: item["value"] for item in effective["flags"]}
        if (int(actual["rebuild_index_batch_size"]),
                int(actual["rebuild_index_part_rate_limit"])) != (args.batch, args.rate):
            raise AssertionError(actual)
        execute(f"CREATE SPACE {args.name}(partition_num=1, replica_factor=1, vid_type=INT64)")
        time.sleep(5)
        execute(f"USE {args.name}")
        execute("CREATE " + args.kind + " sample(" +
                ",".join(f"c{i} string NOT NULL" for i in range(16)) + ")")
        time.sleep(5)
        insert(0, args.seed, "seed")
        execute("CREATE " + args.kind + " INDEX sample_idx ON sample(" +
                ",".join(f"c{i}(256)" for i in range(16)) + ")")
        time.sleep(5)
        execute("SHOW HOSTS", "hosts")
        execute("SHOW PARTS", "parts")
        # Baseline builds the same index over the same initial records without writes.
        read_storage()
        started = time.monotonic()
        baseline = job_id(execute(f"REBUILD {args.kind} INDEX sample_idx", "baseline_submit"))
        poll_job(baseline, started)
        execute(f"SHOW {args.kind} INDEX STATUS", "baseline_index_status")
        read_storage()
        capture_experiment = True
        started = time.monotonic()
        experiment = job_id(execute(f"REBUILD {args.kind} INDEX sample_idx", "experiment_submit"))
        deadline = time.monotonic() + 30
        while True:
            fresh = read_storage()
            if "Start building index" in fresh:
                if "Processing operation logs" in fresh or "RebuildIndexTask Finished" in fresh:
                    raise RuntimeError("Global scan already ended; workload timing missed")
                event("global_scan_observed", job=experiment)
                break
            if time.monotonic() > deadline:
                raise TimeoutError("Did not observe BUILDING stage; do not assume backlog created")
            time.sleep(0.02)
        insert(100000, args.backlog, "concurrent")
        event("concurrent_writes_finished", records=args.backlog)
        poll_job(experiment, started)
        execute(f"SHOW {args.kind} INDEX STATUS", "experiment_index_status")
        if args.kind == "TAG":
            query = 'LOOKUP ON sample WHERE sample.c0 == "concurrent" YIELD id(vertex) AS id | YIELD count(*) AS n'
        else:
            query = 'LOOKUP ON sample WHERE sample.c0 == "concurrent" YIELD src(edge) AS id | YIELD count(*) AS n'
        result = execute(query, "verify_concurrent_index_rows")
        count = result.row_values(0)[0].as_int()
        if count != args.backlog:
            raise AssertionError((count, args.backlog))
        read_storage()
        combined = "".join(experiment_logs)
        rounds = [int(v) for v in re.findall(
            r"INDEX_DIAG replay_end .*?operations=(\d+)", combined)]
        samples = []
        for line in combined.splitlines():
            if "INDEX_DIAG append " in line:
                fields = dict(re.findall(r"(\w+)=(\d+)", line))
                samples.append({k: int(v) for k, v in fields.items()})
        sleeping = [v for v in samples if v["batch_bytes"] > args.rate
                    and v["limit_ms"] >= 900]
        growing = sum(b["seq"] == a["seq"] + 1
                      and b["entries"] == a["entries"] + 2
                      and b["batch_bytes"] > a["batch_bytes"]
                      for a, b in zip(samples, samples[1:]))
        event("reproduction_evidence", replay_operations_by_round=rounds,
              sampled_append_calls=len(samples), over_burst_one_second_calls=len(sleeping),
              consecutive_growing_batches=growing,
              max_batch_bytes=max((v["batch_bytes"] for v in samples), default=0))
        if not rounds or max(rounds) != args.backlog or len(sleeping) < 3 or growing < 3:
            raise AssertionError("Workload completed but did not prove the target replay pathology")
        event("success", baseline_job=baseline, experiment_job=experiment)
    finally:
        read_storage()
        session.release()
        pool.close()
        reader.close()
        events.close()
        queries.close()
        diag.close()
        experiment_diag.close()


if __name__ == "__main__":
    main()

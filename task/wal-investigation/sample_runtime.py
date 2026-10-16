#!/usr/bin/env python3
"""Read-only multi-storaged memory/RocksDB/WAL sampler (Python stdlib only).

Example:
  python3 sample_runtime.py --pid-file s1=/path/s1.pid --pid-file s2=/path/s2.pid \
    --ports 19779,19789 --data-dir s1=/path/s1/data --data-dir s2=/path/s2/data \
    --spaces-file spaces.txt --interval 10 --duration 900 --output samples

--spaces-file accepts one comma/whitespace separated list of existing space names.
Properties are returned by Nebula 3.6 as an array (request-property order) of
objects {"Engine 0": "decimal string", ...}; they are not named in the response.
Memtable/table-reader statistics are summed across spaces and engines. Block
cache usage/pinned/capacity are shared by every engine in this process, so their
MAXIMUM is recorded, never their sum. Minor within-sample skew is unavoidable.
WAL record estimates use first/last on-disk LogIDs and assume contiguous IDs in
each *.wal file. This is not a full file-integrity validator. On-disk WALs may
contain entries from before process startup and are not a RAM-buffer counter.
"""

import argparse
import concurrent.futures
import csv
import datetime
import json
import math
import os
from pathlib import Path
import re
import struct
import time
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_PROPERTIES = [
    "rocksdb.cur-size-active-mem-table",
    "rocksdb.cur-size-all-mem-tables",
    "rocksdb.size-all-mem-tables",
    "rocksdb.num-entries-active-mem-table",
    "rocksdb.num-entries-imm-mem-tables",
    "rocksdb.num-immutable-mem-table",
    "rocksdb.estimate-table-readers-mem",
    "rocksdb.block-cache-usage",
    "rocksdb.block-cache-pinned-usage",
    "rocksdb.block-cache-capacity",
    "rocksdb.estimate-num-keys",
]
STATUS_KEYS = ["VmRSS", "RssAnon", "RssFile", "RssShmem", "VmSize", "VmData", "VmSwap", "Threads"]
SMAPS_KEYS = ["Rss", "Pss", "Pss_Anon", "Pss_File", "Private_Clean", "Private_Dirty", "Anonymous", "Swap"]
SHARED_PROPERTIES = {p for p in DEFAULT_PROPERTIES if p.startswith("rocksdb.block-cache-")}
WAL_HEADER = struct.Struct("<qqiq")  # LogID, TermID, int32 length, ClusterID
WAL_FOOTER = struct.Struct("<i")
WAL_FIXED_BYTES = 32
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def labelled(value, default_label=None):
    if "=" in value:
        label, value = value.split("=", 1)
    else:
        label = default_label
    if not label or not re.fullmatch(r"[A-Za-z0-9_.-]+", label):
        raise ValueError("Use a label made of letters, digits, _, -, or .: " + str(label))
    return label, value


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def parse_proc(path):
    result = {}
    for line in path.read_text().splitlines():
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        fields = value.split()
        if fields and fields[0].isdigit():
            result[name] = int(fields[0])
    return result


def sample_proc(target):
    row, errors = {}, []
    try:
        pid = int(Path(target["pid_file"]).read_text().strip()) if "pid_file" in target else target["pid"]
        row["pid"] = pid
        base = Path("/proc") / str(pid)
        status = parse_proc(base / "status")
        for key in STATUS_KEYS:
            row["status_" + key + ("" if key == "Threads" else "_kib")] = status.get(key, "")
        try:
            smaps = parse_proc(base / "smaps_rollup")
            for key in SMAPS_KEYS:
                row["smaps_" + key + "_kib"] = smaps.get(key, "")
        except OSError as exc:
            errors.append("smaps: " + str(exc))
        stat = (base / "stat").read_text()
        # Field 22 (starttime), after PID and possibly spaced parenthesized comm.
        row["proc_starttime_ticks"] = int(stat.rsplit(")", 1)[1].split()[19])
    except (OSError, ValueError, IndexError) as exc:
        errors.append("proc: " + str(exc))
    return row, errors


def wal_file_summary(path):
    # pread against one open inode survives file rename/unlink during cleanWAL.
    with path.open("rb", buffering=0) as stream:
        fd = stream.fileno()
        size = os.fstat(fd).st_size
        info = {"path": str(path), "bytes": size, "records_estimate": 0, "payload_bytes_estimate": 0}
        if not size:
            return info
        if size < WAL_FIXED_BYTES:
            raise ValueError("partial first record")
        first = os.pread(fd, WAL_HEADER.size, 0)
        if len(first) != WAL_HEADER.size:
            raise ValueError("short first header")
        first_id, first_term, first_len, _ = WAL_HEADER.unpack(first)
        footer = os.pread(fd, WAL_FOOTER.size, size - WAL_FOOTER.size)
        if len(footer) != WAL_FOOTER.size:
            raise ValueError("short final footer")
        last_len = WAL_FOOTER.unpack(footer)[0]
        if last_len < 0 or last_len + WAL_FIXED_BYTES > size or first_len < 0:
            raise ValueError("invalid or concurrently incomplete final length")
        tail = os.pread(fd, WAL_HEADER.size, size - WAL_FIXED_BYTES - last_len)
        if len(tail) != WAL_HEADER.size:
            raise ValueError("short final header")
        last_id, last_term, header_len, _ = WAL_HEADER.unpack(tail)
        count = last_id - first_id + 1
        payload = size - count * WAL_FIXED_BYTES
        if header_len != last_len or count <= 0 or payload < 0:
            raise ValueError("noncontiguous/corrupt/concurrently incomplete WAL")
        info.update(first_id=first_id, last_id=last_id, first_term=first_term, last_term=last_term,
                    records_estimate=count, payload_bytes_estimate=payload)
        return info


def sample_wal(directories, keep_details):
    row = {"wal_files": 0, "wal_bytes": 0, "wal_records_estimate": 0,
           "wal_payload_bytes_estimate": 0, "wal_parse_errors": 0, "wal_part_dirs": 0}
    errors, details, part_dirs, seen = [], [], set(), set()
    for directory in directories:
        if not Path(directory).is_dir():
            errors.append("WAL directory missing: " + directory)
            continue
        for root, _, names in os.walk(directory):
            for name in names:
                if not name.endswith(".wal"):
                    continue
                path = Path(root) / name
                canonical = str(path.resolve())
                if canonical in seen:
                    continue
                seen.add(canonical)
                row["wal_files"] += 1
                part_dirs.add(root)
                try:
                    size = path.stat().st_size
                    row["wal_bytes"] += size
                    info = wal_file_summary(path)
                    # File may have grown between stat/open; use opened snapshot.
                    row["wal_bytes"] += info["bytes"] - size
                    row["wal_records_estimate"] += info["records_estimate"]
                    row["wal_payload_bytes_estimate"] += info["payload_bytes_estimate"]
                    if keep_details:
                        details.append(info)
                except (OSError, ValueError, struct.error) as exc:
                    row["wal_parse_errors"] += 1
                    if len(errors) < 5:
                        errors.append("WAL " + str(path) + ": " + str(exc))
    row["wal_part_dirs"] = len(part_dirs)
    row["wal_empty_record_equivalents"] = row["wal_bytes"] / WAL_FIXED_BYTES
    return row, errors, details


def request_properties(endpoint, space, properties, timeout):
    # This Nebula/Proxygen handler splits the raw query value on literal commas.
    query = urllib.parse.urlencode({"space": space, "property": ",".join(properties)}, safe=",")
    url = endpoint.rstrip("/") + "/rocksdb_property?" + query
    result = {"time_utc": utc(), "url": url, "space": space, "properties": properties}
    try:
        with HTTP.open(url, timeout=timeout) as response:
            result["status"] = response.status
            result["body"] = response.read().decode("utf-8", errors="replace")
        data = json.loads(result["body"])
        if not isinstance(data, list) or len(data) != len(properties):
            raise ValueError("Expected ordered list of one engine mapping per requested property")
        values = {}
        for prop, mapping in zip(properties, data):
            if not isinstance(mapping, dict):
                raise ValueError("Expected property engine mapping")
            values[prop] = {engine: int(number) for engine, number in mapping.items()}
        result["values"] = values
    except urllib.error.HTTPError as exc:
        result["status"] = exc.code
        result["body"] = exc.read().decode("utf-8", errors="replace")
        result["error"] = str(exc)
    except (OSError, ValueError, TypeError) as exc:
        result["error"] = str(exc)
    return result


def sample_http(target, spaces, properties, timeout, pool):
    if not target.get("endpoint") or not spaces:
        return {"http_spaces_ok": 0, "http_spaces_failed": 0, "http_engines": 0}, [], []
    futures = [pool.submit(request_properties, target["endpoint"], space, properties, timeout) for space in spaces]
    responses = [future.result() for future in futures]
    numbers = {prop: [] for prop in properties}
    errors, ok, engines = [], 0, 0
    for response in responses:
        if "error" in response:
            errors.append("HTTP " + response["space"] + ": " + response["error"])
            continue
        ok += 1
        engines += len(next(iter(response["values"].values()), {}))
        for prop, values in response["values"].items():
            numbers[prop].extend(values.values())
    row = {"http_spaces_ok": ok, "http_spaces_failed": len(spaces) - ok, "http_engines": engines}
    for prop, values in numbers.items():
        row[prop] = (max(values) if prop in SHARED_PROPERTIES else sum(values)) if values else ""
    return row, errors, responses


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid-file", action="append", default=[], metavar="LABEL=PATH")
    parser.add_argument("--pid", action="append", default=[], metavar="LABEL=PID")
    parser.add_argument("--ports", default="", help="Comma-separated localhost HTTP ports, in PID declaration order")
    parser.add_argument("--endpoint", action="append", default=[], metavar="LABEL=HTTP_URL")
    parser.add_argument("--data-dir", action="append", default=[], metavar="LABEL=PATH", help="Recursively inspect *.wal; repeat for multiple data paths")
    parser.add_argument("--spaces", default="", help="Comma-separated existing space names")
    parser.add_argument("--spaces-file", type=Path)
    parser.add_argument("--properties", default=",".join(DEFAULT_PROPERTIES))
    parser.add_argument("--duration", type=float, default=600, help="Seconds; 0 samples once")
    parser.add_argument("--interval", type=float, default=10, help="Seconds between sample starts")
    parser.add_argument("--output", type=Path, required=True, help="New output directory (will not overwrite samples)")
    parser.add_argument("--http-timeout", type=float, default=3)
    parser.add_argument("--http-workers", type=int, default=8)
    parser.add_argument("--raw-every", type=int, default=6, help="Save full HTTP + WAL details on first and every N samples; errors always retained")
    args = parser.parse_args()
    if args.interval <= 0 or args.duration < 0 or args.http_workers < 1 or args.raw_every < 1:
        parser.error("interval/workers/raw-every must be positive; duration must be nonnegative")
    targets = {}
    try:
        for kind, declarations in (("pid_file", args.pid_file), ("pid", args.pid)):
            for declaration in declarations:
                label, value = labelled(declaration, "s" + str(len(targets) + 1))
                if label in targets:
                    raise ValueError("Duplicate process label: " + label)
                targets[label] = {kind: int(value) if kind == "pid" else str(Path(value).resolve()), "data_dirs": []}
        if not targets:
            raise ValueError("At least one --pid-file or --pid is required")
        ports = [p.strip() for p in args.ports.split(",") if p.strip()]
        if ports and len(ports) != len(targets):
            raise ValueError("--ports must have exactly one entry per process")
        for target, port in zip(targets.values(), ports):
            target["endpoint"] = "http://127.0.0.1:" + str(int(port))
        for declaration in args.endpoint:
            label, endpoint = labelled(declaration, next(iter(targets)) if len(targets) == 1 else None)
            targets[label]["endpoint"] = endpoint
        for declaration in args.data_dir:
            label, directory = labelled(declaration, next(iter(targets)) if len(targets) == 1 else None)
            targets[label]["data_dirs"].append(str(Path(directory).resolve()))
    except (ValueError, KeyError) as exc:
        parser.error(str(exc))
    space_text = args.spaces + " " + (args.spaces_file.read_text() if args.spaces_file else "")
    spaces = list(dict.fromkeys(filter(None, re.split(r"[,\s]+", space_text))))
    properties = list(filter(None, args.properties.split(",")))
    args.output.mkdir(parents=True, exist_ok=True)
    if any((args.output / (label + ".csv")).exists() for label in targets):
        parser.error("Output already contains samples; choose a new output directory")
    manifest = {"created_utc": utc(), "targets": targets, "spaces": spaces, "properties": properties,
                "duration_seconds": args.duration, "interval_seconds": args.interval,
                "shared_properties_aggregation": "max", "other_properties_aggregation": "sum",
                "proc_units": "KiB except Threads", "rocksdb_memory_units": "bytes",
                "wal_record_method": "first/last ID range per file, assuming contiguous entries; fixed record overhead 32 bytes",
                "proc_note": "RSS includes allocator retained pages and non-WAL allocations; not equivalent to live heap bytes"}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    columns = ["time_utc", "elapsed_seconds", "sample", "label", "pid", "proc_starttime_ticks"]
    columns += ["status_" + k + ("" if k == "Threads" else "_kib") for k in STATUS_KEYS]
    columns += ["smaps_" + k + "_kib" for k in SMAPS_KEYS]
    columns += ["wal_files", "wal_bytes", "wal_records_estimate", "wal_payload_bytes_estimate", "wal_parse_errors", "wal_part_dirs", "wal_empty_record_equivalents"]
    columns += ["http_spaces_ok", "http_spaces_failed", "http_engines"] + properties + ["sample_cost_seconds", "errors"]
    files = {label: (args.output / (label + ".csv")).open("x", newline="") for label in targets}
    writers = {label: csv.DictWriter(stream, fieldnames=columns) for label, stream in files.items()}
    for writer in writers.values():
        writer.writeheader()
    started = time.monotonic()
    index = 0
    raw_stream = (args.output / "raw-samples.jsonl").open("x")
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.http_workers) as pool:
            while True:
                now = time.monotonic()
                if index and now - started > args.duration:
                    break
                keep_raw = index % args.raw_every == 0
                for label, target in targets.items():
                    sample_started = time.monotonic()
                    row = {"time_utc": utc(), "elapsed_seconds": round(sample_started - started, 3), "sample": index, "label": label}
                    proc, errors = sample_proc(target)
                    row.update(proc)
                    wal, wal_errors, details = sample_wal(target["data_dirs"], keep_raw)
                    row.update(wal)
                    errors.extend(wal_errors)
                    http, http_errors, responses = sample_http(target, spaces, properties, args.http_timeout, pool)
                    row.update(http)
                    errors.extend(http_errors)
                    row["sample_cost_seconds"] = round(time.monotonic() - sample_started, 3)
                    row["errors"] = " | ".join(errors)
                    writers[label].writerow(row)
                    files[label].flush()
                    if keep_raw or errors:
                        raw_stream.write(json.dumps({"sample": index, "label": label, "time_utc": row["time_utc"], "http": responses,
                                                     "wal_files": details, "errors": errors}) + "\n")
                        raw_stream.flush()
                    print(json.dumps({key: row.get(key) for key in ("time_utc", "label", "pid", "status_VmRSS_kib", "wal_records_estimate", "rocksdb.cur-size-all-mem-tables", "errors")}), flush=True)
                index += 1
                if args.duration == 0:
                    break
                # Skip missed ticks instead of doing catch-up samples back to back.
                elapsed = time.monotonic() - started
                next_tick = max(index, math.floor(elapsed / args.interval) + 1) * args.interval
                if next_tick > args.duration:
                    break
                time.sleep(max(0, started + next_tick - time.monotonic()))
    except KeyboardInterrupt:
        print("Interrupted; collected samples have been flushed.", flush=True)
    finally:
        raw_stream.close()
        for stream in files.values():
            stream.close()


if __name__ == "__main__":
    main()

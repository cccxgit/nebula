#!/usr/bin/env python3
"""Streaming, auditable corpus preparation and Nebula benchmarks.

No command repeats records to manufacture a larger corpus. Query measurements
include client-to-graph round trips; imports measure acknowledged graph writes,
not index visibility. See docs/tantivy/benchmark-data.md for the protocol.
"""
import argparse
import concurrent.futures
import contextlib
import csv
import gzip
import hashlib
import io
import json
import math
import os
import queue
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request


def stamp():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_json(path, value):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def log(event, **fields):
    print(json.dumps({**fields, "time": stamp(), "event": event}, ensure_ascii=False),
          file=sys.stderr, flush=True)


def atomic_json(path, value):
    temporary = Path(str(path) + ".tmp")
    write_json(temporary, value)
    temporary.replace(path)


def fetch_segments(args, partial, metadata_path, metadata, expected):
    """Resume verified byte ranges into one file, without duplicate chunk files."""
    if not expected or metadata["remote"]["Accept-Ranges"] != "bytes":
        raise ValueError("Parallel download requires Content-Length and Accept-Ranges: bytes")
    chunk = args.chunk_mib * 1024 * 1024
    if metadata.get("chunk_bytes", chunk) != chunk:
        raise ValueError("Resume must use the original --chunk-mib")
    completed = set(metadata.get("completed_chunks", []))
    if completed and not partial.exists():
        raise ValueError("Resume metadata exists but partial data is missing")
    metadata["chunk_bytes"] = chunk
    metadata["download_mode"] = "parallel_ranges"
    remaining = expected - sum(min(chunk, expected - i * chunk) for i in completed)
    if shutil.disk_usage(partial.parent).free < remaining + 20 * 1024**3:
        raise ValueError("Download would leave less than 20 GiB free")
    descriptor = os.open(partial, os.O_CREAT | os.O_RDWR, 0o644)
    os.ftruncate(descriptor, expected)
    lock = threading.Lock()
    start_time = time.perf_counter()
    initial_bytes = expected - remaining
    def one(index):
        start, end = index * chunk, min(expected, (index + 1) * chunk) - 1
        for attempt in range(9):
            try:
                headers = {"Range": f"bytes={start}-{end}", "X-Ms-Version": "2019-12-12"}
                if metadata["remote"].get("ETag"):
                    headers["If-Range"] = metadata["remote"]["ETag"]
                request = urllib.request.Request(args.url, headers=headers)
                with urllib.request.urlopen(request, timeout=90) as response:
                    required_range = f"bytes {start}-{end}/{expected}"
                    if response.status != 206 or response.headers.get("Content-Range") != required_range:
                        raise ValueError("Server did not honor the requested byte range")
                    offset = start
                    while True:
                        block = response.read(min(1024 * 1024, end - offset + 1))
                        if not block:
                            break
                        while block:
                            written = os.pwrite(descriptor, block, offset)
                            if not written:
                                raise IOError("Failed to write download range")
                            offset += written
                            block = block[written:]
                        if offset > end:
                            break
                    if offset != end + 1:
                        raise IOError("Incomplete byte range")
                with lock:
                    completed.add(index)
                    metadata["completed_chunks"] = sorted(completed)
                    metadata["downloaded_bytes"] = sum(min(chunk, expected - i * chunk) for i in completed)
                    atomic_json(metadata_path, metadata)
                    log("download_progress", completed_chunks=len(completed),
                        total_chunks=math.ceil(expected / chunk), bytes=metadata["downloaded_bytes"],
                        bytes_per_second=(metadata["downloaded_bytes"] - initial_bytes) /
                        max(.001, time.perf_counter() - start_time))
                return
            except Exception as error:
                if attempt == 8:
                    raise
                log("download_retry", chunk=index, attempt=attempt + 1, error=str(error))
                time.sleep(min(30, 2 ** attempt))
    try:
        atomic_json(metadata_path, metadata)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.connections) as executor:
            list(executor.map(one, [i for i in range(math.ceil(expected / chunk)) if i not in completed]))
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


class PrefixReader(io.RawIOBase):
    """Expose only the verified contiguous prefix of an active sparse download."""
    def __init__(self, stream, length):
        self.stream, self.remaining = stream, length

    def readable(self):
        return True

    def readinto(self, buffer):
        data = self.stream.read(min(len(buffer), self.remaining))
        buffer[:len(data)] = data
        self.remaining -= len(data)
        return len(data)


def verified_prefix(path):
    metadata_path = Path(path.removesuffix(".partial") + ".download.json")
    metadata = json.loads(metadata_path.read_text())
    if metadata.get("download_mode") != "parallel_ranges":
        raise ValueError("Partial input requires verified parallel-range metadata")
    complete = set(metadata.get("completed_chunks", []))
    chunks = 0
    while chunks in complete:
        chunks += 1
    length = min(chunks * metadata["chunk_bytes"], int(metadata["remote"]["Content-Length"]))
    if length < 10240:
        raise ValueError("No sufficiently long contiguous verified archive prefix yet")
    return length


def source_rows(path, prefix_bytes=0):
    """Read JSONL, gzip JSONL, or tar containing gzip JSONL without extraction."""
    if path == "-":
        yield from sys.stdin
    elif path.endswith((".tar", ".tar.gz", ".tgz", ".tar.partial")):
        if path.endswith(".partial") and not prefix_bytes:
            raise ValueError("Partial archive input needs --allow-partial-download and --limit")
        with contextlib.ExitStack() as outer:
            raw = outer.enter_context(open(path, "rb"))
            bounded = io.BufferedReader(PrefixReader(raw, prefix_bytes)) if prefix_bytes else raw
            archive = outer.enter_context(tarfile.open(fileobj=bounded, mode="r|*"))
            for member in archive:
                if not member.isfile():
                    continue
                stream = archive.extractfile(member)
                with contextlib.ExitStack() as stack:
                    stack.enter_context(stream)
                    if member.name.endswith(".gz"):
                        stream = stack.enter_context(gzip.GzipFile(fileobj=stream))
                    for line in stream:
                        yield line.decode("utf-8")
    else:
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt", encoding="utf-8") as stream:
            yield from stream


def fetch(args):
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(args.url, method="HEAD")
    with urllib.request.urlopen(request, timeout=30) as response:
        remote = {k: response.headers.get(k) for k in
                  ("Content-Length", "ETag", "Last-Modified", "Accept-Ranges")}
    expected = int(remote["Content-Length"]) if remote["Content-Length"] else None
    partial = Path(str(destination) + ".partial")
    metadata_path = Path(str(destination) + ".download.json")
    previous = None
    if metadata_path.exists():
        previous = json.loads(metadata_path.read_text())
        if previous["url"] != args.url or previous["remote"].get("ETag") != remote.get("ETag"):
            raise ValueError("Source changed; choose a new output file instead of appending")
    metadata = previous or {"time": stamp(), "url": args.url, "remote": remote, "state": "partial"}
    if destination.exists():
        if not expected or destination.stat().st_size != expected:
            raise ValueError("Existing final file has unexpected size; inspect it manually")
        log("already_downloaded", path=str(destination), bytes=expected)
        return
    write_json(metadata_path, metadata)
    log("download_start", url=args.url, expected_bytes=expected)
    command = ["curl", "--fail", "--location", "--retry", "8", "--retry-delay", "3",
               "--connect-timeout", "30", "--speed-time", "120", "--speed-limit", "1024",
               "--header", "X-Ms-Version: 2019-12-12", "--continue-at", "-",
               "--output", str(partial), args.url]
    if args.connections > 1:
        fetch_segments(args, partial, metadata_path, metadata, expected)
    else:
        if metadata.get("download_mode") == "parallel_ranges":
            raise ValueError("Resume a parallel download with --connections greater than 1")
        subprocess.run(command, check=True)
    if expected is not None and partial.stat().st_size != expected:
        raise ValueError("Downloaded size differs from server Content-Length")
    digest = hashlib.sha256()
    with partial.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    partial.rename(destination)
    metadata.update(state="complete", bytes=destination.stat().st_size,
                    sha256=digest.hexdigest(), finished_at=stamp())
    write_json(metadata_path, metadata)
    log("download_complete", **metadata)


def normalize(args):
    start = time.perf_counter()
    digest = hashlib.sha256()
    count = text_bytes = truncated = empty = source_count = 0
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # Writing to a temporary file avoids mistaking interrupted output for a complete corpus.
    temp_path = Path(str(output_path) + ".partial")
    opener = gzip.open if args.output.endswith(".gz") else open
    prefix_bytes = 0
    if args.input.endswith(".partial"):
        if not args.allow_partial_download or not args.limit:
            raise ValueError("Partial download input requires --allow-partial-download and a positive --limit")
        prefix_bytes = verified_prefix(args.input)
        log("verified_download_prefix", bytes=prefix_bytes)
    with opener(temp_path, "wt", encoding="utf-8") as output:
        for line in source_rows(args.input, prefix_bytes):
            if not line.strip():
                continue
            source_count += 1
            if source_count <= args.skip:
                continue
            record = json.loads(line)
            if args.format == "msmarco":
                identifier, text = record["pid"], record["passage"]
            elif args.format == "amazon":
                identifier = f"{args.id_prefix}{source_count}"
                text = ((record.get("title") or "") + "\n" + (record.get("text") or "")).strip()
            elif args.format == "c4":
                identifier, text = f"{args.id_prefix}{source_count}", record["text"]
            else:
                identifier, text = str(record["id"]), record["text"]
            if not isinstance(text, str):
                raise ValueError(f"Text is not a string at source row {source_count}")
            if len(identifier.encode("utf-8")) > 64:
                raise ValueError(f"VID exceeds FIXED_STRING(64): {identifier}")
            was_truncated = False
            if args.max_chars and len(text) > args.max_chars:
                text = text[:args.max_chars]
                was_truncated = True
            encoded = text.encode("utf-8")
            if args.max_text_bytes and len(encoded) > args.max_text_bytes:
                text = encoded[:args.max_text_bytes].decode("utf-8", errors="ignore")
                encoded = text.encode("utf-8")
                was_truncated = True
            truncated += was_truncated
            text_bytes += len(encoded)
            empty += not bool(text)
            normalized = json.dumps({"id": identifier, "text": text},
                                    ensure_ascii=False, separators=(",", ":")) + "\n"
            digest.update(normalized.encode("utf-8"))
            output.write(normalized)
            count += 1
            if count % 100000 == 0:
                log("normalize_progress", rows=count, elapsed_s=time.perf_counter() - start)
            if args.limit and count >= args.limit:
                break
    temp_path.rename(output_path)
    report = {"time": stamp(), "kind": "corpus_preparation", "source": args.input,
              "source_format": args.format, "output": args.output, "source_rows_skipped": args.skip,
              "verified_partial_archive_prefix_bytes": prefix_bytes or None,
              "rows": count, "text_utf8_bytes": text_bytes,
              "mean_text_utf8_bytes": text_bytes / count if count else 0,
              "max_text_bytes_setting": args.max_text_bytes, "truncated_rows": truncated,
              "max_chars_setting": args.max_chars,
              "empty_text_rows": empty, "normalized_jsonl_sha256": digest.hexdigest(),
              "output_file_bytes": output_path.stat().st_size,
              "sampling": "prefix_after_skip" if args.limit or args.skip else "all_rows",
              "repeated_or_synthetic_rows_added": 0, "elapsed_s": time.perf_counter() - start}
    write_json(args.report or args.output + ".manifest.json", report)
    log("normalize_complete", **report)


def identifier(value):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", value):
        raise ValueError("Invalid nGQL identifier: " + value)
    return "`" + value + "`"


def quote(value):
    return json.dumps(value, ensure_ascii=False)


def connect(args):
    try:
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
    except ImportError as exc:
        raise RuntimeError("Install scripts/tantivy-benchmark/requirements.txt in a venv") from exc
    config = Config()
    config.max_connection_pool_size = max(4, args.concurrency + 2)
    config.timeout = args.timeout_ms
    pool = ConnectionPool()
    if not pool.init([(args.host, args.port)], config):
        raise RuntimeError("Could not connect to graphd")
    return pool


def session(pool, args):
    password = os.environ.get(args.password_env, "nebula")
    result = pool.get_session(args.user, password)
    use = result.execute("USE " + identifier(args.space))
    if not use.is_succeeded():
        result.release()
        raise RuntimeError(use.error_msg())
    return result


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def latency_summary(rows):
    ok = [r["latency_ms"] for r in rows if r["success"]]
    return {"requests": len(rows), "successes": len(ok), "failures": len(rows) - len(ok),
            "latency_success_ms": {"p50": percentile(ok, .50), "p95": percentile(ok, .95),
                                   "p99": percentile(ok, .99), "max": max(ok) if ok else None}}


def batches(args):
    items, size, read = [], 0, 0
    prefix = f"INSERT VERTEX {identifier(args.tag)}({identifier(args.field)}) VALUES "
    for line in source_rows(args.input):
        if not line.strip():
            continue
        read += 1
        if read <= args.skip:
            continue
        record = json.loads(line)
        value = f"{quote(str(record['id']))}:({quote(record['text'])})"
        if items and (len(items) >= args.batch_size or size + len(value.encode()) > args.batch_bytes):
            yield len(items), prefix + ",".join(items) + ";"
            items, size = [], 0
        items.append(value)
        size += len(value.encode())
        if args.limit and read - args.skip >= args.limit:
            break
    if items:
        yield len(items), prefix + ",".join(items) + ";"


def execute_timed(client, query, name, sequence):
    start = time.perf_counter_ns()
    try:
        result = client.execute(query)
        success = result.is_succeeded()
        error = "" if success else str(result.error_msg())
        rows = result.row_size() if success else 0
    except Exception as exc:
        success, error, rows = False, str(exc), 0
    return {"sequence": sequence, "name": name, "success": success,
            "latency_ms": (time.perf_counter_ns() - start) / 1e6,
            "result_rows": rows, "error": error}


def import_data(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Report directory is not empty; use a new run directory to preserve evidence")
    pool = connect(args)
    local = threading.local()
    clients, lock = [], threading.Lock()
    def run(index, count, statement):
        if not hasattr(local, "client"):
            local.client = session(pool, args)
            with lock:
                clients.append(local.client)
        row = execute_timed(local.client, statement, "insert", index)
        row["input_rows"] = count
        return row
    acknowledged = failed = requests = 0
    complete = False
    # A bounded queue prevents retaining millions of INSERT strings in memory.
    start = time.perf_counter()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor, \
                (output / "import-batches.jsonl").open("w", encoding="utf-8") as audit:
            pending, scheduling_error = [], None
            def observe(item):
                nonlocal requests, acknowledged, failed
                future, count, index = item
                try:
                    row = future.result()
                except Exception as error:
                    row = {"sequence": index, "name": "insert", "success": False,
                           "input_rows": count, "latency_ms": None, "error": str(error)}
                audit.write(json.dumps(row, ensure_ascii=False) + "\n")
                audit.flush()
                requests += 1
                if row["success"]:
                    acknowledged += row["input_rows"]
                else:
                    failed += row["input_rows"]
                return row
            try:
                for index, (count, statement) in enumerate(batches(args)):
                    pending.append((executor.submit(run, index, count, statement), count, index))
                    if len(pending) < args.concurrency * 2:
                        continue
                    row = observe(pending.pop(0))
                    if not row["success"]:
                        raise RuntimeError("Import failed: " + row["error"])
                    if requests % 100 == 0:
                        log("import_progress", acknowledged_rows=acknowledged,
                            elapsed_s=time.perf_counter() - start)
            except Exception as error:
                scheduling_error = error
            finally:
                # Observe every already-submitted batch even after the first failure.
                # This preserves an auditable account of concurrent acknowledgements.
                for item in pending:
                    observe(item)
            if scheduling_error:
                raise scheduling_error
            if failed:
                raise RuntimeError(f"{failed} rows were not acknowledged")
            complete = True
    finally:
        elapsed = time.perf_counter() - start
        write_json(output / "import-summary.json", {
            "time": stamp(), "kind": "measured_graph_write_acknowledgement", "input": args.input,
            "completed_without_error": complete,
            "space": args.space, "tag": args.tag, "field": args.field,
            "acknowledged_rows": acknowledged, "failed_rows_observed": failed,
            "batch_requests_observed": requests, "concurrency": args.concurrency,
            "elapsed_s": elapsed, "acknowledged_rows_per_second": acknowledged / elapsed,
            "source_rows_skipped": args.skip, "index_visibility_measured": False,
            "note": "On failure, concurrent in-flight writes may have succeeded; replay fixed VIDs safely."})
        for client in clients:
            client.release()
        pool.close()


def query_benchmark(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Report directory is not empty; use a new run directory to preserve evidence")
    queries = []
    with open(args.queries, encoding="utf-8") as source:
        for line in source:
            if not line.strip() or line.startswith("#"):
                continue
            record = json.loads(line)
            queries.append({"name": record["name"], "query": record["query"]})
    if not queries:
        raise ValueError("No queries found")
    if len({query["name"] for query in queries}) != len(queries):
        raise ValueError("Query names must be unique")
    pool = connect(args)
    clients = []
    try:
        for _ in range(args.concurrency):
            clients.append(session(pool, args))
        for repeat in range(args.warmup):
            for query in queries:
                row = execute_timed(clients[0], query["query"], query["name"], -1)
                if not row["success"]:
                    raise RuntimeError("Warmup failed: " + row["error"])
        work = [(i, q) for i, q in enumerate(queries * args.repeats)]
        work_queue = queue.Queue()
        for item in work:
            work_queue.put(item)
        barrier = threading.Barrier(args.concurrency + 1)
        def worker(worker_id):
            rows = []
            barrier.wait()
            while True:
                try:
                    sequence, query = work_queue.get_nowait()
                except queue.Empty:
                    break
                row = execute_timed(clients[worker_id], query["query"], query["name"], sequence)
                row["worker_id"] = worker_id
                rows.append(row)
                work_queue.task_done()
            return rows
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
            futures = [executor.submit(worker, i) for i in range(args.concurrency)]
            start = time.perf_counter()
            barrier.wait()
            rows = [row for future in futures for row in future.result()]
            elapsed = time.perf_counter() - start
        rows.sort(key=lambda row: row["sequence"])
        with (output / "query-latencies.csv").open("w", newline="", encoding="utf-8") as target:
            writer = csv.DictWriter(target, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        with (output / "query-latencies.jsonl").open("w", encoding="utf-8") as target:
            for row in rows:
                target.write(json.dumps(row, ensure_ascii=False) + "\n")
        report = {"time": stamp(), "kind": "measured_query_roundtrip", "label": args.label,
                  "space": args.space, "concurrency": args.concurrency,
                  "scheduling": "shared_fifo_queue_dynamic_workers",
                  "requests_by_worker": {str(i): sum(row["worker_id"] == i for row in rows)
                                         for i in range(args.concurrency)},
                  "warmup_cycles_excluded": args.warmup, "repeats": args.repeats,
                  "elapsed_s": elapsed, "attempted_qps": len(rows) / elapsed,
                  "successful_qps": sum(row["success"] for row in rows) / elapsed,
                  "percentile_method": "nearest_rank_successful_requests_only",
                  "overall": latency_summary(rows),
                  "queries": {query["name"]: {"query": query["query"], **latency_summary(
                      [row for row in rows if row["name"] == query["name"]])} for query in queries}}
        write_json(output / "query-summary.json", report)
        log("query_complete", **report)
        if report["overall"]["failures"]:
            raise RuntimeError("Some benchmark requests failed; inspect the report")
    finally:
        for client in clients:
            client.release()
        pool.close()


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    download = sub.add_parser("fetch", help="Resumable download, metadata and SHA256")
    download.add_argument("--url", required=True)
    download.add_argument("--output", required=True)
    download.add_argument("--connections", type=int, default=1)
    download.add_argument("--chunk-mib", type=int, default=32)
    download.set_defaults(func=fetch)
    prepare = sub.add_parser("normalize", help="Stream source records into {id,text} JSONL")
    prepare.add_argument("--input", required=True)
    prepare.add_argument("--output", required=True)
    prepare.add_argument("--format", choices=["msmarco", "amazon", "c4", "normalized"], required=True)
    prepare.add_argument("--id-prefix", default="review_")
    prepare.add_argument("--limit", type=int, default=0)
    prepare.add_argument("--skip", type=int, default=0)
    prepare.add_argument("--max-text-bytes", type=int, default=0,
                         help="0 preserves full text; positive limits explicitly mark truncated rows")
    prepare.add_argument("--max-chars", type=int, default=0,
                         help="0 preserves full text; otherwise retain the first N Unicode characters")
    prepare.add_argument("--report")
    prepare.add_argument("--allow-partial-download", action="store_true",
                         help="Read only completed contiguous byte ranges of our downloader; requires --limit")
    prepare.set_defaults(func=normalize)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", default="127.0.0.1")
    common.add_argument("--port", type=int, default=9669)
    common.add_argument("--user", default="root")
    common.add_argument("--password-env", default="NEBULA_PASSWORD")
    common.add_argument("--space", default="ft_bench")
    common.add_argument("--concurrency", type=int, default=1)
    common.add_argument("--timeout-ms", type=int, default=60000)
    common.add_argument("--output", required=True)
    ingest = sub.add_parser("import", parents=[common], help="Batch INSERT normalized records")
    ingest.add_argument("--input", required=True)
    ingest.add_argument("--tag", default="passage")
    ingest.add_argument("--field", default="text")
    ingest.add_argument("--batch-size", type=int, default=200)
    ingest.add_argument("--batch-bytes", type=int, default=131072)
    ingest.add_argument("--skip", type=int, default=0)
    ingest.add_argument("--limit", type=int, default=0)
    ingest.set_defaults(func=import_data)
    query = sub.add_parser("query", parents=[common], help="Measure query QPS and tail latencies")
    query.add_argument("--queries", required=True, help="JSONL containing name and query")
    query.add_argument("--repeats", type=int, default=100)
    query.add_argument("--warmup", type=int, default=3)
    query.add_argument("--label", default="tantivy")
    query.set_defaults(func=query_benchmark)
    return result


def main():
    args = parser().parse_args()
    for key in ("concurrency", "repeats", "batch_size", "batch_bytes", "timeout_ms", "connections", "chunk_mib"):
        if hasattr(args, key) and getattr(args, key) < 1:
            raise ValueError(key + " must be positive")
    for key in ("limit", "skip", "max_text_bytes", "max_chars", "warmup"):
        if hasattr(args, key) and getattr(args, key) < 0:
            raise ValueError(key + " must not be negative")
    if hasattr(args, "connections") and args.connections > 8:
        raise ValueError("connections must not exceed the local safety limit of 8")
    args.func(args)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        log("failed", error=str(error))
        sys.exit(1)

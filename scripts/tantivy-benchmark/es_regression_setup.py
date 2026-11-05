#!/usr/bin/env python3
"""Prepare a bounded ES grammar fixture, then run the existing 18-case checker.

plan is entirely offline. run requires an already running, configured baseline
and a quiet measurement window; it never starts/stops services or imports public
benchmark data. The dedicated space/index must not already exist.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import es_baseline as baseline

ROOT = Path(__file__).resolve().parent
SPACE = "ft_regression_es_v1"
INDEX = "nebula_index_ft_regression_es_v1"
FIXTURE = ROOT / "fulltext-regression-fixture.jsonl"
CASES = ROOT / "fulltext-regression-cases.jsonl"


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, default=str) + "\n")


def fixture_plan():
    fixture = [json.loads(line) for line in FIXTURE.read_text().splitlines() if line.strip()]
    cases = [json.loads(line) for line in CASES.read_text().splitlines() if line.strip()]
    assert len(fixture) == 8 and len({row["id"] for row in fixture}) == 8
    assert len(cases) == 18 and len({case["name"] for case in cases}) == 18
    ids = {row["id"] for row in fixture}
    for row in fixture:
        assert set(row) == {"id", "title", "body"}
        assert all(isinstance(value, str) for value in row.values())
    for case in cases:
        if not case["expected_error"]:
            assert set(case["expected_ids"]) <= ids
    values = [json.dumps(row["id"]) + ":(" + json.dumps(row["title"]) + "," +
              json.dumps(row["body"]) + ")" for row in fixture]
    statements = [
        f"CREATE SPACE `{SPACE}` (partition_num=20,replica_factor=3,vid_type=FIXED_STRING(64))",
        f"USE `{SPACE}`",
        "CREATE TAG ftcase(title string,body string)",
        "ADD LISTENER ELASTICSEARCH 127.0.0.1:29879",
        f'CREATE FULLTEXT TAG INDEX `{INDEX}` ON ftcase(title,body) ANALYZER="standard"',
        "INSERT VERTEX ftcase(title,body) VALUES " + ",".join(values),
        "REBUILD FULLTEXT INDEX",
    ]
    return fixture, cases, {
        "kind": "synthetic_8_row_es_grammar_setup_plan", "space": SPACE, "index": INDEX,
        "host": "127.0.0.1", "port": 29669, "partition_num": 20, "replica_factor": 3,
        "fixture_rows": len(fixture), "regression_cases": len(cases),
        "fixture_sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        "cases_sha256": hashlib.sha256(CASES.read_bytes()).hexdigest(),
        "before_rebuild_metadata_guard": "Read Listener refresh/heartbeat flags; wait max(5, 2*period+2) seconds, reject above 30. Time allowance only, not a readiness proof.",
        "statements": statements,
        "note": "Synthetic correctness fixture, not throughput data. Rebuild ensures fixture preparation; this does not measure online-write visibility. No server start/stop or benchmark-space writes.",
    }


def ensure_no_measurement():
    conflicts = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit() or int(proc.name) == os.getpid():
            continue
        try:
            argv = (proc / "cmdline").read_bytes().decode(errors="replace").split("\0")
            names = {Path(arg).name for arg in argv}
            if (("benchmark.py" in names and {"query", "import"}.intersection(argv)) or
                    ("readiness.py" in names and {"index", "verify"}.intersection(argv)) or
                    ("es_baseline.py" in names and "create-index" in argv) or
                    "regression.py" in names or "native_compare.py" in names):
                conflicts.append({"pid": int(proc.name), "scripts": sorted(names.intersection(
                    {"benchmark.py", "readiness.py", "es_baseline.py", "regression.py", "native_compare.py"}))})
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    if conflicts:
        raise RuntimeError("Active measurement/correctness clients; refusing interference: " + json.dumps(conflicts))


def check_job_rows(rows):
    """Ignore the old display-summary row, while checking its failure count."""
    states, summary = [], None
    for row in rows:
        if isinstance(row[0], str) and row[0].startswith("Total:"):
            summary = row
            if not row[2].startswith("Failed:"):
                raise ValueError("Unexpected job-summary layout")
            if int(row[2].split(":", 1)[1]):
                raise ValueError("Job summary reports failed tasks: " + str(row))
        else:
            states.append(row[2])
    if any(state in ("FAILED", "STOPPED") for state in states):
        raise ValueError("Rebuild failed: " + str(states))
    assert len(states) > 1 and all(state == "FINISHED" for state in states), states
    if summary is not None:
        assert int(summary[0].split(":", 1)[1]) == len(states) - 1
    return states


class Session:
    def __init__(self, output, wait_seconds):
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
        self.output, self.wait_seconds = output, wait_seconds
        config = Config()
        config.timeout = 30000
        config.max_connection_pool_size = 2
        self.pool = ConnectionPool()
        if not self.pool.init([("127.0.0.1", 29669)], config):
            raise RuntimeError("Baseline graph unavailable")
        self.client = self.pool.get_session("root", os.environ.get("NEBULA_PASSWORD", "nebula"))

    def record(self, event, **fields):
        row = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "event": event, **fields}
        with (self.output / "setup-events.jsonl").open("a") as target:
            target.write(json.dumps(row, ensure_ascii=False, default=str) + "\n")
        print(json.dumps(row, ensure_ascii=False, default=str), flush=True)

    def execute(self, query):
        result = self.client.execute(query)
        rows = [[value.cast() for value in result.row_values(i)] for i in range(result.row_size())] if result.is_succeeded() else []
        self.record("ngql", query=query, success=result.is_succeeded(), rows=rows,
                    error="" if result.is_succeeded() else str(result.error_msg()))
        if not result.is_succeeded():
            raise RuntimeError(str(result.error_msg()))
        return rows

    def wait(self, label, callback):
        deadline, last = time.monotonic() + self.wait_seconds, None
        while time.monotonic() < deadline:
            try:
                return callback()
            except (AssertionError, RuntimeError) as error:
                last = str(error)
                self.record("wait", label=label, reason=last)
                time.sleep(1)
        raise RuntimeError(f"Timed out waiting for {label}: {last}")

    def close(self):
        self.client.release()
        self.pool.close()


def run(args, fixture, cases, plan, output):
    report = {**plan, "setup_complete": False, "regression_complete": False, "all_passed": False}
    session = None
    try:
        ensure_no_measurement()
        runtime = Path(baseline.DEFAULT_RUNTIME)
        services = [baseline.process_state(runtime, node[0]) for node in baseline.NODES]
        es_state = baseline.elasticsearch_state(runtime)
        assert all(row["running"] for row in services) and es_state["ready"], "Baseline services not ready"
        report["service_state"] = services + [es_state]
        indices = baseline.es_request("_cat/indices?format=json")
        if any(index["index"] == INDEX for index in indices):
            raise ValueError("Dedicated ES index already exists; preserve it and inspect prior evidence")
        session = Session(output, args.wait_seconds)
        spaces = session.execute("SHOW SPACES")
        if any(row[0] == SPACE for row in spaces):
            raise ValueError("Dedicated regression space already exists; no data overwritten")
        session.execute(plan["statements"][0])
        session.wait("space metadata", lambda: session.execute(plan["statements"][1]))

        def topology():
            rows = session.execute("SHOW PARTS")
            assert len(rows) == 20
            assert {row[0] for row in rows} == set(range(1, 21))
            assert all(row[1] and len(row[2].split(",")) == 3 and not row[3] for row in rows)
        session.wait("20 partitions with three healthy replicas", topology)
        session.execute(plan["statements"][2])
        session.wait("ftcase schema", lambda: session.execute(
            'FETCH PROP ON ftcase "__readiness_probe__" YIELD id(vertex) AS vid'))
        session.execute(plan["statements"][3])

        def listeners():
            rows = session.execute("SHOW LISTENER")
            assert len(rows) == 20 and {row[0] for row in rows} == set(range(1, 21))
            assert all(row[1] == "ELASTICSEARCH" and row[2] in
                       ('"127.0.0.1":29879', '127.0.0.1:29879') and row[3] == "ONLINE" for row in rows)
        session.wait("dedicated Listener assignment", listeners)
        session.wait("fulltext schema", lambda: session.execute(plan["statements"][4]))
        settings = baseline.es_request(INDEX + "/_settings")
        mapping = baseline.es_request(INDEX + "/_mapping")
        report.update(es_settings=settings, es_mapping=mapping)
        properties = mapping[INDEX]["mappings"]["properties"]
        assert all(properties[field]["type"] == "text" and properties[field].get("analyzer") == "standard"
                   for field in ("title", "body")), "Unexpected analyzer/field mapping"
        session.execute(plan["statements"][5])
        report["metadata_time_guard_elapsed_s"] = baseline.listener_metadata_guard(runtime)
        job_id = session.execute(plan["statements"][6])[0][0]
        report["fixture_rebuild_job_id"] = job_id
        session.wait("fixture backfill job/tasks", lambda: check_job_rows(session.execute(f"SHOW JOB {job_id}")))
        expected = {row["id"]: row for row in fixture}

        def visible():
            response = baseline.es_request(INDEX + "/_search", {
                "size": 9, "track_total_hits": True, "query": {"match_all": {}}}, "POST")
            shards = response.get("_shards", {})
            assert not response.get("timed_out") and shards.get("failed") == 0
            assert shards.get("successful") == shards.get("total") and shards.get("total", 0) > 0
            assert response["hits"]["total"] == {"value": 8, "relation": "eq"}
            hits = response["hits"]["hits"]
            assert len(hits) == 8 and len({hit["_source"]["vid"] for hit in hits}) == 8
            actual = {hit["_source"]["vid"]: {"id": hit["_source"]["vid"],
                      "title": hit["_source"]["title"], "body": hit["_source"]["body"]} for hit in hits}
            assert actual == expected, "Visible ES fixture differs from input"
            graph = session.execute(f'LOOKUP ON ftcase WHERE ES_QUERY(`{INDEX}`,"*:*") '
                                    'YIELD id(vertex) AS vid | LIMIT 9')
            assert sorted(row[0] for row in graph) == sorted(expected)
            write_json(output / "visible-fixture.json", response)
        session.wait("exact eight-row fixture visible through ES and graph", visible)
        report["setup_complete"] = True
        session.close()
        session = None
        command = [sys.executable, str(ROOT / "regression.py"), "run", "--host", "127.0.0.1",
                   "--port", "29669", "--space", SPACE, "--index", INDEX,
                   "--function", "ES_QUERY", "--output", str(output / "regression.json")]
        report["regression_command"] = command
        with (output / "regression.stdout.jsonl").open("w") as stdout, (output / "regression.stderr.log").open("w") as stderr:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=180)
        report["regression_exit_code"] = completed.returncode
        result = json.loads((output / "regression.json").read_text())
        assert len(result["cases"]) == 18 and [row["name"] for row in result["cases"]] == [row["name"] for row in cases]
        report["regression_complete"] = True
        report["passed_cases"] = sum(row["passed"] for row in result["cases"])
        report["failed_case_names"] = [row["name"] for row in result["cases"] if not row["passed"]]
        # A connection failure is not evidence that a malformed query was rejected.
        syntax = next(row for row in result["cases"] if row["name"] == "syntax_error")
        report["syntax_rejection_has_parse_evidence"] = not syntax["success"] and "parse" in syntax["error"].lower()
        report["all_passed"] = (completed.returncode == 0 and result["all_passed"] and
                                report["syntax_rejection_has_parse_evidence"])
        report["note"] += " Raw regression results are retained even when expectations differ; no cases are skipped or rewritten."
    except Exception as error:
        report["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if session is not None:
            session.close()
        write_json(output / "setup-summary.json", report)
    print(json.dumps({key: report.get(key) for key in ("setup_complete", "regression_complete", "all_passed", "passed_cases", "failed_case_names", "error")}))
    return 0 if report["all_passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["plan", "run"])
    parser.add_argument("--output", required=True)
    parser.add_argument("--wait-seconds", type=int, default=120)
    args = parser.parse_args()
    if not 1 <= args.wait_seconds <= 300:
        parser.error("wait-seconds must be 1..300")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Use a fresh output directory to preserve earlier evidence")
    fixture, cases, plan = fixture_plan()
    write_json(output / "setup-plan.json", plan)
    if args.command == "plan":
        print(json.dumps({"offline_only": True, "fixture_rows": len(fixture), "cases": len(cases),
                          "plan": str(output / "setup-plan.json")}))
        return 0
    return run(args, fixture, cases, plan, output)


if __name__ == "__main__":
    sys.exit(main())

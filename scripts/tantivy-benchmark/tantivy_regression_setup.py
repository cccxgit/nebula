#!/usr/bin/env python3
"""Create a fresh 20/3 fixture space and run the existing native grammar cases.

Uses eight synthetic correctness rows, never benchmark corpus rows. Does not
start/restart services, register hosts, drop spaces, or change engine flags.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import traceback

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "tantivy-demo"))
from demo import Demo, REPO, quote


class RegressionSetup(Demo):
    def setup(self):
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool

        config = Config()
        config.max_connection_pool_size = 2
        config.timeout = 60000
        self.pool = ConnectionPool()
        if not self.pool.init([(self.args.host, self.args.port)], config):
            raise RuntimeError("Cannot connect to existing Tantivy graphd")
        self.session = self.pool.get_session("root", os.environ.get("NEBULA_PASSWORD", "nebula"))
        # Deliberately no IF NOT EXISTS: never append fixture rows to an old space.
        self.execute(f"CREATE SPACE `{self.args.space}` "
                     "(partition_num=20,replica_factor=3,vid_type=FIXED_STRING(64))")
        self.eventually(lambda: self.execute(f"USE `{self.args.space}`"), "space metadata")

        def topology():
            result = self.execute("SHOW PARTS")
            rows = [[value.cast() for value in result.row_values(i)]
                    for i in range(result.row_size())]
            assert len(rows) == 20, f"expected 20 partitions, got {len(rows)}"
            assert sorted(row[0] for row in rows) == list(range(1, 21))
            for part, leader, peers, lost in rows:
                assert leader, f"partition {part} has no leader"
                assert len(set(peers.split(","))) == 3, f"partition {part} peers: {peers}"
                assert not lost, f"partition {part} has lost replicas: {lost}"
            return rows

        self.report["topology"] = self.eventually(topology, "20 healthy replicated partitions")
        self.execute("CREATE TAG ftcase(title string,body string)")
        # CREATE resolves schema from Graph's cache; DESCRIBE alone is no barrier.
        self.eventually(lambda: self.execute(
            f"CREATE FULLTEXT TAG INDEX `{self.args.index}` "
            'ON ftcase(title,body) ANALYZER="standard"'), "fulltext schema cache")

    def run_checks(self):
        fixture_path = ROOT / "fulltext-regression-fixture.jsonl"
        cases_path = ROOT / "fulltext-regression-cases.jsonl"
        fixture = fixture_path.read_bytes()
        cases = cases_path.read_bytes()
        rows = [json.loads(line) for line in fixture.decode().splitlines()]
        expected_ids = [row["id"] for row in rows]
        assert len(expected_ids) == len(set(expected_ids)) == 8
        case_rows = [json.loads(line) for line in cases.decode().splitlines()]
        provenance = {
            "kind": "synthetic_8_row_correctness",
            "fixture_path": str(fixture_path.relative_to(REPO)),
            "fixture_sha256": hashlib.sha256(fixture).hexdigest(),
            "cases_path": str(cases_path.relative_to(REPO)),
            "cases_sha256": hashlib.sha256(cases).hexdigest(),
            "fixture_rows": 8,
            "case_count": len(case_rows),
            "case_sources": sorted({row["source"] for row in case_rows}),
            "note": "Artificial grammar regression; not public corpus or performance evidence.",
        }
        self.report["provenance"] = provenance
        (self.output / fixture_path.name).write_bytes(fixture)
        (self.output / cases_path.name).write_bytes(cases)
        (self.output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        values = ",".join(f'{quote(row["id"])}:({quote(row["title"])},{quote(row["body"])})'
                          for row in rows)
        self.eventually(lambda: self.execute("INSERT VERTEX ftcase(title,body) VALUES " + values),
                        "fixture insertion")
        self.check("all_eight_fixture_ids_visible",
                   f'LOOKUP ON ftcase WHERE TEXT_SEARCH(`{self.args.index}`,"*") '
                   "YIELD id(vertex) AS id, score() AS score | LIMIT 100",
                   expected_ids, profile=True)

        # Invoke the shared cases runner unchanged; preserve its JSON and stdout.
        result_path = self.output / "regression.json"
        command = [sys.executable, str(ROOT / "regression.py"), "run",
                   "--host", self.args.host, "--port", str(self.args.port),
                   "--space", self.args.space, "--index", self.args.index,
                   "--function", "TEXT_SEARCH", "--output", str(result_path)]
        self.event("regression_started", command=command)
        with (self.output / "regression.log").open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=max(180, self.args.timeout + 60), check=False)
        self.report["regression_exit_code"] = result.returncode
        assert result_path.exists(), "regression.py produced no report; see regression.log"
        report = json.loads(result_path.read_text())
        self.report["regression_report"] = str(result_path)
        assert fixture_path.read_bytes() == fixture and cases_path.read_bytes() == cases, \
            "fixture or cases changed during the run"
        assert result.returncode == 0 and report["all_passed"], \
            "grammar regression failed; inspect regression.json and regression.log"
        assert len(report["cases"]) == len(case_rows), "grammar runner omitted cases"
        assert [row["name"] for row in report["cases"]] == [row["name"] for row in case_rows]
        self.report["grammar_cases_passed"] = len(report["cases"])
        self.event("regression_passed", cases=len(report["cases"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", choices=["127.0.0.1"])
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--space", default="ft_regression_final")
    parser.add_argument("--index", default="ftcase_native_final")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path,
                        default=REPO / "task-logs/tantivy-benchmark/regression-tantivy-final")
    args = parser.parse_args()
    for value in (args.space, args.index):
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,100}", value):
            parser.error("space/index must be lowercase identifiers, at most 101 characters")
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("Use a fresh output directory; existing evidence will not be overwritten")
    task = RegressionSetup(args)
    task.report.update(kind="synthetic_8_row_native_grammar_setup", index=args.index,
                       host=args.host, port=args.port)
    try:
        task.setup()
        task.run_checks()
        task.report["passed"] = True
        return 0
    except Exception as error:
        task.report.update(passed=False, error=str(error), traceback=traceback.format_exc())
        task.event("regression_setup_failed", error=str(error))
        return 1
    finally:
        task.close()


if __name__ == "__main__":
    sys.exit(main())

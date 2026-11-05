#!/usr/bin/env python3
"""Execute explicit native fulltext grammar cases; never silently accept wrong hits.

The 8-row fixture is synthetic correctness data, not a public-scale benchmark.
emit prints DDL/data without executing it; run assumes that setup is ready.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parent


def ident(value):
    if not value.replace("_", "a").isalnum() or not value[0].isalpha():
        raise ValueError("Invalid identifier")
    return "`" + value + "`"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["emit", "run"])
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--space", default="ft_regression")
    parser.add_argument("--index", default="ftcase_text")
    parser.add_argument("--function", choices=["TEXT_SEARCH", "ES_QUERY"], default="TEXT_SEARCH")
    parser.add_argument("--output", default="task-logs/tantivy-benchmark/fulltext-regression.json")
    args = parser.parse_args()
    cases = [json.loads(line) for line in (ROOT / "fulltext-regression-cases.jsonl").read_text().splitlines()]
    if args.command == "emit":
        print(f"CREATE SPACE IF NOT EXISTS {ident(args.space)} (partition_num=20,replica_factor=3,vid_type=FIXED_STRING(64));")
        print("-- Wait for schema propagation before the next group.")
        print(f"USE {ident(args.space)};")
        print("CREATE TAG IF NOT EXISTS ftcase(title string,body string);")
        print("-- Wait for schema propagation. ES baseline additionally needs ADD LISTENER for this space.")
        print(f'CREATE FULLTEXT TAG INDEX {ident(args.index)} ON ftcase(title,body) ANALYZER="standard";')
        print("-- Wait for metadata propagation before INSERT, then wait for fulltext visibility.")
        for line in (ROOT / "fulltext-regression-fixture.jsonl").read_text().splitlines():
            row = json.loads(line)
            print('INSERT VERTEX ftcase(title,body) VALUES '+json.dumps(row['id'])+':('+
                  json.dumps(row['title'])+','+json.dumps(row['body'])+');')
        return
    from nebula3.Config import Config
    from nebula3.gclient.net import ConnectionPool
    config = Config()
    config.timeout = 60000
    pool = ConnectionPool()
    if not pool.init([(args.host, args.port)], config):
        raise RuntimeError("graphd connection failed")
    client = pool.get_session("root", os.environ.get("NEBULA_PASSWORD", "nebula"))
    rows = []
    try:
        result = client.execute("USE " + ident(args.space))
        if not result.is_succeeded():
            raise RuntimeError(str(result.error_msg()))
        for case in cases:
            query = ('LOOKUP ON ftcase WHERE '+args.function+'('+ident(args.index)+','+
                     json.dumps(case['query_text'])+') YIELD id(vertex) AS vid, score() AS score | LIMIT 100;')
            start = time.perf_counter()
            result = client.execute(query)
            ok = result.is_succeeded()
            actual = sorted(v.as_string() for v in result.column_values("vid")) if ok else None
            expected = sorted(case["expected_ids"]) if case["expected_ids"] is not None else None
            passed = not ok if case["expected_error"] else ok and actual == expected
            row = {**case, "ngql": query, "actual_ids": actual, "returned_rows": result.row_size() if ok else 0,
                   "success": ok, "passed": passed, "error": "" if ok else str(result.error_msg()),
                   "elapsed_ms": 1000 * (time.perf_counter() - start)}
            rows.append(row)
            print(json.dumps({"name": case['name'], "passed": passed, "actual_ids": actual,
                              "error": row['error']}), flush=True)
    finally:
        client.release()
        pool.close()
    report = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "kind": "synthetic_8_row_correctness",
              "function": args.function, "port": args.port, "fixture_rows": 8,
              "all_passed": all(row["passed"] for row in rows), "cases": rows,
              "note": "Unsupported grammar must be disclosed; successful empty/wrong results are failures, not support."}
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    if not report["all_passed"]:
        sys.exit(1)


if __name__ == "__main__":
    main()

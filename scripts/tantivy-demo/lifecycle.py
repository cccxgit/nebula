#!/usr/bin/env python3
"""Verify INT64 IDs and embedded fulltext index lifecycle in a fresh 20/3 space.

Requires an already running cluster. Creates and clears only its own unique space;
never starts, stops, or restarts database processes. Keeps the final space for inspection.
"""

import argparse
import datetime
import json
import math
import os
from pathlib import Path
import re
import sys
import time

from demo import Demo, REPO, has_tantivy_plan, quote


class TerminalJobError(Exception):
    pass


def validate_job_rows(rows):
    """Ignore SHOW JOB's textual summary row, but require every actual task to finish."""
    actual = [row for row in rows if isinstance(row[0], int)]
    assert actual, "SHOW JOB returned no job row"
    failed = [row for row in actual if row[2] in ("FAILED", "STOPPED")]
    if failed:
        raise TerminalJobError(f"REBUILD FULLTEXT INDEX failed: {failed}")
    assert actual[0][2] == "FINISHED", f"job state is {actual[0][2]}"
    assert len(actual) > 1, "rebuild job has no storage tasks"
    assert all(row[2] == "FINISHED" for row in actual[1:]), "storage tasks are still running"
    return len(actual) - 1


class Lifecycle(Demo):
    def __init__(self, args):
        super().__init__(args)
        self.report["dataset"] = "synthetic INT64 and index lifecycle regression, not performance data"
        self.tag_index = args.space + "_article"
        self.edge_index = args.space + "_cites"

    def setup(self):
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
        config = Config()
        config.max_connection_pool_size = 2
        config.timeout = 60000
        self.pool = ConnectionPool()
        if not self.pool.init([(self.args.host, self.args.port)], config):
            raise RuntimeError("Cannot connect to graphd; this script never starts database processes")
        self.session = self.pool.get_session(self.args.user,
                                             os.environ.get("NEBULA_PASSWORD", "nebula"))
        self.eventually(lambda: self.execute(
            f"CREATE SPACE `{self.args.space}` "
            "(partition_num=20, replica_factor=3, vid_type=INT64)"), "three online storage hosts")
        self.eventually(lambda: self.execute(f"USE `{self.args.space}`"), "space metadata")
        self.execute("CREATE TAG article(body string)")
        self.execute("CREATE EDGE cites(body string)")

        def topology():
            result = self.execute("SHOW PARTS")
            rows = [[value.cast() for value in result.row_values(i)]
                    for i in range(result.row_size())]
            assert len(rows) == 20, "expected 20 partitions"
            for part, leader, peers, lost in rows:
                assert leader and len(peers.split(",")) == 3 and not lost, \
                    f"partition {part} lacks a leader or three healthy replicas"
            return rows

        self.report["topology"] = self.eventually(topology, "20 healthy replicated partitions")

    def check_int(self, name, query, expected, profile=True, scores=False):
        expected = set(expected)

        def attempt():
            result = self.execute(("PROFILE " if profile else "") + query)
            values = result.column_values("id")
            assert all(value.is_int() for value in values), "INT64 identifiers changed type"
            ids = [value.as_int() for value in values]
            assert len(ids) == len(set(ids)), "replica duplicate identifiers"
            assert set(ids) == expected, \
                f"missing={sorted(expected - set(ids))}, unexpected={sorted(set(ids) - expected)}"
            if scores:
                relevance = [float(value.cast()) for value in result.column_values("relevance")]
                assert len(relevance) == len(ids)
                assert all(math.isfinite(score) and score > 0 for score in relevance)
            if profile:
                (self.output / (name + ".plan.txt")).write_text(str(result.plan_desc()))
                assert has_tantivy_plan(result.plan_desc()), "PROFILE did not select Tantivy"
            return len(ids)

        count = self.eventually(attempt, name)
        self.report["checks"].append({"name": name, "passed": True, "rows": count,
                                       "tantivy_profile_verified": profile})
        self.event("check_passed", name=name, rows=count)

    def insert_vertices(self, ids):
        values = ",".join(f"{vid}:({quote('Existing Graph 数据库')})" for vid in ids)
        self.eventually(lambda: self.execute("INSERT VERTEX article(body) VALUES " + values),
                        "vertex metadata and insertion")

    def insert_edges(self, edges):
        values = ",".join(f"{src}->{dst}@{rank}:({quote('Edge Graph 数据库')})"
                          for src, dst, rank in edges)
        self.eventually(lambda: self.execute("INSERT EDGE cites(body) VALUES " + values),
                        "edge metadata and insertion")

    def create_indexes(self):
        # DESCRIBE reads Meta, while the DDL validator relies on Graph's local schema cache.
        for statement in (
                f"CREATE FULLTEXT TAG INDEX {self.tag_index} ON article(body)",
                f"CREATE FULLTEXT EDGE INDEX {self.edge_index} ON cites(body)"):
            self.eventually(lambda q=statement: self.execute(q), "fulltext DDL metadata cache")

    def check_queries(self, phase, vertex_ids, edge_sources):
        self.check_int(phase + "_tag_contains",
                       'LOOKUP ON article WHERE article.body CONTAINS "Graph" '
                       'YIELD id(vertex) AS id', vertex_ids)
        self.check_int(phase + "_tag_text_search",
                       f'LOOKUP ON article WHERE TEXT_SEARCH({self.tag_index}, "body:graph") '
                       'YIELD id(vertex) AS id, score() AS relevance', vertex_ids, scores=True)
        self.check_int(phase + "_edge_contains",
                       'LOOKUP ON cites WHERE cites.body CONTAINS "Graph" '
                       'YIELD src(edge) AS id', edge_sources)
        self.check_int(phase + "_edge_text_search",
                       f'LOOKUP ON cites WHERE TEXT_SEARCH({self.edge_index}, "body:graph") '
                       'YIELD src(edge) AS id, score() AS relevance', edge_sources, scores=True)

    def rebuild(self):
        result = self.execute("REBUILD FULLTEXT INDEX")
        job_id = result.column_values("New Job Id")[0].as_int()
        self.report["rebuild_job_id"] = job_id

        def finished():
            result = self.execute(f"SHOW JOB {job_id}")
            rows = [[value.cast() if value.is_int() or value.is_string() else str(value)
                     for value in result.row_values(i)] for i in range(result.row_size())]
            (self.output / "rebuild-job.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2))
            self.event("rebuild_job_state", job_id=job_id, rows=rows)
            return validate_job_rows(rows)

        tasks = self.eventually(finished, "REBUILD FULLTEXT INDEX job and storage tasks")
        self.report["checks"].append({"name": "rebuild_job_finished", "passed": True,
                                       "job_id": job_id, "storage_tasks": tasks})

    def run_checks(self):
        # Positive 1..40 covers all 20 INT64 partitions; boundary values test native ID decoding.
        vertices = list(range(1, 41)) + [-7, 9007199254740993]
        edges = [(src, src % 20 + 1, 1) for src in range(1, 21)] + [(-7, 9007199254740993, 3)]
        self.report["fixture_vertex_ids"] = vertices.copy()
        self.report["fixture_edges"] = edges.copy()
        self.insert_vertices(vertices)
        self.insert_edges(edges)
        self.check_int("existing_vertices_before_index",
                       "FETCH PROP ON article " + ",".join(map(str, vertices)) +
                       " YIELD id(vertex) AS id", vertices, profile=False)
        self.check_int("existing_edges_before_index",
                       "FETCH PROP ON cites " + ",".join(f"{s}->{d}@{r}" for s, d, r in edges) +
                       " YIELD src(edge) AS id", [s for s, _, _ in edges], profile=False)

        self.create_indexes()
        self.check_queries("automatic_backfill", vertices, [s for s, _, _ in edges])
        self.rebuild()
        self.check_queries("after_rebuild", vertices, [s for s, _, _ in edges])

        self.execute(f"DROP FULLTEXT INDEX {self.tag_index}")
        self.execute(f"DROP FULLTEXT INDEX {self.edge_index}")

        def dropped():
            result = self.execute("SHOW FULLTEXT INDEXES")
            names = {value.as_string() for value in result.column_values("Name")}
            assert not names.intersection((self.tag_index, self.edge_index)), "indexes still exist"

        self.eventually(dropped, "fulltext index removal")
        # Allow independently refreshing storage metadata to observe the dropped definitions.
        self.event("metadata_settle", seconds=3, reason="observe dropped index definitions")
        time.sleep(3)
        vertices.extend([1001, 1002])
        edges.append((1001, 1002, 2))
        self.insert_vertices([1001, 1002])
        self.insert_edges([edges[-1]])
        self.create_indexes()
        self.check_queries("same_name_recreated", vertices, [s for s, _, _ in edges])

        self.execute(f"CLEAR SPACE `{self.args.space}`")
        self.eventually(lambda: self.execute(f"USE `{self.args.space}`"), "space after clear")
        self.check_queries("after_clear_empty", [], [])
        self.insert_vertices([2026, 2027])
        self.insert_edges([(2026, 2027, 1)])
        self.check_queries("after_clear_insert", [2026, 2027], [2026])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--user", default="root")
    parser.add_argument("--space", default="tantivy_lifecycle_" + datetime.datetime.now().strftime("%m%d_%H%M%S_%f"))
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,100}", args.space):
        parser.error("space must be a lowercase identifier, at most 101 characters")
    if args.output is None:
        args.output = REPO / "task-logs/tantivy-lifecycle" / args.space
    lifecycle = Lifecycle(args)
    try:
        lifecycle.setup()
        lifecycle.run_checks()
        lifecycle.report["passed"] = True
        lifecycle.event("lifecycle_passed", checks=len(lifecycle.report["checks"]))
        return 0
    except Exception as error:
        lifecycle.report["passed"] = False
        lifecycle.report["error"] = str(error)
        lifecycle.event("lifecycle_failed", error=str(error))
        return 1
    finally:
        lifecycle.close()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Verify transparent Tantivy queries on a 20-partition, three-replica local space."""

import argparse
from collections import Counter
import datetime
import json
import math
import os
from pathlib import Path
import re
import sys
import time


REPO = Path(__file__).resolve().parents[2]
TEXTS = [
    "Nebula Graph 支持原生全文检索",
    "nebula graph supports embedded search",
    "中文图数据库支持高性能查询",
    "高性能图数据库",
    "graphical patterns are substring matches",
    "Graph starts here",
    "ends with Graph",
    "literal a.b [x] a+b",
    "",
    None,
    "数据库 数据仓库",
    "Mixed CASE 中文 Graph 数据库",
]


def quote(value):
    return "NULL" if value is None else json.dumps(value, ensure_ascii=False)


def has_tantivy_plan(plan):
    """Inspect actual engine metadata, not index names or query text."""
    def decoded(value):
        return value.decode("utf-8") if isinstance(value, bytes) else value

    for node in getattr(plan, "plan_node_descs", None) or []:
        for item in getattr(node, "description", None) or []:
            key, value = decoded(item.key), decoded(item.value)
            if key == "engine" and value == "tantivy":
                return True
            if key == "indexCtx":
                contexts = json.loads(value)
                if any(context.get("engine") == "tantivy" for context in contexts):
                    return True
    return False


def fixtures():
    # MetaClient::partId treats exactly eight bytes as the uint64 VID on this host.
    # Generate twelve records per partition so missing any one partition fails the oracle.
    buckets = {part: [] for part in range(1, 21)}
    sequence = 0
    while any(len(values) < len(TEXTS) for values in buckets.values()):
        vid = f"{sequence % 10}{sequence // 10:07d}"
        sequence += 1
        part = int.from_bytes(vid.encode(), sys.byteorder) % 20 + 1
        if len(buckets[part]) < len(TEXTS):
            ordinal = len(buckets[part])
            buckets[part].append({"id": vid, "part": part, "body": TEXTS[ordinal],
                                  "title": "原生全文" if ordinal == 0 else f"article {ordinal}",
                                  "category": ordinal % 3})
    return [record for values in buckets.values() for record in values]


class Demo:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.report = {"started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       "space": args.space, "partitions": 20, "replicas": 3,
                       "dataset": "synthetic multilingual semantic regression, not performance data",
                       "checks": []}
        self.pool = None
        self.session = None

    def event(self, event, **fields):
        record = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "space": self.args.space, "event": event, **fields}
        with (self.output / "run.jsonl").open("a") as log:
            log.write(json.dumps(record, ensure_ascii=False) + "\n")
        print(json.dumps(record, ensure_ascii=False), flush=True)

    def execute(self, query):
        begin = time.perf_counter()
        result = self.session.execute(query)
        elapsed = (time.perf_counter() - begin) * 1000
        if not result.is_succeeded():
            self.event("query_failed", query=query, error=str(result.error_msg()), elapsed_ms=elapsed)
            raise RuntimeError(str(result.error_msg()))
        self.event("query", query=query, rows=result.row_size(), elapsed_ms=elapsed)
        return result

    def eventually(self, callback, description):
        deadline = time.monotonic() + self.args.timeout
        last = None
        while time.monotonic() < deadline:
            try:
                return callback()
            except (RuntimeError, AssertionError) as error:
                last = error
                time.sleep(1)
        raise RuntimeError(f"Timed out waiting for {description}: {last}")

    def check(self, name, query, expected, profile=False):
        expected = set(expected)

        def attempt():
            result = self.execute(("PROFILE " if profile else "") + query)
            rows = [value.as_string() for value in result.column_values("id")]
            actual = set(rows)
            assert len(rows) == len(actual), f"duplicate IDs: {len(rows) - len(actual)}"
            assert actual == expected, (f"missing={sorted(expected - actual)[:8]}, "
                                        f"unexpected={sorted(actual - expected)[:8]}")
            if profile:
                plan = str(result.plan_desc())
                (self.output / (name + ".plan.txt")).write_text(plan)
                assert has_tantivy_plan(result.plan_desc()), "PROFILE did not select Tantivy"
            return len(rows)

        count = self.eventually(attempt, name)
        self.report["checks"].append({"name": name, "passed": True, "rows": count,
                                       "tantivy_profile_verified": profile})
        self.event("check_passed", name=name, rows=count)

    def setup(self):
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
        config = Config()
        config.max_connection_pool_size = 4
        config.timeout = 60000
        self.pool = ConnectionPool()
        if not self.pool.init([(self.args.host, self.args.port)], config):
            raise RuntimeError("Cannot connect to graphd; start the isolated cluster first")
        self.session = self.pool.get_session(self.args.user,
                                             os.environ.get("NEBULA_PASSWORD", "nebula"))
        # Host registration is the only operation outside the fresh, uniquely named space.
        hosts = self.execute("SHOW HOSTS")
        present = {(host.as_string(), port.as_int())
                   for host, port in zip(hosts.column_values("Host"), hosts.column_values("Port"))
                   if host.is_string() and port.is_int()}
        missing = [port for port in (9779, 9789, 9799) if ("127.0.0.1", port) not in present]
        if missing:
            self.execute("ADD HOSTS " + ",".join(f'"127.0.0.1":{port}' for port in missing))

        def create_space():
            return self.execute(f"CREATE SPACE `{self.args.space}` "
                                "(partition_num=20, replica_factor=3, vid_type=FIXED_STRING(64))")

        self.eventually(create_space, "three online storage hosts")
        self.eventually(lambda: self.execute(f"USE `{self.args.space}`"), "space metadata")
        self.execute("CREATE TAG article(title string, body string, category int)")
        self.execute("CREATE EDGE cites(body string)")
        self.eventually(lambda: self.execute("DESCRIBE TAG article"), "tag metadata")
        self.eventually(lambda: self.execute("DESCRIBE EDGE cites"), "edge metadata")
        # DESCRIBE reads Meta directly; Graph's local schema cache may still lag it.
        for statement in (
                f"CREATE FULLTEXT TAG INDEX {self.args.space}_article ON article(title, body)",
                f"CREATE FULLTEXT TAG INDEX {self.args.space}_title ON article(title)",
                f"CREATE FULLTEXT EDGE INDEX {self.args.space}_cites ON cites(body)"):
            self.eventually(lambda q=statement: self.execute(q), "fulltext index schema cache")

        def topology():
            result = self.execute("SHOW PARTS")
            assert result.row_size() == 20, "expected 20 partitions"
            rows = [[value.cast() for value in result.row_values(i)]
                    for i in range(result.row_size())]
            for part, leader, peers, lost in rows:
                assert leader, f"partition {part} has no leader"
                assert len(peers.split(",")) == 3, f"partition {part} lacks three peers: {peers}"
                assert not lost, f"partition {part} has lost replicas: {lost}"
            return rows

        self.report["topology"] = self.eventually(topology, "20 healthy replicated partitions")

    def run_checks(self):
        records = fixtures()
        by_id = {record["id"]: record for record in records}
        self.report["fixture_rows"] = len(records)
        self.report["fixture_rows_by_partition"] = dict(Counter(r["part"] for r in records))
        (self.output / "fixtures.json").write_text(json.dumps(records, ensure_ascii=False, indent=2))
        for start in range(0, len(records), 40):
            chunk = records[start:start + 40]
            values = [f'{quote(r["id"])}:({quote(r["title"])},{quote(r["body"])},{r["category"]})'
                      for r in chunk]
            query = "INSERT VERTEX article(title, body, category) VALUES " + ",".join(values)
            self.eventually(lambda q=query: self.execute(q), "vertex insertion")

        def expected(predicate):
            return [r["id"] for r in by_id.values() if predicate(r)]

        def contains(record, token):
            return record["body"] is not None and token in record["body"]

        cases = [
            ("contains_case_sensitive", 'body CONTAINS "Graph"', lambda r: contains(r, "Graph")),
            ("contains_lowercase", 'body CONTAINS "graph"', lambda r: contains(r, "graph")),
            ("contains_chinese", 'body CONTAINS "数据库"', lambda r: contains(r, "数据库")),
            ("contains_one_character", 'body CONTAINS "图"', lambda r: contains(r, "图")),
            ("contains_inside_token", 'body CONTAINS "aph"', lambda r: contains(r, "aph")),
            ("contains_literal_regex_characters", 'body CONTAINS "a.b"', lambda r: contains(r, "a.b")),
            ("contains_empty", 'body CONTAINS ""', lambda r: contains(r, "")),
            ("starts_with", 'body STARTS WITH "Graph"', lambda r: r["body"] is not None and r["body"].startswith("Graph")),
            ("ends_with", 'body ENDS WITH "Graph"', lambda r: r["body"] is not None and r["body"].endswith("Graph")),
            ("multi_field", 'title CONTAINS "原生"', lambda r: "原生" in r["title"]),
        ]
        for name, condition, predicate in cases:
            self.check("lookup_" + name,
                       f"LOOKUP ON article WHERE article.{condition} YIELD id(vertex) AS id",
                       expected(predicate), profile=True)
            self.check("match_" + name,
                       f"MATCH (v:article) WHERE v.article.{condition} RETURN id(v) AS id",
                       expected(predicate), profile=True)
        self.check("lookup_and_residual",
                   'LOOKUP ON article WHERE article.body CONTAINS "Graph" '
                   'AND article.category == 2 YIELD id(vertex) AS id',
                   expected(lambda r: contains(r, "Graph") and r["category"] == 2), profile=True)
        self.check("match_and_residual",
                   'MATCH (v:article) WHERE v.article.body CONTAINS "Graph" '
                   'AND v.article.category == 2 RETURN id(v) AS id',
                   expected(lambda r: contains(r, "Graph") and r["category"] == 2), profile=True)

        # All twenty partitions contain matching vertices. A global LIMIT must cap the merge.
        limited = self.execute('MATCH (v:article) WHERE v.article.body CONTAINS "Graph" '
                               'RETURN id(v) AS id LIMIT 7')
        ids = [value.as_string() for value in limited.column_values("id")]
        assert len(ids) == len(set(ids)) == 7, "global LIMIT or replica deduplication is incorrect"
        assert set(ids).issubset(set(expected(lambda r: contains(r, "Graph"))))
        self.report["checks"].append({"name": "global_limit", "passed": True, "rows": 7})

        edge_sources = [r["id"] for r in records if r["title"] == "原生全文"]
        for i, src in enumerate(edge_sources):
            dst = edge_sources[(i + 1) % len(edge_sources)]
            self.execute(f'INSERT EDGE cites(body) VALUES {quote(src)}->{quote(dst)}@1:'
                         '("引用 Graph 数据库")')
        self.check("lookup_edge",
                   'LOOKUP ON cites WHERE cites.body CONTAINS "Graph" YIELD src(edge) AS id',
                   edge_sources, profile=True)
        self.check("match_edge",
                   'MATCH (a)-[e:cites]->(b) WHERE e.body CONTAINS "Graph" RETURN id(a) AS id',
                   edge_sources, profile=True)

        # TEXT_SEARCH uses analyzer terms/phrases/Boolean syntax, distinct from raw CONTAINS.
        index = self.args.space + "_article"
        words = lambda r: re.findall(r"[a-z]+", (r["body"] or "").lower())
        text_cases = [
            ("term", "body:graph", lambda r: "graph" in words(r)),
            ("phrase", 'body:"nebula graph"', lambda r: "nebula graph" in (r["body"] or "").lower()),
            ("boolean_and", "body:graph AND body:nebula", lambda r: "graph" in words(r) and "nebula" in words(r)),
            ("boolean_or", "body:graph OR body:patterns", lambda r: "graph" in words(r) or "patterns" in words(r)),
            ("boolean_not", "body:graph NOT body:nebula", lambda r: "graph" in words(r) and "nebula" not in words(r)),
            ("multiple_fields", "title:article AND body:graph", lambda r: "article" in r["title"] and "graph" in words(r)),
            ("chinese_phrase", 'body:"数据库"', lambda r: contains(r, "数据库")),
        ]
        for name, search, predicate in text_cases:
            self.check("text_search_" + name,
                       f"LOOKUP ON article WHERE TEXT_SEARCH({index}, {quote(search)}) "
                       "YIELD id(vertex) AS id", expected(predicate), profile=True)
        ranked_query = (f'LOOKUP ON article WHERE TEXT_SEARCH({index}, "body:graph") '
                        'YIELD id(vertex) AS id, score() AS relevance '
                        '| ORDER BY $-.relevance DESC, $-.id ASC')

        def ranked():
            result = self.execute("PROFILE " + ranked_query)
            ids = [value.as_string() for value in result.column_values("id")]
            scores = [float(value.cast()) for value in result.column_values("relevance")]
            assert set(ids) == set(expected(lambda r: "graph" in words(r)))
            assert len(ids) == len(set(ids)), "duplicate ranked results"
            assert scores and all(math.isfinite(score) and score > 0 for score in scores)
            ordered = list(zip(scores, ids))
            assert ordered == sorted(ordered, key=lambda item: (-item[0], item[1])), \
                "scores and tied IDs are not globally sorted"
            assert has_tantivy_plan(result.plan_desc()), "PROFILE did not select Tantivy"
            (self.output / "text_search_score.plan.txt").write_text(str(result.plan_desc()))
            return ids, scores

        def ranked_page():
            # Each replica's index refreshes asynchronously. Read the full ranking and
            # its page together so an initial follower catch-up does not cause a false failure.
            full_ids, full_scores = ranked()
            page = self.execute(ranked_query + " | LIMIT 7, 11")
            page_ids = [value.as_string() for value in page.column_values("id")]
            assert page_ids == full_ids[7:18], "global relevance pagination differs from full result"
            return full_ids, full_scores, page_ids

        ranked_ids, ranked_scores, page_ids = self.eventually(
            ranked_page, "TEXT_SEARCH relevance scores and global pagination")
        self.report["checks"].append({"name": "text_search_score", "passed": True,
                                       "rows": len(ranked_ids), "max_score": max(ranked_scores),
                                       "min_score": min(ranked_scores)})
        self.report["checks"].append({"name": "text_search_global_offset_limit", "passed": True,
                                       "rows": len(page_ids)})
        self.check("text_search_edge",
                   f'LOOKUP ON cites WHERE TEXT_SEARCH({self.args.space}_cites, "body:graph") '
                   'YIELD src(edge) AS id', edge_sources, profile=True)

        target = edge_sources[0]
        self.execute(f'UPDATE VERTEX ON article {quote(target)} SET body="replacement 更新"')
        by_id[target]["body"] = "replacement 更新"
        self.check("vertex_update_removes_old_terms",
                   'LOOKUP ON article WHERE article.body CONTAINS "Graph" YIELD id(vertex) AS id',
                   expected(lambda r: contains(r, "Graph")))
        self.check("vertex_update_adds_new_terms",
                   'LOOKUP ON article WHERE article.body CONTAINS "replacement" YIELD id(vertex) AS id',
                   [target])
        self.execute(f"DELETE VERTEX {quote(target)}")
        del by_id[target]
        self.check("vertex_delete",
                   'LOOKUP ON article WHERE article.body CONTAINS "replacement" YIELD id(vertex) AS id', [])
        destination = edge_sources[1]
        self.execute(f"DELETE EDGE cites {quote(target)}->{quote(destination)}@1")
        self.check("edge_delete",
                   'LOOKUP ON cites WHERE cites.body CONTAINS "Graph" YIELD src(edge) AS id',
                   edge_sources[1:])

    def close(self):
        self.report["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (self.output / "report.json").write_text(json.dumps(self.report, ensure_ascii=False, indent=2))
        # Preserve the completed validation report even if a disconnected server
        # makes session cleanup fail, and still attempt to close the connection pool.
        for name, resource, method in (("session", self.session, "release"),
                                       ("pool", self.pool, "close")):
            if resource is not None:
                try:
                    getattr(resource, method)()
                except Exception as error:
                    self.event("cleanup_failed", resource=name, error=str(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--user", default="root")
    parser.add_argument("--space", default="tantivy_demo_" + datetime.datetime.now().strftime("%m%d_%H%M%S"))
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path, default=REPO / "task-logs/tantivy-demo")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,100}", args.space):
        parser.error("space must be a lowercase identifier, at most 101 characters")
    demo = Demo(args)
    try:
        demo.setup()
        demo.run_checks()
        demo.report["passed"] = True
        demo.event("demo_passed", checks=len(demo.report["checks"]), space=args.space)
        return 0
    except Exception as error:
        demo.report["passed"] = False
        demo.report["error"] = str(error)
        demo.event("demo_failed", error=str(error))
        return 1
    finally:
        demo.close()


if __name__ == "__main__":
    sys.exit(main())

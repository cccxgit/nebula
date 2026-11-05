#!/usr/bin/env python3
"""Check bounded TEXT_SEARCH against complete ranking on a fresh 20/3 space.

Requires an already running cluster; never starts, stops, or restarts services.
Uses artificial equal-score documents for correctness, not a performance claim.
Missing-primary and embedded-NUL fallback are covered by TantivyTopKTest.cpp,
which exercises the actual production helper without corrupting primary data.
"""

import argparse
import datetime
import json
import math
from pathlib import Path
import re
import sys
import traceback

from demo import Demo, REPO, has_tantivy_plan, quote
from lifecycle import validate_job_rows


def fixture_ids(rows_per_part=32):
    buckets = {part: [] for part in range(1, 21)}
    sequence = 0
    while any(len(ids) < rows_per_part for ids in buckets.values()):
        vid = f"{sequence % 10}{sequence // 10:07d}"
        sequence += 1
        part = int.from_bytes(vid.encode(), sys.byteorder) % 20 + 1
        if len(buckets[part]) < rows_per_part:
            buckets[part].append(vid)
    ids = [vid for values in buckets.values() for vid in values]
    # Canonical prefix relationships and multi-byte UTF-8 whose bytes exceed 0x7f.
    # The standalone C++ helper also covers arbitrary 0x80/0xff bytes.
    ids.extend(["a", "aA", "aB", "aa", "é", "éa", "中", "中a", "😀", "😀a"])
    assert len(ids) == len(set(ids)) and all("\0" not in vid for vid in ids)
    return ids, {part: len(values) for part, values in buckets.items()}


def decode(value):
    return value.decode("utf-8") if isinstance(value, bytes) else value


def storage_nodes(plan):
    nodes = []
    for node in getattr(plan, "plan_node_descs", None) or []:
        for profile in getattr(node, "profiles", None) or []:
            for key, value in (getattr(profile, "other_stats", None) or {}).items():
                if not decode(key).startswith("resp["):
                    continue
                detail = json.loads(decode(value)).get("storage_detail", {})
                nodes.extend(name for name in detail if name.startswith("TantivyIndexScanNode("))
    return nodes


def fulltext_limits(plan):
    limits = []
    for node in getattr(plan, "plan_node_descs", None) or []:
        if decode(getattr(node, "name", "")) != "FulltextIndexScan":
            continue
        description = {decode(item.key): decode(item.value)
                       for item in getattr(node, "description", None) or []}
        limits.append(int(description["limit"]))
    return limits


class TopKDemo(Demo):
    def ranked(self, query, name, expected_topk):
        result = self.execute("PROFILE " + query)
        plan = result.plan_desc()
        (self.output / f"{name}.plan.txt").write_text(str(plan))
        assert has_tantivy_plan(plan), "PROFILE did not select Tantivy"
        nodes = storage_nodes(plan)
        assert nodes, "PROFILE did not expose the storage scan; bounded execution is unverified"
        assert all(re.search(rf",topk={expected_topk},fallback=false\)$", node)
                   for node in nodes), f"unexpected storage TopK/fallback: {nodes}"
        ids = [value.as_string() for value in result.column_values("id")]
        scores = [float(value.cast()) for value in result.column_values("relevance")]
        assert len(ids) == len(set(ids)), "duplicate identifiers"
        assert all(math.isfinite(score) for score in scores), "non-finite score"
        return list(zip(ids, scores)), fulltext_limits(plan), nodes

    def run_checks(self):
        ids, distribution = fixture_ids(self.args.rows_per_part)
        self.report["dataset"] = "synthetic equal-score TopK semantic regression, not performance data"
        self.report["fixture_rows"] = len(ids)
        self.report["known_fixture_rows_by_partition"] = distribution
        self.report["extra_utf8_and_prefix_rows"] = 10
        (self.output / "fixture-ids.json").write_text(json.dumps(ids, ensure_ascii=False, indent=2))
        for begin in range(0, len(ids), 100):
            values = ",".join(f'{quote(vid)}:("constant", "rankingtie", 1)'
                              for vid in ids[begin:begin + 100])
            self.eventually(lambda q="INSERT VERTEX article(title, body, category) VALUES " + values:
                            self.execute(q), "fixture insertion")

        # Rebuild is also a barrier for all three replica-local BM25 corpora.
        response = self.execute("REBUILD FULLTEXT INDEX")
        job_id = response.column_values("New Job Id")[0].as_int()

        def finished():
            result = self.execute(f"SHOW JOB {job_id}")
            rows = [[value.cast() if value.is_int() or value.is_string() else str(value)
                     for value in result.row_values(i)] for i in range(result.row_size())]
            (self.output / "rebuild-job.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2))
            assert validate_job_rows(rows) == 3, "expected one completed task per storage process"

        self.eventually(finished, "all three replica indexes rebuilt")
        # AllQuery gives exactly 1.0 per document. Identical BM25 text need not
        # give identical scores across replicas with different deleted-doc
        # segment statistics, even when the live document sets are identical.
        base = (f'LOOKUP ON article WHERE TEXT_SEARCH({self.args.space}_article, "*") '
                'YIELD id(vertex) AS id, score() AS relevance')
        ordered = base + " | ORDER BY $-.relevance DESC, $-.id ASC"
        full, limits, _ = self.eventually(lambda: self.ranked(ordered, "full_ranking", 0),
                                          "complete fulltext ranking")
        assert limits == [-1], f"complete reference was unexpectedly bounded: {limits}"
        assert [vid for vid, _ in full] == sorted(ids, key=lambda vid: vid.encode("utf-8")), \
            "constant-score reference differs from the complete VID byte ordering"
        assert len({score for _, score in full}) == 1, "fixture scores are not all tied"
        assert full and full[0][1] > 0, "expected positive BM25 scores"
        (self.output / "full-ranking.json").write_text(json.dumps(full, ensure_ascii=False, indent=2))
        self.report["checks"].append({"name": "complete_equal_score_reference", "passed": True,
                                       "rows": len(full)})

        pages = [(0, 1), (7, 11), (39, 37), (201, 53), (len(ids) - 7, 30), (len(ids) + 2, 3)]
        for offset, count in pages:
            name = f"page_{offset}_{count}"
            end = offset + count
            page, limits, nodes = self.ranked(base + f" | LIMIT {offset}, {count}", name, end)
            assert limits == [end], f"Graph did not propagate offset+count: {limits}"
            assert page == full[offset:end], f"{name} differs from full sorted reference"
            self.report["checks"].append({"name": name, "passed": True, "rows": len(page),
                                           "partition_topk": end, "storage_nodes": nodes})

        # A user-provided order remains on the complete path. This scope does not
        # introduce a new TopN optimizer rule, even for score DESC plus ID ASC.
        ordered_page, limits, _ = self.ranked(ordered + " | LIMIT 7, 11", "explicit_order", 0)
        assert ordered_page == full[7:18] and limits == [-1], \
            "explicit ORDER BY pagination differs from the complete reference"
        self.report["checks"].append({"name": "explicit_order_remains_complete", "passed": True})

        # Also exercise non-constant BM25 scores against their own complete
        # ranking, without assuming corpus statistics are bit-identical.
        bm25 = base.replace(', "*")', ', "body:rankingtie")')
        bm25_full, _, _ = self.ranked(
            bm25 + " | ORDER BY $-.relevance DESC, $-.id ASC", "bm25_full_ranking", 0)
        assert {vid for vid, _ in bm25_full} == set(ids), "BM25 reference has missing/unexpected IDs"
        bm25_page, limits, nodes = self.ranked(bm25 + " | LIMIT 7, 31", "bm25_page_7_31", 38)
        assert limits == [38] and bm25_page == bm25_full[7:38], \
            "bounded BM25 pagination differs from complete score/VID ranking"
        self.report["checks"].append({"name": "bm25_page_matches_complete_ranking", "passed": True,
                                       "rows": len(bm25_page), "storage_nodes": nodes,
                                       "distinct_scores": len({score for _, score in bm25_full})})

        result = self.execute('PROFILE LOOKUP ON article WHERE article.body CONTAINS "rank" '
                              'YIELD id(vertex) AS id | LIMIT 11')
        nodes = storage_nodes(result.plan_desc())
        (self.output / "contains.plan.txt").write_text(str(result.plan_desc()))
        assert result.row_size() == 11 and nodes, "CONTAINS limit or storage profile is missing"
        assert all(",mode=1,topk=0," in node for node in nodes), nodes
        self.report["checks"].append({"name": "contains_remains_complete", "passed": True,
                                       "storage_nodes": nodes})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--user", default="root")
    parser.add_argument("--space", default="tantivy_topk_" + datetime.datetime.now().strftime("%m%d_%H%M%S"))
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--rows-per-part", type=int, default=32)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,100}", args.space):
        parser.error("space must be a lowercase identifier, at most 101 characters")
    if not 16 <= args.rows_per_part <= 500:
        parser.error("rows-per-part must be between 16 and 500")
    if args.output is None:
        args.output = REPO / "task-logs/tantivy-topk" / args.space
    demo = TopKDemo(args)
    try:
        demo.setup()
        demo.run_checks()
        demo.report["passed"] = True
        demo.event("topk_passed", checks=len(demo.report["checks"]))
        return 0
    except Exception as error:
        demo.report["passed"] = False
        demo.report["error"] = str(error) or type(error).__name__
        demo.report["traceback"] = traceback.format_exc()
        demo.event("topk_failed", error=demo.report["error"])
        return 1
    finally:
        demo.close()


if __name__ == "__main__":
    sys.exit(main())

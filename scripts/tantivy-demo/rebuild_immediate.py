#!/usr/bin/env python3
"""Verify CREATE FULLTEXT immediately followed by REBUILD on an existing 20/3 cluster.

The first candidate reads after FINISHED go directly to storage, so Graph's separate
metadata cache cannot hide premature job completion. No candidate-read retries occur.
"""

import argparse
from collections import defaultdict
import datetime
import json
from pathlib import Path
import re
import sys
import time
import traceback

from nebula3.common.ttypes import SchemaID
from nebula3.fbthrift.Thrift import TType
from nebula3.fbthrift.protocol import THeaderProtocol
from nebula3.fbthrift.transport import THeaderTransport, TSocket, TTransport
from nebula3.storage import GraphStorageService
from nebula3.storage.ttypes import IndexQueryContext, IndexSpec, LookupIndexRequest, RequestCommon
from nebula3.meta import MetaService
from nebula3.meta.ttypes import ListTagsReq

from demo import REPO
from lifecycle import Lifecycle, validate_job_rows


class NativeQueryContext(IndexQueryContext):
    """Serialize the four optional fields added in this repository's storage.thrift.

    nebula3-python's generated bindings predate those fields. Use its nonaccelerated
    protocol and existing service client; do not modify the installed package.
    """
    thrift_spec = None

    def __init__(self, name):
        super().__init__()
        self.name = name

    def write(self, protocol):
        protocol.writeStructBegin("IndexQueryContext")
        for number, name, value in ((4, "fulltext_index_name", self.name),
                                     (5, "fulltext_field", ""),
                                     (6, "fulltext_value", "body:graph")):
            protocol.writeFieldBegin(name, TType.STRING, number)
            protocol.writeString(value.encode("utf-8"))
            protocol.writeFieldEnd()
        protocol.writeFieldBegin("fulltext_mode", TType.I32, 7)
        protocol.writeI32(0)
        protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()


class ImmediateRebuild(Lifecycle):
    def run_checks(self):
        vertices = list(range(1, self.args.rows + 1))
        for start in range(0, len(vertices), 100):
            self.insert_vertices(vertices[start:start + 100])
        self.check_int("primary_rows_exist_before_create",
                       "FETCH PROP ON article " + ",".join(map(str, vertices)) +
                       " YIELD id(vertex) AS id", vertices, profile=False)
        space_id = self.execute(f"DESCRIBE SPACE `{self.args.space}`").column_values("ID")[0].as_int()
        # SHOW TAGS exposes names only in this branch. Obtain the actual schema
        # ID from Meta before CREATE; no FT definition exists or is awaited yet.
        socket = TSocket.TSocket(self.args.host, self.args.meta_port)
        socket.setTimeout(60000)
        transport = THeaderTransport.THeaderTransport(TTransport.TBufferedTransport(socket))
        protocol = THeaderProtocol.THeaderProtocol(transport)
        transport.open()
        try:
            tags = MetaService.Client(protocol).listTags(ListTagsReq(space_id=space_id))
            assert tags.code == 0, f"Meta listTags failed: {tags.code}"
            tag_id = next(tag.tag_id for tag in tags.tags
                          if tag.tag_name in ("article", b"article"))
            self.report["tag_id"] = tag_id
        finally:
            transport.close()
        leaders = defaultdict(list)
        for part, leader, _, _ in self.report["topology"]:
            host, port = leader.rsplit(":", 1)
            leaders[(host, int(port))].append(part)
        self.report["storage_partitions"] = {f"{host}:{port}": parts
                                              for (host, port), parts in leaders.items()}
        assert sorted(part for parts in leaders.values() for part in parts) == list(range(1, 21))
        connections = []
        try:
            # Establish connections and requests before CREATE, keeping the post-job
            # validation path free from connection setup or Graph cache waits.
            for address, parts in leaders.items():
                socket = TSocket.TSocket(*address)
                socket.setTimeout(60000)
                transport = THeaderTransport.THeaderTransport(TTransport.TBufferedTransport(socket))
                protocol = THeaderProtocol.THeaderProtocol(transport)
                transport.open()
                request = LookupIndexRequest(
                    space_id=space_id, parts=parts,
                    indices=IndexSpec(contexts=[NativeQueryContext(self.tag_index)],
                                      schema_id=SchemaID(tag_id=tag_id)),
                    return_columns=[b"_vid", b"_score"],
                    common=RequestCommon(session_id=0, plan_id=0, profile_detail=False),
                    limit=(1 << 63) - 1)
                connections.append((address, transport, GraphStorageService.Client(protocol), request))

            # Only CREATE is retried for schema cache propagation. After it succeeds,
            # submit REBUILD immediately, without querying or waiting for the FT cache.
            self.eventually(lambda: self.execute(
                f"CREATE FULLTEXT TAG INDEX {self.tag_index} ON article(body)"), "CREATE metadata")
            created_at = time.perf_counter()
            result = self.execute("REBUILD FULLTEXT INDEX")
            self.report["create_to_rebuild_response_ms"] = (time.perf_counter() - created_at) * 1000
            job_id = result.column_values("New Job Id")[0].as_int()
            self.report["rebuild_job_id"] = job_id
            deadline = time.monotonic() + self.args.timeout
            while True:
                result = self.execute(f"SHOW JOB {job_id}")
                rows = [[value.cast() if value.is_int() or value.is_string() else str(value)
                         for value in result.row_values(i)] for i in range(result.row_size())]
                try:
                    tasks = validate_job_rows(rows)
                except AssertionError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"Job {job_id} did not finish: {rows}")
                    time.sleep(0.1)
                    continue
                assert tasks == 3, f"expected exactly three completed storage tasks, got {tasks}"
                actual = [row for row in rows if isinstance(row[0], int)]
                assert all(row[5] == "SUCCEEDED" for row in actual), f"job/task error codes: {rows}"
                assert len({row[0] for row in actual[1:]}) == 3, "duplicate storage task IDs"
                finished_at = time.perf_counter()
                self.report["job_rows"] = rows
                break

            ids = []
            reads = []
            for address, _, client, request in connections:
                started = time.perf_counter()
                response = client.lookupIndex(request)
                errors = [(item.part_id, item.code) for item in response.result.failed_parts or []]
                assert not errors, f"job FINISHED but storage {address} failed: {errors}"
                assert response.data is not None, f"storage {address} returned no data"
                local_ids = [row.values[0].get_iVal() for row in response.data.rows]
                assert all(isinstance(vid, int) for vid in local_ids), "INT64 IDs changed type"
                ids.extend(local_ids)
                reads.append({"host": address[0], "port": address[1], "parts": request.parts,
                              "rows": len(local_ids), "ids": local_ids,
                              "after_finished_ms": (started - finished_at) * 1000,
                              "elapsed_ms": (time.perf_counter() - started) * 1000})
            self.report["immediate_storage_reads"] = reads
            (self.output / "immediate-storage-reads.json").write_text(
                json.dumps(reads, ensure_ascii=False, indent=2))
            assert len(ids) == len(set(ids)), "duplicate candidates across partitions"
            assert set(ids) == set(vertices), \
                f"job FINISHED with missing={sorted(set(vertices) - set(ids))}, unexpected={sorted(set(ids) - set(vertices))}"
            self.report["checks"].append({"name": "finished_implies_all_storage_candidates_ready",
                                           "passed": True, "rows": len(ids), "storage_tasks": tasks,
                                           "candidate_read_retries": 0})
            self.event("immediate_storage_check_passed", rows=len(ids), storage_tasks=tasks)
            # Graph may still be learning the new FT definition. This later check is
            # separate from the strict, no-retry storage assertion above.
            self.check_int("graph_text_search_after_metadata_propagation",
                           f'LOOKUP ON article WHERE TEXT_SEARCH({self.tag_index}, "body:graph") '
                           "YIELD id(vertex) AS id", vertices)
        finally:
            for _, transport, _, _ in connections:
                transport.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9669)
    parser.add_argument("--meta-port", type=int, default=9559)
    parser.add_argument("--user", default="root")
    parser.add_argument("--space", default="tantivy_immediate_" + datetime.datetime.now().strftime("%m%d_%H%M%S_%f"))
    parser.add_argument("--rows", type=int, default=200)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,100}", args.space):
        parser.error("space must be a lowercase identifier, at most 101 characters")
    if args.rows < 20 or args.rows % 20:
        parser.error("rows must be a positive multiple of 20")
    if args.output is None:
        args.output = REPO / "task-logs/tantivy-rebuild-immediate" / args.space
    runner = ImmediateRebuild(args)
    try:
        runner.setup()
        runner.run_checks()
        runner.report["passed"] = True
        runner.event("immediate_rebuild_passed", checks=len(runner.report["checks"]))
        return 0
    except Exception as error:
        runner.report["passed"] = False
        runner.report["error"] = str(error) or type(error).__name__
        runner.report["traceback"] = traceback.format_exc()
        runner.event("immediate_rebuild_failed", error=runner.report["error"])
        return 1
    finally:
        runner.close()


if __name__ == "__main__":
    sys.exit(main())

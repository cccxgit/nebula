#!/usr/bin/env python3
"""Prepare a fresh benchmark space and verify complete query hit sets.

oracle is offline; all other commands require an already running database.
No command starts services, changes graph flags, or deletes existing data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import benchmark as bench

PREDICATES = [
    ('contains_water', 'CONTAINS', 'water', lambda text: 'water' in text),
    ('contains_new_york', 'CONTAINS', 'New York', lambda text: 'New York' in text),
    ('starts_with_the_united', 'STARTS WITH', 'The United', lambda text: text.startswith('The United')),
    ('ends_with_water', 'ENDS WITH', 'water.', lambda text: text.endswith('water.')),
]


def oracle(args):
    rows, seen = 0, set()
    found = {name: [] for name, *_ in PREDICATES}
    digest = hashlib.sha256()
    for raw in bench.source_rows(args.input):
        if not raw.strip():
            continue
        row = json.loads(raw)
        vid, text = str(row['id']), row['text']
        if vid in seen:
            raise ValueError('Duplicate source VID: ' + vid)
        seen.add(vid)
        rows += 1
        digest.update(raw.encode('utf-8'))
        for name, _, _, predicate in PREDICATES:
            if predicate(text):
                found[name].append(vid)
                if len(found[name]) > args.max_query_hits:
                    raise ValueError(name + ': hit set exceeds the safety limit; choose a narrower query')
    if rows != args.expect_rows:
        raise ValueError(f'Expected {args.expect_rows} source rows, observed {rows}')
    report = {'utc': bench.stamp(), 'kind': 'offline_source_oracle', 'input': args.input,
              'rows': rows, 'unique_ids': len(seen), 'jsonl_sha256': digest.hexdigest(),
              'max_query_hits': args.max_query_hits,
              'queries': [{'name': name, 'query': f'MATCH (v:passage) WHERE v.passage.text {op} '
                          f'{json.dumps(value)} RETURN id(v) AS vid',
                          'expected_ids': sorted(found[name]), 'expected_hits': len(found[name])}
                         for name, op, value, _ in PREDICATES],
              'note': 'Complete ID sets for these predicates only; this does not prove every index term.'}
    bench.write_json(args.output, report)
    print(json.dumps({k: v for k, v in report.items() if k != 'queries'}))
    print(json.dumps({x['name']: x['expected_hits'] for x in report['queries']}))


class Runner:
    def __init__(self, args):
        self.args = args
        self.directory = Path(args.output)
        self.directory.mkdir(parents=True, exist_ok=True)
        if any(self.directory.iterdir()):
            raise ValueError('Use a fresh output directory to preserve earlier evidence')
        self.pool = bench.connect(args)
        self.client = self.pool.get_session(args.user, os.environ.get(args.password_env, 'nebula'))

    def record(self, event, **fields):
        row = {'utc': bench.stamp(), 'event': event, **fields}
        with (self.directory / 'events.jsonl').open('a') as target:
            target.write(json.dumps(row, ensure_ascii=False, default=str) + '\n')
        print(json.dumps(row, ensure_ascii=False, default=str), flush=True)

    def execute(self, query):
        start = time.perf_counter()
        result = self.client.execute(query)
        ok = result.is_succeeded()
        self.record('query', query=query, success=ok, rows=result.row_size() if ok else 0,
                    elapsed_s=time.perf_counter() - start,
                    error='' if ok else str(result.error_msg()))
        if not ok:
            raise RuntimeError(str(result.error_msg()))
        return result

    def eventually(self, callback, label):
        deadline, last = time.monotonic() + self.args.wait_seconds, None
        while time.monotonic() < deadline:
            try:
                return callback()
            except (RuntimeError, AssertionError) as error:
                last = str(error)
                self.record('waiting', label=label, reason=last)
                time.sleep(2)
        raise RuntimeError(f'Timed out waiting for {label}: {last}')

    def topology(self):
        result = self.execute('SHOW PARTS')
        assert result.row_size() == 20, 'Expected 20 partitions'
        rows = [[value.cast() for value in result.row_values(i)] for i in range(result.row_size())]
        for part, leader, peers, lost in rows:
            assert leader and len(peers.split(',')) == 3 and not lost, f'Unhealthy partition: {part}'
        self.record('topology_healthy', partitions=20, replicas=3, details=rows)

    def prepare(self):
        # No IF NOT EXISTS: accidentally reusing a populated space must fail.
        self.execute(f'CREATE SPACE {bench.identifier(self.args.space)} '
                     '(partition_num=20,replica_factor=3,vid_type=FIXED_STRING(64))')
        self.eventually(lambda: self.execute('USE ' + bench.identifier(self.args.space)), 'space propagation')
        self.eventually(self.topology, '20 healthy partitions and three replicas')
        self.execute('CREATE TAG passage(text string)')
        self.eventually(lambda: self.execute('DESCRIBE TAG passage'), 'tag propagation')
        self.eventually(lambda: self.execute('CREATE TAG INDEX passage_tag_lookup ON passage()'),
                        'graph local tag schema cache')
        self.eventually(lambda: self.execute('DESCRIBE TAG INDEX passage_tag_lookup'), 'tag index propagation')
        self.eventually(lambda: self.execute('FETCH PROP ON passage "__readiness_probe__" '
                                            'YIELD id(vertex) AS vid'), 'storage schema readiness')
        self.record('schema_ready', space=self.args.space, fulltext_created=False)

    def job(self, statement):
        start = time.perf_counter()
        result = self.execute(statement)
        job_id = result.row_values(0)[0].as_int()
        def poll():
            status = self.execute(f'SHOW JOB {job_id}')
            # The final summary row says Failed:N; only actual job/task statuses count.
            statuses = [v.as_string() for v in status.column_values('Status')]
            task_statuses = [s for s in statuses if not s.startswith('Failed:')]
            if any(s in ('FAILED', 'STOPPED') for s in task_statuses):
                raise ValueError(f'Job {job_id} failed: {task_statuses}')
            assert len(task_statuses) > 1 and all(s == 'FINISHED' for s in task_statuses), task_statuses
            self.record('job_finished', job_id=job_id, statuses=task_statuses,
                        elapsed_s=time.perf_counter() - start)
        self.eventually(poll, f'job {job_id} completion')

    def index(self):
        self.eventually(self.topology, 'healthy topology')
        self.job('REBUILD TAG INDEX passage_tag_lookup')
        start = time.perf_counter()
        self.eventually(lambda: self.execute(
            f'CREATE FULLTEXT TAG INDEX {bench.identifier(self.args.index)} '
            'ON passage(text) ANALYZER="standard"'), 'graph schema cache for fulltext DDL')
        self.eventually(lambda: self.fulltext_present(), 'fulltext metadata propagation')
        # Auto backfill starts on CREATE. A completed rebuild explicitly fences every storage task.
        self.job('REBUILD FULLTEXT INDEX')
        self.record('index_build_jobs_finished', elapsed_s=time.perf_counter() - start,
                    note='Run verify next; job completion and query-set visibility are separate evidence.')

    def fulltext_present(self):
        result = self.execute('SHOW FULLTEXT INDEXES')
        values = [str(v.cast()) for i in range(result.row_size()) for v in result.row_values(i)]
        assert self.args.index in values, 'Fulltext index has not appeared in graph metadata'

    def verify(self):
        source = json.loads(Path(self.args.oracle).read_text())
        self.eventually(self.topology, 'healthy topology')
        checks, start = [], time.perf_counter()
        for query in source['queries']:
            expected = set(query['expected_ids'])
            if len(expected) > self.args.max_query_hits:
                raise ValueError('Oracle exceeds max-query-hits safety bound')
            def check():
                result = self.execute('PROFILE ' + query['query'])
                actual_rows = [v.as_string() for v in result.column_values('vid')]
                actual = set(actual_rows)
                assert len(actual_rows) == len(actual), 'Duplicate returned VID'
                assert actual == expected, f'missing={len(expected - actual)},unexpected={len(actual - expected)}'
                plan = result.plan_desc()
                selected = False
                for node in getattr(plan, 'plan_node_descs', None) or []:
                    for item in getattr(node, 'description', None) or []:
                        key = item.key.decode() if isinstance(item.key, bytes) else item.key
                        value = item.value.decode() if isinstance(item.value, bytes) else item.value
                        if key == 'engine' and value == 'tantivy':
                            selected = True
                        if key == 'indexCtx' and any(x.get('engine') == 'tantivy' for x in json.loads(value)):
                            selected = True
                if self.args.expect_pushdown != 'any':
                    assert selected == (self.args.expect_pushdown == 'on'), f'Unexpected Tantivy plan: {selected}'
                (self.directory / (query['name'] + '.plan.txt')).write_text(str(plan))
                return {'name': query['name'], 'expected_hits': len(expected), 'actual_hits': len(actual),
                        'passed': True, 'tantivy_plan': selected}
            checks.append(self.eventually(check, query['name'] + ' complete hit set'))
        bench.write_json(self.directory / 'visibility-summary.json',
                         {'utc': bench.stamp(), 'space': self.args.space, 'source_rows': source['rows'],
                          'source_jsonl_sha256': source['jsonl_sha256'], 'all_checks_passed': True,
                          'query_set_visible_seconds_since_verification_start': time.perf_counter() - start,
                          'checks': checks, 'note': 'Not import-to-visible time; combine import/index timestamps.'})

    def run(self):
        try:
            if self.args.command != 'prepare':
                self.execute('USE ' + bench.identifier(self.args.space))
            getattr(self, self.args.command)()
        finally:
            self.client.release()
            self.pool.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['oracle', 'prepare', 'index', 'verify'])
    parser.add_argument('--input')
    parser.add_argument('--expect-rows', type=int)
    parser.add_argument('--oracle')
    parser.add_argument('--output', required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9669)
    parser.add_argument('--user', default='root')
    parser.add_argument('--password-env', default='NEBULA_PASSWORD')
    parser.add_argument('--space', default='ft_bench')
    parser.add_argument('--index', default='ft_passage_text')
    parser.add_argument('--timeout-ms', type=int, default=60000)
    parser.add_argument('--wait-seconds', type=int, default=600)
    parser.add_argument('--max-query-hits', type=int, default=100000)
    parser.add_argument('--expect-pushdown', choices=['on', 'off', 'any'], default='on')
    args = parser.parse_args()
    args.concurrency = 1
    if args.command == 'oracle':
        if not args.input or not args.expect_rows or Path(args.output).exists():
            parser.error('oracle needs input, positive expect-rows and a fresh output path')
        oracle(args)
    elif args.command == 'verify' and not args.oracle:
        parser.error('verify needs --oracle')
    else:
        Runner(args).run()


if __name__ == '__main__':
    main()

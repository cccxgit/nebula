#!/usr/bin/env python3
"""Small synthetic framing tests; never opens a database data directory."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('parse_wal', HERE / 'parse_wal.py')
wal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wal)
ROOT = HERE / 'wal-parser-selftest'
ROOT.mkdir(exist_ok=True)


def record(log_id, term=1, payload=b'', cluster=7, footer=None):
    length = len(payload)
    return struct.pack('<qqiq', log_id, term, length, cluster) + payload + struct.pack('<i', length if footer is None else footer)


def fixture(case, name, content):
    base = ROOT / case
    path = base / 'nebula' / '4' / 'wal' / '2' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return base, path


def check(name, condition, **details):
    if not condition:
        raise AssertionError((name, details))
    print(json.dumps({'test': name, 'result': 'PASS', **details}))


base, _ = fixture('good', '1.wal', record(1) + record(2, payload=b'abc'))
fixture('good', '3.wal', record(3, term=2) + record(4, term=2, payload=b'x' * (1024 * 1024 + 17)))
report = wal.scan([('s1', base)])
check('all-records-and-rollover', not report['has_anomalies'] and report['total']['records'] == 4
      and report['total']['empty_records'] == 2 and report['total']['nonempty_records'] == 2
      and report['total']['payload_bytes'] == 1024 * 1024 + 20
      and report['total']['snapshot_bytes'] == 4 * 32 + 1024 * 1024 + 20
      and report['partitions'][0]['first_id'] == 1 and report['partitions'][0]['last_id'] == 4,
      records=report['total']['records'], payload_bytes=report['total']['payload_bytes'])

base, path = fixture('short-header', '1.wal', record(1) + b'123')
result = wal.parse_file('s1', base, path)
check('incomplete-tail-header', result['status'] == 'incomplete_tail' and result['records'] == 1
      and result['unvalidated_bytes'] == 3, status=result['status'])
base, path = fixture('short-record', '1.wal', record(1) + record(2, payload=b'abc')[:-2])
result = wal.parse_file('s1', base, path)
check('incomplete-tail-footer', result['status'] == 'incomplete_tail' and result['records'] == 1,
      issue=result['issues'][0]['kind'])
base, path = fixture('wrong-footer', '1.wal', record(1, payload=b'abc', footer=8))
result = wal.parse_file('s1', base, path)
check('footer-validation', result['status'] == 'invalid_record' and result['records'] == 0,
      issue=result['issues'][0]['kind'])
base, path = fixture('negative', '1.wal', struct.pack('<qqiq', 1, 1, -1, 7) + struct.pack('<i', -1))
result = wal.parse_file('s1', base, path)
check('negative-length', result['status'] == 'invalid_record', issue=result['issues'][0]['kind'])
base, _ = fixture('gaps', '1.wal', record(1, 3) + record(3, 2) + record(3, 2))
fixture('gaps', '8.wal', record(8, 1))
report = wal.scan([('s1', base)])
check('within-and-across-file-id-and-term-checks', report['has_anomalies']
      and report['total']['id_gap_count'] == 2 and report['total']['id_missing_count'] == 5
      and report['total']['id_nonincreasing_count'] == 1 and report['total']['term_regressions'] == 2,
      gaps=report['total']['id_gap_count'], missing=report['total']['id_missing_count'])
base, path = fixture('append-during-read', '1.wal', record(1))
real_fstat = os.fstat
calls = 0

def fstat_then_append(fd):
    global calls
    result = real_fstat(fd)
    if calls == 0:
        with path.open('ab') as stream:
            stream.write(record(2))
    calls += 1
    return result

with patch.object(wal.os, 'fstat', fstat_then_append):
    result = wal.parse_file('s1', base, path)
check('fstat-size-bounds-ignore-concurrent-append', result['snapshot_bytes'] == 32
      and result['final_bytes'] == 64 and result['records'] == 1 and result['changed_during_read'],
      snapshot_bytes=result['snapshot_bytes'], final_bytes=result['final_bytes'])
base, path = fixture('shrink-during-read', '1.wal', record(1) + record(2))
calls = 0

def fstat_then_truncate(fd):
    global calls
    result = real_fstat(fd)
    if calls == 0:
        with path.open('r+b') as stream:
            stream.truncate(32)
    calls += 1
    return result

with patch.object(wal.os, 'fstat', fstat_then_truncate):
    result = wal.parse_file('s1', base, path)
check('concurrent-truncation-reported', result['status'] == 'incomplete_tail'
      and result['records'] == 1 and any(i['kind'] == 'file_shrank_during_read' for i in result['issues']))
report = wal.scan([('first', ROOT / 'good'), ('second', ROOT / 'short-header')])
check('multiple-labelled-directories', len(report['instances']) == 2 and report['total']['records'] == 5)
wal.output_report(report, ROOT / 'report.json')
wal.output_report(report, ROOT / 'report.csv')
check('json-csv-output', json.loads((ROOT / 'report.json').read_text())['total']['records'] == 5
      and 'incomplete_tail_header' in (ROOT / 'report.csv').read_text())
print('All 10 synthetic WAL checks passed.')

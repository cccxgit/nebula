#!/usr/bin/env python3
"""Read every complete Nebula 3.6 WAL record without modifying source WALs.

Example: python3 parse_wal.py --data-dir s1=/path/storage1 \
  --data-dir s2=/path/storage2 --output wal-full.json --output wal-full.csv
A suffixless --output PREFIX writes PREFIX.json and PREFIX.csv. JSON preserves
all anomalies; CSV contains file/partition/instance/total rows. Every file is
bounded by fstat(fd).st_size captured on opening it. This is NOT a transactionally
consistent snapshot across files. An incomplete tail may be an in-progress write
or truncation/corruption; the reader does not infer which from a partial record.
Exit 0: structurally complete/continuous. Exit 2: unavailable, incomplete or
invalid data / ID anomalies. Reports are written for either outcome.
"""
import argparse
from collections import Counter, defaultdict
import csv
import datetime
import json
import os
from pathlib import Path
import re
import struct
import sys

HEADER = struct.Struct('<qqiq')  # LogID, TermID, int32 payload length, ClusterID
FOOTER = struct.Struct('<i')
FIXED = HEADER.size + FOOTER.size
SUM_FIELDS = ('records', 'empty_records', 'nonempty_records', 'payload_bytes',
              'snapshot_bytes', 'validated_bytes', 'unvalidated_bytes', 'id_gap_count',
              'id_missing_count', 'id_nonincreasing_count', 'term_regressions')


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds')


def empty_stats():
    return {**dict.fromkeys(SUM_FIELDS, 0), 'first_id': None, 'last_id': None,
            'first_term': None, 'last_term': None, 'term_counts': {}, 'cluster_counts': {}}


def identity(path, base):
    relative = path.relative_to(base)
    parts = relative.parts
    # Normal storage path: nebula/<space_id>/wal/<partition_id>/<first_id>.wal
    if len(parts) >= 4 and parts[-3] == 'wal':
        return parts[-4], parts[-2], str(relative.parent)
    return '', '', str(relative.parent)


def id_transition(result, previous, current, offset, boundary=False):
    if previous is None or current == previous + 1:
        return
    gap = current > previous + 1
    result['id_gap_count' if gap else 'id_nonincreasing_count'] += 1
    if gap:
        result['id_missing_count'] += current - previous - 1
    result['issues'].append({'kind': 'id_gap' if gap else 'id_nonincreasing',
                             'offset': offset, 'previous_id': previous, 'current_id': current,
                             'file_boundary': boundary})


def parse_file(label, base, path):
    space, part, partdir = identity(path, base)
    result = {**empty_stats(), 'scope': 'file', 'label': label, 'space_id': space,
              'partition_id': part, 'partition_dir': partdir, 'path': str(path),
              'snapshot_time_utc': utc(), 'status': 'complete', 'issues': [],
              'files': 1, 'partial_tail': False}
    offset = 0
    terms, clusters = Counter(), Counter()
    try:
        with path.open('rb', buffering=1024 * 1024) as stream:
            before = os.fstat(stream.fileno())
            size = before.st_size
            result.update(snapshot_bytes=size, inode=before.st_ino, device=before.st_dev,
                          snapshot_mtime_ns=before.st_mtime_ns)
            while offset < size:
                remaining = size - offset
                if remaining < HEADER.size:
                    result['issues'].append({'kind': 'incomplete_tail_header', 'offset': offset,
                                             'available_bytes': remaining,
                                             'required_header_bytes': HEADER.size})
                    result['status'], result['partial_tail'] = 'incomplete_tail', True
                    break
                header = stream.read(HEADER.size)
                if len(header) != HEADER.size:
                    result['issues'].append({'kind': 'short_read_header', 'offset': offset,
                                             'expected_bytes': HEADER.size, 'read_bytes': len(header)})
                    result['status'], result['partial_tail'] = 'incomplete_tail', True
                    break
                log_id, term, length, cluster = HEADER.unpack(header)
                if length < 0:
                    result['issues'].append({'kind': 'negative_payload_length', 'offset': offset,
                                             'log_id': log_id, 'payload_length': length})
                    result['status'] = 'invalid_record'
                    break
                record_size = FIXED + length
                if record_size > remaining:
                    result['issues'].append({'kind': 'incomplete_tail_record', 'offset': offset,
                                             'log_id': log_id, 'available_bytes': remaining,
                                             'required_record_bytes': record_size})
                    result['status'], result['partial_tail'] = 'incomplete_tail', True
                    break
                # Read all payload bytes, in bounded chunks; no seek-based assumption that they exist.
                unread = length
                while unread:
                    chunk = stream.read(min(unread, 1024 * 1024))
                    if not chunk:
                        break
                    unread -= len(chunk)
                footer = stream.read(FOOTER.size) if not unread else b''
                if unread or len(footer) != FOOTER.size:
                    result['issues'].append({'kind': 'short_read_payload_or_footer', 'offset': offset,
                                             'log_id': log_id, 'unread_payload_bytes': unread,
                                             'footer_read_bytes': len(footer)})
                    result['status'], result['partial_tail'] = 'incomplete_tail', True
                    break
                tail_length = FOOTER.unpack(footer)[0]
                if tail_length != length:
                    result['issues'].append({'kind': 'header_footer_length_mismatch', 'offset': offset,
                                             'log_id': log_id, 'header_length': length,
                                             'footer_length': tail_length})
                    result['status'] = 'invalid_record'
                    break
                id_transition(result, result['last_id'], log_id, offset)
                if result['last_term'] is not None and term < result['last_term']:
                    result['term_regressions'] += 1
                    result['issues'].append({'kind': 'term_regression', 'offset': offset,
                                             'previous_term': result['last_term'], 'current_term': term})
                if result['first_id'] is None:
                    result['first_id'], result['first_term'] = log_id, term
                    if path.stem.isdecimal() and log_id != int(path.stem):
                        result['issues'].append({'kind': 'filename_first_id_mismatch', 'offset': 0,
                                                 'filename_id': int(path.stem), 'first_id': log_id})
                result['last_id'], result['last_term'] = log_id, term
                result['records'] += 1
                result['empty_records' if not length else 'nonempty_records'] += 1
                result['payload_bytes'] += length
                terms[str(term)] += 1
                clusters[str(cluster)] += 1
                offset += record_size
            after = os.fstat(stream.fileno())
            result.update(final_bytes=after.st_size, final_mtime_ns=after.st_mtime_ns,
                          changed_during_read=(before.st_size != after.st_size or
                                               before.st_mtime_ns != after.st_mtime_ns),
                          grew_after_snapshot_bytes=max(0, after.st_size - size))
            if after.st_size < size:
                result['issues'].append({'kind': 'file_shrank_during_read',
                                         'snapshot_bytes': size, 'final_bytes': after.st_size})
    except OSError as exc:
        result['status'] = 'unavailable'
        result['issues'].append({'kind': 'os_error', 'error': str(exc)})
    result.update(validated_bytes=offset, unvalidated_bytes=result['snapshot_bytes'] - offset,
                  term_counts=dict(terms), cluster_counts=dict(clusters),
                  snapshot_end_utc=utc())
    if result['status'] == 'complete' and result['issues']:
        result['status'] = 'complete_with_anomalies'
    return result


def summarize(rows, scope, **identity_fields):
    result = {**empty_stats(), 'scope': scope, **identity_fields, 'files': len(rows),
              'partitions': len({(r['label'], r['partition_dir']) for r in rows}),
              'issues': [], 'status_counts': dict(Counter(r['status'] for r in rows)),
              'changed_files': sum(r.get('changed_during_read', False) for r in rows),
              'partial_tail_files': sum(r['partial_tail'] for r in rows)}
    terms, clusters = Counter(), Counter()
    for row in rows:
        for field in SUM_FIELDS:
            result[field] += row[field]
        terms.update(row['term_counts'])
        clusters.update(row['cluster_counts'])
    result['term_counts'], result['cluster_counts'] = dict(terms), dict(clusters)
    return result


def scan(directories):
    report = {'schema': 1, 'started_utc': utc(), 'format': '<qqiq payload <i',
              'record_header_bytes': HEADER.size, 'record_fixed_bytes': FIXED,
              'snapshot_semantics': 'Per opened file fstat size; not atomic across files. Partial tails may be concurrent writes or corruption. No payload checksum exists in this framing.',
              'data_directories': [], 'input_errors': [], 'files': [], 'partitions': [], 'instances': []}
    seen = set()
    for label, base in directories:
        base = base.resolve()
        report['data_directories'].append({'label': label, 'path': str(base)})
        if not base.is_dir():
            report['input_errors'].append({'label': label, 'path': str(base), 'error': 'directory missing'})
            continue
        # Inventory first; files created after this inventory are intentionally not included.
        for path in sorted(base.rglob('*.wal')):
            key = (label, str(path.resolve()))
            if key in seen:
                continue
            seen.add(key)
            report['files'].append(parse_file(label, base, path))
    grouped = defaultdict(list)
    for row in report['files']:
        grouped[row['label'], row['partition_dir']].append(row)
    for (label, partdir), rows in sorted(grouped.items()):
        rows.sort(key=lambda r: (int(Path(r['path']).stem) if Path(r['path']).stem.isdecimal() else 2**64, r['path']))
        aggregate = summarize(rows, 'partition', label=label, partition_dir=partdir,
                              space_id=rows[0]['space_id'], partition_id=rows[0]['partition_id'])
        previous = None
        for row in rows:
            if row['first_id'] is None:
                # Missing/corrupt files break our ability to assess continuity across this boundary.
                previous = None
                continue
            if aggregate['first_id'] is None:
                aggregate['first_id'], aggregate['first_term'] = row['first_id'], row['first_term']
            if previous is not None:
                id_transition(aggregate, previous['last_id'], row['first_id'], row['path'], True)
                if row['first_term'] < previous['last_term']:
                    aggregate['term_regressions'] += 1
                    aggregate['issues'].append({'kind': 'term_regression', 'path': row['path'],
                                                'previous_term': previous['last_term'],
                                                'current_term': row['first_term'], 'file_boundary': True})
            aggregate['last_id'], aggregate['last_term'] = row['last_id'], row['last_term']
            previous = row if row['status'] in ('complete', 'complete_with_anomalies') else None
        report['partitions'].append(aggregate)
    for label in sorted({label for label, _ in directories}):
        rows = [row for row in report['files'] if row['label'] == label]
        instance = summarize(rows, 'instance', label=label)
        # Include cross-file transitions, counted by partition summaries but not individual files.
        for key in ('id_gap_count', 'id_missing_count', 'id_nonincreasing_count', 'term_regressions'):
            instance[key] = sum(p[key] for p in report['partitions'] if p['label'] == label)
        report['instances'].append(instance)
    report['total'] = summarize(report['files'], 'total')
    for key in ('id_gap_count', 'id_missing_count', 'id_nonincreasing_count', 'term_regressions'):
        report['total'][key] = sum(p[key] for p in report['partitions'])
    report['finished_utc'] = utc()
    report['has_anomalies'] = bool(report['input_errors'] or any(r['issues'] for r in report['files']) or
                                   any(p['issues'] for p in report['partitions']))
    return report


def output_report(report, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.suffix.lower() == '.json':
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        return
    rows = report['files'] + report['partitions'] + report['instances'] + [report['total']]
    fields = ['scope', 'label', 'space_id', 'partition_id', 'partition_dir', 'path', 'status',
              'files', 'partitions', *SUM_FIELDS, 'first_id', 'last_id', 'first_term', 'last_term',
              'term_counts', 'cluster_counts', 'status_counts', 'partial_tail', 'partial_tail_files',
              'snapshot_time_utc', 'snapshot_end_utc', 'changed_during_read', 'changed_files',
              'final_bytes', 'grew_after_snapshot_bytes', 'issues']
    with destination.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, ensure_ascii=False) if isinstance(v, (dict, list)) else v
                             for k, v in row.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--data-dir', action='append', required=True, metavar='LABEL=PATH')
    parser.add_argument('--output', action='append', type=Path, required=True, metavar='JSON_OR_CSV_OR_PREFIX')
    args = parser.parse_args()
    directories = []
    for item in args.data_dir:
        if '=' not in item:
            parser.error('--data-dir requires LABEL=PATH')
        label, path = item.split('=', 1)
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', label) or not path:
            parser.error('Invalid LABEL=PATH: ' + item)
        directories.append((label, Path(path)))
    outputs = []
    for output in args.output:
        if output.suffix.lower() in ('.json', '.csv'):
            outputs.append(output)
        else:
            outputs += [Path(str(output) + '.json'), Path(str(output) + '.csv')]
    for output in outputs:
        if output.exists():
            parser.error('Refusing to overwrite existing report: ' + str(output))
    report = scan(directories)
    for output in outputs:
        output_report(report, output)
    print(json.dumps({'outputs': [str(p) for p in outputs], 'has_anomalies': report['has_anomalies'],
                      'total': report['total']}, ensure_ascii=False))
    return 2 if report['has_anomalies'] else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Recompute stored benchmark summaries without connecting to any service."""
import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import time


def read_json(path):
    return json.loads(path.read_text())


def audit(run):
    result = {'run': str(run), 'checks': []}
    csv_path = run / 'resources.csv'
    summary = read_json(run / 'resources.csv.summary.json')
    rows = list(csv.DictReader(csv_path.open()))
    groups = defaultdict(list)
    for row in rows:
        groups[row['elapsed_s']].append(row)
    expected_pids = set(map(str, summary['pids']))
    samples = []
    for elapsed, group in sorted(groups.items(), key=lambda x: float(x[0])):
        assert {row['pid'] for row in group} == expected_pids
        assert len(group) == len(expected_pids)
        alive = all(row['alive'] == 'True' for row in group)
        pss_complete = alive and all(row['pss_bytes'] != '' for row in group)
        cpu_complete = alive and all(row['cpu_percent_one_core'] != '' for row in group)
        samples.append({'elapsed_s': float(elapsed), 'all_alive': alive,
                        'rss_bytes': sum(int(row['rss_bytes'] or 0) for row in group),
                        'pss_bytes': sum(int(row['pss_bytes']) for row in group) if pss_complete else None,
                        'cpu_percent_one_core': sum(float(row['cpu_percent_one_core']) for row in group)
                        if cpu_complete else None})
    assert len(samples) == summary['sample_count']
    assert max(s['rss_bytes'] for s in samples) == summary['max_sum_rss_bytes']
    pss = [s['pss_bytes'] for s in samples if s['pss_bytes'] is not None]
    cpu = [s['cpu_percent_one_core'] for s in samples if s['cpu_percent_one_core'] is not None]
    if len(pss) == len(samples):
        assert max(pss) == summary['max_sum_available_pss_bytes']
    if cpu:
        assert abs(max(cpu) - summary['max_sum_cpu_percent_one_core']) <= len(expected_pids) * 0.00051
    intervals = [samples[i]['elapsed_s']-samples[i-1]['elapsed_s'] for i in range(1,len(samples))]
    result['resources'] = {'pids': sorted(map(int, expected_pids)), 'samples': len(samples),
                           'complete_pss_samples': len(pss), 'complete_cpu_intervals': len(cpu),
                           'all_observed_processes_alive': all(s['all_alive'] for s in samples),
                           'sample_interval_min_s': min(intervals) if intervals else None,
                           'sample_interval_max_s': max(intervals) if intervals else None,
                           'observed_peak_sum_rss_bytes': max(s['rss_bytes'] for s in samples),
                           'observed_peak_sum_pss_bytes': max(pss) if pss else None,
                           'observed_peak_sum_cpu_cores': max(cpu)/100 if cpu else None,
                           'warning': 'Sampled maximum, not exact peak; includes setup/warmup. Missing CPU intervals mean unmeasured, not zero.'}
    result['checks'].append('Resource maxima recomputed from simultaneous sample groups')
    query_file = run / 'results/query-summary.json'
    if query_file.exists():
        query = read_json(query_file)
        observations = list(csv.DictReader((run / 'results/query-latencies.csv').open()))
        assert sorted(int(r['sequence']) for r in observations) == list(range(len(observations)))
        def latency(items):
            good = [float(r['latency_ms']) for r in items if r['success'] == 'True']
            good.sort()
            return {'requests':len(items), 'successes':len(good), 'failures':len(items)-len(good),
                    'latency_success_ms': {f'p{p}': good[math.ceil(len(good)*p/100)-1] if good else None for p in [50,95,99]} |
                    {'max':max(good) if good else None}}
        assert latency(observations) == query['overall']
        for name, expected in query['queries'].items():
            assert latency([r for r in observations if r['name'] == name]) == {k:v for k,v in expected.items() if k != 'query'}
        success = sum(r['success']=='True' for r in observations)
        assert math.isclose(success/query['elapsed_s'], query['successful_qps'], rel_tol=1e-12)
        result['query'] = {'rows':len(observations), 'summary_recomputed':True,
                           'successful_qps':query['successful_qps'], 'measured_elapsed_s':query['elapsed_s'],
                           'serial_latency_sum_s':sum(float(r['latency_ms']) for r in observations)/1000,
                           'overall':query['overall'],
                           'returned_row_counts_by_query':{name:sorted({int(r['result_rows']) for r in observations if r['name']==name}) for name in query['queries']}}
        result['checks'].append('Every query percentile, count, sequence and QPS recomputed')
    import_file = run / 'results/import-summary.json'
    if import_file.exists():
        ingestion=read_json(import_file)
        batches=[json.loads(line) for line in (run/'results/import-batches.jsonl').read_text().splitlines()]
        assert len(batches)==ingestion['batch_requests_observed']
        assert sum(r['input_rows'] for r in batches if r['success'])==ingestion['acknowledged_rows']
        assert sum(r['input_rows'] for r in batches if not r['success'])==ingestion['failed_rows_observed']
        assert math.isclose(ingestion['acknowledged_rows']/ingestion['elapsed_s'],ingestion['acknowledged_rows_per_second'])
        result['checks'].append('Import acknowledgement counts and rows/s recomputed from batches')
    result['passed']=True
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs',nargs='+',required=True)
    parser.add_argument('--output',required=True)
    args=parser.parse_args()
    path=Path(args.output)
    if path.exists():
        parser.error('Use a fresh output path')
    report={'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'kind':'offline_recomputation_of_existing_measurements',
            'runs':[audit(Path(run)) for run in args.runs]}
    report['all_passed']=all(r['passed'] for r in report['runs'])
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Summarize preserved measurements and render plots; no database interaction."""
import csv
import datetime as dt
import json
from pathlib import Path
import re
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
MIB = 1024 ** 2


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def csv_rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def parse_observations():
    result = {}
    for label in ['s1', 's2', 's3']:
        rows = []
        paths = {p.resolve() for p in (ROOT / 'service-logs' / label).glob('*INFO*') if p.is_file()}
        for path in sorted(paths):
            with path.open(errors='replace') as stream:
                for line in stream:
                    if 'WAL_MEMORY_OBSERVE' not in line:
                        continue
                    stamp = re.match(r'I(\d{8}|\d{4})\s+(\d\d:\d\d:\d\d\.\d+)', line)
                    if not stamp:
                        raise ValueError('Unknown glog timestamp: ' + line[:80])
                    day = stamp[1] if len(stamp[1]) == 8 else '2026' + stamp[1]
                    moment = dt.datetime.strptime(day + ' ' + stamp[2], '%Y%m%d %H:%M:%S.%f')
                    moment = moment.replace(tzinfo=ZoneInfo('Asia/Shanghai')).astimezone(dt.timezone.utc)
                    row = {'time_utc': moment.isoformat(), 'epoch': moment.timestamp(), 'label': label}
                    for key, value in re.findall(r'(\w+)=([^\s]+)', line.split('WAL_MEMORY_OBSERVE', 1)[1]):
                        row[key] = int(value) if re.fullmatch(r'-?\d+', value) else value
                    rows.append(row)
        rows.sort(key=lambda x: x['epoch'])
        result[label] = rows
        if rows:
            with (ROOT / f'wal-observe-{label}.csv').open('w') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
    return result


def summarize(observations):
    summaries = []
    for folder in sorted(ROOT.glob('phase-*')):
        if not folder.is_dir():
            continue
        for path in sorted(folder.glob('s*.csv')):
            rows = csv_rows(path)
            # Startup/failed/partial samples cannot support a full 50-space comparison.
            valid = [r for r in rows if r.get('http_spaces_ok') == '50'
                     and r.get('http_spaces_failed') == '0' and r.get('http_engines') == '50'
                     and r.get('wal_parse_errors') == '0' and r.get('wal_part_dirs') == '1000']
            if len(valid) < 2:
                continue
            first, last = valid[0], valid[-1]
            start, end = (dt.datetime.fromisoformat(r['time_utc']).timestamp() for r in (first, last))
            duration = end - start
            entry = {'phase': folder.name, 'label': path.stem, 'samples': len(valid),
                     'first_utc': first['time_utc'], 'last_utc': last['time_utc'],
                     'duration_seconds': duration,
                     'same_pid': len({r['pid'] for r in valid}) == 1,
                     'same_starttime': len({r['proc_starttime_ticks'] for r in valid}) == 1}
            keys = ['status_VmRSS_kib', 'status_RssAnon_kib', 'smaps_Rss_kib', 'smaps_Anonymous_kib',
                    'wal_records_estimate', 'wal_bytes', 'rocksdb.cur-size-all-mem-tables',
                    'rocksdb.block-cache-usage', 'rocksdb.estimate-table-readers-mem']
            for key in keys:
                values = [number(r.get(key)) for r in (first, last)]
                if all(v is not None for v in values):
                    entry[key] = {'first': values[0], 'last': values[1], 'delta': values[1] - values[0]}
            obs = [r for r in observations.get(path.stem, []) if start <= r['epoch'] <= end]
            if len(obs) >= 2:
                a, b = obs[0], obs[-1]
                entry['wal_observer'] = {'first_utc': a['time_utc'], 'last_utc': b['time_utc'],
                                         'duration_seconds': b['epoch'] - a['epoch']}
                for key in ['pushes', 'empty_pushes', 'created', 'freed', 'live_nodes',
                            'live_logical_bytes', 'node_object_bytes', 'releases', 'gc_runs', 'resets']:
                    entry['wal_observer'][key] = {'first': a[key], 'last': b[key], 'delta': b[key] - a[key]}
                entry['wal_observer']['estimated_node_usable_delta'] = (b['live_nodes'] - a['live_nodes']) * 3584
            summaries.append(entry)
    (ROOT / 'summary.json').write_text(json.dumps(summaries, indent=2))
    return summaries


def plots(observations):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'figure.dpi': 150, 'axes.spines.top': False, 'axes.spines.right': False,
                         'font.size': 10, 'axes.grid': True, 'grid.alpha': 0.2})
    colors = {'s1': '#1769aa', 's2': '#cf6200', 's3': '#258247'}
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.2), constrained_layout=True)
    for count in [1, 100, 1000]:
        rows = [r for r in csv_rows(ROOT / f'atomic-jemalloc-default-{count}.csv') if r['phase'] == 'push']
        axes[0].plot([int(r['records_per_partition']) for r in rows],
                     [int(r['node_malloc_usable_bytes']) / MIB for r in rows], marker='o', label=f'{count} buffers')
    axes[0].set(xlabel='Empty records per buffer', ylabel='Live Node allocation (MiB)', title='Unmodified WAL code: growth before capacity')
    axes[0].legend()
    rows = [r for r in csv_rows(ROOT / 'atomic-jemalloc-default-threshold.csv') if r['phase'] == 'push']
    axes[1].plot([int(r['records_per_partition']) for r in rows],
                 [int(r['node_malloc_usable_bytes']) / MIB for r in rows], marker='o', label='Live Node allocation')
    axes[1].plot([int(r['records_per_partition']) for r in rows],
                 [int(r['logical_bytes']) / MIB for r in rows], marker='o', label='Logical capacity accounting')
    axes[1].axvline(524288, color='#777', linestyle='--', linewidth=1)
    axes[1].set(xlabel='Empty records appended (one buffer)', ylabel='MiB', title='Normal reader release: eventual plateau')
    axes[1].legend()
    fig.savefig(ROOT / 'component-wal.png')
    fig.savefig(ROOT / 'component-wal.svg')
    plt.close(fig)
    runtime = []
    for folder in sorted(ROOT.glob('phase-*')):
        if not folder.is_dir():
            continue
        for path in sorted(folder.glob('s*.csv')):
            for row in csv_rows(path):
                if row.get('http_spaces_ok') == '50' and row.get('http_spaces_failed') == '0':
                    row['phase'] = folder.name
                    row['epoch'] = dt.datetime.fromisoformat(row['time_utc']).timestamp()
                    runtime.append(row)
    if not runtime:
        return
    start = min(r['epoch'] for r in runtime)
    fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=True, constrained_layout=True)
    for label, color in colors.items():
        rows = sorted([r for r in runtime if r['label'] == label], key=lambda r: r['epoch'])
        x, rss, mem = [], [], []
        previous = None
        for row in rows:
            if previous is not None and row['epoch'] - previous > 30:
                # No invented interpolation through gaps between sampled phases.
                x.append(float('nan')); rss.append(float('nan')); mem.append(float('nan'))
            x.append((row['epoch'] - start) / 60)
            rss.append(float(row['smaps_Rss_kib']) / 1024)
            mem.append(float(row['rocksdb.cur-size-all-mem-tables']) / MIB)
            previous = row['epoch']
        axes[0].plot(x, rss, color=color, label=label)
        axes[1].plot(x, mem, color=color, label=label)
        rows = [r for r in observations.get(label, []) if r['epoch'] >= start]
        axes[2].plot([(r['epoch'] - start) / 60 for r in rows],
                     [r['node_object_bytes'] / MIB for r in rows], color=color, label=label)
    for ax, title in zip(axes, ['Process RSS (smaps)', 'RocksDB MemTables (sum of 50 spaces)', 'Live WAL Nodes (sizeof; excludes allocator rounding)']):
        ax.set(title=title, ylabel='MiB')
        ax.legend(loc='upper left', ncols=3)
    events = [json.loads(line) for line in (ROOT / 'events.jsonl').read_text().splitlines()]
    seen = set()
    for event in events:
        if event['action'] == 'raft_interval_change' and event['port'] == 39779:
            when = (dt.datetime.fromisoformat(event['time_utc']).timestamp() - start) / 60
            key = (event['seconds'], round(when))
            if key not in seen:
                seen.add(key)
                for ax in axes:
                    ax.axvline(when, color='#666', linestyle='--', linewidth=1)
                axes[0].text(when, 1.02, f"Raft flag = {event['seconds']}s", transform=axes[0].get_xaxis_transform(), fontsize=9)
        if event['action'] == 'command' and event.get('log') == 'flush.log':
            when = (dt.datetime.fromisoformat(event['time_utc']).timestamp() - start) / 60
            for ax in axes:
                ax.axvline(when, color='#993366', linestyle=':', linewidth=1)
            axes[1].text(when, 1.02, 'Flush submitted', transform=axes[1].get_xaxis_transform(), fontsize=9)
    axes[-1].set_xlabel('Minutes since first 50-space sample')
    fig.savefig(ROOT / 'runtime-memory.png')
    fig.savefig(ROOT / 'runtime-memory.svg')
    plt.close(fig)


if __name__ == '__main__':
    observations = parse_observations()
    result = summarize(observations)
    plots(observations)
    print(json.dumps({'phase_process_summaries': len(result), 'observer_samples': {k: len(v) for k, v in observations.items()}}))

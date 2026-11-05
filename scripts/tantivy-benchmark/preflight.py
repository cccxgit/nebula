#!/usr/bin/env python3
"""Fail closed before measured work if compilers or low-resource conditions exist."""
import argparse
import json
import os
from pathlib import Path
import shutil
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--path', default='/home/sch/nebula-data/tantivy')
    parser.add_argument('--output', required=True)
    parser.add_argument('--min-free-gib', type=float, default=40)
    parser.add_argument('--min-available-gib', type=float, default=8)
    parser.add_argument('--allow-download', action='store_true',
                        help='Only for ingestion preparation; measured query runs should pause the downloader')
    args = parser.parse_args()
    builds, downloads, paused_downloads = [], [], []
    compiler_names = {'make', 'ninja', 'cc1plus', 'cc1', 'clang', 'clang++', 'rustc', 'ld.lld', 'lto1', 'collect2', 'ccache'}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            name = (entry / 'comm').read_text().strip()
            if name in compiler_names:
                builds.append({'pid': int(entry.name), 'name': name})
            if name.startswith('python'):
                argv = (entry / 'cmdline').read_bytes().split(b'\0')
                if any(x.endswith(b'benchmark.py') for x in argv) and b'fetch' in argv:
                    state = next(line for line in (entry / 'status').read_text().splitlines()
                                 if line.startswith('State:')).split()[1]
                    (paused_downloads if state in ('T', 't') else downloads).append(int(entry.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        memory[key] = int(value.split()[0]) * 1024
    checks = {'no_active_compiler_processes': not builds,
              'no_active_dataset_downloads': args.allow_download or not downloads,
              'disk_free': shutil.disk_usage(args.path).free >= args.min_free_gib * 1024**3,
              'memory_available': memory['MemAvailable'] >= args.min_available_gib * 1024**3}
    report = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'checks': checks, 'passed': all(checks.values()), 'compiler_processes': builds,
              'dataset_download_pids': downloads, 'paused_dataset_download_pids': paused_downloads,
              'mem_available_bytes': memory['MemAvailable'],
              'disk_free_bytes': shutil.disk_usage(args.path).free,
              'note': 'This is a point-in-time preflight, not a guarantee against later resource spikes.'}
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(2)


if __name__ == '__main__':
    main()

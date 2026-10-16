#!/usr/bin/env python3
"""Build/run pinned WAL component probes in a new directory; no Nebula services.

smoke builds both probes and performs short roundtrip/Node boundary exercises.
full additionally reproduces five capacities and four payload/after-stop cases.
Neither mode tests the proposed space-level feature or a running cluster.
"""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


PINNED = {
    'AtomicLogBuffer.cpp': '3af5641a37c66d5a80ed27670857c9beb8538a6406ff7fa1bd74ccc4b7a2e47a',
    'AtomicLogBuffer.h': 'da9731116d69c04cb5800c72ec375fe2ba4164b502b773636639a4feeeac28fd',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--third-party', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path,
                        help='Target Nebula build directory; defaults to REPO/build')
    parser.add_argument('--compiler', default='c++', help='Executable name or path, no shell syntax')
    parser.add_argument('--output', type=Path, required=True, help='Must not exist')
    parser.add_argument('--mode', choices=['smoke', 'full'], default='smoke')
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('These measurement probes require Linux /proc and allocator interfaces')
    repo = args.repo.expanduser().resolve()
    prefix = args.third_party.expanduser().resolve()
    build = (args.build_dir or repo / 'build').expanduser().resolve()
    output = args.output.expanduser().resolve()
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error('Compiler not found: ' + args.compiler)
    if output.exists():
        parser.error('Refusing to overwrite existing output: ' + str(output))
    task = repo / 'task/wal-investigation'
    baseline = task / 'baseline/kvstore/wal'
    for name, expected in PINNED.items():
        path = baseline / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            parser.error('Pinned baseline missing or modified: ' + str(path))
    for path in (prefix / 'include', prefix / 'lib', repo / 'src', build / 'src'):
        if not path.is_dir():
            parser.error('Required directory missing: ' + str(path))
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment['LD_LIBRARY_PATH'] = ':'.join(str(prefix / part) for part in ('lib', 'lib64')) + (
        ':' + environment['LD_LIBRARY_PATH'] if environment.get('LD_LIBRARY_PATH') else '')
    commands = []
    report = {'schema': 1, 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'mode': args.mode, 'repo': str(repo), 'third_party': str(prefix),
              'build_dir': str(build), 'compiler': compiler, 'platform': platform.platform(),
              'baseline_commit': 'de9b3ed800a6627d9845e9289b6bbc5b6faf460a',
              'baseline_sha256': PINNED, 'commands': commands, 'checks': [],
              'boundary': 'Pinned single-writer component comparison; no services, no product mutation, no space-level feature/performance validation.'}

    def save():
        (output / 'run.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')

    def run(argv, logname, errname=None, timeout=180):
        item = {'argv': [str(v) for v in argv], 'stdout': logname,
                'stderr': errname or logname,
                'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
        commands.append(item)
        save()
        print('Running ' + logname, flush=True)
        with (output / logname).open('x') as stdout:
            stderr = (output / errname).open('x') if errname else subprocess.STDOUT
            try:
                result = subprocess.run(item['argv'], cwd=repo, env=environment,
                                        stdout=stdout, stderr=stderr, timeout=timeout)
                item['exit_code'] = result.returncode
            finally:
                if errname:
                    stderr.close()
        save()
        if result.returncode:
            raise RuntimeError('Command failed; inspect ' + str(output / logname))

    try:
        run([compiler, '--version'], 'compiler-version.log')
        common = [compiler, '-std=c++17', '-O2', '-g', '-DENABLE_JEMALLOC',
                  '-DENABLE_MEMORY_TRACKER', '-DS2_USE_GFLAGS', '-DS2_USE_GLOG',
                  '-D__STDC_FORMAT_MACROS', '-I', task / 'baseline', '-I', repo / 'src',
                  '-I', build / 'src', '-isystem', prefix / 'include']
        libraries = ['-L', prefix / 'lib', '-L', prefix / 'lib64', '-lfolly',
                     '-lglog', '-lgflags', '-ljemalloc', '-ldl', '-pthread']
        drivers = [('atomic', task / 'atomic-buffer-probe.cpp'),
                   ('payload', task / 'write-idle-comparison-20261011/payload-probe.cpp')]
        for label, source in drivers:
            report[label + '_source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
            run(common + [source, baseline / 'AtomicLogBuffer.cpp'] + libraries +
                ['-o', output / (label + '-probe')], label + '-build.log')
        if args.mode == 'smoke':
            run([output / 'atomic-probe', 1, 1048576, 'each', '0,1,64,65,129'],
                'capacity-smoke.csv', 'capacity-smoke.meta')
            run([output / 'payload-probe', 1024, 0, '0,1,64,65,129'],
                'payload-smoke.csv', 'payload-smoke.meta')
        else:
            for capacity in (8388608, 4194304, 2097152, 1048576, 262144):
                n = capacity // 16
                steps = ','.join(map(str, (0, 1, 64, 65, n, n + 1, n + 2,
                                           n + 320, n * 2, n * 4)))
                name = 'capacity-' + str(capacity)
                run([output / 'atomic-probe', 1, capacity, 'each', steps],
                    name + '.csv', name + '.meta')
                with (output / (name + '.csv')).open() as stream:
                    rows = list(csv.DictReader(stream))
                last = [row for row in rows if row['phase'] == 'push'][-1]
                ok = (last['refs'] == '0' and last['dirty_nodes'] == '0' and
                      rows[-1]['phase'] == 'destroy_all' and rows[-1]['nodes'] == '0')
                report['checks'].append({'capacity': capacity, 'passed': ok,
                                         'last_push': last})
                if not ok:
                    raise RuntimeError('Reader/dirty/destruction check failed: ' + name)
            cases = [('empty', 0, 0, '1,64,65,8192,32768,524288,600000,1100000'),
                     ('payload256', 256, 0, '1,64,65,8192,32768,65536,131072'),
                     ('payload1024', 1024, 0, '1,64,65,8065,8066,8192,32768,65536,131072'),
                     ('after-stop', 1024, 32768, '0,64,1024,8192,32768,131072,262144,524288,600000')]
            for name, size, warmup, steps in cases:
                run([output / 'payload-probe', size, warmup, steps], name + '.csv', name + '.meta')
        report['binary_sha256'] = {label: hashlib.sha256((output / (label + '-probe')).read_bytes()).hexdigest()
                                   for label, _ in drivers}
        report['status'] = 'passed'
        save()
        print('Component run passed: ' + str(output), flush=True)
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        report.update(status='failed', error=str(exc))
        save()
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())

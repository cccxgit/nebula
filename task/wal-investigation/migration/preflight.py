#!/usr/bin/env python3
"""Read-only migration audit; never starts services, compiles or contacts HTTP.

Only a new --output directory is written. Existing scripts are parsed as data;
they are never imported or executed. Optional runtime/dependency paths are only
inspected. This is an inventory aid, not proof of a working build/deployment.
"""
import argparse
import ast
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import subprocess
import sys


BASE = 'de9b3ed800a6627d9845e9289b6bbc5b6faf460a'
TASK = Path('task/wal-investigation')


def command(argv):
    try:
        run = subprocess.run(argv, capture_output=True, text=True, timeout=15,
                             env={**os.environ, 'GIT_OPTIONAL_LOCKS': '0'})
        return {'exit_code': run.returncode, 'stdout': run.stdout.strip(),
                'stderr': run.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {'exit_code': None, 'error': str(exc)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path,
                        default=Path(__file__).resolve().parents[3])
    parser.add_argument('--output', type=Path, required=True,
                        help='New directory; existing paths are refused')
    parser.add_argument('--install', type=Path, help='Optional test installation')
    parser.add_argument('--console', type=Path, help='Optional console binary')
    parser.add_argument('--third-party', type=Path, help='Optional dependency prefix')
    parser.add_argument('--pid', type=int, action='append', default=[],
                        help='Optional local PID to inspect; no signals are sent')
    args = parser.parse_args()
    repo = args.repo.expanduser().resolve()
    if not (repo / TASK).is_dir():
        parser.error('--repo must contain task/wal-investigation')
    if args.output.exists():
        parser.error('Refusing to overwrite existing output: ' + str(args.output))
    failures, warnings = [], []
    if sys.version_info < (3, 9):
        failures.append('Python 3.9+ is required for the complete analysis toolset')
    if platform.system() != 'Linux':
        warnings.append('Live sampling and C++ probes use Linux /proc; offline parsing is portable')
    dependencies = {name: shutil.which(name) for name in
                    ('git', 'bash', 'c++', 'cmake', 'make', 'tar', 'sha256sum')}
    for name, path in dependencies.items():
        if path is None:
            warnings.append('Executable not found: ' + name)
    syntax, hardcoded, binaries, links = [], [], [], []
    task_root = repo / TASK
    for path in sorted(task_root.rglob('*')):
        if path.is_relative_to(args.output.resolve()):
            continue
        relative = str(path.relative_to(repo))
        if path.is_symlink():
            links.append({'path': relative, 'target': os.readlink(path),
                          'target_exists': path.exists()})
            if not path.exists():
                warnings.append('Broken evidence symlink: ' + relative)
        if not path.is_file():
            continue
        if path.suffix in ('.py', '.sh'):
            source = path.read_text(encoding='utf-8')
            item = {'path': relative, 'sha256': hashlib.sha256(source.encode()).hexdigest()}
            if path.suffix == '.py':
                try:
                    ast.parse(source, filename=relative)
                    item['syntax_ok'] = True
                except SyntaxError as exc:
                    item.update(syntax_ok=False, error=str(exc))
                    failures.append('Python syntax: ' + relative)
            elif dependencies['bash']:
                result = command([dependencies['bash'], '-n', str(path)])
                item.update(syntax_ok=result['exit_code'] == 0, check=result)
                if not item['syntax_ok']:
                    failures.append('Shell syntax: ' + relative)
            else:
                item['syntax_ok'] = None
            syntax.append(item)
            for number, line in enumerate(source.splitlines(), 1):
                if '/home/sch/' in line or '/opt/vesoft/' in line:
                    # Record locations only; never echo arbitrary source/config text.
                    hardcoded.append({'path': relative, 'line': number})
        elif path.suffix == '' and path.stat().st_size:
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    binaries.append({'path': relative, 'bytes': path.stat().st_size})
    if hardcoded:
        warnings.append('Historical scripts contain host-specific paths; see hardcoded_path_locations')
    if binaries:
        warnings.append('Historical ELF binaries are machine-specific; rebuild probes on the target host')
    baseline = []
    if dependencies['git']:
        git = dependencies['git']
        identity = command([git, '-C', str(repo), 'rev-parse', 'HEAD'])
        for filename in ('AtomicLogBuffer.cpp', 'AtomicLogBuffer.h'):
            extracted = task_root / 'baseline/kvstore/wal' / filename
            original = command([git, '-C', str(repo), 'rev-parse',
                                BASE + ':src/kvstore/wal/' + filename])
            actual = command([git, '-C', str(repo), 'hash-object', str(extracted)])
            same = (original['exit_code'] == 0 and actual['exit_code'] == 0 and
                    actual['stdout'] == original['stdout'])
            baseline.append({'file': filename, 'matches_recorded_commit': same,
                             'expected_blob': original, 'actual_blob': actual})
            if not same:
                warnings.append('Baseline missing/unavailable/different: ' + filename)
    else:
        identity = {'exit_code': None, 'error': 'git unavailable'}
    optional = {}
    checks = [('install', args.install), ('console', args.console),
              ('third_party', args.third_party)]
    for name, value in checks:
        if value is None:
            continue
        path = value.expanduser().resolve()
        item = {'path': str(path), 'exists': path.exists()}
        if name == 'install':
            item['required_paths'] = {part: (path / part).exists() for part in
                                     ('bin/nebula-storaged', 'scripts/nebula.service')}
            item['data_nonempty'] = ((path / 'data').is_dir() and
                                     any((path / 'data').iterdir()))
        elif name == 'third_party':
            item['required_paths'] = {part: (path / part).exists() for part in
                                     ('include', 'lib', 'include/folly',
                                      'include/rocksdb', 'include/gflags')}
        else:
            item['executable'] = os.access(path, os.X_OK)
        optional[name] = item
        if not path.exists():
            warnings.append('Optional path does not exist: ' + str(path))
    processes = []
    for pid in args.pid:
        item = {'pid': pid}
        try:
            base = Path('/proc') / str(pid)
            item['exe'] = os.readlink(base / 'exe')
            item['cwd'] = os.readlink(base / 'cwd')
            item['status'] = '\n'.join(line for line in (base / 'status').read_text().splitlines()
                                       if line.startswith(('Name:', 'VmRSS:', 'VmSwap:', 'Threads:')))
        except OSError as exc:
            item['error'] = str(exc)
            warnings.append('Cannot inspect PID ' + str(pid))
        processes.append(item)
    report = {'schema': 1, 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'repo': str(repo), 'python': sys.version, 'platform': platform.platform(),
              'git_head': identity, 'recorded_baseline': BASE,
              'maximum_make_jobs': 5, 'nofile_soft_hard': list(resource.getrlimit(resource.RLIMIT_NOFILE)),
              'generated_headers_present': (repo / 'build/src/interface/gen-cpp2').is_dir(),
              'dependencies': dependencies, 'syntax_checks': syntax,
              'hardcoded_path_locations': hardcoded, 'historical_elf_binaries': binaries,
              'evidence_symlinks': links, 'baseline_checks': baseline,
              'optional_paths': optional, 'processes': processes,
              'failures': failures, 'warnings': warnings,
              'boundary': 'No service/network/build/config mutation; output directory only. Passing is not build/runtime/functional validation.'}
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'preflight.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    summary = ['Migration preflight', 'repo: ' + str(repo),
               'Python/shell files checked: ' + str(len(syntax)),
               'host-path occurrences: ' + str(len(hardcoded)),
               'historical ELF binaries: ' + str(len(binaries)),
               'failures: ' + str(len(failures)), 'warnings: ' + str(len(warnings))]
    summary += ['ERROR: ' + value for value in failures]
    summary += ['WARN: ' + value for value in warnings]
    summary.append(report['boundary'])
    (args.output / 'preflight.log').write_text('\n'.join(summary) + '\n')
    print('\n'.join(summary))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

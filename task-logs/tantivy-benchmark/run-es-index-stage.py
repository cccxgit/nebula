"""Record the isolated ES index stage and all seven service processes."""
import datetime
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root / 'task-logs/tantivy-benchmark' / (
    datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-es-index-1m')
out.mkdir()
subprocess.run([sys.executable, 'scripts/tantivy-benchmark/preflight.py',
                '--output', str(out / 'preflight.json')], cwd=root, check=True)
runtime_path = Path('/home/sch/nebula-run/3.6-tantivy-es-baseline')
pids = [int((runtime_path / 'pids' / (name + '.pid')).read_text())
        for name in ['metad', 'graphd', 'storage1', 'storage2', 'storage3', 'listener', 'elasticsearch']]
runtime = str(runtime_path).encode()
for pid in pids:
    assert runtime in Path(f'/proc/{pid}/cmdline').read_bytes(), f'Wrong process {pid}'
cmd = [sys.executable, 'scripts/tantivy-benchmark/es_baseline.py', 'create-index',
       '--space', 'ft_bench_1m', '--index', 'nebula_index_ft_passage_1m',
       '--expected-rows', '1000000', '--analyzer', 'standard',
       '--result-window', '100001', '--wait-seconds', '3600']
(out / 'command.json').write_text(json.dumps({'command': cmd, 'pids': pids}, indent=2))
monitor = [sys.executable, 'scripts/tantivy-benchmark/monitor.py',
           '--interval', '1', '--output', str(out / 'resources.csv')]
for pid in pids:
    monitor += ['--pid', str(pid)]
process = subprocess.Popen(monitor, cwd=root)
try:
    with (out / 'stdout.log').open('w') as stdout, (out / 'stderr.log').open('w') as stderr:
        result = subprocess.run(cmd, cwd=root, stdout=stdout, stderr=stderr)
finally:
    process.terminate()
    process.wait(timeout=30)
print('Evidence directory:', out, flush=True)
sys.exit(result.returncode)

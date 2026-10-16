#!/usr/bin/env python3
"""Reproducible local 3-replica idle-space investigation. No business writes."""
import argparse
import datetime
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
INSTALL = Path('/home/sch/nebula-run/nebula-memory')
CONSOLE = Path('/home/sch/nebula-run/nebula-console')
META_PORT = 29559
GRAPH_PORT = 29669
PORTS = [39779, 39789, 39799]
STORAGE_PORTS = [29779, 29789, 29799]
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def event(action, **details):
    item = {'time_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'action': action, **details}
    with (HERE / 'events.jsonl').open('a') as f:
        f.write(json.dumps(item) + '\n')
    print(json.dumps(item), flush=True)


def limit_files():
    _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (65536, hard))


def run(command, logname, check=True):
    event('command', argv=[str(v) for v in command], log=logname)
    with (HERE / logname).open('a') as output:
        result = subprocess.run([str(v) for v in command], cwd=INSTALL,
                                stdout=output, stderr=subprocess.STDOUT,
                                preexec_fn=limit_files)
    if check and result.returncode:
        raise RuntimeError(f'{command} failed ({result.returncode}); see {logname}')
    return result.returncode


def config(base, values):
    lines = []
    for line in base.splitlines():
        if line.startswith('--') and line.split('=', 1)[0][2:] in values:
            continue
        lines.append(line)
    lines += ['', '# Local investigation overrides (recorded, no code fix).']
    lines += [f'--{key}={value}' for key, value in values.items()]
    return '\n'.join(lines) + '\n'


def prepare():
    # Existing data is never overwritten by this command.
    if (INSTALL / 'data').exists() and any((INSTALL / 'data').iterdir()):
        raise RuntimeError('Existing runtime data found; prepare refuses to overwrite it.')
    for path in [INSTALL / 'etc', INSTALL / 'pids', HERE / 'configs', HERE / 'service-logs']:
        path.mkdir(parents=True, exist_ok=True)
    for service in ['metad', 'graphd']:
        values = {'ws_ip': '127.0.0.1', 'heartbeat_interval_secs': 1,
                  'meta_server_addrs': f'127.0.0.1:{META_PORT}',
                  'port': META_PORT if service == 'metad' else GRAPH_PORT,
                  'ws_http_port': 39559 if service == 'metad' else 39669,
                  'log_dir': HERE / 'service-logs' / service}
        if service == 'graphd':
            values.update(num_netio_threads=4, num_worker_threads=4, enable_udf='false', ws_meta_http_port=39559)
        else:
            values.update(ws_storage_http_port=39779)
        (HERE / 'service-logs' / service).mkdir(exist_ok=True)
        text = config((REPO / 'conf' / f'nebula-{service}.conf.default').read_text(), values)
        name = f'nebula-{service}.conf'
        (INSTALL / 'etc' / name).write_text(text)
        (HERE / 'configs' / name).write_text(text)
    for i, (port, http) in enumerate(zip(STORAGE_PORTS, PORTS), 1):
        logdir = HERE / 'service-logs' / f's{i}'
        logdir.mkdir(exist_ok=True)
        values = {'ws_ip': '127.0.0.1', 'port': port, 'ws_http_port': http,
                  'meta_server_addrs': f'127.0.0.1:{META_PORT}',
                  'pid_file': f'pids/nebula-storaged-s{i}.pid',
                  'data_path': f'data/storage{i}', 'log_dir': logdir,
                  'heartbeat_interval_secs': 1, 'raft_heartbeat_interval_secs': 30,
                  'wal_memory_observe': 'true', 'wal_memory_observe_interval_secs': 10,
                  'rocksdb_disable_wal': 'false'}
        text = config((REPO / 'conf' / 'nebula-storaged.conf.default').read_text(), values)
        name = f'nebula-storaged-s{i}.conf'
        (INSTALL / 'etc' / name).write_text(text)
        (HERE / 'configs' / name).write_text(text)
    (HERE / 'spaces.txt').write_text(''.join(f'wal_idle_{i:02d}\n' for i in range(1, 51)))
    (HERE / 'create-spaces.ngql').write_text(
        ''.join(f'CREATE SPACE IF NOT EXISTS wal_idle_{i:02d}'
                '(partition_num=20, replica_factor=3, vid_type=FIXED_STRING(32));\n'
                for i in range(1, 51)) + 'SHOW HOSTS;\nSHOW SPACES;\n')
    event('prepared', install=str(INSTALL))


def service(action, name, index=None):
    args = [INSTALL / 'scripts' / 'nebula.service']
    if index:
        args += ['-c', INSTALL / 'etc' / f'nebula-storaged-s{index}.conf']
    args += [action, name]
    return run(args, 'service-control.log', check=action == 'start')


def console(statement=None, filename=None, log='console.log'):
    command = [CONSOLE, '-addr', '127.0.0.1', '-port', str(GRAPH_PORT), '-u', 'root', '-p', 'nebula']
    command += ['-f', filename] if filename else ['-e', statement]
    run(command, log)


def flags(seconds):
    for port in PORTS:
        data = json.dumps({'raft_heartbeat_interval_secs': str(seconds)}).encode()
        request = urllib.request.Request(f'http://127.0.0.1:{port}/flags', data=data,
                                         method='PUT', headers={'Content-Type': 'application/json'})
        with HTTP.open(request, timeout=10) as reply:
            response = json.load(reply)
        event('raft_interval_change', port=port, seconds=seconds, response=response)
        if response.get('errCode') != 0:
            raise RuntimeError(response)
    snapshot_flags(f'flags-raft-{seconds}-{int(time.time())}.json')


def snapshot_flags(filename):
    result = {}
    for port in PORTS:
        with HTTP.open(f'http://127.0.0.1:{port}/flags?format=json&verbose=true', timeout=10) as reply:
            result[str(port)] = json.load(reply)
    (HERE / filename).write_text(json.dumps(result, indent=2))


def sample(name, duration, interval, empty=False):
    args = [sys.executable, HERE / 'sample_runtime.py']
    for i in range(1, 4):
        args += ['--pid-file', f's{i}={INSTALL}/pids/nebula-storaged-s{i}.pid',
                 '--data-dir', f's{i}={INSTALL}/data/storage{i}']
    if not empty:
        args += ['--spaces-file', HERE / 'spaces.txt']
    args += ['--ports', ','.join(map(str, PORTS)), '--duration', str(duration),
             '--interval', str(interval), '--output', HERE / name]
    event('phase_start', phase=name, duration=duration, interval=interval)
    run(args, f'{name}.log')
    event('phase_end', phase=name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'start', 'register', 'create', 'sample', 'flags', 'flush', 'jobs', 'inspect', 'stop'])
    parser.add_argument('--name', default='samples')
    parser.add_argument('--duration', type=float, default=180)
    parser.add_argument('--interval', type=float, default=15)
    parser.add_argument('--heartbeat', type=int, default=30)
    parser.add_argument('--empty', action='store_true')
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare()
    elif args.action == 'start':
        service('start', 'metad')
        time.sleep(3)
        for i in range(1, 4):
            service('start', 'storaged', i)
        service('start', 'graphd')
    elif args.action == 'register':
        console('ADD HOSTS ' + ', '.join(f'"127.0.0.1":{p}' for p in STORAGE_PORTS) + ';',
                log='register-hosts.log')
    elif args.action == 'create':
        console(filename=HERE / 'create-spaces.ngql', log='create-spaces.log')
        snapshot_flags('flags-initial.json')
    elif args.action == 'sample':
        sample(args.name, args.duration, args.interval, args.empty)
    elif args.action == 'flags':
        flags(args.heartbeat)
    elif args.action == 'flush':
        statement = ''.join(f'USE wal_idle_{i:02d}; SUBMIT JOB FLUSH;\n' for i in range(1, 51))
        filename = HERE / 'flush.ngql'
        filename.write_text(statement)
        console(filename=filename, log='flush.log')
    elif args.action == 'inspect':
        filename = HERE / 'inspect.ngql'
        filename.write_text('SHOW HOSTS;\nSHOW SPACES;\n' + ''.join(
            f'USE wal_idle_{i:02d};\nSHOW PARTS;\nSHOW TAGS;\nSHOW EDGES;\n' for i in range(1, 51)))
        console(filename=filename, log='cluster-inspect.log')
    elif args.action == 'jobs':
        filename = HERE / 'jobs.ngql'
        filename.write_text(''.join(f'USE wal_idle_{i:02d}; SHOW JOBS;\n' for i in range(1, 51)))
        console(filename=filename, log='job-status.log')
    elif args.action == 'stop':
        service('stop', 'graphd')
        for i in range(1, 4):
            service('stop', 'storaged', i)
        service('stop', 'metad')


if __name__ == '__main__':
    main()

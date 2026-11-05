#!/usr/bin/env python3
"""Read/set only the isolated loopback graph's Tantivy pushdown flag.

Run between measured stages, then use readiness.py verify to assert PROFILE.
Does not start/stop services or change any other setting.
"""
import argparse
import json
import os
from pathlib import Path
import time
import urllib.request

BASE = 'http://127.0.0.1:19669/flags'
FLAG = 'enable_tantivy_pushdown'


def active_clients():
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            argv = (entry / 'cmdline').read_bytes().split(b'\0')
            for script, command in ((b'benchmark.py', b'query'), (b'readiness.py', b'verify'),
                                    (b'regression.py', b'run')):
                if command in argv and any(x.endswith(script) for x in argv):
                    found.append(int(entry.name))
                    break
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return found


def request(method='GET', value=None):
    # Ignore environment proxy routing for this fixed loopback endpoint.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    body = json.dumps({FLAG: 'true' if value else 'false'}).encode() if method == 'PUT' else None
    url = BASE if method == 'PUT' else BASE + '?flags=' + FLAG + '&format=json'
    req = urllib.request.Request(url, data=body, method=method,
                                 headers={'Content-Type': 'application/json'})
    with opener.open(req, timeout=10) as response:
        return json.load(response)


def current():
    payload = request()
    matches = [f['value'] for f in payload.get('flags', []) if f.get('name') == FLAG]
    if len(matches) != 1 or not isinstance(matches[0], bool):
        raise ValueError('The isolated graph did not return the expected boolean flag')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('value', choices=['read', 'on', 'off'])
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    path = Path(args.output)
    if path.exists():
        parser.error('Use a fresh output path to preserve flag evidence')
    path.parent.mkdir(parents=True, exist_ok=True)
    report = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'endpoint': BASE,
              'flag': FLAG, 'requested': args.value, 'success': False,
              'note': 'PROFILE must be checked separately. External clients require operator coordination.'}
    try:
        if args.value != 'read':
            clients = active_clients()
            if clients:
                raise ValueError(f'Measured/correctness clients still active: {clients}')
        report['before'] = current()
        if args.value != 'read':
            report['put_response'] = request('PUT', args.value == 'on')
            if report['put_response'].get('errCode') != 0:
                raise ValueError('Graph rejected flag update')
        report['after'] = current()
        if args.value != 'read' and report['after'] != (args.value == 'on'):
            raise ValueError('Flag readback did not match requested state')
        report['success'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        path.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

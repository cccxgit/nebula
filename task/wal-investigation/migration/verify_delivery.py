#!/usr/bin/env python3
"""Verify the explicit delivery, including safe relative evidence symlinks.

Reads files only; does not run historical scripts or contact any service.
The manifest cannot hash itself. Its identity is anchored by Git or the
external digest of the generated overlay archive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys

TASK = Path('task/wal-investigation')
MANIFEST = TASK / 'migration/DELIVERY-MANIFEST.json'


def sha_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(data)
    return digest.hexdigest()


def verify(repo):
    manifest_path = repo / MANIFEST
    data = json.loads(manifest_path.read_text())
    if data['format_version'] != 1:
        raise ValueError('Unsupported delivery format')
    failures, seen = [], set()
    records = data['files']
    names = {item['path'] for item in records}
    for item in records:
        name = item['path']
        part = PurePosixPath(name)
        if part.is_absolute() or '..' in part.parts or name in seen:
            failures.append({'path': name, 'reason': 'invalid or duplicate path'})
            continue
        seen.add(name)
        path = repo / name
        # Parent links must not redirect verification outside the checkout.
        if not path.parent.resolve().is_relative_to(repo):
            failures.append({'path': name, 'reason': 'parent escapes repository'})
            continue
        if item['kind'] == 'symlink':
            if not path.is_symlink():
                failures.append({'path': name, 'reason': 'symlink missing or wrong type'})
                continue
            target = os.readlink(path)
            digest = hashlib.sha256(os.fsencode(target)).hexdigest()
            destination = path.resolve()
            if (target != item['target'] or digest != item['sha256'] or
                    Path(target).is_absolute() or not destination.is_relative_to(repo) or
                    not destination.is_file() or str(destination.relative_to(repo)) not in names):
                failures.append({'path': name, 'reason': 'link target changed, unsafe or absent'})
        elif item['kind'] == 'file':
            if path.is_symlink() or not path.is_file():
                failures.append({'path': name, 'reason': 'file missing or wrong type'})
            elif path.stat().st_size != item['bytes'] or sha_file(path) != item['sha256']:
                failures.append({'path': name, 'reason': 'content changed'})
        else:
            failures.append({'path': name, 'reason': 'unsupported type'})
    selected = (repo / TASK / 'migration/publish-files.txt').read_text().splitlines()
    if len(selected) != len(set(selected)) or set(selected) != names | {str(MANIFEST)}:
        failures.append({'path': 'publish-files.txt', 'reason': 'selection differs from manifest'})
    return {
        'passed': not failures,
        'checked_paths': len(records),
        'selected_paths': len(selected),
        'regular_files': sum(item['kind'] == 'file' for item in records),
        'symlinks': sum(item['kind'] == 'symlink' for item in records),
        'manifest_sha256': sha_file(manifest_path),
        'failures': failures,
        'boundary': 'Selected file integrity only; no service, build, runtime or production validation.'
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    try:
        result = verify(args.repo.expanduser().resolve())
    except (OSError, ValueError, KeyError) as exc:
        print(json.dumps({'passed': False, 'error': str(exc)}))
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

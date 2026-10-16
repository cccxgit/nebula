#!/usr/bin/env python3
"""Build an explicit publication list and integrity manifest, optionally an overlay.

No git add/commit/push, service operation, source mutation or history rewriting.
The original audit's candidate list is immutable input. Only named delivery
metadata is refreshed; --bundle-dir must not exist.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile

TASK = Path('task/wal-investigation')
META = TASK / 'migration'
MANIFEST = META / 'DELIVERY-MANIFEST.json'
CHECKSUM = META / 'PUBLISH-SHA256SUMS'
SELECTION = META / 'publish-files.txt'
EVIDENCE_DIRS = {'component-smoke-20261016', 'component-full-20261016',
                 'preflight-complete-20261016'}


def sha_file(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--bundle-dir', type=Path,
                        help='Optional new directory to contain the overlay tar.gz and external digest')
    args = parser.parse_args()
    root = args.repo.expanduser().resolve()
    if args.bundle_dir:
        args.bundle_dir = args.bundle_dir.expanduser().resolve()
    if args.bundle_dir and args.bundle_dir.exists():
        parser.error('Bundle output must be new: ' + str(args.bundle_dir))
    selected = set((root / META / 'recommended-files.txt').read_text().splitlines())
    selected.update(str(path.relative_to(root)) for path in (root / TASK / 'service-logs').rglob('*')
                    if path.is_symlink())
    selected.update({str(TASK / '.gitignore'), str(TASK / '.gitattributes'), str(TASK / 'DELIVERY-AND-MIGRATION.md'),
                     str(TASK / 'DEVELOPER-HANDOFF.md'), str(TASK / 'ARTIFACT-INDEX.md'), 'README.md',
                     str(MANIFEST), str(CHECKSUM), str(SELECTION)})
    # New migration source, reports and controlled experiment outputs only.
    for path in (root / META).rglob('*'):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(root / META)
        if path.name.startswith('build-delivery') and path.suffix == '.log':
            continue
        if len(relative.parts) > 1 and relative.parts[0] not in EVIDENCE_DIRS:
            continue
        if path.suffix in {'.py', '.md', '.json', '.txt', '.log', '.csv', '.meta'}:
            selected.add(str(path.relative_to(root)))
    selected = sorted(selected)
    for name in selected:
        part = PurePosixPath(name)
        if (part.is_absolute() or '..' in part.parts or '\n' in name or
                not (name.startswith(str(TASK) + '/') or name in {
                    'README.md', 'AGENTS.md', 'CODEX-CLI-RESUME.md', 'nebula_graph_readable_analysis_prompts.md',
                    'task/task-wal.md', 'src/kvstore/wal/AtomicLogBuffer.cpp',
                    'chatgpt_shutdown_2026-10-10.log'})):
            raise ValueError('Out-of-scope path: ' + name)
        if name in {str(MANIFEST), str(CHECKSUM), str(SELECTION)}:
            continue
        path = root / name
        if path.is_symlink():
            target = os.readlink(path)
            destination = path.resolve()
            if (Path(target).is_absolute() or not destination.is_relative_to(root) or
                    not destination.is_file() or str(destination.relative_to(root)) not in selected):
                raise ValueError('Unsafe/unselected link target: ' + name)
        elif not path.is_file():
            raise ValueError('Missing regular file: ' + name)
        else:
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    raise ValueError('Machine binary must not be published: ' + name)
    (root / SELECTION).write_text('\n'.join(selected) + '\n')
    checksums = []
    for name in selected:
        path = root / name
        if name not in {str(MANIFEST), str(CHECKSUM)} and not path.is_symlink():
            checksums.append(f'{sha_file(path)}  {name}\n')
    (root / CHECKSUM).write_text(''.join(checksums))
    records = []
    for name in selected:
        if name == str(MANIFEST):
            continue
        path = root / name
        if path.is_symlink():
            target = os.readlink(path)
            raw = os.fsencode(target)
            records.append({'path': name, 'kind': 'symlink', 'target': target,
                            'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
        else:
            records.append({'path': name, 'kind': 'file', 'bytes': path.stat().st_size,
                            'sha256': sha_file(path)})
    data = {
        'format_version': 1,
        'generated_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_head_at_packaging': subprocess.check_output(
            ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
        'analysis_baseline': 'de9b3ed800a6627d9845e9289b6bbc5b6faf460a',
        'scope': 'WAL investigation, observation-only product patch, evidence and migration tools',
        'space_level_configuration_implemented': False,
        'manifest_self_excluded': str(MANIFEST),
        'selected_paths': len(selected),
        'regular_file_bytes_excluding_manifest': sum(r['bytes'] for r in records if r['kind'] == 'file'),
        'symlink_bytes': sum(r['bytes'] for r in records if r['kind'] == 'symlink'),
        'files': records
    }
    (root / MANIFEST).write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
    result = {key: data[key] for key in ('selected_paths', 'regular_file_bytes_excluding_manifest', 'symlink_bytes')}
    result['manifest_sha256'] = sha_file(root / MANIFEST)
    if args.bundle_dir:
        out = args.bundle_dir.expanduser().resolve()
        out.mkdir(parents=True, exist_ok=False)
        archive = out / 'nebula-wal-investigation-overlay.tar.gz'
        with tarfile.open(archive, 'w:gz', dereference=False) as tar:
            for name in selected:
                tar.add(root / name, arcname=name, recursive=False)
        digest = sha_file(archive)
        (out / 'SHA256SUMS').write_text(f'{digest}  {archive.name}\n')
        result.update(archive=str(archive), archive_bytes=archive.stat().st_size, archive_sha256=digest)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

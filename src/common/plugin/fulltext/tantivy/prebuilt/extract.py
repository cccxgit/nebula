#!/usr/bin/env python3
"""Validate the bundled Rust archive and extract it without a Rust toolchain."""

import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import sys


def check_host(manifest):
    libc, version = platform.libc_ver()
    if platform.system() != "Linux" or platform.machine() != "x86_64" or libc != "glibc":
        raise RuntimeError("requires Linux x86_64 with glibc")
    current = tuple(int(part) for part in version.split(".")[:2])
    minimum = tuple(int(part) for part in manifest["minimum_glibc"].split("."))
    if current < minimum:
        raise RuntimeError(f"requires glibc >= {manifest['minimum_glibc']}; found {version}")
    print(f"Linux x86_64, glibc {version}")


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def check_source(manifest, manifest_path):
    rust = manifest_path.resolve().parent.parent / "rust"
    for name, expected in manifest["source_sha256"].items():
        if digest(rust / name) != expected:
            raise RuntimeError(f"Rust source changed since the bundled archive was built: {name}")


def extract(manifest, bundle, output):
    if bundle.name != manifest["bundle"] or digest(bundle) != manifest["bundle_sha256"]:
        raise RuntimeError("bundled Tantivy archive checksum mismatch")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".partial")
    value = hashlib.sha256()
    size = 0
    try:
        with gzip.open(bundle, "rb") as source, temporary.open("wb") as destination:
            for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
                destination.write(block)
                value.update(block)
                size += len(block)
        if size != manifest["archive_bytes"] or value.hexdigest() != manifest["archive_sha256"]:
            raise RuntimeError("extracted Tantivy archive checksum or length mismatch")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Verified Tantivy archive: {output}")


def main():
    if len(sys.argv) not in (3, 5) or sys.argv[1] not in ("check", "extract"):
        raise SystemExit("usage: extract.py check MANIFEST | extract MANIFEST BUNDLE OUTPUT")
    manifest_path = Path(sys.argv[2])
    manifest = json.loads(manifest_path.read_text())
    check_host(manifest)
    check_source(manifest, manifest_path)
    if sys.argv[1] == "extract":
        if len(sys.argv) != 5:
            raise SystemExit("extract needs BUNDLE and OUTPUT")
        extract(manifest, Path(sys.argv[3]), Path(sys.argv[4]))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error)) from error

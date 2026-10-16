#!/usr/bin/env bash
# Read-only experiment against the previously built, original-source component.
# Does not start Nebula services or change any production configuration.
set -euo pipefail
cd /home/sch/nebula/nebula-3.6-memory
out=task/wal-investigation/production-tuning-evidence
probe=task/wal-investigation/atomic-buffer-probe-jemalloc
base=de9b3ed800a6627d9845e9289b6bbc5b6faf460a
mkdir -p "$out"
test ! -e "$out/capacity-8388608.csv"
exec > >(tee "$out/capacity-run.log") 2>&1
set -x
date --iso-8601=seconds
git rev-parse HEAD
uname -a
for source in AtomicLogBuffer.cpp AtomicLogBuffer.h; do
  git show "$base:src/kvstore/wal/$source" | cmp - "task/wal-investigation/baseline/kvstore/wal/$source"
done
sha256sum "$probe" task/wal-investigation/atomic-buffer-probe.cpp task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.{cpp,h}
for capacity in 8388608 4194304 2097152 1048576 262144; do
  records=$((capacity / 16))
  checkpoints="0,1,64,65,$records,$((records+1)),$((records+2)),$((records+320)),$((records*2)),$((records*4))"
  "$probe" 1 "$capacity" each "$checkpoints" > "$out/capacity-$capacity.csv" 2> "$out/capacity-$capacity.meta"
  cat "$out/capacity-$capacity.csv"
done
date --iso-8601=seconds

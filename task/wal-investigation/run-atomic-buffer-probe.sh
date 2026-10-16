#!/usr/bin/env bash
# Diagnostic harness only; original implementation is extracted from the recorded commit.
set -euo pipefail
set -x
cd /home/sch/nebula/nebula-3.6-memory
OUT=task/wal-investigation
BASE=de9b3ed800a6627d9845e9289b6bbc5b6faf460a
mkdir -p "$OUT/baseline/kvstore/wal"
for source in AtomicLogBuffer.cpp AtomicLogBuffer.h; do
  git show "$BASE:src/kvstore/wal/$source" > "$OUT/baseline/kvstore/wal/$source"
done
{
  date --iso-8601=seconds
  uname -a
  /usr/bin/c++ --version
  ldd --version
  git rev-parse HEAD
  sha256sum "$OUT/baseline/kvstore/wal/AtomicLogBuffer.cpp" "$OUT/baseline/kvstore/wal/AtomicLogBuffer.h" "$OUT/atomic-buffer-probe.cpp"
} > "$OUT/atomic-buffer-probe-environment.txt"
for allocator in glibc jemalloc; do
  extra=()
  suffix=
  if [[ "$allocator" == jemalloc ]]; then extra=(-ljemalloc); suffix=-jemalloc; fi
  /usr/bin/c++ -std=c++17 -O2 -g -DENABLE_JEMALLOC -DENABLE_MEMORY_TRACKER -DS2_USE_GFLAGS -DS2_USE_GLOG -D__STDC_FORMAT_MACROS \
    -I "$OUT/baseline" -I src -I build/src -isystem /opt/vesoft/third-party/3.3/include \
    "$OUT/atomic-buffer-probe.cpp" "$OUT/baseline/kvstore/wal/AtomicLogBuffer.cpp" \
    -L /opt/vesoft/third-party/3.3/lib -lfolly -lglog -lgflags "${extra[@]}" -ldl -pthread \
    -o "$OUT/atomic-buffer-probe$suffix"
  prefix=atomic
  if [[ "$allocator" == jemalloc ]]; then prefix=atomic-jemalloc; fi
  for parts in 1 100 1000; do
    "$OUT/atomic-buffer-probe$suffix" "$parts" 8388608 each 0,1,64,65,128,256,512,1024,2048,4096 \
      > "$OUT/$prefix-default-${parts}.csv" 2> "$OUT/$prefix-default-${parts}.meta"
  done
  "$OUT/atomic-buffer-probe$suffix" 1 8388608 each 0,1,64,65,524288,524289,524290,524608,525000,600000,1100000 \
    > "$OUT/$prefix-default-threshold.csv" 2> "$OUT/$prefix-default-threshold.meta"
  for mode in each none held; do
    "$OUT/atomic-buffer-probe$suffix" 100 4096 "$mode" 0,1,64,65,256,512,1024,4096,16384 \
      > "$OUT/$prefix-small-${mode}.csv" 2> "$OUT/$prefix-small-${mode}.meta"
  done
done
nm "$OUT/atomic-buffer-probe-jemalloc" | rg ' T (malloc|aligned_alloc|malloc_usable_size)$'

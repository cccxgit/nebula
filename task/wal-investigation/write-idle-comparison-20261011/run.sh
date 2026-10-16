#!/usr/bin/env bash
set -euo pipefail
cd /home/sch/nebula/nebula-3.6-memory
probe_out=task/wal-investigation/write-idle-comparison-20261011
probe_baseline=task/wal-investigation/baseline
# Refuse to replace any previous run. Source files are kept separate from results.
for result in environment.txt build.log empty.csv payload256.csv payload1024.csv after-stop.csv; do
  if [[ -e "$probe_out/$result" ]]; then
    echo "Existing evidence: $probe_out/$result" >&2
    exit 2
  fi
done
for source in AtomicLogBuffer.cpp AtomicLogBuffer.h; do
  git show "de9b3ed800a6627d9845e9289b6bbc5b6faf460a:src/kvstore/wal/$source" |
    cmp - "$probe_baseline/kvstore/wal/$source"
done
{
  date --iso-8601=seconds
  git rev-parse HEAD
  /usr/bin/c++ --version
  sha256sum "$probe_baseline/kvstore/wal/AtomicLogBuffer.cpp" \
    "$probe_baseline/kvstore/wal/AtomicLogBuffer.h" "$probe_out/payload-probe.cpp"
} > "$probe_out/environment.txt"
/usr/bin/c++ -std=c++17 -O2 -g -DENABLE_JEMALLOC -DENABLE_MEMORY_TRACKER \
  -DS2_USE_GFLAGS -DS2_USE_GLOG -D__STDC_FORMAT_MACROS \
  -I "$probe_baseline" -I src -I build/src -isystem /opt/vesoft/third-party/3.3/include \
  "$probe_out/payload-probe.cpp" "$probe_baseline/kvstore/wal/AtomicLogBuffer.cpp" \
  -L /opt/vesoft/third-party/3.3/lib -lfolly -lglog -lgflags -ljemalloc -ldl -pthread \
  -o "$probe_out/payload-probe" > "$probe_out/build.log" 2>&1
"$probe_out/payload-probe" 0 0 1,64,65,8192,32768,524288,600000,1100000 \
  > "$probe_out/empty.csv" 2> "$probe_out/empty.meta"
"$probe_out/payload-probe" 256 0 1,64,65,8192,32768,65536,131072 \
  > "$probe_out/payload256.csv" 2> "$probe_out/payload256.meta"
"$probe_out/payload-probe" 1024 0 1,64,65,8065,8066,8192,32768,65536,131072 \
  > "$probe_out/payload1024.csv" 2> "$probe_out/payload1024.meta"
"$probe_out/payload-probe" 1024 32768 0,64,1024,8192,32768,131072,262144,524288,600000 \
  > "$probe_out/after-stop.csv" 2> "$probe_out/after-stop.meta"
nm "$probe_out/payload-probe" | rg ' T (malloc|aligned_alloc|malloc_usable_size)$'
echo 'Four component comparisons passed; no database service was started.'

#!/usr/bin/env bash
#
# Copyright (c) 2026 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

set -Eeuo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly TEST_DIR
TOOL_DIR="$(cd "${TEST_DIR}/.." && pwd)"
readonly TOOL_DIR
readonly TOOL="${TOOL_DIR}/nebula-disk-benchmark.sh"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "${TMP_DIR}"' EXIT
mkdir -p "${TMP_DIR}/target" "${TMP_DIR}/results"
printf '%s\n' '--data_path=/srv/nebula-production-data' >"${TMP_DIR}/safe.conf"

cat >"${TMP_DIR}/fio" <<'FAKE_FIO'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--version" ]]; then
    echo fio-3.99-test
    exit 0
fi
if [[ "${1:-}" == "--enghelp" ]]; then
    printf 'psync\nlibaio\n'
    exit 0
fi

output=""
filename=""
profile="precondition"
for argument in "$@"; do
    case "${argument}" in
        --output=*) output="${argument#--output=}" ;;
        --filename=*) filename="${argument#--filename=}" ;;
        *.fio) profile="$(basename "${argument}" .fio)" ;;
    esac
done
[[ -n "${output}" ]] || exit 2
if [[ -n "${filename}" ]]; then
    mkdir -p "$(dirname "${filename}")"
    : >"${filename}"
fi

read_iops=0
write_iops=0
sync_ios=0
case "${profile}" in
    seq-read) read_iops=1000 ;;
    wal-sync)
        write_iops=500
        sync_ios=500
        ;;
    *) write_iops=1000 ;;
esac

cat >"${output}" <<EOF
{
  "fio version": "fio-3.99-test",
  "jobs": [
    {
      "jobname": "${profile}",
      "read": {
        "total_ios": ${read_iops}, "io_bytes": $((read_iops * 8192)),
        "iops": ${read_iops}, "bw_bytes": $((read_iops * 8192)),
        "clat_ns": {"mean": 3000, "percentile": {"50.000000": 2000, "95.000000": 4000, "99.000000": 5000, "99.900000": 9000}}
      },
      "write": {
        "total_ios": ${write_iops}, "io_bytes": $((write_iops * 4096)),
        "iops": ${write_iops}, "bw_bytes": $((write_iops * 4096)),
        "clat_ns": {"mean": 4000, "percentile": {"50.000000": 3000, "95.000000": 5000, "99.000000": 7000, "99.900000": 11000}}
      },
      "sync": {
        "total_ios": ${sync_ios},
        "lat_ns": {"mean": 100000, "percentile": {"50.000000": 80000, "95.000000": 150000, "99.000000": 200000, "99.900000": 350000}}
      }
    }
  ]
}
EOF
FAKE_FIO
chmod 755 "${TMP_DIR}/fio"

"${TOOL}" run \
    --target-dir "${TMP_DIR}/target" \
    --output-dir "${TMP_DIR}/results" \
    --nebula-config "${TMP_DIR}/safe.conf" \
    --nebula-home "${TMP_DIR}" \
    --label runner-test \
    --preset quick \
    --size 16M \
    --runtime 1 \
    --ramp-time 0 \
    --repeat 2 \
    --jobs 1 \
    --iodepth 1 \
    --cooldown 0 \
    --profiles seq_read,wal_sync \
    --fio-bin "${TMP_DIR}/fio" \
    --allow-active-nebula \
    --allow-non-disk-fs \
    --yes >/dev/null

run_dir="$(find "${TMP_DIR}/results" -mindepth 1 -maxdepth 1 -type d -name 'runner-test-*' | head -n 1)"
[[ -n "${run_dir}" ]]
[[ -f "${run_dir}/summary.json" && -f "${run_dir}/summary.md" ]]
python3 - "${run_dir}/summary.json" <<'PY'
import json
import sys

summary = json.load(open(sys.argv[1], encoding="utf-8"))
assert summary["manifest"]["status"] == "complete"
assert len(summary["manifest"]["parameters"]["profile_hash"]) == 64
assert set(summary["profiles"]) == {"seq_read", "wal_sync"}
assert summary["profiles"]["seq_read"]["read"]["round_count"] == 2
assert summary["profiles"]["wal_sync"]["write"]["metrics"]["p99_9_latency_ns"]["median"] == 350000
PY

if find "${TMP_DIR}/target" -mindepth 1 -print -quit | grep -q .; then
    echo "benchmark workspace was not cleaned" >&2
    exit 1
fi

python3 "${TOOL_DIR}/lib/report.py" compare \
    --baseline "${run_dir}" \
    --candidate "${run_dir}" \
    --output-dir "${TMP_DIR}/comparison" >/dev/null
[[ -f "${TMP_DIR}/comparison/comparison.json" ]]

if "${TOOL}" run \
    --target-dir "${TMP_DIR}/target" \
    --label duplicate-test \
    --size 16M \
    --profiles seq_read,seq_read \
    --fio-bin "${TMP_DIR}/fio" \
    --yes >/dev/null 2>&1; then
    echo "duplicate profiles were accepted" >&2
    exit 1
fi

if "${TOOL}" run \
    --target-dir "${TMP_DIR}/target" \
    --label overflow-test \
    --size 5000000T \
    --profiles seq_read,compaction \
    --fio-bin "${TMP_DIR}/fio" \
    --yes >/dev/null 2>&1; then
    echo "overflowing test size was accepted" >&2
    exit 1
fi

mkdir "${TMP_DIR}/target/nebula"
if "${TOOL}" check \
    --target-dir "${TMP_DIR}/target" \
    --size 16M \
    --fio-bin "${TMP_DIR}/fio" \
    --allow-non-disk-fs >/dev/null 2>&1; then
    echo "Nebula data tree target was accepted" >&2
    exit 1
fi

mkdir -p "${TMP_DIR}/nebula-home/data/storage/scratch"
printf '%s\n' '--data_path=data/storage' >"${TMP_DIR}/relative.conf"
if "${TOOL}" check \
    --target-dir "${TMP_DIR}/nebula-home/data/storage/scratch" \
    --nebula-config "${TMP_DIR}/relative.conf" \
    --nebula-home "${TMP_DIR}/nebula-home" \
    --size 16M \
    --fio-bin "${TMP_DIR}/fio" \
    --allow-non-disk-fs >/dev/null 2>&1; then
    echo "relative production data path was accepted" >&2
    exit 1
fi

echo "runner integration test passed"

#!/usr/bin/env bash
# One measured stage, after schema/readiness/correctness checks are completed.
# Never starts or stops a database. Compilers and active downloads block execution.
set -euo pipefail
if [[ $# -lt 2 ]]; then
  echo 'Usage: TANTIVY_BENCH_PIDS="pid ..." bash run-measured-stage.sh LABEL import|query|index|verify [options]' >&2
  exit 2
fi
task_label=$1
shift
if [[ ! "$task_label" =~ ^[A-Za-z0-9_-]+$ ]]; then
  echo 'LABEL must contain only letters, digits, underscore, or hyphen' >&2
  exit 2
fi
if [[ "$1" != import && "$1" != query && "$1" != index && "$1" != verify ]]; then
  echo 'Only import, query, index, and verify stages are supported' >&2
  exit 2
fi
for task_arg in "$@"; do
  if [[ "$task_arg" == --output || "$task_arg" == --output=* ]]; then
    echo 'The wrapper selects a fresh evidence directory; omit --output' >&2
    exit 2
  fi
done
if [[ -z "${TANTIVY_BENCH_PIDS:-}" ]]; then
  echo 'Set TANTIVY_BENCH_PIDS to all graph/meta/storage PIDs; include Listener and ES for the baseline' >&2
  exit 2
fi
task_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd -- "$task_repo"
task_output="task-logs/tantivy-benchmark/$(date -u +%Y%m%dT%H%M%SZ)-${task_label}"
mkdir -p -- "$task_output"
python3 scripts/tantivy-benchmark/preflight.py --output "$task_output/preflight.json"
task_pid_args=()
for task_pid in $TANTIVY_BENCH_PIDS; do
  if [[ ! "$task_pid" =~ ^[0-9]+$ || ! -r "/proc/$task_pid/status" ]]; then
    echo "Invalid or absent service PID: $task_pid" >&2
    exit 2
  fi
  task_pid_args+=(--pid "$task_pid")
done
python3 - "$task_output/command.json" "$@" <<'PY'
import json,sys,time
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
    'command_arguments':sys.argv[2:], 'flag_state':'Record verified graph flag state and PROFILE separately'},indent=2)+'\n')
PY
python3 scripts/tantivy-benchmark/monitor.py "${task_pid_args[@]}" --interval 1 \
  --output "$task_output/resources.csv" &
task_monitor_pid=$!
task_finish() {
  task_exit_code=$?
  trap - EXIT
  kill -TERM "$task_monitor_pid" 2>/dev/null || true
  wait "$task_monitor_pid" 2>/dev/null || true
  echo "Evidence directory: $task_repo/$task_output"
  exit "$task_exit_code"
}
trap task_finish EXIT
task_entry=scripts/tantivy-benchmark/benchmark.py
if [[ "$1" == index || "$1" == verify ]]; then
  task_entry=scripts/tantivy-benchmark/readiness.py
fi
python3 "$task_entry" "$@" --output "$task_output/results" \
  > "$task_output/stdout.log" 2> "$task_output/stderr.log"

#!/usr/bin/env bash
set -euo pipefail

PID_FILE="/usr/local/nebula/pids/nebula-graphd.pid"
GDB_SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gdb_oom_combo.gdb"

if [[ ! -f "$PID_FILE" ]]; then
  echo "pid file not found: $PID_FILE"
  exit 1
fi

PID=$(cat "$PID_FILE")
if [[ -z "$PID" ]]; then
  echo "empty graphd pid"
  exit 1
fi

if [[ ! -f "$GDB_SCRIPT" ]]; then
  echo "gdb script not found: $GDB_SCRIPT"
  exit 1
fi

echo "Attaching graphd pid=$PID with script=$GDB_SCRIPT"
exec sudo gdb -p "$PID" -x "$GDB_SCRIPT"

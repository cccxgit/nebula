#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="/home/sch/nebula/nebula-console"
SERVICE="/usr/local/nebula/scripts/nebula.service"
BASE_GRAPH_CONF="/usr/local/nebula/etc/nebula-graphd.conf"
GRAPH_CONF_TEMPLATE="$BASE_GRAPH_CONF"
GRAPH_LOG="/usr/local/nebula/logs/nebula-graphd.INFO"
GRAPH_PID_FILE="/usr/local/nebula/pids/nebula-graphd.pid"

SUDO_PASS="${SUDO_PASS:-1016}"
MAIN_SPACE="${MAIN_SPACE:-stress_test_0221}"
SECONDARY_SPACE="${SECONDARY_SPACE:-stress_test_0220}"
LOAD_WORKERS="${LOAD_WORKERS:-4}"
CASE_DURATION="${CASE_DURATION:-20}"
QUERY_TIMEOUT_SEC="${QUERY_TIMEOUT_SEC:-20}"
HEAVY_QUERY_TIMEOUT_SEC="${HEAVY_QUERY_TIMEOUT_SEC:-45}"
RUNTIME_GUARD_ATTEMPTS="${RUNTIME_GUARD_ATTEMPTS:-3}"

RUNTIME_DIR="$ROOT_DIR/oom_existing_spaces_runtime"
CONF_DIR="$RUNTIME_DIR/conf"
RESULT_DIR="$RUNTIME_DIR/results"
LOG_DIR="$RUNTIME_DIR/logs"
REPORT_PATH="$ROOT_DIR/NebulaGraph_OOM_ExistingSpaces_Test_Report.md"

OOM_PATTERN='E_GRAPH_MEMORY_EXCEEDED|GraphMemoryExceeded|Used memory hits the high watermark|MemoryExceeded'
GUARD_LOG_TOKEN='[OOM_GUARD_TRIGGER]'

BASE_QUERY_MAIN="$RUNTIME_DIR/query_baseline_${MAIN_SPACE}.ngql"
HEAVY_QUERY_MAIN="$RUNTIME_DIR/query_heavy_${MAIN_SPACE}.ngql"
BASE_QUERY_SECONDARY="$RUNTIME_DIR/query_baseline_${SECONDARY_SPACE}.ngql"

sudo_cmd() {
  echo "$SUDO_PASS" | sudo -S "$@"
}

require_env() {
  [[ -x "$CONSOLE" ]] || { echo "missing $CONSOLE"; exit 1; }
  [[ -x "$SERVICE" ]] || { echo "missing $SERVICE"; exit 1; }
  [[ -f "$BASE_GRAPH_CONF" ]] || { echo "missing $BASE_GRAPH_CONF"; exit 1; }
}

run_ngql() {
  local q="$1"
  "$CONSOLE" -addr 127.0.0.1 -port 9669 -u root -p nebula -e "$q"
}

run_ngql_file() {
  local file="$1"
  "$CONSOLE" -addr 127.0.0.1 -port 9669 -u root -p nebula -f "$file"
}

wait_graph_ready() {
  for _ in {1..60}; do
    if run_ngql "SHOW HOSTS;" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_graph_process_alive() {
  for _ in {1..30}; do
    if [[ -f "$GRAPH_PID_FILE" ]]; then
      local pid
      pid=$(cat "$GRAPH_PID_FILE")
      if [[ -n "$pid" ]] && ps -p "$pid" >/dev/null 2>&1; then
        return 0
      fi
    fi
    sleep 1
  done
  return 1
}

require_spaces() {
  local out
  out=$(run_ngql "SHOW SPACES;" 2>&1 || true)
  echo "$out" > "$LOG_DIR/show_spaces.out"
  echo "$out" | rg -q "\"${MAIN_SPACE}\"" || {
    echo "missing space ${MAIN_SPACE}"
    return 1
  }
  echo "$out" | rg -q "\"${SECONDARY_SPACE}\"" || {
    echo "missing space ${SECONDARY_SPACE}"
    return 1
  }
}

gen_queries() {
  cat > "$BASE_QUERY_MAIN" <<EOF
USE ${MAIN_SPACE};
MATCH (v) RETURN id(v) LIMIT 20;
EOF

  cat > "$HEAVY_QUERY_MAIN" <<EOF
USE ${MAIN_SPACE};
MATCH (v)-[e]->(w) RETURN id(w) LIMIT 200000;
EOF

  cat > "$BASE_QUERY_SECONDARY" <<EOF
USE ${SECONDARY_SPACE};
MATCH (v) RETURN id(v) LIMIT 20;
EOF
}

system_used_ratio() {
  python3 - <<'PY'
import re
total = 0
avail = 0
with open("/proc/meminfo", "r", encoding="utf-8") as f:
    for line in f:
        if line.startswith("MemTotal:"):
            total = int(re.findall(r"\d+", line)[0]) * 1024
        elif line.startswith("MemAvailable:"):
            avail = int(re.findall(r"\d+", line)[0]) * 1024
if total <= 0:
    print("0.50")
else:
    print(f"{1 - (avail / total):.6f}")
PY
}

resolve_normal_watermark() {
  local used
  used=$(system_used_ratio)
  python3 - <<PY
used = float("$used")
wm = min(0.92, max(0.70, used + 0.15))
print(f"{wm:.4f}")
PY
}

resolve_reject_watermark() {
  local used
  used=$(system_used_ratio)
  python3 - <<PY
used = float("$used")
wm = max(0.15, used - 0.05)
print(f"{wm:.4f}")
PY
}

make_graph_conf() {
  local out_conf="$1"
  local inflight="$2"
  local rows_check="$3"
  local watermark="$4"
  local force_guard="$5"

  cp "$GRAPH_CONF_TEMPLATE" "$out_conf"
  chmod u+w "$out_conf"
  {
    echo "--max_storage_inflight_per_query=${inflight}"
    echo "--num_rows_to_check_memory=${rows_check}"
    echo "--system_memory_high_watermark_ratio=${watermark}"
    echo "--test_force_graph_memory_guard_trigger=${force_guard}"
  } >> "$out_conf"
}

install_graph_conf() {
  local conf="$1"
  sudo_cmd install -m 444 "$conf" "$BASE_GRAPH_CONF" >/dev/null
}

restart_graphd() {
  sudo_cmd "$SERVICE" stop graphd >/dev/null 2>&1 || true
  local stopped=0
  for _ in {1..45}; do
    local status_out
    status_out=$(sudo_cmd "$SERVICE" status graphd 2>&1 || true)
    if echo "$status_out" | rg -q "Exited"; then
      stopped=1
      break
    fi
    sleep 1
  done

  if ((stopped == 0)) && [[ -f "$GRAPH_PID_FILE" ]]; then
    local old_pid
    old_pid=$(cat "$GRAPH_PID_FILE" 2>/dev/null || true)
    if [[ -n "$old_pid" ]] && ps -p "$old_pid" >/dev/null 2>&1; then
      sudo_cmd kill -9 "$old_pid" >/dev/null 2>&1 || true
    fi
  fi

  sudo_cmd "$SERVICE" start graphd >/dev/null 2>&1 || true
  sleep 3
}

restore_base_graph_conf() {
  local backup="$1"
  [[ -f "$backup" ]] || return 0
  sudo_cmd install -m 444 "$backup" "$BASE_GRAPH_CONF" >/dev/null
  restart_graphd
}

log_line_count() {
  sudo_cmd wc -l "$GRAPH_LOG" | awk '{print $1}'
}

dump_case_log_delta() {
  local start_line="$1"
  local out_file="$2"
  local begin=$((start_line + 1))
  sudo_cmd sed -n "${begin},\$p" "$GRAPH_LOG" > "$out_file" || true
}

sample_graph_mem() {
  local out_file="$1"
  local seconds="$2"
  : > "$out_file"
  local pid=""
  if [[ -f "$GRAPH_PID_FILE" ]]; then
    pid=$(cat "$GRAPH_PID_FILE")
  fi
  [[ -n "$pid" ]] || return 0
  for ((t=1; t<=seconds; t++)); do
    local rss_kb
    rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    [[ -n "$rss_kb" ]] && echo "$t $rss_kb" >> "$out_file"
    sleep 1
  done
}

classify_query_result() {
  local out="$1"
  local rc="$2"
  local -n succ_ref="$3"
  local -n fail_ref="$4"
  local -n oom_ref="$5"
  local -n timeout_ref="$6"
  local -n other_ref="$7"

  if echo "$out" | rg -q "$OOM_PATTERN"; then
    oom_ref=$((oom_ref + 1))
    fail_ref=$((fail_ref + 1))
  elif ((rc == 124)); then
    timeout_ref=$((timeout_ref + 1))
    fail_ref=$((fail_ref + 1))
  elif ((rc == 0)) && ! echo "$out" | rg -q "\[ERROR|SemanticError|SpaceNotFound"; then
    succ_ref=$((succ_ref + 1))
  else
    other_ref=$((other_ref + 1))
    fail_ref=$((fail_ref + 1))
  fi
}

run_workers_case() {
  local case_name="$1"
  local query_file="$2"
  local duration="$3"
  local timeout_sec="$4"
  local workers="$5"
  local case_dir="$RESULT_DIR/$case_name"
  mkdir -p "$case_dir"

  local pids=()
  for ((w=1; w<=workers; w++)); do
    (
      local end=$((SECONDS + duration))
      local succ=0 fail=0 oom=0 timeout_cnt=0 other=0
      while ((SECONDS < end)); do
        set +e
        local out
        out=$(timeout "${timeout_sec}s" "$CONSOLE" -addr 127.0.0.1 -port 9669 -u root -p nebula -f "$query_file" 2>&1)
        local rc=$?
        set -e
        classify_query_result "$out" "$rc" succ fail oom timeout_cnt other
      done
      echo "$succ $fail $oom $timeout_cnt $other" > "$case_dir/worker_${w}.txt"
    ) &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do
    wait "$pid"
  done
}

run_single_case() {
  local case_name="$1"
  local query_file="$2"
  local timeout_sec="$3"
  local attempts="$4"
  local case_dir="$RESULT_DIR/$case_name"
  mkdir -p "$case_dir"

  local succ=0 fail=0 oom=0 timeout_cnt=0 other=0
  for ((a=1; a<=attempts; a++)); do
    set +e
    local out
    out=$(timeout "${timeout_sec}s" "$CONSOLE" -addr 127.0.0.1 -port 9669 -u root -p nebula -f "$query_file" 2>&1)
    local rc=$?
    set -e
    echo "$out" > "$case_dir/attempt_${a}.out"
    classify_query_result "$out" "$rc" succ fail oom timeout_cnt other
  done
  echo "$succ $fail $oom $timeout_cnt $other" > "$case_dir/worker_1.txt"
}

summarize_case() {
  local case_name="$1"
  local space="$2"
  local mode="$3"
  local expected_trigger="$4"
  local expect_guard_log="$5"
  local case_dir="$RESULT_DIR/$case_name"
  local mem_file="$case_dir/mem.txt"

  local succ=0 fail=0 oom=0 timeout_cnt=0 other=0
  while read -r s f o t x; do
    succ=$((succ + s)); fail=$((fail + f)); oom=$((oom + o)); timeout_cnt=$((timeout_cnt + t)); other=$((other + x))
  done < <(cat "$case_dir"/worker_*.txt)

  local peak_kb=0 avg_kb=0 samples=0 sum=0
  while read -r _ rss; do
    [[ -z "$rss" ]] && continue
    (( rss > peak_kb )) && peak_kb=$rss
    sum=$((sum + rss)); samples=$((samples + 1))
  done < "$mem_file"
  ((samples > 0)) && avg_kb=$((sum / samples))

  local guard_logs=0
  if [[ -f "$case_dir/graph_log_delta.log" ]]; then
    guard_logs=$(grep -F -c "$GUARD_LOG_TOKEN" "$case_dir/graph_log_delta.log" || true)
  fi

  local verdict="PASS"
  if [[ "$expected_trigger" == "Y" ]]; then
    ((oom > 0 && guard_logs > 0)) || verdict="FAIL"
  elif [[ "$expected_trigger" == "E" ]]; then
    ((oom > 0 && guard_logs == 0)) || verdict="FAIL"
  elif [[ "$expected_trigger" == "N" ]]; then
    ((oom == 0 && fail == 0 && guard_logs == 0)) || verdict="FAIL"
  fi
  if [[ "$expect_guard_log" == "Y" ]]; then
    ((guard_logs > 0)) || verdict="FAIL"
  fi

  echo "$case_name,$space,$mode,$expected_trigger,$expect_guard_log,$verdict,$succ,$fail,$oom,$timeout_cnt,$other,$guard_logs,$peak_kb,$avg_kb" >> "$RESULT_DIR/summary.csv"
}

write_report() {
  local now
  now="$(date '+%Y-%m-%d %H:%M:%S')"
  cat > "$REPORT_PATH" <<EOR
# NebulaGraph OOM 保护测试报告（预置 LDBC 空间）

- 执行时间: ${now}
- 环境: /usr/local/nebula service 管理模式
- 图空间: ${MAIN_SPACE}, ${SECONDARY_SPACE}
- 并发与时长: workers=${LOAD_WORKERS}, workers-case=${CASE_DURATION}s

## 结果汇总

| Case | Space | Mode | ExpectTrigger | ExpectGuardLog | Verdict | Success | Fail | OOMProtected | Timeout | OtherFail | GuardLogs | PeakRSS(KB) | AvgRSS(KB) |
|---|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
EOR

  while IFS=, read -r case_name space mode expected_trigger expect_guard_log verdict succ fail oom timeout_cnt other guard_logs peak avg; do
    printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
      "$case_name" "$space" "$mode" "$expected_trigger" "$expect_guard_log" "$verdict" "$succ" "$fail" "$oom" "$timeout_cnt" "$other" "$guard_logs" "$peak" "$avg" >> "$REPORT_PATH"
  done < <(tail -n +2 "$RESULT_DIR/summary.csv")

  cat >> "$REPORT_PATH" <<'EOR'

## 判读

1. `ExpectTrigger=Y`：`OOMProtected > 0` 且 `GuardLogs > 0`。
2. `ExpectTrigger=E`：`OOMProtected > 0` 且 `GuardLogs = 0`。
3. `ExpectTrigger=N`：`Fail=0` 且 `OOMProtected=0` 且 `GuardLogs=0`。
4. `ExpectTrigger=OBS`：观察型 case，用于比较参数与 RSS，不做强通过判定。

## 产物目录

- 结果目录: `oom_existing_spaces_runtime/results/`
- case 配置: `oom_existing_spaces_runtime/conf/`
- 运行日志: `oom_existing_spaces_runtime/logs/`
- 增量日志: `oom_existing_spaces_runtime/results/<case>/graph_log_delta.log`

EOR
}

main() {
  rm -rf "$RUNTIME_DIR"
  mkdir -p "$CONF_DIR" "$RESULT_DIR" "$LOG_DIR"

  require_env
  sudo_cmd "$SERVICE" start all >/dev/null 2>&1 || true
  wait_graph_ready
  require_spaces
  gen_queries

  local normal_wm reject_wm
  normal_wm=$(resolve_normal_watermark)
  reject_wm=$(resolve_reject_watermark)
  echo "normal_wm=${normal_wm}" | tee "$LOG_DIR/watermark.txt"
  echo "reject_wm=${reject_wm}" | tee -a "$LOG_DIR/watermark.txt"

  local base_graph_backup="$RUNTIME_DIR/nebula-graphd.conf.base.bak"
  cp "$BASE_GRAPH_CONF" "$base_graph_backup"
  GRAPH_CONF_TEMPLATE="$base_graph_backup"
  trap "restore_base_graph_conf '$base_graph_backup'" EXIT

  echo "case,space,mode,expected_trigger,expect_guard_log,verdict,success,fail,oom,timeout,other,guard_logs,peak_kb,avg_kb" > "$RESULT_DIR/summary.csv"

  local cases=(
    "case_baseline_0221 ${MAIN_SPACE} workers ${BASE_QUERY_MAIN} 8 256 ${normal_wm} false N N 12 ${QUERY_TIMEOUT_SEC} 2 1"
    "case_runtime_guard_0221 ${MAIN_SPACE} single ${HEAVY_QUERY_MAIN} 8 16 ${normal_wm} true Y Y 0 ${HEAVY_QUERY_TIMEOUT_SEC} 1 ${RUNTIME_GUARD_ATTEMPTS}"
    "case_start_reject_0221 ${MAIN_SPACE} single ${BASE_QUERY_MAIN} 8 256 ${reject_wm} false E N 0 ${QUERY_TIMEOUT_SEC} 1 2"
    "case_recovery_0221 ${MAIN_SPACE} workers ${BASE_QUERY_MAIN} 8 256 ${normal_wm} false N N 12 ${QUERY_TIMEOUT_SEC} 2 1"
    "case_inflight16_heavy_0221 ${MAIN_SPACE} workers ${HEAVY_QUERY_MAIN} 16 256 ${normal_wm} false OBS N ${CASE_DURATION} ${HEAVY_QUERY_TIMEOUT_SEC} ${LOAD_WORKERS} 1"
    "case_inflight8_heavy_0221 ${MAIN_SPACE} workers ${HEAVY_QUERY_MAIN} 8 256 ${normal_wm} false OBS N ${CASE_DURATION} ${HEAVY_QUERY_TIMEOUT_SEC} ${LOAD_WORKERS} 1"
    "case_inflight4_heavy_0221 ${MAIN_SPACE} workers ${HEAVY_QUERY_MAIN} 4 256 ${normal_wm} false OBS N ${CASE_DURATION} ${HEAVY_QUERY_TIMEOUT_SEC} ${LOAD_WORKERS} 1"
    "case_baseline_0220 ${SECONDARY_SPACE} workers ${BASE_QUERY_SECONDARY} 8 256 ${normal_wm} false N N 12 ${QUERY_TIMEOUT_SEC} 2 1"
  )

  for c in "${cases[@]}"; do
    read -r case_name space mode query_file inflight rows_check wm force_guard expected_trigger expect_guard duration timeout_sec workers attempts <<<"$c"
    echo ">>> running ${case_name} space=${space} mode=${mode} inflight=${inflight} rows_check=${rows_check} wm=${wm} force_guard=${force_guard}"

    local conf="$CONF_DIR/${case_name}.graphd.conf"
    make_graph_conf "$conf" "$inflight" "$rows_check" "$wm" "$force_guard"
    install_graph_conf "$conf"
    restart_graphd
    if [[ "$expected_trigger" == "E" ]]; then
      wait_graph_process_alive
    else
      wait_graph_ready
    fi

    local case_dir="$RESULT_DIR/$case_name"
    mkdir -p "$case_dir"
    local log_start
    log_start=$(log_line_count)

    if [[ "$mode" == "workers" ]]; then
      sample_graph_mem "$case_dir/mem.txt" "$duration" &
      local sampler_pid=$!
      run_workers_case "$case_name" "$query_file" "$duration" "$timeout_sec" "$workers"
      wait "$sampler_pid" || true
    else
      local sample_seconds=$((timeout_sec * attempts))
      sample_graph_mem "$case_dir/mem.txt" "$sample_seconds" &
      local sampler_pid=$!
      run_single_case "$case_name" "$query_file" "$timeout_sec" "$attempts"
      wait "$sampler_pid" || true
    fi

    dump_case_log_delta "$log_start" "$case_dir/graph_log_delta.log"
    summarize_case "$case_name" "$space" "$mode" "$expected_trigger" "$expect_guard"
  done

  write_report
  echo "Report generated: $REPORT_PATH"
}

main "$@"

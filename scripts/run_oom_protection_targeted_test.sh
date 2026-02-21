#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="/home/sch/nebula/nebula-console"
SERVICE="/usr/local/nebula/scripts/nebula.service"
BASE_GRAPH_CONF="/usr/local/nebula/etc/nebula-graphd.conf"
GRAPH_CONF_TEMPLATE="$BASE_GRAPH_CONF"
SUDO_PASS="1016"

RUNTIME_DIR="$ROOT_DIR/oom_protection_targeted_runtime"
CONF_DIR="$RUNTIME_DIR/conf"
RESULT_DIR="$RUNTIME_DIR/results"
LOG_DIR="$RUNTIME_DIR/logs"
REPORT_PATH="$ROOT_DIR/NebulaGraph_OOM_Protection_Targeted_Test_Report.md"

GRAPH_ADDR="127.0.0.1"
GRAPH_PORT=9669
STORAGE_PORT=9779
SPACE_NAME="oom_pt"

VERTEX_COUNT=${VERTEX_COUNT:-5000}
EDGE_DEGREE=${EDGE_DEGREE:-15}
INSERT_BATCH=${INSERT_BATCH:-400}
LOAD_WORKERS=${LOAD_WORKERS:-6}
CASE_DURATION=${CASE_DURATION:-25}
QUERY_TIMEOUT_SEC=${QUERY_TIMEOUT_SEC:-12}
AUTO_WM_DELTA=${AUTO_WM_DELTA:-0.10}
MEM_HOG_DELAY_SEC=${MEM_HOG_DELAY_SEC:-2}

HEAVY_QUERY_FILE="$RUNTIME_DIR/heavy_query.ngql"
MATCH_QUERY_FILE="$RUNTIME_DIR/match_trigger_query.ngql"
LOAD_DATA_FILE="$RUNTIME_DIR/load_data.ngql"
OOM_PATTERN='E_GRAPH_MEMORY_EXCEEDED|GraphMemoryExceeded|Used memory hits the high watermark|MemoryExceeded'
GRAPH_LOG="/usr/local/nebula/logs/nebula-graphd.INFO"
GUARD_LOG_TOKEN='[OOM_GUARD_TRIGGER]'
MATCH_QUERY_TIMEOUT_SEC=${MATCH_QUERY_TIMEOUT_SEC:-45}
MATCH_HOG_DELAY_SEC=${MATCH_HOG_DELAY_SEC:-1.2}
MATCH_HOG_MB=${MATCH_HOG_MB:-6144}
MATCH_TRIGGER_ATTEMPTS=${MATCH_TRIGGER_ATTEMPTS:-3}

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
  "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -e "$q"
}

run_ngql_file() {
  local f="$1"
  "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$f"
}

run_ngql_block() {
  local block="$1"
  local tmp_file
  tmp_file=$(mktemp "$RUNTIME_DIR/.tmp_ngql.XXXXXX")
  printf "%b\n" "$block" >"$tmp_file"
  local out
  out=$(run_ngql_file "$tmp_file" 2>&1 || true)
  rm -f "$tmp_file"
  echo "$out"
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

system_available_mb() {
  python3 - <<'PY'
import re
avail = 0
with open("/proc/meminfo", "r", encoding="utf-8") as f:
    for line in f:
        if line.startswith("MemAvailable:"):
            avail = int(re.findall(r"\d+", line)[0]) // 1024
            break
print(max(0, avail))
PY
}

wait_used_ratio_below() {
  local target="$1"
  local timeout_sec="$2"
  for ((i=0; i<timeout_sec; i++)); do
    local used
    used=$(system_used_ratio)
    if python3 - <<PY
used = float("$used")
target = float("$target")
import sys
sys.exit(0 if used < target else 1)
PY
    then
      return 0
    fi
    sleep 1
  done
  return 1
}

resolve_case_watermark() {
  local wm_token="$1"
  if [[ "$wm_token" != "AUTO_RUNTIME" ]]; then
    echo "$wm_token"
    return 0
  fi

  local used_ratio
  used_ratio=$(system_used_ratio)
  python3 - <<PY
used = float("$used_ratio")
delta = float("$AUTO_WM_DELTA")
wm = min(0.98, max(0.30, used + delta))
print(f"{wm:.4f}")
PY
}

compute_hog_mb() {
  local available_mb
  available_mb=$(system_available_mb)
  python3 - <<PY
avail = int("$available_mb")
hog = int(avail * 0.25)
hog = max(256, hog)
hog = min(2048, hog)
print(hog)
PY
}

start_memory_hog() {
  local case_dir="$1"
  local hog_mb="$2"
  local delay_sec="$3"
  local hold_sec="$4"
  (
    sleep "$delay_sec"
    python3 - <<PY > "$case_dir/memory_hog.out" 2>&1
import time
hog_mb = int("$hog_mb")
hold = int("$hold_sec") + 5
buf = []
ok = 0
for i in range(hog_mb):
    try:
        buf.append(bytearray(1024 * 1024))
        ok += 1
    except MemoryError:
        break
print(f"allocated_mb={ok}")
time.sleep(max(1, hold))
PY
  ) &
  echo $!
}

log_line_count() {
  sudo_cmd wc -l "$GRAPH_LOG" | awk '{print $1}'
}

dump_case_log_delta() {
  local case_dir="$1"
  local start_line="$2"
  local out_file="${3:-$case_dir/graph_log_delta.log}"
  local begin=$((start_line + 1))
  sudo_cmd sed -n "${begin},\$p" "$GRAPH_LOG" > "$out_file" || true
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

ensure_hosts_online() {
  local out
  out=$(run_ngql "ADD HOSTS \"127.0.0.1\":${STORAGE_PORT};" 2>&1 || true)
  if echo "$out" | rg -q "\[ERROR"; then
    if ! echo "$out" | rg -q "Existed"; then
      echo "ADD HOSTS failed: $out"
      return 1
    fi
  fi

  for _ in {1..40}; do
    if run_ngql "SHOW HOSTS;" 2>/dev/null | rg -q '"ONLINE"'; then
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_schema_ready() {
  local stmt="$1"
  local desc="$2"
  local out=""

  for _ in {1..90}; do
    out=$(run_ngql_block "USE ${SPACE_NAME};\n${stmt}")
    if ! echo "$out" | rg -q "\[ERROR|SpaceNotFound|TagNotFound|EdgeNotFound|SemanticError"; then
      return 0
    fi
    sleep 1
  done

  echo "${desc} not ready: $out"
  return 1
}

wait_data_write_ready() {
  local out=""
  local probe
  for _ in {1..120}; do
    probe="p${RANDOM}"
    out=$(run_ngql_block "USE ${SPACE_NAME};\nINSERT VERTEX person(name) VALUES \"${probe}\":(\"${probe}\");\nINSERT EDGE follow(weight) VALUES \"${probe}\"->\"${probe}\":(1);")
    if ! echo "$out" | rg -q "\[ERROR|SpaceNotFound|TagNotFound|EdgeNotFound|SemanticError"; then
      return 0
    fi
    sleep 1
  done

  echo "Schema write readiness failed: $out"
  return 1
}

wait_space_leader_ready() {
  local out
  for _ in {1..60}; do
    out=$(run_ngql "SHOW HOSTS;" 2>&1 || true)
    if ! echo "$out" | rg -q "\[ERROR" && echo "$out" | rg -q "\"${SPACE_NAME}:10\""; then
      return 0
    fi
    sleep 1
  done
  return 1
}

prepare_space() {
  local out
  run_ngql "DROP SPACE IF EXISTS ${SPACE_NAME};" >/dev/null 2>&1 || true
  out=$(run_ngql "CREATE SPACE IF NOT EXISTS ${SPACE_NAME}(partition_num=10, replica_factor=1, vid_type=FIXED_STRING(16));" 2>&1 || true)
  if echo "$out" | rg -q "\[ERROR"; then
    echo "CREATE SPACE failed: $out"
    return 1
  fi

  for _ in {1..60}; do
    out=$(run_ngql "USE ${SPACE_NAME};" 2>&1 || true)
    if ! echo "$out" | rg -q "\[ERROR"; then
      break
    fi
    sleep 1
  done
  if echo "$out" | rg -q "\[ERROR"; then
    echo "USE space failed: $out"
    return 1
  fi

  out=$(run_ngql_block "USE ${SPACE_NAME};\nCREATE TAG IF NOT EXISTS person(name string);\nCREATE EDGE IF NOT EXISTS follow(weight int);")
  if echo "$out" | rg -q "\[ERROR"; then
    echo "CREATE SCHEMA failed: $out"
    return 1
  fi

  wait_schema_ready "DESCRIBE TAG person;" "TAG person schema"
  wait_schema_ready "DESCRIBE EDGE follow;" "EDGE follow schema"
  wait_data_write_ready
}

gen_data_files() {
  python3 - <<PY
from pathlib import Path

runtime = Path(r"$RUNTIME_DIR")
load_file = Path(r"$LOAD_DATA_FILE")
heavy_file = Path(r"$HEAVY_QUERY_FILE")
match_file = Path(r"$MATCH_QUERY_FILE")

N = int($VERTEX_COUNT)
DEG = int($EDGE_DEGREE)
BATCH = int($INSERT_BATCH)
space = "$SPACE_NAME"

def vid(i):
    return f'v{i:06d}'

with load_file.open('w', encoding='utf-8') as f:
    f.write(f"USE {space};\n")

    vals = []
    for i in range(1, N + 1):
        vals.append(f'"{vid(i)}":("u{i:06d}")')
        if len(vals) >= BATCH:
            f.write("INSERT VERTEX person(name) VALUES " + ",".join(vals) + ";\n")
            vals = []
    if vals:
        f.write("INSERT VERTEX person(name) VALUES " + ",".join(vals) + ";\n")

    edges = []
    for i in range(1, N + 1):
        for d in range(1, DEG + 1):
            j = ((i + d - 1) % N) + 1
            edges.append(f'"{vid(i)}"->"{vid(j)}":({d})')
            if len(edges) >= BATCH:
                f.write("INSERT EDGE follow(weight) VALUES " + ",".join(edges) + ";\n")
                edges = []
    if edges:
        f.write("INSERT EDGE follow(weight) VALUES " + ",".join(edges) + ";\n")

srcs = [f'"{vid(i)}"' for i in range(1, min(260, N) + 1)]
with heavy_file.open('w', encoding='utf-8') as f:
    f.write(f"USE {space};\n")
    f.write("GO 1 TO 2 STEPS FROM " + ",".join(srcs) + " OVER follow YIELD dst(edge) AS v;\n")

with match_file.open('w', encoding='utf-8') as f:
    f.write(f"USE {space};\n")
    f.write("MATCH (v1:person)-[:follow]->(v2:person)-[:follow]->(v3:person)-[:follow]->(v4:person) RETURN id(v4);\n")
PY
}

load_data() {
  local attempt
  local last_out="$RESULT_DIR/load_data.out"
  for attempt in {1..5}; do
    local attempt_out="$RESULT_DIR/load_data_attempt_${attempt}.out"
    run_ngql_file "$LOAD_DATA_FILE" >"$attempt_out" 2>&1 || true
    cp "$attempt_out" "$last_out"

    if ! rg -q "\[ERROR" "$attempt_out"; then
      return 0
    fi

    if ! rg -q "Not the leader" "$attempt_out"; then
      echo "Data load failed, see $attempt_out"
      return 1
    fi
    sleep 2
  done

  echo "Data load failed after retries, see $last_out"
  return 1
}

make_graph_conf() {
  local out_conf="$1"
  local inflight="$2"
  local rows_check="$3"
  local watermark="$4"
  local force_guard="${5:-false}"

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
  sudo_cmd install -m 444 "$conf" "$BASE_GRAPH_CONF"
}

restart_graphd() {
  sudo_cmd "$SERVICE" stop graphd >/dev/null 2>&1 || true
  sleep 1
  sudo_cmd "$SERVICE" start graphd >/dev/null 2>&1 || true
  sleep 2
}

restore_base_graph_conf() {
  local backup="$1"
  [[ -f "$backup" ]] || return 0
  sudo_cmd install -m 444 "$backup" "$BASE_GRAPH_CONF" >/dev/null
  restart_graphd
}

graph_pid_from_conf() {
  local conf="$1"
  local pid_file
  pid_file=$(awk -F= '/^--pid_file=/{print $2; exit}' "$conf")
  [[ -n "$pid_file" ]] || return 1
  if [[ "$pid_file" != /* ]]; then
    pid_file="/usr/local/nebula/${pid_file}"
  fi
  [[ -f "$pid_file" ]] || return 1
  cat "$pid_file"
}

sample_graph_mem() {
  local conf="$1"
  local out_file="$2"
  local seconds="$3"
  : > "$out_file"

  local pid
  pid=$(graph_pid_from_conf "$conf" || true)
  [[ -n "$pid" ]] || return 0

  for ((t=1; t<=seconds; t++)); do
    rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    [[ -n "$rss_kb" ]] && echo "$t $rss_kb" >> "$out_file"
    sleep 1
  done
}

precheck_expected_oom() {
  local case_name="$1"
  local case_dir="$RESULT_DIR/$case_name"
  : > "$case_dir/precheck_oom.out"

  for _ in {1..20}; do
    set +e
    out=$(timeout "${QUERY_TIMEOUT_SEC}s" "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$HEAVY_QUERY_FILE" 2>&1)
    set -e
    echo "$out" >> "$case_dir/precheck_oom.out"
    if echo "$out" | rg -q "$OOM_PATTERN"; then
      return 0
    fi
    sleep 1
  done
  return 1
}

run_case_workers() {
  local case_name="$1"
  local duration="$2"
  local workers="$3"
  local case_dir="$RESULT_DIR/$case_name"
  mkdir -p "$case_dir"

  local worker_pids=()
  for ((w=1; w<=workers; w++)); do
    (
      end=$((SECONDS + duration))
      succ=0
      fail=0
      oom=0
      timeout_cnt=0
      other=0
      while ((SECONDS < end)); do
        set +e
        out=$(timeout "${QUERY_TIMEOUT_SEC}s" "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$HEAVY_QUERY_FILE" 2>&1)
        rc=$?
        set -e

        if echo "$out" | rg -q "$OOM_PATTERN"; then
          oom=$((oom + 1)); fail=$((fail + 1))
        elif ((rc == 124)); then
          timeout_cnt=$((timeout_cnt + 1)); fail=$((fail + 1))
        elif ((rc == 0)) && ! echo "$out" | rg -q "\[ERROR|SemanticError|SpaceNotFound"; then
          succ=$((succ + 1))
        else
          other=$((other + 1)); fail=$((fail + 1))
        fi
      done
      echo "$succ $fail $oom $timeout_cnt $other" > "$case_dir/worker_${w}.txt"
    ) &
    worker_pids+=($!)
  done

  for pid in "${worker_pids[@]}"; do
    wait "$pid"
  done
}

classify_query_result() {
  local out="$1"
  local rc="$2"
  local case_dir="$3"
  local prefix="$4"
  local -n succ_ref="$5"
  local -n fail_ref="$6"
  local -n oom_ref="$7"
  local -n timeout_ref="$8"
  local -n other_ref="$9"

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
  echo "$out" > "$case_dir/${prefix}.out"
}

run_single_query_case() {
  local case_name="$1"
  local query_file="$2"
  local timeout_sec="$3"
  local attempts="$4"
  local hog_mode="$5"
  local hog_delay="$6"
  local hog_mb="$7"
  local restart_each_attempt="${8:-N}"
  local settle_ratio="${9:-0.00}"
  local case_dir="$RESULT_DIR/$case_name"
  mkdir -p "$case_dir"
  : > "$case_dir/graph_log_delta.log"

  local succ=0 fail=0 oom=0 timeout_cnt=0 other=0
  local guard_logs=0

  for ((a=1; a<=attempts; a++)); do
    if [[ "$restart_each_attempt" == "Y" ]]; then
      restart_graphd
      wait_graph_ready
    fi
    if [[ "$settle_ratio" != "0.00" ]]; then
      wait_used_ratio_below "$settle_ratio" 60 || true
    fi

    local log_start_line
    log_start_line=$(log_line_count)

    local hog_pid=""
    if [[ "$hog_mode" == "auto" ]]; then
      hog_pid=$(start_memory_hog "$case_dir" "$hog_mb" "$hog_delay" "$timeout_sec")
    fi

    set +e
    out=$(timeout "${timeout_sec}s" "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$query_file" 2>&1)
    rc=$?
    set -e

    if [[ -n "$hog_pid" ]]; then
      kill "$hog_pid" >/dev/null 2>&1 || true
      wait "$hog_pid" >/dev/null 2>&1 || true
    fi

    classify_query_result "$out" "$rc" "$case_dir" "attempt_${a}" succ fail oom timeout_cnt other

    local attempt_log="$case_dir/graph_log_delta_attempt_${a}.log"
    dump_case_log_delta "$case_dir" "$log_start_line" "$attempt_log"
    cat "$attempt_log" >> "$case_dir/graph_log_delta.log"
    local attempt_guard
    attempt_guard=$(grep -F -c "$GUARD_LOG_TOKEN" "$attempt_log" || true)
    guard_logs=$((guard_logs + attempt_guard))
  done

  echo "$succ $fail $oom $timeout_cnt $other" > "$case_dir/worker_1.txt"
  echo "$guard_logs" > "$case_dir/guard_logs_override.txt"
}

summarize_case() {
  local case_name="$1"
  local expected_trigger="$2"
  local expect_guard_log="$3"
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
  if [[ -f "$case_dir/guard_logs_override.txt" ]]; then
    guard_logs=$(cat "$case_dir/guard_logs_override.txt")
  elif [[ -f "$case_dir/graph_log_delta.log" ]]; then
    guard_logs=$(grep -F -c "$GUARD_LOG_TOKEN" "$case_dir/graph_log_delta.log" || true)
  fi

  local pass="PASS"
  if [[ "$expected_trigger" == "Y" ]]; then
    ((oom > 0 && guard_logs > 0)) || pass="FAIL"
  elif [[ "$expected_trigger" == "E" ]]; then
    ((oom > 0 && guard_logs == 0)) || pass="FAIL"
  else
    ((oom == 0 && fail == 0 && guard_logs == 0)) || pass="FAIL"
  fi

  if [[ "$expect_guard_log" == "Y" ]]; then
    ((guard_logs > 0)) || pass="FAIL"
  fi

  echo "$case_name,$expected_trigger,$expect_guard_log,$pass,$succ,$fail,$oom,$timeout_cnt,$other,$guard_logs,$peak_kb,$avg_kb" >> "$RESULT_DIR/summary.csv"
}

write_report() {
  local now
  now="$(date '+%Y-%m-%d %H:%M:%S')"

  cat > "$REPORT_PATH" <<EOR
# NebulaGraph OOM 保护功能定向测试报告

- 执行时间: ${now}
- 环境: /usr/local/nebula service 管理模式
- 图空间: ${SPACE_NAME}
- 压测数据规模: vertex=${VERTEX_COUNT}, edge_degree=${EDGE_DEGREE}, 近似边数=$((VERTEX_COUNT * EDGE_DEGREE))
- 并发与时长: workers=${LOAD_WORKERS}, 每 case ${CASE_DURATION}s

## 测试目标

1. 验证“内存不足主动终止查询”保护可以被稳定命中（OOMProtected > 0 且出现关键日志）。
2. 验证基线 case 不误触发保护（避免误杀查询）。
3. 验证恢复 case（从低水位触发恢复到常规水位）可正常返回成功查询。
4. 区分“执行期触发”与“启动期拒绝”两类错误路径。
5. 通过 graphd 增量日志确认触发点来自 checkMemoryAndAbortQuery。

## 用例设计

- \`case_baseline_control\`: 常规阈值 + GO 查询基线，不应触发。
- \`case_runtime_guard_match\`: MATCH 长查询 + 测试强制触发开关，目标是稳定命中 checkMemoryAndAbortQuery 并出现关键日志。
- \`case_start_phase_reject_control\`: 低水位 + 无内存干扰，验证启动期拒绝（应 OOM 但不应出现关键日志）。
- \`case_recovery_normal\`: 恢复常规阈值后，应恢复稳定成功。

## 结果汇总

| Case | ExpectTrigger | ExpectGuardLog | Verdict | Success | Fail | OOMProtected | Timeout | OtherFail | GuardLogs | PeakRSS(KB) | AvgRSS(KB) |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
EOR

  while IFS=, read -r case_name expected_trigger expect_guard_log verdict succ fail oom timeout_cnt other guard_logs peak avg; do
    printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
      "$case_name" "$expected_trigger" "$expect_guard_log" "$verdict" "$succ" "$fail" "$oom" "$timeout_cnt" "$other" "$guard_logs" "$peak" "$avg" >> "$REPORT_PATH"
  done < <(tail -n +2 "$RESULT_DIR/summary.csv")

  cat >> "$REPORT_PATH" <<'EOR'

## 判读标准

- 执行期触发用例（ExpectTrigger=Y）：`OOMProtected > 0` 且 `GuardLogs > 0` 判定通过。
- 启动期拒绝用例（ExpectTrigger=E）：`OOMProtected > 0` 且 `GuardLogs = 0` 判定通过。
- 非触发型用例（ExpectTrigger=N）：`Fail=0 且 OOMProtected=0 且 GuardLogs=0` 判定通过。

## 产物目录

- 结果目录: `oom_protection_targeted_runtime/results/`
- case 配置: `oom_protection_targeted_runtime/conf/`
- 重载数据日志: `oom_protection_targeted_runtime/results/load_data*.out`
- case 增量日志: `oom_protection_targeted_runtime/results/<case>/graph_log_delta.log`

EOR
}

main() {
  rm -rf "$RUNTIME_DIR"
  mkdir -p "$CONF_DIR" "$RESULT_DIR" "$LOG_DIR"

  local base_graph_backup="$RUNTIME_DIR/nebula-graphd.conf.base.bak"
  cp "$BASE_GRAPH_CONF" "$base_graph_backup"
  GRAPH_CONF_TEMPLATE="$base_graph_backup"
  trap "restore_base_graph_conf '$base_graph_backup'" EXIT

  require_env
  sudo_cmd "$SERVICE" start all >/dev/null 2>&1 || true
  wait_graph_ready
  ensure_hosts_online
  prepare_space
  wait_space_leader_ready

  gen_data_files
  load_data

  echo "case,expected_trigger,expect_guard_log,verdict,success,fail,oom,timeout,other,guard_logs,peak_kb,avg_kb" > "$RESULT_DIR/summary.csv"

  CASES=(
    "case_baseline_control workers_go 8 256 0.80 false N N none 0 0 1 ${QUERY_TIMEOUT_SEC} N 0.00"
    "case_runtime_guard_match single_match 8 16 0.80 true Y Y none 0 0 2 ${MATCH_QUERY_TIMEOUT_SEC} Y 0.00"
    "case_start_phase_reject_control single_go 8 256 0.28 false E N none 0 0 2 ${QUERY_TIMEOUT_SEC} N 0.00"
    "case_recovery_normal workers_go 8 256 0.80 false N N none 0 0 1 ${QUERY_TIMEOUT_SEC} N 0.00"
  )

  for c in "${CASES[@]}"; do
    read -r case_name mode inflight rows_check watermark_token force_guard expected_trigger expect_guard_log hog_mode hog_delay hog_mb attempts query_timeout restart_each_attempt settle_ratio <<<"$c"
    local watermark
    watermark=$(resolve_case_watermark "$watermark_token")
    echo ">>> running ${case_name} mode=${mode} inflight=${inflight} rows_check=${rows_check} wm=${watermark} expect_trigger=${expected_trigger} expect_guard_log=${expect_guard_log}"

    local conf="$CONF_DIR/${case_name}.graphd.conf"
    make_graph_conf "$conf" "$inflight" "$rows_check" "$watermark" "$force_guard"

    install_graph_conf "$conf"
    restart_graphd
    wait_graph_ready

    mkdir -p "$RESULT_DIR/$case_name"
    local case_dir="$RESULT_DIR/$case_name"

    if [[ "$mode" == "workers_go" ]]; then
      local log_start_line
      log_start_line=$(log_line_count)
      sample_graph_mem "$conf" "$RESULT_DIR/$case_name/mem.txt" "$CASE_DURATION" &
      local sampler_pid=$!
      run_case_workers "$case_name" "$CASE_DURATION" "$LOAD_WORKERS"
      wait "$sampler_pid" || true
      dump_case_log_delta "$case_dir" "$log_start_line"
    else
      local query_file="$HEAVY_QUERY_FILE"
      if [[ "$mode" == "single_match" ]]; then
        query_file="$MATCH_QUERY_FILE"
      fi
      local sample_seconds=$((query_timeout * attempts))
      sample_graph_mem "$conf" "$RESULT_DIR/$case_name/mem.txt" "$sample_seconds" &
      local sampler_pid=$!
      run_single_query_case "$case_name" "$query_file" "$query_timeout" "$attempts" "$hog_mode" "$hog_delay" "$hog_mb" "$restart_each_attempt" "$settle_ratio"
      wait "$sampler_pid" || true
    fi

    summarize_case "$case_name" "$expected_trigger" "$expect_guard_log"
  done

  write_report
  echo "Report generated: $REPORT_PATH"
}

main "$@"

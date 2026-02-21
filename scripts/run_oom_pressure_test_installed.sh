#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONSOLE="/home/sch/nebula/nebula-console"
SERVICE="/usr/local/nebula/scripts/nebula.service"
BASE_GRAPH_CONF="/usr/local/nebula/etc/nebula-graphd.conf"
GRAPH_CONF_TEMPLATE="$BASE_GRAPH_CONF"
SUDO_PASS="1016"

RUNTIME_DIR="$ROOT_DIR/oom_pressure_runtime_installed"
CONF_DIR="$RUNTIME_DIR/conf"
RESULT_DIR="$RUNTIME_DIR/results"
LOG_DIR="$RUNTIME_DIR/logs"
REPORT_PATH="$ROOT_DIR/NebulaGraph_OOM_Pressure_Execution_Report.md"

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

HEAVY_QUERY_FILE="$RUNTIME_DIR/heavy_query.ngql"
LOAD_DATA_FILE="$RUNTIME_DIR/load_data.ngql"

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

srcs = [f'"{vid(i)}"' for i in range(1, min(220, N) + 1)]
with heavy_file.open('w', encoding='utf-8') as f:
    f.write(f"USE {space};\n")
    f.write("GO 1 TO 2 STEPS FROM " + ",".join(srcs) + " OVER follow YIELD dst(edge) AS v;\n")
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

    # Retry only transient leader transfer errors during fresh space initialization.
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

  cp "$GRAPH_CONF_TEMPLATE" "$out_conf"
  chmod u+w "$out_conf"
  {
    echo "--max_storage_inflight_per_query=${inflight}"
    echo "--num_rows_to_check_memory=${rows_check}"
    echo "--system_memory_high_watermark_ratio=${watermark}"
  } >> "$out_conf"
}

install_graph_conf() {
  local conf="$1"
  sudo_cmd install -m 444 "$conf" "$BASE_GRAPH_CONF"
}

restore_base_graph_conf() {
  local backup="$1"
  [[ -f "$backup" ]] || return 0
  sudo_cmd install -m 444 "$backup" "$BASE_GRAPH_CONF" >/dev/null
  sudo_cmd "$SERVICE" restart graphd >/dev/null || true
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
      while ((SECONDS < end)); do
        set +e
        out=$(timeout "${QUERY_TIMEOUT_SEC}s" "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$HEAVY_QUERY_FILE" 2>&1)
        rc=$?
        set -e

        if echo "$out" | rg -q "E_GRAPH_MEMORY_EXCEEDED|GraphMemoryExceeded"; then
          oom=$((oom + 1)); fail=$((fail + 1))
        elif ((rc == 124)); then
          timeout_cnt=$((timeout_cnt + 1)); fail=$((fail + 1))
        elif ((rc == 0)) && ! echo "$out" | rg -q "\[ERROR|SemanticError|SpaceNotFound"; then
          succ=$((succ + 1))
        else
          fail=$((fail + 1))
        fi
      done
      echo "$succ $fail $oom $timeout_cnt" > "$case_dir/worker_${w}.txt"
    ) &
    worker_pids+=($!)
  done

  for pid in "${worker_pids[@]}"; do
    wait "$pid"
  done
}

summarize_case() {
  local case_name="$1"
  local case_dir="$RESULT_DIR/$case_name"
  local mem_file="$case_dir/mem.txt"

  local succ=0 fail=0 oom=0 timeout_cnt=0
  while read -r s f o t; do
    succ=$((succ + s)); fail=$((fail + f)); oom=$((oom + o)); timeout_cnt=$((timeout_cnt + t))
  done < <(cat "$case_dir"/worker_*.txt)

  local peak_kb=0 avg_kb=0 samples=0 sum=0
  while read -r _ rss; do
    [[ -z "$rss" ]] && continue
    (( rss > peak_kb )) && peak_kb=$rss
    sum=$((sum + rss)); samples=$((samples + 1))
  done < "$mem_file"
  ((samples > 0)) && avg_kb=$((sum / samples))

  echo "$case_name,$succ,$fail,$oom,$timeout_cnt,$peak_kb,$avg_kb" >> "$RESULT_DIR/summary.csv"
}

write_report() {
  local now
  now="$(date '+%Y-%m-%d %H:%M:%S')"
  cat > "$REPORT_PATH" <<EOR
# NebulaGraph OOM 防护压测执行报告

- 执行时间: ${now}
- 环境: /usr/local/nebula service 管理模式
- 图空间: ${SPACE_NAME}
- 压测数据规模: vertex=${VERTEX_COUNT}, edge_degree=${EDGE_DEGREE}, 近似边数=$((VERTEX_COUNT * EDGE_DEGREE))
- 并发与时长: workers=${LOAD_WORKERS}, 每 case ${CASE_DURATION}s

## 执行步骤

1. 使用 \`/usr/local/nebula/scripts/nebula.service start all\` 启动集群。
2. 执行 \`SHOW HOSTS\`，并执行 \`ADD HOSTS "127.0.0.1":9779\`（若已存在则忽略）。
3. 创建压测空间与 schema，导入测试数据。
4. 逐 case 重启 graphd（自定义 config），执行高并发重查询压测并采样 RSS。

## 结果汇总

| Case | Success | Fail | OOMProtected | Timeout | PeakRSS(KB) | AvgRSS(KB) |
|---|---:|---:|---:|---:|---:|---:|
EOR

  while IFS=, read -r case_name succ fail oom timeout_cnt peak avg; do
    printf '| %s | %s | %s | %s | %s | %s | %s |\n' "$case_name" "$succ" "$fail" "$oom" "$timeout_cnt" "$peak" "$avg" >> "$REPORT_PATH"
  done < <(tail -n +2 "$RESULT_DIR/summary.csv")

  cat >> "$REPORT_PATH" <<'EOR'

## 判读

- `OOMProtected` > 0：命中“内存不足主动终止查询”保护。
- `PeakRSS` 越低：峰值内存压力越小。
- `Timeout` 越高：说明查询时延风险上升，需要权衡保护强度。

## 产物

- 明细结果: `oom_pressure_runtime_installed/results/`
- 每 case graphd 配置: `oom_pressure_runtime_installed/conf/`
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

  echo "case,success,fail,oom,timeout,peak_kb,avg_kb" > "$RESULT_DIR/summary.csv"

  CASES=(
    "case0_unlimited 0 1024 0.80"
    "case1_inflight16 16 1024 0.80"
    "case2_inflight8_rows256 8 256 0.80"
    "case3_inflight4_rows256 4 256 0.80"
    "case4_inflight8_rows256_wm075 8 256 0.75"
  )

  for c in "${CASES[@]}"; do
    read -r case_name inflight rows_check watermark <<<"$c"
    echo ">>> running ${case_name} inflight=${inflight} rows_check=${rows_check} wm=${watermark}"

    conf="$CONF_DIR/${case_name}.graphd.conf"
    make_graph_conf "$conf" "$inflight" "$rows_check" "$watermark"

    install_graph_conf "$conf"
    sudo_cmd "$SERVICE" restart graphd >/dev/null
    wait_graph_ready

    mkdir -p "$RESULT_DIR/$case_name"
    sample_graph_mem "$conf" "$RESULT_DIR/$case_name/mem.txt" "$CASE_DURATION" &
    sampler_pid=$!

    run_case_workers "$case_name" "$CASE_DURATION" "$LOAD_WORKERS"
    wait "$sampler_pid" || true

    summarize_case "$case_name"
  done

  write_report
  echo "Report generated: $REPORT_PATH"
}

main "$@"

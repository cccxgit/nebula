#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/build/bin"
CONSOLE="/home/sch/nebula/nebula-console"
RUNTIME_DIR="$ROOT_DIR/oom_pressure_runtime"
CONF_DIR="$RUNTIME_DIR/conf"
DATA_DIR="$RUNTIME_DIR/data"
LOG_DIR="$RUNTIME_DIR/logs"
PID_DIR="$RUNTIME_DIR/pids"
RESULT_DIR="$RUNTIME_DIR/results"
REPORT_PATH="$ROOT_DIR/NebulaGraph_OOM_Pressure_Execution_Report.md"
TZ_FILE="$ROOT_DIR/resources/date_time_zonespec.csv"

GRAPH_ADDR="127.0.0.1"
GRAPH_PORT=9669
META_PORT=9559
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

GRAPH_PID=""
META_PID=""
STORAGE_PID=""

rm -rf "$RUNTIME_DIR"
mkdir -p "$CONF_DIR" "$DATA_DIR/meta" "$DATA_DIR/storage" "$LOG_DIR" "$PID_DIR" "$RESULT_DIR"

cleanup() {
  set +e
  if [[ -n "$GRAPH_PID" ]] && kill -0 "$GRAPH_PID" 2>/dev/null; then
    kill "$GRAPH_PID" 2>/dev/null
    wait "$GRAPH_PID" 2>/dev/null
  fi
  if [[ -n "$STORAGE_PID" ]] && kill -0 "$STORAGE_PID" 2>/dev/null; then
    kill "$STORAGE_PID" 2>/dev/null
    wait "$STORAGE_PID" 2>/dev/null
  fi
  if [[ -n "$META_PID" ]] && kill -0 "$META_PID" 2>/dev/null; then
    kill "$META_PID" 2>/dev/null
    wait "$META_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

require_bins() {
  [[ -x "$BIN_DIR/nebula-graphd" ]] || { echo "missing $BIN_DIR/nebula-graphd"; exit 1; }
  [[ -x "$BIN_DIR/nebula-metad" ]] || { echo "missing $BIN_DIR/nebula-metad"; exit 1; }
  [[ -x "$BIN_DIR/nebula-storaged" ]] || { echo "missing $BIN_DIR/nebula-storaged"; exit 1; }
  [[ -x "$CONSOLE" ]] || { echo "missing $CONSOLE"; exit 1; }
  [[ -f "$TZ_FILE" ]] || { echo "missing timezone file $TZ_FILE"; exit 1; }
}

write_conf_files() {
  cat > "$CONF_DIR/metad.conf" <<EOC
--daemonize=false
--local_ip=127.0.0.1
--meta_server_addrs=127.0.0.1:${META_PORT}
--port=${META_PORT}
--ws_http_port=19559
--pid_file=${PID_DIR}/nebula-metad.pid
--log_dir=${LOG_DIR}
--data_path=${DATA_DIR}/meta
--minloglevel=1
--v=0
--timezone_file=${TZ_FILE}
EOC

  cat > "$CONF_DIR/storaged.conf" <<EOC
--daemonize=false
--local_config=true
--local_ip=127.0.0.1
--meta_server_addrs=127.0.0.1:${META_PORT}
--port=${STORAGE_PORT}
--ws_http_port=19779
--pid_file=${PID_DIR}/nebula-storaged.pid
--log_dir=${LOG_DIR}
--data_path=${DATA_DIR}/storage
--heartbeat_interval_secs=1
--raft_heartbeat_interval_secs=5
--minloglevel=1
--v=0
--timezone_file=${TZ_FILE}
EOC

  cat > "$CONF_DIR/graphd.base.conf" <<EOC
--daemonize=false
--local_config=true
--meta_server_addrs=127.0.0.1:${META_PORT}
--local_ip=127.0.0.1
--port=${GRAPH_PORT}
--ws_http_port=19669
--pid_file=${PID_DIR}/nebula-graphd.pid
--log_dir=${LOG_DIR}
--enable_authorize=false
--num_worker_threads=4
--num_netio_threads=2
--num_rows_to_check_memory=1024
--system_memory_high_watermark_ratio=0.8
--max_storage_inflight_per_query=8
--storage_client_timeout_ms=120000
--minloglevel=2
--v=0
--timezone_file=${TZ_FILE}
EOC
}

run_ngql() {
  local q="$1"
  "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -e "$q"
}

run_ngql_file() {
  local f="$1"
  "$CONSOLE" -addr "$GRAPH_ADDR" -port "$GRAPH_PORT" -u root -p nebula -f "$f"
}

wait_graph_ready() {
  local retries=60
  for ((i=1; i<=retries; i++)); do
    if run_ngql "SHOW HOSTS;" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

start_metad() {
  "$BIN_DIR/nebula-metad" --flagfile="$CONF_DIR/metad.conf" >"$LOG_DIR/metad.run.log" 2>&1 &
  META_PID=$!
}

start_storaged() {
  "$BIN_DIR/nebula-storaged" --flagfile="$CONF_DIR/storaged.conf" >"$LOG_DIR/storaged.run.log" 2>&1 &
  STORAGE_PID=$!
}

start_graphd() {
  local inflight="$1"
  local rows_check="$2"
  local watermark="$3"
  "$BIN_DIR/nebula-graphd" \
    --flagfile="$CONF_DIR/graphd.base.conf" \
    --max_storage_inflight_per_query="$inflight" \
    --num_rows_to_check_memory="$rows_check" \
    --system_memory_high_watermark_ratio="$watermark" \
    >"$LOG_DIR/graphd.run.log" 2>&1 &
  GRAPH_PID=$!
}

stop_graphd() {
  if [[ -n "$GRAPH_PID" ]] && kill -0 "$GRAPH_PID" 2>/dev/null; then
    kill "$GRAPH_PID" 2>/dev/null || true
    wait "$GRAPH_PID" 2>/dev/null || true
  fi
  GRAPH_PID=""
}

prepare_space() {
  local out
  out=$(run_ngql "ADD HOSTS \"127.0.0.1\":${STORAGE_PORT};" 2>&1 || true)
  if echo "$out" | rg -q "\\[ERROR"; then
    if ! echo "$out" | rg -q "Existed"; then
      echo "ADD HOSTS failed: $out"
      return 1
    fi
  fi

  # wait host online
  for _ in {1..30}; do
    if run_ngql "SHOW HOSTS;" 2>/dev/null | rg -q "ONLINE"; then
      break
    fi
    sleep 1
  done

  run_ngql "DROP SPACE IF EXISTS ${SPACE_NAME};" >/dev/null 2>&1 || true
  out=$(run_ngql "CREATE SPACE IF NOT EXISTS ${SPACE_NAME}(partition_num=10, replica_factor=1, vid_type=FIXED_STRING(16));" 2>&1 || true)
  if echo "$out" | rg -q "\\[ERROR"; then
    echo "CREATE SPACE failed: $out"
    return 1
  fi

  for _ in {1..60}; do
    out=$(run_ngql "USE ${SPACE_NAME};" 2>&1 || true)
    if ! echo "$out" | rg -q "\\[ERROR"; then
      break
    fi
    sleep 1
  done
  if echo "$out" | rg -q "\\[ERROR"; then
    echo "USE ${SPACE_NAME} not ready: $out"
    return 1
  fi

  out=$(run_ngql "USE ${SPACE_NAME}; CREATE TAG IF NOT EXISTS person(name string); CREATE EDGE IF NOT EXISTS follow(weight int);" 2>&1 || true)
  if echo "$out" | rg -q "\\[ERROR"; then
    echo "Create schema failed: $out"
    return 1
  fi
  sleep 1
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

srcs = [f'"{vid(i)}"' for i in range(1, min(600, N) + 1)]
with heavy_file.open('w', encoding='utf-8') as f:
    f.write(f"USE {space};\n")
    srcs = srcs[:200]
    f.write("GO 1 TO 2 STEPS FROM " + ",".join(srcs) + " OVER follow YIELD dst(edge) AS v;\n")
PY
}

load_data() {
  run_ngql_file "$LOAD_DATA_FILE" >"$RESULT_DIR/load_data.out" 2>&1
  if rg -q "\\[ERROR" "$RESULT_DIR/load_data.out"; then
    echo "Data load failed, see $RESULT_DIR/load_data.out"
    return 1
  fi
}

sample_graph_mem() {
  local out_file="$1"
  local seconds="$2"
  : > "$out_file"
  for ((t=1; t<=seconds; t++)); do
    if kill -0 "$GRAPH_PID" 2>/dev/null; then
      rss_kb=$(ps -o rss= -p "$GRAPH_PID" 2>/dev/null | tr -d ' ')
      if [[ -n "$rss_kb" ]]; then
        echo "$t $rss_kb" >> "$out_file"
      fi
    fi
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
          oom=$((oom + 1))
          fail=$((fail + 1))
        elif ((rc == 124)); then
          timeout_cnt=$((timeout_cnt + 1))
          fail=$((fail + 1))
        elif ((rc == 0)) && ! echo "$out" | rg -q "\\[ERROR|SemanticError|SpaceNotFound"; then
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
    succ=$((succ + s))
    fail=$((fail + f))
    oom=$((oom + o))
    timeout_cnt=$((timeout_cnt + t))
  done < <(cat "$case_dir"/worker_*.txt)

  local peak_kb=0 avg_kb=0 samples=0 sum=0
  while read -r _ rss; do
    [[ -z "$rss" ]] && continue
    (( rss > peak_kb )) && peak_kb=$rss
    sum=$((sum + rss))
    samples=$((samples + 1))
  done < "$mem_file"
  if ((samples > 0)); then
    avg_kb=$((sum / samples))
  fi

  echo "$case_name,$succ,$fail,$oom,$timeout_cnt,$peak_kb,$avg_kb" >> "$RESULT_DIR/summary.csv"
}

write_report() {
  local now
  now="$(date '+%Y-%m-%d %H:%M:%S')"

  cat > "$REPORT_PATH" <<EOR
# NebulaGraph OOM 防护压测执行报告

- 执行时间: ${now}
- 执行目录: ${ROOT_DIR}
- 图空间: ${SPACE_NAME}
- 压测数据规模: vertex=${VERTEX_COUNT}, edge_degree=${EDGE_DEGREE}, 近似边数=$((VERTEX_COUNT * EDGE_DEGREE))
- 并发与时长: workers=${LOAD_WORKERS}, 每 case ${CASE_DURATION}s

## 测试方法

1. 启动本地单机集群（metad/storaged/graphd）。
2. 导入压测图数据，并固定重查询:
   - \
     \
	t	tGO 1 TO 3 STEPS FROM 600 个起点 OVER follow YIELD dst(edge) AS v;
3. 逐 case 重启 graphd，施加不同参数：
   - \
     \
	t	max_storage_inflight_per_query
   - \
     \
	t	num_rows_to_check_memory
   - \
     \
	t	system_memory_high_watermark_ratio
4. 每个 case 期间采样 graphd RSS，并统计查询成功/失败/内存超限次数。

## 结果汇总

| Case | Success | Fail | OOMProtected | Timeout | PeakRSS(KB) | AvgRSS(KB) |
|---|---:|---:|---:|---:|---:|---:|
EOR

  while IFS=, read -r case_name succ fail oom timeout_cnt peak avg; do
    printf '| %s | %s | %s | %s | %s | %s | %s |\n' "$case_name" "$succ" "$fail" "$oom" "$timeout_cnt" "$peak" "$avg" >> "$REPORT_PATH"
  done < <(tail -n +2 "$RESULT_DIR/summary.csv")

  cat >> "$REPORT_PATH" <<'EOR'

## 结论

- `OOMProtected` > 0 表示触发了“内存不足主动终止查询”而非进程被 OOM Killer 杀死。
- 对比 `PeakRSS` 可评估 in-flight 限流参数对峰值内存的抑制效果。
- 对比 `Success/Fail` 可评估防护强度与可用性的平衡点。

## 产物目录

- 逐 case 明细: `oom_pressure_runtime/results/`
- 运行日志: `oom_pressure_runtime/logs/`
- 导入数据脚本: `oom_pressure_runtime/load_data.ngql`
- 重查询脚本: `oom_pressure_runtime/heavy_query.ngql`
EOR
}

main() {
  require_bins
  write_conf_files
  start_metad
  sleep 2
  start_storaged
  sleep 3
  start_graphd 8 1024 0.8

  wait_graph_ready || { echo "graphd not ready"; exit 1; }

  prepare_space
  gen_data_files
  load_data

  echo "case,success,fail,oom,timeout,peak_kb,avg_kb" > "$RESULT_DIR/summary.csv"

  # case_name inflight rows_check watermark
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

    stop_graphd
    sleep 1
    start_graphd "$inflight" "$rows_check" "$watermark"
    wait_graph_ready || { echo "graphd not ready for $case_name"; exit 1; }

    mkdir -p "$RESULT_DIR/$case_name"
    sample_graph_mem "$RESULT_DIR/$case_name/mem.txt" "$CASE_DURATION" &
    sampler_pid=$!

    run_case_workers "$case_name" "$CASE_DURATION" "$LOAD_WORKERS"
    wait "$sampler_pid" || true

    summarize_case "$case_name"
  done

  write_report
  echo "Report generated: $REPORT_PATH"
}

main "$@"

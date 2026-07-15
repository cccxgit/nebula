#!/usr/bin/env bash
#
# Copyright (c) 2026 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

set -Eeuo pipefail
umask 077

readonly TOOL_VERSION="1.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly REPORT_TOOL="${SCRIPT_DIR}/lib/report.py"
readonly PROFILE_DIR="${SCRIPT_DIR}/profiles"
readonly GIB=$((1024 * 1024 * 1024))
readonly PIB=$((1024 * 1024 * 1024 * 1024 * 1024))
readonly DEFAULT_PROFILES="seq_write,seq_read,randread_8k_qd1,randread_8k,randrw_8k,compaction,raft_wal,wal_sync"

ACTION=""
TARGET_DIR=""
WAL_TARGET_DIR=""
OUTPUT_DIR="${PWD}/nebula-disk-results"
LABEL=""
PRESET="standard"
SIZE_OVERRIDE=""
RUNTIME_OVERRIDE=""
RAMP_OVERRIDE=""
REPEAT_OVERRIDE=""
JOBS_OVERRIDE=""
IODEPTH_OVERRIDE=""
COOLDOWN_OVERRIDE=""
READ_MIX=70
RANDSEED_BASE=20260714
PROFILES_CSV="${DEFAULT_PROFILES}"
FIO_BIN="fio"
NEBULA_CONFIG=""
NEBULA_HOME=""
ALLOW_ACTIVE_NEBULA=0
ALLOW_NON_DISK_FS=0
ASSUME_YES=0
KEEP_TEST_DATA=0
BASELINE=""
CANDIDATE=""
THRESHOLD_PERCENT=10

RUN_DIR=""
DATA_WORK_DIR=""
WAL_WORK_DIR=""
RUN_STATE="not-started"
IOSTAT_PID=""
PROBE_DIR=""
PROFILE_HASH=""

usage() {
    cat <<'EOF'
NebulaGraph disk benchmark for RocksDB and Raft WAL workloads.

Usage:
  nebula-disk-benchmark.sh check --target-dir DIR [options]
  nebula-disk-benchmark.sh run --target-dir DIR --label NAME [options]
  nebula-disk-benchmark.sh compare --baseline RESULT --candidate RESULT [options]

Run/check options:
  --target-dir DIR          Dedicated scratch directory on the data filesystem.
  --wal-target-dir DIR      Scratch directory on the WAL filesystem (default: target-dir).
  --nebula-config FILE      Record and validate relevant nebula-storaged configuration.
  --nebula-home DIR         Service working directory used to resolve relative data paths.
  --output-dir DIR          Result parent directory (default: ./nebula-disk-results).
  --label NAME              Environment label, required by run.
  --preset NAME             quick, standard, or extended (default: standard).
  --size SIZE               Override data file size; integer with K/M/G/T suffix.
  --runtime SEC             Measured seconds per profile.
  --ramp-time SEC           Warm-up seconds per profile.
  --repeat N                Repetitions; use at least 3 for comparisons.
  --jobs N                  Concurrent random-I/O jobs.
  --iodepth N               Queue depth per asynchronous job.
  --read-mix PERCENT        Read percentage in randrw_8k (default: 70).
  --cooldown SEC            Pause between profiles.
  --profiles LIST           Comma-separated profile names.
  --randseed N              Reproducible base random seed.
  --fio-bin PATH            Flexible I/O Tester binary.
  --allow-active-nebula     Permit a disruptive run while a storage daemon is active.
  --allow-non-disk-fs       Permit tmpfs/overlay for development smoke tests only.
  --keep-test-data          Preserve benchmark data files after the run.
  --yes                     Skip the destructive-load confirmation prompt.

Compare options:
  --baseline PATH           Baseline run directory or summary.json.
  --candidate PATH          Candidate run directory or summary.json.
  --output-dir DIR          Comparison output directory.
  --threshold-percent N     Similarity threshold (default: 10).

Profiles:
  seq_write, seq_read, randread_8k_qd1, randread_8k, randrw_8k,
  compaction, raft_wal, wal_sync
EOF
}

info() {
    printf '[INFO] %s\n' "$*"
}

warn() {
    printf '[WARN] %s\n' "$*" >&2
}

die() {
    printf '[ERROR] %s\n' "$*" >&2
    exit 1
}

require_option_value() {
    local option="$1"
    local value="${2:-}"
    [[ -n "${value}" && "${value}" != --* ]] || die "${option} requires a value"
}

parse_args() {
    [[ $# -gt 0 ]] || {
        usage
        exit 1
    }

    ACTION="$1"
    shift
    case "${ACTION}" in
        check|run|compare|help|-h|--help) ;;
        *) die "unknown command: ${ACTION}" ;;
    esac

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --target-dir)
                require_option_value "$1" "${2:-}"
                TARGET_DIR="$2"
                shift 2
                ;;
            --wal-target-dir)
                require_option_value "$1" "${2:-}"
                WAL_TARGET_DIR="$2"
                shift 2
                ;;
            --output-dir)
                require_option_value "$1" "${2:-}"
                OUTPUT_DIR="$2"
                shift 2
                ;;
            --label)
                require_option_value "$1" "${2:-}"
                LABEL="$2"
                shift 2
                ;;
            --preset)
                require_option_value "$1" "${2:-}"
                PRESET="$2"
                shift 2
                ;;
            --size)
                require_option_value "$1" "${2:-}"
                SIZE_OVERRIDE="$2"
                shift 2
                ;;
            --runtime)
                require_option_value "$1" "${2:-}"
                RUNTIME_OVERRIDE="$2"
                shift 2
                ;;
            --ramp-time)
                require_option_value "$1" "${2:-}"
                RAMP_OVERRIDE="$2"
                shift 2
                ;;
            --repeat)
                require_option_value "$1" "${2:-}"
                REPEAT_OVERRIDE="$2"
                shift 2
                ;;
            --jobs)
                require_option_value "$1" "${2:-}"
                JOBS_OVERRIDE="$2"
                shift 2
                ;;
            --iodepth)
                require_option_value "$1" "${2:-}"
                IODEPTH_OVERRIDE="$2"
                shift 2
                ;;
            --cooldown)
                require_option_value "$1" "${2:-}"
                COOLDOWN_OVERRIDE="$2"
                shift 2
                ;;
            --read-mix)
                require_option_value "$1" "${2:-}"
                READ_MIX="$2"
                shift 2
                ;;
            --profiles)
                require_option_value "$1" "${2:-}"
                PROFILES_CSV="$2"
                shift 2
                ;;
            --randseed)
                require_option_value "$1" "${2:-}"
                RANDSEED_BASE="$2"
                shift 2
                ;;
            --fio-bin)
                require_option_value "$1" "${2:-}"
                FIO_BIN="$2"
                shift 2
                ;;
            --nebula-config)
                require_option_value "$1" "${2:-}"
                NEBULA_CONFIG="$2"
                shift 2
                ;;
            --nebula-home)
                require_option_value "$1" "${2:-}"
                NEBULA_HOME="$2"
                shift 2
                ;;
            --baseline)
                require_option_value "$1" "${2:-}"
                BASELINE="$2"
                shift 2
                ;;
            --candidate)
                require_option_value "$1" "${2:-}"
                CANDIDATE="$2"
                shift 2
                ;;
            --threshold-percent)
                require_option_value "$1" "${2:-}"
                THRESHOLD_PERCENT="$2"
                shift 2
                ;;
            --allow-active-nebula)
                ALLOW_ACTIVE_NEBULA=1
                shift
                ;;
            --allow-non-disk-fs)
                ALLOW_NON_DISK_FS=1
                shift
                ;;
            --keep-test-data)
                KEEP_TEST_DATA=1
                shift
                ;;
            --yes)
                ASSUME_YES=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *) die "unknown option: $1" ;;
        esac
    done
}

apply_preset() {
    case "${PRESET}" in
        quick)
            TEST_SIZE="${SIZE_OVERRIDE:-1G}"
            RUNTIME="${RUNTIME_OVERRIDE:-15}"
            RAMP_TIME="${RAMP_OVERRIDE:-3}"
            REPEAT="${REPEAT_OVERRIDE:-1}"
            JOBS="${JOBS_OVERRIDE:-2}"
            IODEPTH="${IODEPTH_OVERRIDE:-4}"
            COOLDOWN="${COOLDOWN_OVERRIDE:-2}"
            ;;
        standard)
            TEST_SIZE="${SIZE_OVERRIDE:-8G}"
            RUNTIME="${RUNTIME_OVERRIDE:-60}"
            RAMP_TIME="${RAMP_OVERRIDE:-10}"
            REPEAT="${REPEAT_OVERRIDE:-3}"
            JOBS="${JOBS_OVERRIDE:-4}"
            IODEPTH="${IODEPTH_OVERRIDE:-8}"
            COOLDOWN="${COOLDOWN_OVERRIDE:-5}"
            ;;
        extended)
            TEST_SIZE="${SIZE_OVERRIDE:-32G}"
            RUNTIME="${RUNTIME_OVERRIDE:-180}"
            RAMP_TIME="${RAMP_OVERRIDE:-20}"
            REPEAT="${REPEAT_OVERRIDE:-3}"
            JOBS="${JOBS_OVERRIDE:-4}"
            IODEPTH="${IODEPTH_OVERRIDE:-16}"
            COOLDOWN="${COOLDOWN_OVERRIDE:-15}"
            ;;
        *) die "invalid preset: ${PRESET}" ;;
    esac
}

is_uint() {
    [[ "$1" =~ ^(0|[1-9][0-9]*)$ ]]
}

is_positive_uint() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

size_to_bytes() {
    local value="${1^^}"
    [[ "${value}" =~ ^([1-9][0-9]*)([KMGT]?)$ ]] || return 1
    local number="${BASH_REMATCH[1]}"
    local suffix="${BASH_REMATCH[2]}"
    local factor=1
    case "${suffix}" in
        K) factor=$((1024)) ;;
        M) factor=$((1024 * 1024)) ;;
        G) factor=$((1024 * 1024 * 1024)) ;;
        T) factor=$((1024 * 1024 * 1024 * 1024)) ;;
    esac
    local bytes=$((number * factor))
    ((bytes > 0 && bytes / factor == number)) || return 1
    printf '%s\n' "${bytes}"
}

profile_file() {
    case "$1" in
        seq_write) printf '%s/seq-write.fio\n' "${PROFILE_DIR}" ;;
        seq_read) printf '%s/seq-read.fio\n' "${PROFILE_DIR}" ;;
        randread_8k_qd1) printf '%s/randread-8k-qd1.fio\n' "${PROFILE_DIR}" ;;
        randread_8k) printf '%s/randread-8k.fio\n' "${PROFILE_DIR}" ;;
        randrw_8k) printf '%s/randrw-8k.fio\n' "${PROFILE_DIR}" ;;
        compaction) printf '%s/compaction.fio\n' "${PROFILE_DIR}" ;;
        raft_wal) printf '%s/raft-wal.fio\n' "${PROFILE_DIR}" ;;
        wal_sync) printf '%s/wal-sync.fio\n' "${PROFILE_DIR}" ;;
        *) return 1 ;;
    esac
}

profile_fingerprint() {
    local -a arguments=()
    local profile
    for profile in "${PROFILES[@]}"; do
        arguments+=("${profile}" "$(profile_file "${profile}")")
    done
    python3 - "${arguments[@]}" <<'PY'
import hashlib
import pathlib
import sys

digest = hashlib.sha256()
arguments = sys.argv[1:]
for index in range(0, len(arguments), 2):
    name = arguments[index].encode("utf-8")
    content = pathlib.Path(arguments[index + 1]).read_bytes()
    digest.update(len(name).to_bytes(4, "big"))
    digest.update(name)
    digest.update(len(content).to_bytes(8, "big"))
    digest.update(content)
print(digest.hexdigest())
PY
}

validate_options() {
    apply_preset
    TEST_SIZE="${TEST_SIZE^^}"
    TEST_SIZE_BYTES="$(size_to_bytes "${TEST_SIZE}")" || die "invalid --size: ${TEST_SIZE}"
    ((TEST_SIZE_BYTES >= 16 * 1024 * 1024)) || die "--size must be at least 16M"
    ((TEST_SIZE_BYTES <= PIB)) || die "--size must not exceed 1024T"

    is_positive_uint "${RUNTIME}" || die "--runtime must be a positive integer"
    is_uint "${RAMP_TIME}" || die "--ramp-time must be a non-negative integer"
    is_positive_uint "${REPEAT}" || die "--repeat must be a positive integer"
    is_positive_uint "${JOBS}" || die "--jobs must be a positive integer"
    is_positive_uint "${IODEPTH}" || die "--iodepth must be a positive integer"
    is_uint "${COOLDOWN}" || die "--cooldown must be a non-negative integer"
    is_positive_uint "${RANDSEED_BASE}" || die "--randseed must be a positive integer"
    is_uint "${READ_MIX}" || die "--read-mix must be an integer"
    ((READ_MIX > 0 && READ_MIX < 100)) || die "--read-mix must be between 1 and 99"

    IFS=',' read -r -a PROFILES <<<"${PROFILES_CSV}"
    ((${#PROFILES[@]} > 0)) || die "--profiles must not be empty"
    NEEDS_DATA=0
    NEEDS_COMPACTION_FILE=0
    NEEDS_WAL=0
    local profile
    local -A seen_profiles=()
    local index
    for index in "${!PROFILES[@]}"; do
        profile="${PROFILES[${index}]}"
        profile="${profile#"${profile%%[![:space:]]*}"}"
        profile="${profile%"${profile##*[![:space:]]}"}"
        PROFILES["${index}"]="${profile}"
        [[ -n "${profile}" ]] || die "--profiles contains an empty name"
        local job_file
        job_file="$(profile_file "${profile}")" || die "unknown profile: ${profile}"
        [[ -z "${seen_profiles[${profile}]+x}" ]] || die "duplicate profile: ${profile}"
        seen_profiles["${profile}"]=1
        [[ -r "${job_file}" ]] || die "missing fio profile: ${job_file}"
        case "${profile}" in
            raft_wal|wal_sync) NEEDS_WAL=1 ;;
            compaction)
                NEEDS_DATA=1
                NEEDS_COMPACTION_FILE=1
                ;;
            *) NEEDS_DATA=1 ;;
        esac
    done
    PROFILES_CSV="$(IFS=','; printf '%s' "${PROFILES[*]}")"

    if [[ "${ACTION}" == "run" ]]; then
        [[ -n "${LABEL}" ]] || die "--label is required for run"
        [[ "${LABEL}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] ||
            die "--label must use 1-64 letters, digits, dot, underscore, or dash"
    fi
}

canonical_existing_dir() {
    local path="$1"
    [[ -d "${path}" ]] || die "directory does not exist: ${path}"
    [[ -w "${path}" && -x "${path}" ]] || die "directory is not writable/searchable: ${path}"
    readlink -f -- "${path}"
}

canonical_searchable_dir() {
    local path="$1"
    [[ -d "${path}" ]] || die "directory does not exist: ${path}"
    [[ -x "${path}" ]] || die "directory is not searchable: ${path}"
    readlink -f -- "${path}"
}

path_is_within() {
    local child="${1%/}/"
    local parent="${2%/}/"
    [[ "${child}" == "${parent}"* ]]
}

config_value() {
    local key="$1"
    awk -v key="${key}" '
        /^[[:space:]]*#/ { next }
        $0 ~ "^[[:space:]]*--" key "=" {
            sub("^[[:space:]]*--" key "=", "")
            sub(/[[:space:]]*$/, "")
            value = $0
        }
        END { print value }
    ' "${NEBULA_CONFIG}"
}

validate_against_nebula_paths() {
    local target="$1"
    [[ -n "${NEBULA_CONFIG}" ]] || return 0
    [[ -r "${NEBULA_CONFIG}" && -f "${NEBULA_CONFIG}" ]] ||
        die "cannot read --nebula-config: ${NEBULA_CONFIG}"

    local key raw path resolved
    for key in data_path wal_path rocksdb_wal_dir; do
        raw="$(config_value "${key}")"
        [[ -n "${raw}" ]] || continue
        IFS=',' read -r -a configured_paths <<<"${raw}"
        for path in "${configured_paths[@]}"; do
            path="${path#"${path%%[![:space:]]*}"}"
            path="${path%"${path##*[![:space:]]}"}"
            [[ -n "${path}" ]] || continue
            if [[ "${path}" != /* ]]; then
                [[ -n "${NEBULA_HOME}" ]] ||
                    die "relative Nebula ${key} '${path}' requires --nebula-home"
                path="${NEBULA_HOME}/${path}"
            fi
            resolved="$(realpath -m -- "${path}")"
            if path_is_within "${target}" "${resolved}"; then
                die "scratch path ${target} is inside production ${key} ${resolved}; use a sibling directory on the same mount"
            fi
        done
    done
}

filesystem_type() {
    if command -v findmnt >/dev/null 2>&1; then
        findmnt -T "$1" -n -o FSTYPE 2>/dev/null | head -n 1
    else
        stat -f -c %T "$1" 2>/dev/null || true
    fi
}

available_bytes() {
    df -PB1 -- "$1" | awk 'NR == 2 {print $4}'
}

human_bytes() {
    local bytes="$1"
    if command -v numfmt >/dev/null 2>&1; then
        numfmt --to=iec-i --suffix=B "${bytes}"
    else
        printf '%s bytes' "${bytes}"
    fi
}

active_nebula_processes() {
    ps -eo pid=,comm=,args= | awk '
        {
            executable = $3
            sub(/^.*\//, "", executable)
            if (executable ~ /^nebula-(storaged|metad|standalone)$/ ||
                $2 ~ /^nebula-(storaged|metad|standal)/) {
                print
            }
        }
    '
}

resolve_fio() {
    local resolved
    resolved="$(command -v "${FIO_BIN}" 2>/dev/null || true)"
    [[ -n "${resolved}" && -x "${resolved}" ]] ||
        die "Flexible I/O Tester not found: ${FIO_BIN}; install the fio package"
    FIO_BIN="${resolved}"
    FIO_VERSION="$(${FIO_BIN} --version 2>/dev/null || true)"
    [[ "${FIO_VERSION}" == fio-* ]] ||
        die "${FIO_BIN} is not Flexible I/O Tester (unexpected version: ${FIO_VERSION:-none})"
    "${FIO_BIN}" --enghelp 2>/dev/null | grep -qw libaio || die "fio lacks the libaio engine"
    "${FIO_BIN}" --enghelp 2>/dev/null | grep -qw psync || die "fio lacks the psync engine"
}

validate_python() {
    command -v python3 >/dev/null 2>&1 || die "python3 is required"
    [[ -r "${REPORT_TOOL}" ]] || die "missing report tool: ${REPORT_TOOL}"
    python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' ||
        die "Python 3.8 or newer is required"
}

validate_filesystem() {
    local path="$1"
    local role="$2"
    local fstype
    fstype="$(filesystem_type "${path}")"
    case "${fstype}" in
        tmpfs|ramfs|overlay|overlayfs|aufs)
            if ((ALLOW_NON_DISK_FS == 0)); then
                die "${role} path ${path} is on ${fstype}, not a comparable disk filesystem"
            fi
            warn "${role} path is on ${fstype}; results are for development only"
            ;;
        nfs|nfs4|cifs|fuse*)
            warn "${role} path is on ${fstype}; results include network/server behavior"
            ;;
        "") warn "could not determine filesystem type for ${role} path ${path}" ;;
    esac
}

minimum_reserved_bytes() {
    local value=""
    if [[ -n "${NEBULA_CONFIG}" ]]; then
        value="$(config_value minimum_reserved_bytes)"
    fi
    if is_uint "${value}"; then
        printf '%s\n' "${value}"
    else
        printf '%s\n' "$((256 * 1024 * 1024))"
    fi
}

check_free_space() {
    local reserve available data_files required wal_size
    reserve="$(minimum_reserved_bytes)"
    wal_size="${TEST_SIZE_BYTES}"
    ((wal_size > GIB)) && wal_size="${GIB}"
    WAL_TEST_SIZE_BYTES="${wal_size}"

    data_files=0
    ((NEEDS_DATA == 1)) && data_files=1
    ((NEEDS_COMPACTION_FILE == 1)) && data_files=2
    required=0
    if ((data_files > 0)); then
        required=$((data_files * TEST_SIZE_BYTES + reserve + GIB))
    fi
    if ((NEEDS_WAL == 1)) && [[ "${TARGET_DIR}" == "${WAL_TARGET_DIR}" ]]; then
        if ((required > 0)); then
            required=$((required + WAL_TEST_SIZE_BYTES))
        else
            required=$((WAL_TEST_SIZE_BYTES + reserve + GIB))
        fi
    fi
    if ((required > 0)); then
        available="$(available_bytes "${TARGET_DIR}")"
        is_uint "${available}" || die "cannot determine free space for ${TARGET_DIR}"
        ((available >= required)) ||
            die "insufficient data filesystem space: need $(human_bytes "${required}"), available $(human_bytes "${available}")"
    fi

    if ((NEEDS_WAL == 1)) && [[ "${TARGET_DIR}" != "${WAL_TARGET_DIR}" ]]; then
        required=$((WAL_TEST_SIZE_BYTES + reserve + GIB))
        available="$(available_bytes "${WAL_TARGET_DIR}")"
        is_uint "${available}" || die "cannot determine free space for ${WAL_TARGET_DIR}"
        ((available >= required)) ||
            die "insufficient WAL filesystem space: need $(human_bytes "${required}"), available $(human_bytes "${available}")"
    fi
}

remove_owned_dir() {
    local path="$1"
    local parent="$2"
    [[ -n "${path}" && -d "${path}" ]] || return 0
    [[ -f "${path}/.nebula-disk-benchmark-owned" ]] || {
        warn "refusing to remove unowned directory: ${path}"
        return 1
    }
    path_is_within "${path}" "${parent}" || {
        warn "refusing to remove directory outside ${parent}: ${path}"
        return 1
    }
    rm -rf -- "${path}"
}

direct_io_probe() {
    local probe_json
    PROBE_DIR="$(mktemp -d "${TARGET_DIR}/.nebula-disk-probe.XXXXXX")"
    : >"${PROBE_DIR}/.nebula-disk-benchmark-owned"
    probe_json="${PROBE_DIR}/probe.json"
    if ! "${FIO_BIN}" \
        --name=direct_io_probe \
        --filename="${PROBE_DIR}/probe.bin" \
        --size=4k \
        --rw=write \
        --bs=4k \
        --ioengine=libaio \
        --iodepth=1 \
        --direct=1 \
        --output-format=json \
        --output="${probe_json}" \
        --eta=never >/dev/null 2>"${PROBE_DIR}/probe.stderr"; then
        cat "${PROBE_DIR}/probe.stderr" >&2 || true
        remove_owned_dir "${PROBE_DIR}" "${TARGET_DIR}" || true
        PROBE_DIR=""
        die "4 KiB direct-I/O probe failed on ${TARGET_DIR}"
    fi
    remove_owned_dir "${PROBE_DIR}" "${TARGET_DIR}"
    PROBE_DIR=""
}

prepare_paths() {
    [[ -n "${TARGET_DIR}" ]] || die "--target-dir is required"
    TARGET_DIR="$(canonical_existing_dir "${TARGET_DIR}")"
    [[ "${TARGET_DIR}" != "/" ]] || die "--target-dir must not be the root directory"
    [[ "${TARGET_DIR}" != *$'\n'* && "${TARGET_DIR}" != *:* ]] ||
        die "target path must not contain newline or colon"

    if [[ -z "${WAL_TARGET_DIR}" ]]; then
        WAL_TARGET_DIR="${TARGET_DIR}"
    else
        WAL_TARGET_DIR="$(canonical_existing_dir "${WAL_TARGET_DIR}")"
    fi
    [[ "${WAL_TARGET_DIR}" != "/" ]] || die "--wal-target-dir must not be the root directory"
    [[ "${WAL_TARGET_DIR}" != *$'\n'* && "${WAL_TARGET_DIR}" != *:* ]] ||
        die "WAL target path must not contain newline or colon"

    if [[ -n "${NEBULA_CONFIG}" ]]; then
        [[ -r "${NEBULA_CONFIG}" && -f "${NEBULA_CONFIG}" ]] ||
            die "cannot read --nebula-config: ${NEBULA_CONFIG}"
        NEBULA_CONFIG="$(readlink -f -- "${NEBULA_CONFIG}")"
        if [[ -n "${NEBULA_HOME}" ]]; then
            NEBULA_HOME="$(canonical_searchable_dir "${NEBULA_HOME}")"
        else
            local config_dir
            config_dir="$(dirname "${NEBULA_CONFIG}")"
            case "$(basename "${config_dir}")" in
                conf|etc)
                    local inferred_home
                    inferred_home="$(readlink -f -- "${config_dir}/..")"
                    if [[ -x "${inferred_home}/bin/nebula-storaged" ||
                          -x "${inferred_home}/bin/nebula-metad" ||
                          -x "${inferred_home}/bin/nebula-standalone" ]]; then
                        NEBULA_HOME="${inferred_home}"
                        info "inferred Nebula home for relative paths: ${NEBULA_HOME}"
                    fi
                    ;;
            esac
        fi
    elif [[ -n "${NEBULA_HOME}" ]]; then
        die "--nebula-home requires --nebula-config"
    fi
    validate_against_nebula_paths "${TARGET_DIR}"
    [[ "${WAL_TARGET_DIR}" == "${TARGET_DIR}" ]] ||
        validate_against_nebula_paths "${WAL_TARGET_DIR}"
    [[ ! -d "${TARGET_DIR}/nebula" ]] ||
        die "${TARGET_DIR} contains a Nebula data tree; use a dedicated sibling scratch directory"
    if [[ "${WAL_TARGET_DIR}" != "${TARGET_DIR}" ]]; then
        [[ ! -d "${WAL_TARGET_DIR}/nebula" ]] ||
            die "${WAL_TARGET_DIR} contains a Nebula WAL tree; use a dedicated sibling scratch directory"
    fi
}

preflight() {
    local for_run="$1"
    [[ "$(uname -s)" == "Linux" ]] || die "this benchmark supports Linux only"
    validate_python
    resolve_fio
    prepare_paths
    validate_filesystem "${TARGET_DIR}" "data"
    [[ "${WAL_TARGET_DIR}" == "${TARGET_DIR}" ]] || validate_filesystem "${WAL_TARGET_DIR}" "WAL"
    check_free_space

    local processes
    processes="$(active_nebula_processes)"
    if [[ -n "${processes}" ]]; then
        warn "active Nebula storage processes detected:"
        printf '%s\n' "${processes}" >&2
        if ((for_run == 1 && ALLOW_ACTIVE_NEBULA == 0)); then
            die "stop/drain storaged/metad/standalone first, or explicitly pass --allow-active-nebula"
        fi
    fi

    direct_io_probe
    info "fio: ${FIO_VERSION} (${FIO_BIN})"
    info "data target: ${TARGET_DIR} ($(filesystem_type "${TARGET_DIR}"))"
    info "WAL target: ${WAL_TARGET_DIR} ($(filesystem_type "${WAL_TARGET_DIR}"))"
    info "free-space and direct-I/O checks passed"
    [[ -n "${NEBULA_CONFIG}" ]] ||
        warn "no --nebula-config supplied; production path/config comparability cannot be checked"
}

confirm_run() {
    local estimated_seconds
    estimated_seconds=$((REPEAT * ${#PROFILES[@]} * (RUNTIME + RAMP_TIME + COOLDOWN)))
    printf '\n'
    warn "This is a saturating write/read benchmark and can increase application latency."
    info "preset=${PRESET}, size=${TEST_SIZE}, rounds=${REPEAT}, profiles=${#PROFILES[@]}"
    info "estimated timed phase: about $((estimated_seconds / 60))m $((estimated_seconds % 60))s, plus preconditioning"
    if ((ASSUME_YES == 1)); then
        return 0
    fi
    [[ -t 0 ]] || die "non-interactive run requires --yes"
    local answer
    read -r -p "Type RUN to continue: " answer
    [[ "${answer}" == "RUN" ]] || die "cancelled"
}

create_workspace() {
    local run_id timestamp
    timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
    run_id="${LABEL}-${timestamp}-$$"
    mkdir -p -- "${OUTPUT_DIR}"
    OUTPUT_DIR="$(readlink -f -- "${OUTPUT_DIR}")"
    RUN_DIR="${OUTPUT_DIR}/${run_id}"
    mkdir -- "${RUN_DIR}"

    DATA_WORK_DIR="${TARGET_DIR}/.nebula-disk-benchmark-${run_id}"
    mkdir -- "${DATA_WORK_DIR}"
    : >"${DATA_WORK_DIR}/.nebula-disk-benchmark-owned"
    if [[ "${WAL_TARGET_DIR}" == "${TARGET_DIR}" ]]; then
        WAL_WORK_DIR="${DATA_WORK_DIR}/wal"
        mkdir -- "${WAL_WORK_DIR}"
    else
        WAL_WORK_DIR="${WAL_TARGET_DIR}/.nebula-disk-benchmark-wal-${run_id}"
        mkdir -- "${WAL_WORK_DIR}"
        : >"${WAL_WORK_DIR}/.nebula-disk-benchmark-owned"
    fi

    mkdir -p -- "${RUN_DIR}/raw" "${RUN_DIR}/telemetry" "${RUN_DIR}/profiles"
    local profile source
    for profile in "${PROFILES[@]}"; do
        source="$(profile_file "${profile}")"
        cp -- "${source}" "${RUN_DIR}/profiles/"
    done
}

finish_manifest() {
    local status="$1"
    [[ -n "${RUN_DIR}" && -f "${RUN_DIR}/manifest.json" ]] || return 0
    python3 "${REPORT_TOOL}" finish-run \
        --manifest "${RUN_DIR}/manifest.json" \
        --status "${status}" >/dev/null 2>&1 || true
}

stop_iostat() {
    if [[ -n "${IOSTAT_PID}" ]]; then
        kill "${IOSTAT_PID}" >/dev/null 2>&1 || true
        wait "${IOSTAT_PID}" >/dev/null 2>&1 || true
        IOSTAT_PID=""
    fi
}

cleanup() {
    local exit_code=$?
    stop_iostat
    if [[ -n "${PROBE_DIR}" ]]; then
        remove_owned_dir "${PROBE_DIR}" "${TARGET_DIR}" || true
        PROBE_DIR=""
    fi
    if [[ "${RUN_STATE}" == "running" ]]; then
        finish_manifest failed
    fi
    if ((KEEP_TEST_DATA == 0)); then
        if [[ -n "${WAL_WORK_DIR}" && "${WAL_WORK_DIR}" != "${DATA_WORK_DIR}/wal" ]]; then
            remove_owned_dir "${WAL_WORK_DIR}" "${WAL_TARGET_DIR}" || true
        fi
        [[ -z "${DATA_WORK_DIR}" ]] || remove_owned_dir "${DATA_WORK_DIR}" "${TARGET_DIR}" || true
    elif [[ -n "${DATA_WORK_DIR}" ]]; then
        warn "benchmark data preserved at ${DATA_WORK_DIR}"
        [[ "${WAL_WORK_DIR}" == "${DATA_WORK_DIR}/wal" ]] ||
            warn "WAL benchmark data preserved at ${WAL_WORK_DIR}"
    fi
    return "${exit_code}"
}

init_result_metadata() {
    local -a init_args
    PROFILE_HASH="$(profile_fingerprint)"
    init_args=(
        init-run
        --output "${RUN_DIR}/manifest.json"
        --label "${LABEL}"
        --preset "${PRESET}"
        --size "${TEST_SIZE}"
        --runtime "${RUNTIME}"
        --ramp "${RAMP_TIME}"
        --cooldown "${COOLDOWN}"
        --repeat "${REPEAT}"
        --jobs "${JOBS}"
        --iodepth "${IODEPTH}"
        --profiles "${PROFILES_CSV}"
        --target "${TARGET_DIR}"
        --wal "${WAL_TARGET_DIR}"
        --tool-version "${TOOL_VERSION}"
        --read-mix "${READ_MIX}"
        --fio-version "${FIO_VERSION}"
        --randseed "${RANDSEED_BASE}"
        --profile-hash "${PROFILE_HASH}"
    )
    python3 "${REPORT_TOOL}" "${init_args[@]}"

    local -a env_args
    env_args=(
        collect-env
        --output "${RUN_DIR}/environment.json"
        --label "${LABEL}"
        --target-dir "${TARGET_DIR}"
        --wal-target-dir "${WAL_TARGET_DIR}"
        --fio-bin "${FIO_BIN}"
    )
    [[ -z "${NEBULA_CONFIG}" ]] || env_args+=(--nebula-config "${NEBULA_CONFIG}")
    [[ -z "${NEBULA_HOME}" ]] || env_args+=(--nebula-home "${NEBULA_HOME}")
    python3 "${REPORT_TOOL}" "${env_args[@]}"
}

precondition_data_file() {
    ((NEEDS_DATA == 1)) || return 0
    info "preconditioning ${TEST_SIZE} data file (not included in measured results)"
    "${FIO_BIN}" \
        --name=precondition \
        --filename="${DATA_FILE}" \
        --size="${TEST_SIZE}" \
        --rw=write \
        --bs=1m \
        --ioengine=libaio \
        --iodepth="${IODEPTH}" \
        --direct=1 \
        --fallocate=none \
        --refill_buffers=1 \
        --end_fsync=1 \
        --group_reporting=1 \
        --output-format=json \
        --output="${RUN_DIR}/raw/precondition.json" \
        --eta=never 2>"${RUN_DIR}/raw/precondition.stderr.log"
    python3 -c 'import json, sys; json.load(open(sys.argv[1], encoding="utf-8"))' \
        "${RUN_DIR}/raw/precondition.json"
}

start_iostat() {
    local output="$1"
    IOSTAT_PID=""
    command -v iostat >/dev/null 2>&1 || return 0
    iostat -xz 1 >"${output}" 2>&1 &
    IOSTAT_PID=$!
}

run_profile() {
    local profile="$1"
    local round_dir="$2"
    local telemetry_dir="$3"
    local job_file result_file stderr_file
    job_file="$(profile_file "${profile}")"
    result_file="${round_dir}/${profile}.json"
    stderr_file="${round_dir}/${profile}.stderr.log"

    case "${profile}" in
        compaction) rm -f -- "${COMPACTION_FILE}" ;;
        raft_wal|wal_sync) rm -f -- "${WAL_FILE}" ;;
    esac

    info "round ${ROUND}/${REPEAT}: ${profile}"
    start_iostat "${telemetry_dir}/${profile}.iostat.log"
    local status=0
    "${FIO_BIN}" "${job_file}" \
        --output-format=json \
        --output="${result_file}" \
        --eta=never 2>"${stderr_file}" || status=$?
    stop_iostat
    ((status == 0)) || {
        cat "${stderr_file}" >&2 || true
        die "fio profile ${profile} failed with exit code ${status}"
    }
    python3 -c 'import json, sys; json.load(open(sys.argv[1], encoding="utf-8"))' \
        "${result_file}" || die "invalid fio JSON from ${profile}"
}

run_benchmark() {
    preflight 1
    confirm_run
    create_workspace
    RUN_STATE="running"

    DATA_FILE="${DATA_WORK_DIR}/rocksdb-sst.bin"
    COMPACTION_FILE="${DATA_WORK_DIR}/compaction-output.bin"
    WAL_FILE="${WAL_WORK_DIR}/nebula-wal.bin"
    WAL_TEST_SIZE="${WAL_TEST_SIZE_BYTES}"
    export DATA_FILE COMPACTION_FILE WAL_FILE WAL_TEST_SIZE
    export TEST_SIZE RUNTIME RAMP_TIME JOBS IODEPTH READ_MIX

    init_result_metadata
    precondition_data_file

    local round_dir telemetry_dir profile
    for ((ROUND = 1; ROUND <= REPEAT; ROUND++)); do
        printf -v round_dir '%s/raw/round-%02d' "${RUN_DIR}" "${ROUND}"
        printf -v telemetry_dir '%s/telemetry/round-%02d' "${RUN_DIR}" "${ROUND}"
        mkdir -p -- "${round_dir}" "${telemetry_dir}"
        RANDSEED=$((RANDSEED_BASE + ROUND - 1))
        export RANDSEED
        for profile in "${PROFILES[@]}"; do
            run_profile "${profile}" "${round_dir}" "${telemetry_dir}"
            ((COOLDOWN == 0)) || sleep "${COOLDOWN}"
        done
    done

    finish_manifest complete
    python3 "${REPORT_TOOL}" summarize --run-dir "${RUN_DIR}"
    RUN_STATE="complete"
    info "benchmark complete: ${RUN_DIR}"
    info "human-readable report: ${RUN_DIR}/summary.md"
}

run_check() {
    preflight 0
    info "environment check complete; no benchmark workload was run"
}

run_compare() {
    validate_python
    [[ -n "${BASELINE}" ]] || die "--baseline is required for compare"
    [[ -n "${CANDIDATE}" ]] || die "--candidate is required for compare"
    is_uint "${THRESHOLD_PERCENT}" || die "--threshold-percent must be a non-negative integer"
    mkdir -p -- "${OUTPUT_DIR}"
    OUTPUT_DIR="$(readlink -f -- "${OUTPUT_DIR}")"
    python3 "${REPORT_TOOL}" compare \
        --baseline "${BASELINE}" \
        --candidate "${CANDIDATE}" \
        --output-dir "${OUTPUT_DIR}" \
        --threshold-percent "${THRESHOLD_PERCENT}"
    info "comparison report: ${OUTPUT_DIR}/comparison.md"
}

main() {
    parse_args "$@"
    case "${ACTION}" in
        help|-h|--help)
            usage
            ;;
        compare)
            run_compare
            ;;
        check|run)
            ((BASH_VERSINFO[0] >= 4)) || die "Bash 4 or newer is required"
            validate_options
            trap cleanup EXIT
            trap 'exit 130' INT
            trap 'exit 143' TERM
            if [[ "${ACTION}" == "check" ]]; then
                run_check
            else
                run_benchmark
            fi
            ;;
    esac
}

main "$@"

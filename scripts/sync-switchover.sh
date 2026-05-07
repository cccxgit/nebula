#!/usr/bin/env bash
#
# Copyright (c) 2024 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.
#
# sync-switchover.sh - Planned switchover for NebulaGraph cross-cluster sync.
#
# This script performs a graceful role swap between primary and secondary clusters.
# After switchover, the old secondary becomes the new primary and vice versa,
# with reverse sync direction established.
#
# Steps:
#   1.  Validate arguments
#   2.  SET read_only=true on old primary (quiesce writes)
#   3.  Poll SHOW SYNC STATUS until all partitions have lag=0
#   4.  Poll SHOW DRAINER SYNC STATUS until secondary is fully caught up
#   5.  REMOVE DRAINER on secondary
#   6.  SET read_only=false on secondary (now new primary)
#   7.  Increment epoch on new primary
#   8.  REMOVE LISTENER SYNC + SIGN OUT DRAINER SVC on old primary
#   9.  Establish reverse sync direction (listener on new primary, drainer on old)
#   10. Print completion summary
#
# Usage:
#   ./sync-switchover.sh \
#     --primary-meta <host:port[,host:port,...]> \
#     --secondary-meta <host:port[,host:port,...]> \
#     --space <space_name> \
#     [--timeout <seconds>] \
#     [--poll-interval <seconds>] \
#     [--nebula-console <path>] \
#     [--skip-reverse-sync]
#
# Requirements:
#   - nebula-console binary accessible
#   - Network connectivity to both clusters
#   - Both clusters healthy and sync operational
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/utils.sh"

# Defaults
NEBULA_CONSOLE="${NEBULA_CONSOLE:-nebula-console}"
PRIMARY_META=""
SECONDARY_META=""
SPACE_NAME=""
TIMEOUT=300
POLL_INTERVAL=2
SKIP_REVERSE_SYNC=0

##############################################################################
# Usage
##############################################################################
function usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Perform a planned switchover between primary and secondary sync clusters.

Required options:
  --primary-meta      Comma-separated meta addresses of current primary (host:port)
  --secondary-meta    Comma-separated meta addresses of current secondary (host:port)
  --space             Name of the graph space

Optional:
  --timeout           Max seconds to wait for sync to catch up (default: 300)
  --poll-interval     Seconds between status polls (default: 2)
  --nebula-console    Path to nebula-console binary (default: nebula-console)
  --skip-reverse-sync Skip establishing reverse sync after switchover
  -h, --help          Show this help message

Environment variables:
  NEBULA_CONSOLE      Path to nebula-console binary

Examples:
  $(basename "$0") \\
    --primary-meta 10.0.0.1:9559 \\
    --secondary-meta 10.0.1.1:9559 \\
    --space my_graph \\
    --timeout 600
EOF
    exit 1
}

##############################################################################
# Parse arguments
##############################################################################
function parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --primary-meta)
                PRIMARY_META="$2"; shift 2 ;;
            --secondary-meta)
                SECONDARY_META="$2"; shift 2 ;;
            --space)
                SPACE_NAME="$2"; shift 2 ;;
            --timeout)
                TIMEOUT="$2"; shift 2 ;;
            --poll-interval)
                POLL_INTERVAL="$2"; shift 2 ;;
            --nebula-console)
                NEBULA_CONSOLE="$2"; shift 2 ;;
            --skip-reverse-sync)
                SKIP_REVERSE_SYNC=1; shift ;;
            -h|--help)
                usage ;;
            *)
                ERROR "Unknown option: $1"
                usage ;;
        esac
    done
}

##############################################################################
# Validate arguments
##############################################################################
function validate_args() {
    local has_error=0

    if [[ -z "${PRIMARY_META}" ]]; then
        ERROR "Missing required option: --primary-meta"
        has_error=1
    fi
    if [[ -z "${SECONDARY_META}" ]]; then
        ERROR "Missing required option: --secondary-meta"
        has_error=1
    fi
    if [[ -z "${SPACE_NAME}" ]]; then
        ERROR "Missing required option: --space"
        has_error=1
    fi

    if ! command -v "${NEBULA_CONSOLE}" &>/dev/null; then
        ERROR "nebula-console not found at: ${NEBULA_CONSOLE}"
        has_error=1
    fi

    if [[ ${has_error} -ne 0 ]]; then
        echo ""
        usage
    fi
}

##############################################################################
# Helper: execute nGQL on a cluster
##############################################################################
function exec_ngql() {
    local meta_addr="$1"
    local stmt="$2"
    local first_meta
    first_meta=$(echo "${meta_addr}" | cut -d',' -f1)
    local host
    local port
    host=$(echo "${first_meta}" | cut -d':' -f1)
    port=$(echo "${first_meta}" | cut -d':' -f2)

    ${NEBULA_CONSOLE} \
        --addr "${host}" \
        --port "${port}" \
        --user root \
        --password nebula \
        -e "${stmt}" 2>&1
}

##############################################################################
# Helper: execute nGQL and check for error
##############################################################################
function exec_ngql_checked() {
    local meta_addr="$1"
    local stmt="$2"
    local context="$3"

    local result
    result=$(exec_ngql "${meta_addr}" "${stmt}")

    if echo "${result}" | grep -qi "error\|failed"; then
        ERROR "${context}"
        ERROR "Statement: ${stmt}"
        ERROR "Output: ${result}"
        ERROR_AND_EXIT "Switchover aborted."
    fi
    echo "${result}"
}

##############################################################################
# Step 2: Quiesce primary
##############################################################################
function quiesce_primary() {
    INFO "=== Step 2: Setting read_only=true on primary (quiescing writes) ==="

    exec_ngql_checked "${PRIMARY_META}" \
        "USE ${SPACE_NAME}; SET VARIABLES read_only=true" \
        "Failed to set primary to read-only mode."

    INFO "Primary cluster is now read-only."
}

##############################################################################
# Step 3: Wait for listener sync lag to reach zero
##############################################################################
function wait_for_listener_caught_up() {
    INFO "=== Step 3: Waiting for SYNC listener to catch up (lag=0) ==="

    local elapsed=0
    while [[ ${elapsed} -lt ${TIMEOUT} ]]; do
        local status
        status=$(exec_ngql "${PRIMARY_META}" "USE ${SPACE_NAME}; SHOW SYNC STATUS")

        # Check if all partitions show lag=0
        # The output format has columns: Part | Listener | Status | Lag
        local total_lag
        total_lag=$(echo "${status}" | grep -oP 'lag[=:]\s*\K[0-9]+' | awk '{s+=$1} END {print s+0}')

        if [[ -z "${total_lag}" ]]; then
            # Try alternative parsing (tab-separated output)
            total_lag=$(echo "${status}" | awk -F'|' '/^[[:space:]]*[0-9]/ {gsub(/[[:space:]]/, "", $NF); sum+=$NF} END {print sum+0}')
        fi

        if [[ "${total_lag}" == "0" ]]; then
            INFO "All sync listener partitions have lag=0."
            return 0
        fi

        INFO "  Sync listener total lag: ${total_lag}, waiting... (${elapsed}s/${TIMEOUT}s)"
        sleep "${POLL_INTERVAL}"
        elapsed=$((elapsed + POLL_INTERVAL))
    done

    ERROR_AND_EXIT "Timeout (${TIMEOUT}s) waiting for sync listener to catch up. Switchover aborted."
}

##############################################################################
# Step 4: Wait for drainer to be fully caught up
##############################################################################
function wait_for_drainer_caught_up() {
    INFO "=== Step 4: Waiting for DRAINER to be fully caught up ==="

    local elapsed=0
    while [[ ${elapsed} -lt ${TIMEOUT} ]]; do
        local status
        status=$(exec_ngql "${SECONDARY_META}" "USE ${SPACE_NAME}; SHOW DRAINER SYNC STATUS")

        # Check if all partitions are caught up
        local total_lag
        total_lag=$(echo "${status}" | grep -oP 'lag[=:]\s*\K[0-9]+' | awk '{s+=$1} END {print s+0}')

        if [[ -z "${total_lag}" ]]; then
            total_lag=$(echo "${status}" | awk -F'|' '/^[[:space:]]*[0-9]/ {gsub(/[[:space:]]/, "", $NF); sum+=$NF} END {print sum+0}')
        fi

        if [[ "${total_lag}" == "0" ]]; then
            INFO "All drainer partitions are fully caught up."
            return 0
        fi

        INFO "  Drainer total lag: ${total_lag}, waiting... (${elapsed}s/${TIMEOUT}s)"
        sleep "${POLL_INTERVAL}"
        elapsed=$((elapsed + POLL_INTERVAL))
    done

    ERROR_AND_EXIT "Timeout (${TIMEOUT}s) waiting for drainer to catch up. Switchover aborted."
}

##############################################################################
# Step 5: Remove drainer on secondary
##############################################################################
function remove_drainer() {
    INFO "=== Step 5: Removing DRAINER on secondary ==="

    exec_ngql_checked "${SECONDARY_META}" \
        "USE ${SPACE_NAME}; REMOVE DRAINER" \
        "Failed to remove drainer on secondary."

    INFO "Drainer removed from secondary."
}

##############################################################################
# Step 6: Enable writes on new primary (old secondary)
##############################################################################
function enable_new_primary() {
    INFO "=== Step 6: Enabling writes on new primary (old secondary) ==="

    exec_ngql_checked "${SECONDARY_META}" \
        "USE ${SPACE_NAME}; SET VARIABLES read_only=false" \
        "Failed to enable writes on new primary."

    INFO "New primary (old secondary) is now writable."
}

##############################################################################
# Step 7: Increment epoch on new primary
##############################################################################
function increment_epoch() {
    INFO "=== Step 7: Incrementing epoch on new primary ==="

    exec_ngql_checked "${SECONDARY_META}" \
        "USE ${SPACE_NAME}; INCREMENT SYNC EPOCH" \
        "Failed to increment epoch on new primary."

    INFO "Epoch incremented on new primary."
}

##############################################################################
# Step 8: Tear down sync on old primary
##############################################################################
function teardown_old_primary() {
    INFO "=== Step 8: Tearing down sync infrastructure on old primary ==="

    # Remove listener
    exec_ngql_checked "${PRIMARY_META}" \
        "USE ${SPACE_NAME}; REMOVE LISTENER SYNC" \
        "Failed to remove sync listener on old primary."
    INFO "SYNC listener removed from old primary."

    # Sign out drainer service registration
    local result
    result=$(exec_ngql "${PRIMARY_META}" "USE ${SPACE_NAME}; SIGN OUT DRAINER SERVICE")
    # Sign out may fail if no drainer service registered - that's acceptable
    if echo "${result}" | grep -qi "error"; then
        WARN "SIGN OUT DRAINER SERVICE returned an error (may be expected if none registered)."
        WARN "Output: ${result}"
    else
        INFO "Drainer service signed out from old primary."
    fi
}

##############################################################################
# Step 9: Establish reverse sync
##############################################################################
function setup_reverse_sync() {
    INFO "=== Step 9: Establishing reverse sync direction ==="

    # Add listener on new primary (old secondary) -> pointing to old primary
    exec_ngql_checked "${SECONDARY_META}" \
        "USE ${SPACE_NAME}; ADD LISTENER SYNC" \
        "Failed to add sync listener on new primary."
    INFO "SYNC listener added on new primary."

    # Add drainer on old primary (now new secondary)
    exec_ngql_checked "${PRIMARY_META}" \
        "USE ${SPACE_NAME}; ADD DRAINER" \
        "Failed to add drainer on old primary (new secondary)."
    INFO "DRAINER added on old primary (new secondary)."

    INFO "Reverse sync established: ${SECONDARY_META} -> ${PRIMARY_META}"
}

##############################################################################
# Main
##############################################################################
function main() {
    parse_args "$@"
    validate_args

    INFO "============================================"
    INFO " NebulaGraph Planned Switchover"
    INFO "============================================"
    INFO ""
    INFO "Current primary:   ${PRIMARY_META}"
    INFO "Current secondary: ${SECONDARY_META}"
    INFO "Space:             ${SPACE_NAME}"
    INFO "Timeout:           ${TIMEOUT}s"
    INFO ""
    INFO "After switchover:"
    INFO "  New primary:   ${SECONDARY_META}"
    INFO "  New secondary: ${PRIMARY_META}"
    INFO ""

    # Step 1: Validation already done above

    # Step 2: Quiesce old primary
    quiesce_primary

    # Step 3: Wait for listener lag=0
    wait_for_listener_caught_up

    # Step 4: Wait for drainer caught up
    wait_for_drainer_caught_up

    # Step 5: Remove drainer on secondary
    remove_drainer

    # Step 6: Enable writes on new primary
    enable_new_primary

    # Step 7: Increment epoch
    increment_epoch

    # Step 8: Tear down old primary sync
    teardown_old_primary

    # Step 9: Establish reverse sync (unless skipped)
    if [[ ${SKIP_REVERSE_SYNC} -eq 0 ]]; then
        setup_reverse_sync
    else
        WARN "Reverse sync setup skipped (--skip-reverse-sync). Manual setup required."
    fi

    INFO ""
    INFO "============================================"
    INFO " Switchover Complete!"
    INFO "============================================"
    INFO ""
    INFO "Role swap successful:"
    INFO "  New primary:   ${SECONDARY_META} (writable)"
    INFO "  New secondary: ${PRIMARY_META} (read-only)"
    INFO ""
    if [[ ${SKIP_REVERSE_SYNC} -eq 0 ]]; then
        INFO "Reverse sync is active. Monitor with:"
        INFO "  New primary:   USE ${SPACE_NAME}; SHOW SYNC STATUS;"
        INFO "  New secondary: USE ${SPACE_NAME}; SHOW DRAINER SYNC STATUS;"
    fi
    INFO ""
    INFO "To switch back, run this script with swapped --primary-meta and --secondary-meta."
    INFO ""
}

main "$@"

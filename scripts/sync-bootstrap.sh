#!/usr/bin/env bash
#
# Copyright (c) 2024 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.
#
# sync-bootstrap.sh - Bootstrap a cross-cluster sync relationship for NebulaGraph.
#
# This script performs a cold bootstrap to establish data sync between a primary
# cluster and a secondary cluster using BR (Backup/Restore) for initial data
# transfer, then configures the sync listener and drainer.
#
# Steps:
#   1. Validate arguments (primary/secondary meta addrs, space name)
#   2. Perform a full backup on the primary cluster via nebula-br
#   3. Ship the backup to secondary (local path or S3 URI)
#   4. Restore the backup on the secondary cluster
#   5. Extract latestCommittedLogId from the backup manifest
#   6. Add SYNC listener on primary cluster
#   7. Add DRAINER on secondary cluster
#   8. Print success message with initial sync position
#
# Usage:
#   ./sync-bootstrap.sh \
#     --primary-meta <host:port[,host:port,...]> \
#     --secondary-meta <host:port[,host:port,...]> \
#     --space <space_name> \
#     --backup-path <local_path_or_s3_uri> \
#     [--nebula-console <path_to_nebula_console>] \
#     [--nebula-br <path_to_nebula_br>] \
#     [--drainer-addrs <host:port[,host:port,...]>]
#
# Requirements:
#   - nebula-br binary accessible
#   - nebula-console binary accessible
#   - Network connectivity to both primary and secondary meta services
#   - Sufficient storage at backup-path for full backup
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/utils.sh"

# Defaults
NEBULA_CONSOLE="${NEBULA_CONSOLE:-nebula-console}"
NEBULA_BR="${NEBULA_BR:-nebula-br}"
PRIMARY_META=""
SECONDARY_META=""
SPACE_NAME=""
BACKUP_PATH=""
DRAINER_ADDRS=""

##############################################################################
# Usage
##############################################################################
function usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Bootstrap a cross-cluster sync relationship.

Required options:
  --primary-meta    Comma-separated meta addresses of the primary cluster (host:port)
  --secondary-meta  Comma-separated meta addresses of the secondary cluster (host:port)
  --space           Name of the graph space to sync
  --backup-path     Local path or S3 URI for storing the backup

Optional:
  --nebula-console  Path to nebula-console binary (default: nebula-console)
  --nebula-br       Path to nebula-br binary (default: nebula-br)
  --drainer-addrs   Comma-separated drainer addresses on secondary (host:port)
  -h, --help        Show this help message

Environment variables:
  NEBULA_CONSOLE    Path to nebula-console binary
  NEBULA_BR         Path to nebula-br binary

Examples:
  $(basename "$0") \\
    --primary-meta 10.0.0.1:9559,10.0.0.2:9559 \\
    --secondary-meta 10.0.1.1:9559,10.0.1.2:9559 \\
    --space my_graph \\
    --backup-path /data/backup/sync_init \\
    --drainer-addrs 10.0.1.10:9789,10.0.1.11:9789
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
            --backup-path)
                BACKUP_PATH="$2"; shift 2 ;;
            --nebula-console)
                NEBULA_CONSOLE="$2"; shift 2 ;;
            --nebula-br)
                NEBULA_BR="$2"; shift 2 ;;
            --drainer-addrs)
                DRAINER_ADDRS="$2"; shift 2 ;;
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
    if [[ -z "${BACKUP_PATH}" ]]; then
        ERROR "Missing required option: --backup-path"
        has_error=1
    fi

    # Verify binaries are accessible
    if ! command -v "${NEBULA_CONSOLE}" &>/dev/null; then
        ERROR "nebula-console not found at: ${NEBULA_CONSOLE}"
        has_error=1
    fi
    if ! command -v "${NEBULA_BR}" &>/dev/null; then
        ERROR "nebula-br not found at: ${NEBULA_BR}"
        has_error=1
    fi

    if [[ ${has_error} -ne 0 ]]; then
        echo ""
        usage
    fi
}

##############################################################################
# Helper: execute nGQL on a cluster via nebula-console
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

    INFO "Executing nGQL on ${host}:${port}: ${stmt}"
    ${NEBULA_CONSOLE} \
        --addr "${host}" \
        --port "${port}" \
        --user root \
        --password nebula \
        -e "${stmt}" 2>&1
}

##############################################################################
# Step 1: Full backup on primary
##############################################################################
function do_backup() {
    INFO "=== Step 1: Creating full backup on primary cluster ==="
    INFO "Meta addresses: ${PRIMARY_META}"
    INFO "Backup path: ${BACKUP_PATH}"
    INFO "Space: ${SPACE_NAME}"

    ${NEBULA_BR} backup full \
        --meta "${PRIMARY_META}" \
        --storage "${BACKUP_PATH}" \
        --space "${SPACE_NAME}" \
        2>&1

    if [[ $? -ne 0 ]]; then
        ERROR_AND_EXIT "Backup failed. Please check nebula-br output above."
    fi
    INFO "Backup completed successfully."
}

##############################################################################
# Step 2: Ship backup to secondary (for local path, this is a no-op if shared
# storage is used; for S3, nebula-br handles it)
##############################################################################
function ship_backup() {
    INFO "=== Step 2: Shipping backup to secondary ==="
    # For S3/OSS paths, the backup is already accessible by both clusters.
    # For local paths, the operator must ensure the path is accessible
    # (e.g., NFS mount, or manual rsync).
    if [[ "${BACKUP_PATH}" == s3://* ]] || [[ "${BACKUP_PATH}" == oss://* ]]; then
        INFO "Backup is on shared object storage - no additional shipping needed."
    else
        WARN "Backup is on local storage at: ${BACKUP_PATH}"
        WARN "Ensure this path is accessible from the secondary cluster nodes."
        WARN "If using separate storage, manually copy the backup before proceeding."
        # Give operator a moment to read the warning
        read -p "Press Enter to continue once backup is accessible from secondary..." < /dev/tty || true
    fi
}

##############################################################################
# Step 3: Restore on secondary
##############################################################################
function do_restore() {
    INFO "=== Step 3: Restoring backup on secondary cluster ==="
    INFO "Meta addresses: ${SECONDARY_META}"

    ${NEBULA_BR} restore full \
        --meta "${SECONDARY_META}" \
        --storage "${BACKUP_PATH}" \
        --space "${SPACE_NAME}" \
        2>&1

    if [[ $? -ne 0 ]]; then
        ERROR_AND_EXIT "Restore failed. Please check nebula-br output above."
    fi
    INFO "Restore completed successfully."
}

##############################################################################
# Step 4: Extract latestCommittedLogId from manifest
##############################################################################
function extract_log_id() {
    INFO "=== Step 4: Extracting latestCommittedLogId from backup manifest ==="

    local manifest_file
    # Find the manifest file in backup path
    if [[ "${BACKUP_PATH}" == s3://* ]] || [[ "${BACKUP_PATH}" == oss://* ]]; then
        # For object storage, use nebula-br show to get manifest info
        manifest_file=$(${NEBULA_BR} show \
            --storage "${BACKUP_PATH}" 2>/dev/null | grep -o '"latestCommittedLogId":[0-9]*' | head -1)
        if [[ -n "${manifest_file}" ]]; then
            LATEST_LOG_ID=$(echo "${manifest_file}" | grep -o '[0-9]*$')
        fi
    else
        # For local storage, parse the manifest JSON directly
        local manifest
        manifest=$(find "${BACKUP_PATH}" -name "*.manifest" -o -name "manifest.json" 2>/dev/null | head -1)
        if [[ -z "${manifest}" ]]; then
            manifest=$(find "${BACKUP_PATH}" -name "MANIFEST" 2>/dev/null | head -1)
        fi
        if [[ -n "${manifest}" ]]; then
            # Extract the latestCommittedLogId from the manifest
            LATEST_LOG_ID=$(grep -o '"latestCommittedLogId"[[:space:]]*:[[:space:]]*[0-9]*' "${manifest}" \
                | grep -o '[0-9]*$' | sort -rn | head -1)
        fi
    fi

    if [[ -z "${LATEST_LOG_ID:-}" ]] || [[ "${LATEST_LOG_ID}" == "0" ]]; then
        # Fallback: try to get it from nebula-br show command
        LATEST_LOG_ID=$(${NEBULA_BR} show \
            --meta "${PRIMARY_META}" \
            --storage "${BACKUP_PATH}" 2>/dev/null \
            | grep -i "commit" | grep -o '[0-9]*' | sort -rn | head -1)
    fi

    if [[ -z "${LATEST_LOG_ID:-}" ]]; then
        ERROR_AND_EXIT "Failed to extract latestCommittedLogId from backup manifest."
    fi

    INFO "Latest committed log ID from backup: ${LATEST_LOG_ID}"
}

##############################################################################
# Step 5: Add SYNC listener on primary
##############################################################################
function add_listener() {
    INFO "=== Step 5: Adding SYNC listener on primary cluster ==="

    local stmt="USE ${SPACE_NAME}; ADD LISTENER SYNC"
    local result
    result=$(exec_ngql "${PRIMARY_META}" "${stmt}")

    if echo "${result}" | grep -qi "error\|failed"; then
        ERROR "Failed to add SYNC listener on primary."
        ERROR "Output: ${result}"
        ERROR_AND_EXIT "Please check that the space exists and listener service is running."
    fi
    INFO "SYNC listener added on primary cluster."
}

##############################################################################
# Step 6: Add DRAINER on secondary
##############################################################################
function add_drainer() {
    INFO "=== Step 6: Adding DRAINER on secondary cluster ==="

    local stmt
    if [[ -n "${DRAINER_ADDRS}" ]]; then
        stmt="USE ${SPACE_NAME}; ADD DRAINER ${DRAINER_ADDRS}"
    else
        stmt="USE ${SPACE_NAME}; ADD DRAINER"
    fi

    local result
    result=$(exec_ngql "${SECONDARY_META}" "${stmt}")

    if echo "${result}" | grep -qi "error\|failed"; then
        ERROR "Failed to add DRAINER on secondary."
        ERROR "Output: ${result}"
        ERROR_AND_EXIT "Please check that the drainer service is running on secondary."
    fi
    INFO "DRAINER added on secondary cluster."
}

##############################################################################
# Main
##############################################################################
function main() {
    parse_args "$@"
    validate_args

    INFO "============================================"
    INFO " NebulaGraph Cross-Cluster Sync Bootstrap"
    INFO "============================================"
    INFO ""
    INFO "Primary meta:   ${PRIMARY_META}"
    INFO "Secondary meta: ${SECONDARY_META}"
    INFO "Space:          ${SPACE_NAME}"
    INFO "Backup path:    ${BACKUP_PATH}"
    INFO ""

    do_backup
    ship_backup
    do_restore
    extract_log_id
    add_listener
    add_drainer

    INFO ""
    INFO "============================================"
    INFO " Bootstrap Complete!"
    INFO "============================================"
    INFO ""
    INFO "Sync relationship established successfully."
    INFO "  Space:             ${SPACE_NAME}"
    INFO "  Initial Log ID:   ${LATEST_LOG_ID}"
    INFO "  Primary meta:     ${PRIMARY_META}"
    INFO "  Secondary meta:   ${SECONDARY_META}"
    INFO ""
    INFO "The sync listener on primary will begin forwarding WAL entries"
    INFO "starting from log ID ${LATEST_LOG_ID} to the drainer on secondary."
    INFO ""
    INFO "To monitor sync status:"
    INFO "  Primary:   SHOW SYNC STATUS;"
    INFO "  Secondary: SHOW DRAINER SYNC STATUS;"
    INFO ""
}

main "$@"

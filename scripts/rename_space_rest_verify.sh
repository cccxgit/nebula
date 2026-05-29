#!/usr/bin/env bash
# Verify nebula-metad hard space rename REST endpoint with dry-run first.
# Usage:
#   scripts/rename_space_rest_verify.sh \
#     http://127.0.0.1:11000 old_space new_space 100 token [operator] [comment]

set -euo pipefail

if [[ $# -lt 5 || $# -gt 7 ]]; then
  echo "Usage: $0 <meta_http_base_url> <old_name> <new_name> <expected_space_id> <token> [operator] [comment]" >&2
  exit 2
fi

BASE_URL=${1%/}
OLD_NAME=$2
NEW_NAME=$3
EXPECTED_SPACE_ID=$4
TOKEN=$5
OPERATOR=${6:-ops}
COMMENT=${7:-hard rename verification}
ENDPOINT="${BASE_URL}/admin/space/rename"

payload() {
  local dry_run=$1
  python3 - "$OLD_NAME" "$NEW_NAME" "$EXPECTED_SPACE_ID" "$dry_run" "$OPERATOR" "$COMMENT" <<'PY'
import json
import sys
old_name, new_name, space_id, dry_run, operator, comment = sys.argv[1:]
print(json.dumps({
    "old_name": old_name,
    "new_name": new_name,
    "expected_space_id": int(space_id),
    "dry_run": dry_run.lower() == "true",
    "operator": operator,
    "comment": comment,
}))
PY
}

post() {
  local body=$1
  curl -fsS \
    -X POST \
    -H 'Content-Type: application/json' \
    -H "X-Nebula-Admin-Token: ${TOKEN}" \
    -d "${body}" \
    "${ENDPOINT}"
}

echo "[1/2] Dry-run hard rename ${OLD_NAME} -> ${NEW_NAME} (space_id=${EXPECTED_SPACE_ID})"
post "$(payload true)"
echo

echo "[2/2] Execute hard rename ${OLD_NAME} -> ${NEW_NAME} (space_id=${EXPECTED_SPACE_ID})"
post "$(payload false)"
echo

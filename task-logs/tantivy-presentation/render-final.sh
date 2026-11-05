#!/usr/bin/env bash
set -euo pipefail

task_repo=/home/sch/nebula/nebula-3.6-tantivy
task_presentation_dir="$task_repo/task-logs/tantivy-presentation"
task_runtime=/home/sch/.cache/codex-runtimes/codex-primary-runtime/dependencies
task_stamp=$(date -u +%Y%m%dT%H%M%SZ)

export DECK_BUILD_DIR=${DECK_BUILD_DIR:-"$task_presentation_dir/$task_stamp-final"}
export DECK_OUTPUT=${DECK_OUTPUT:-"$task_repo/docs/tantivy/nebula-tantivy-competition-$task_stamp.pptx"}
export PERFORMANCE_SLIDE_JSON=${PERFORMANCE_SLIDE_JSON:-"$task_presentation_dir/performance-slide.pending.json"}
export RUNTIME_NODE_MODULES="$task_runtime/node/node_modules"

if [[ -e "$DECK_OUTPUT" ]]; then
  printf '%s\n' "Refusing to overwrite existing deck: $DECK_OUTPUT" >&2
  exit 1
fi
if [[ ! -f "$PERFORMANCE_SLIDE_JSON" ]]; then
  printf '%s\n' "Missing presentation facts: $PERFORMANCE_SLIDE_JSON" >&2
  exit 1
fi
mkdir -p "$DECK_BUILD_DIR"
cd "$task_repo"
"$task_runtime/node/bin/node" "$task_presentation_dir/build-deck.mjs" \
  > "$DECK_BUILD_DIR/build.log" 2>&1
printf '%s\n' "Deck: $DECK_OUTPUT" "QA images and receipt: $DECK_BUILD_DIR"

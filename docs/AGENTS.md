
---

## 文件 2：`AGENTS.md`

```md
# AGENTS.md

## Purpose
This repository uses Codex primarily for complex bug investigation, minimal-risk fixes, test backfilling, and PR drafting.

## Working style
- Prefer Ask-first, Code-second workflow for non-trivial changes.
- For complex bugs, first build a hypothesis tree before editing code.
- Prefer minimal patches over broad refactors.
- Clearly separate confirmed facts from assumptions.
- When uncertain, explain uncertainty explicitly.

## For bug-fix tasks
Before changing code, Codex should:
1. Identify the most relevant modules and files.
2. Explain the execution flow and state transitions.
3. Propose a root-cause hypothesis tree.
4. Map logs to code paths whenever logs are provided.
5. Propose a minimal fix plan and test plan.

## Change constraints
- Do not perform unrelated refactors.
- Do not add unnecessary dependencies.
- Follow existing naming, error handling, and test style.
- Prefer adding or updating regression tests where feasible.

## Output requirements
Every substantial task should include:
- Problem understanding
- Relevant files
- Assumptions vs confirmed facts
- Fix plan
- Patch summary
- Test summary
- Risks
- PR-ready summary

## Testing
When tests exist, prefer running the smallest relevant test set first, then broader validation if needed.

## For distributed-system bugs
Explicitly reason about:
- state transitions
- retries
- idempotency
- concurrency/races
- partial failure handling
- observability/logging

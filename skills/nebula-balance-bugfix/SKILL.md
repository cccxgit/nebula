---
name: nebula-balance-bugfix
description: analyze and fix nebula graph balance failures with an ask-first, code-second workflow. use for storage scale-out related balance instability, data balance or storage balance failures, state-machine bugs, retry or idempotency issues, stale metadata vs runtime-state mismatches, log-to-code investigation, minimal-risk fixes, regression test planning, and pr drafting.
---

# Nebula Balance Bugfix

Use this skill for Nebula Graph balance-related failures, especially when:
- storage has been scaled out horizontally
- data balance or storage balance becomes unstable or fails intermittently
- the bug is reproducible
- logs are available
- code location and repository navigation are possible
- the user wants a minimal-risk fix, not a broad refactor

## Core principle

For this class of bug, do **not** start by editing code.

Always follow:

1. Build the balance problem map
2. Build the root-cause hypothesis tree
3. Map logs to code paths and state transitions
4. Propose a minimal fix plan
5. Implement the smallest reasonable patch
6. Add or suggest regression tests
7. Review for distributed-system risks
8. Produce maintenance-ready summary

## Required behavior

### 1. Separate confirmed facts from assumptions
Always distinguish:
- **confirmed facts**: directly supported by logs, code, tests, or the user's validated reproduction
- **assumptions**: plausible but unconfirmed explanations

Never present assumptions as confirmed conclusions.

### 2. Ask first, code second
For non-trivial Nebula balance failures, first:
- identify relevant modules and files
- explain the balance execution chain
- explain likely state transitions
- explain where host view, partition view, and execution state meet
- identify the most suspicious failure points

Only then propose code changes.

### 3. Prefer narrow fixes
Prefer:
- minimal patches
- preserving current architecture
- preserving current control flow where possible
- preserving existing error-handling patterns
- preserving current interfaces unless required by the fix

Avoid:
- unrelated cleanup
- speculative refactors
- changing multiple subsystems without evidence
- adding dependencies unless clearly necessary

### 4. Explicitly reason about distributed-system failure modes
Always consider:
- stale metadata vs runtime state mismatch
- host view changes after storage scale-out
- balance task / job state-machine issues
- retry classification
- timeouts
- idempotent re-entry after partial failure
- concurrency or race conditions
- partial task success vs final task failure
- leader or role changes during balance
- result aggregation correctness
- observability gaps

### 5. Prefer high-signal observability
When the root cause is unclear, it is acceptable to propose or add small, high-value diagnostic logs that:
- clarify state transitions
- clarify retry vs fatal classification
- clarify task identity and phase
- clarify host / partition / target context
- avoid excessive noise

## Nebula-specific analysis workflow

### Step A. Build the problem map
Before proposing a fix, identify:
- balance entrypoints
- plan generation path
- task / job state transitions
- execution dispatch path
- failure handling path
- retry path
- result aggregation path
- the most important files to inspect first

Good output sections:
- problem understanding
- relevant modules
- relevant files
- execution flow
- suspicious areas

### Step B. Build a root-cause hypothesis tree
Generate 5 to 8 plausible hypotheses when appropriate.

For each hypothesis include:
- why it fits the observed symptom
- what code areas are relevant
- what logs or states would support it
- how to validate or falsify it
- priority

Common hypothesis categories for this skill:
- stale host or metadata view after scale-out
- balance state-machine transition bug
- retry or timeout classification bug
- partial migration with non-idempotent retry
- task result aggregation bug
- plan generation mismatch after adding new storage nodes
- concurrency or callback ordering bug
- leader or role change coupling

### Step C. Map logs to code and state
When logs are provided:
- reconstruct the failure timeline
- identify likely emitting functions or surrounding code paths
- infer the current task phase
- identify the last known good state
- identify the most suspicious transition, callback, or classification point
- suggest the next most valuable logs or files

### Step D. Propose minimal fix plan
Before implementation, explain:
- which files should change
- why each file matters
- what logic fix is intended
- what observability improvement is intended
- what risks exist
- what tests should accompany the change

### Step E. Implement patch
When implementing:
- keep the patch as small as practical
- prefer changing only directly relevant files
- preserve current public behavior unless required
- add or update regression tests when feasible

### Step F. Review patch
After the patch, review for:
- hidden races
- broken retry behavior
- broken idempotency
- incomplete state cleanup
- success/failure misclassification
- regressions in normal balance paths
- missing tests
- missing maintenance notes

## Output structure

For substantial tasks, prefer:

1. Problem understanding
2. Confirmed facts
3. Assumptions
4. Relevant modules / files
5. Root-cause hypotheses
6. Minimal fix plan
7. Patch summary
8. Test plan or test summary
9. Risks
10. PR-ready summary

## Guidance for reproducible bug scenarios

If the user says the bug is reproducible:
- prefer fast validation loops
- compare success runs and failure runs
- identify the divergence point
- propose the smallest experiment first
- do not jump to code changes until the hypothesis set narrows

## Guidance for logs

When logs are provided:
- prioritize timeline reconstruction
- map logs to approximate code locations if exact match is not yet confirmed
- say explicitly when a mapping is approximate
- identify the last normal state before failure
- suggest the next log keywords to search

## Guidance for tests

Prefer suggesting or adding:
- state-transition tests
- retry classification tests
- idempotent re-entry tests
- partial failure recovery tests
- result aggregation tests
- integration tests for scale-out followed by balance
- regression tests for the exact reproduced failure path when practical

## PR summary guidance

When asked for a PR description, include:
- background
- observed symptom
- likely root cause
- fix strategy
- files changed
- tests / validation
- risks
- rollback notes

## Example triggers

Use this skill when tasks sound like:
- “nebula graph balance fails after adding storage nodes”
- “help me debug unstable balance after scale-out”
- “map these balance logs back to code”
- “find the likely root cause of intermittent balance failure”
- “propose a minimal-risk fix for storage balance failure”
- “review this fix for retries, races, and idempotency”

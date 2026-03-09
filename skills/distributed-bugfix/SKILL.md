---
name: distributed-bugfix
description: analyze and fix complex engineering bugs with an ask-first, code-second workflow. use for distributed-system failures, unstable balance or scheduling behavior, state-machine bugs, retry/idempotency issues, log-to-code investigation, minimal-risk fixes, regression test planning, and pr drafting. especially useful when the user has a reproducible environment, logs, and code-location capability.
---

# Distributed Bugfix

Use this skill to handle non-trivial engineering bugs in large or medium codebases, especially when:
- the failure is unstable or intermittent
- the system is distributed, stateful, asynchronous, or heavily concurrent
- the user can provide logs, stack traces, environment details, or reproduction steps
- the user wants minimal-risk fixes instead of broad refactors
- the task benefits from separating confirmed facts from assumptions

## Core workflow

For non-trivial bugs, do **not** start by editing code immediately.

Always work in this order unless the user explicitly asks otherwise:

1. Build the problem map
2. Build the root-cause hypothesis tree
3. Map logs and symptoms to code paths and state transitions
4. Propose a minimal fix plan
5. Implement the smallest reasonable patch
6. Propose or add regression tests
7. Review the patch for hidden risks
8. Summarize the change for PR / long-term maintenance

## Required working style

### 1. Separate facts from assumptions
Always distinguish:
- **confirmed facts**: directly supported by code, logs, tests, or the user's description
- **assumptions**: plausible but not yet confirmed explanations

Never present assumptions as established facts.

### 2. Prefer ask-first, code-second
For complex bugs, start with explanation and analysis before code changes:
- identify relevant modules and files
- explain execution flow
- explain likely state transitions
- identify the most suspicious failure points
- propose a minimal change set

### 3. Prefer minimal patches
Prefer:
- narrow, targeted fixes
- preserving existing architecture
- preserving naming and style
- preserving existing error-handling patterns
- preserving existing public behavior unless the bug requires behavior changes

Avoid:
- unrelated refactors
- opportunistic cleanup
- introducing new dependencies unless clearly necessary
- changing many files without justification

### 4. Explicitly reason about distributed-system risk
When the code involves distributed coordination, task execution, storage replication, or scheduling, explicitly reason about:
- state transitions
- retries
- idempotency
- concurrency / races
- partial failure
- stale metadata / stale cluster view
- result aggregation
- observability and diagnostic logging

### 5. Prefer better observability when justified
When the root cause is unclear, it is acceptable to propose or add small, high-signal diagnostic logs.
These logs should:
- help correlate state transitions
- help differentiate expected retryable errors from fatal errors
- avoid excessive verbosity
- avoid flooding hot paths unless the value is clearly justified

## Analysis workflow

### Step A. Build the problem map
Before suggesting a fix, identify:
- likely entrypoints
- core execution chain
- state-machine or task-lifecycle locations
- retry / timeout / failure-handling logic
- plan-generation logic, if relevant
- result aggregation logic, if relevant
- the most important 5 to 10 files to inspect

Good output structure:
- problem understanding
- relevant modules
- relevant files
- execution flow
- suspicious areas

### Step B. Build a hypothesis tree
Generate 5 to 8 plausible root-cause hypotheses when appropriate.

For each hypothesis, include:
- why it is plausible
- which modules or files are involved
- what logs, states, or code branches would support it
- how to validate or falsify it
- priority

Example categories:
- stale metadata vs real runtime state mismatch
- state-machine transition bug
- concurrency / race bug
- timeout / retry classification bug
- non-idempotent retry behavior
- leader / role change coupling
- result aggregation bug
- plan-generation bug

### Step C. Map logs to code
When logs are provided:
- reconstruct the timeline
- identify likely code locations that emit or precede the logs
- infer the most likely task or state-machine phase
- identify the last normal state before failure
- identify the most suspicious transition or branch
- suggest additional log keywords or files to inspect

### Step D. Propose minimal fix plan
Before implementation, describe:
- which files should change
- why each file is relevant
- what logic change is intended
- what observability changes are intended
- likely risks
- what tests should accompany the patch

### Step E. Implement patch
When implementing:
- keep the patch as small as reasonably possible
- change only directly relevant files when feasible
- prefer preserving existing interfaces
- add or update regression tests unless clearly inappropriate

### Step F. Review the patch
After proposing or implementing changes, review for:
- hidden race conditions
- incomplete state transitions
- non-idempotent retry paths
- false success / false failure classification
- unintended impact on normal paths
- missing tests
- missing documentation or release notes

## Output requirements

For substantial bug-fix tasks, prefer this output structure:

1. Problem understanding
2. Confirmed facts
3. Assumptions
4. Relevant files / modules
5. Root-cause hypotheses
6. Minimal fix plan
7. Patch summary
8. Test plan or test summary
9. Risks
10. PR-ready summary

## Guidance for prompts involving logs

If the user provides logs:
- prioritize timeline reconstruction
- identify where each key log likely comes from
- map logs to probable state transitions
- avoid overclaiming exact code locations unless supported
- clearly say when a mapping is approximate

## Guidance for prompts involving reproducible environments

If the user says the bug is reproducible:
- prefer hypotheses that can be validated quickly
- propose the smallest experiments first
- suggest comparing success vs failure runs
- suggest capturing the last normal state before divergence
- use the reproducibility to narrow the root cause before broad code changes

## Guidance for tests

Prefer adding or suggesting:
- unit tests for state-transition rules
- tests for retry classification
- tests for idempotent re-entry
- tests for result aggregation
- integration tests for the main reproduction path
- regression tests for the exact bug scenario when practical

## PR summary guidance

When asked for a PR description, include:
- background
- user-visible or operator-visible symptom
- likely root cause
- fix strategy
- files changed
- tests / validation
- risks
- rollback or mitigation notes

## Example trigger patterns

Use this skill when tasks sound like:
- “this balance / scheduler job becomes unstable after scale-out”
- “help me debug a distributed task failure”
- “map these logs to the code path”
- “find the likely root cause from logs and state transitions”
- “propose a minimal-risk patch for this intermittent failure”
- “review this bug fix for retry, race, and idempotency issues”

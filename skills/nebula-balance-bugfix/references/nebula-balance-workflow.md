# Nebula Balance Workflow Reference

## Goal

Use this reference when investigating Nebula Graph balance failures after storage scale-out.

## Standard workflow

1. Explain the relevant balance modules and files
2. Identify entrypoints and task lifecycle
3. Build the root-cause hypothesis tree
4. Map logs to code and task phases
5. Propose the minimal fix plan
6. Implement the narrowest viable patch
7. Add or suggest regression tests
8. Review for hidden distributed-system risks
9. Draft PR summary

## High-priority reasoning checklist

Always consider:

- stale metadata vs actual runtime state
- storage host view stability after scale-out
- balance task state-machine completeness
- timeout handling
- retry classification
- idempotent retry after partial execution
- callback ordering or race conditions
- leader or role changes during execution
- result aggregation correctness
- observability gaps

## Suggested analysis order

### 1. Problem map
Output:
- relevant modules
- relevant files
- execution flow
- state-transition locations
- suspicious areas

### 2. Hypothesis tree
For each hypothesis:
- why plausible
- files involved
- logs to check
- validation method
- priority

### 3. Log mapping
Output:
- timeline
- likely file/function
- current phase
- last normal state
- suspicious transition
- next logs to collect

### 4. Minimal fix plan
Output:
- files to change
- intended logic changes
- observability changes
- tests
- risks

### 5. Patch review
Check:
- state cleanup
- retries
- idempotency
- false failure classification
- regressions in normal paths

## Common hypothesis buckets

1. stale host or metadata view
2. state-machine transition bug
3. timeout or retry classification bug
4. non-idempotent re-entry
5. plan-generation mismatch
6. race / callback ordering issue
7. leader change coupling
8. result aggregation bug

## Maintenance guidance

Prefer:
- minimal patch
- explicit assumptions
- reproducible validation
- regression tests
- PR summaries that explain risk and rollback

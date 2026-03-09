# Bugfix Workflow Reference

## Standard workflow

1. Read codebase and identify relevant modules
2. Build root-cause hypothesis tree
3. Map logs to code and state transitions
4. Propose minimal fix plan
5. Implement smallest viable patch
6. Add regression tests
7. Review for hidden distributed-system risks
8. Draft PR summary

## Distributed-system checklist

Always consider:
- stale metadata
- leader or role changes
- retries
- idempotency
- result aggregation
- timeout classification
- partial failures
- race conditions
- task lifecycle completeness

## Recommended output sections

- Problem understanding
- Confirmed facts
- Assumptions
- Relevant files
- Hypotheses
- Fix plan
- Patch summary
- Tests
- Risks
- PR summary

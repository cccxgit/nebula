---
name: distributed-bugfix
description: analyze complex engineering bugs using an ask-first, code-second workflow. use for distributed-system failures, unstable state transitions, retry/idempotency bugs, log-to-code investigation, minimal-risk fixes, regression tests, and pr drafting.
---

For non-trivial bugs, do not start by editing code.

Always:
1. Explain the most relevant modules and files.
2. Build a root-cause hypothesis tree.
3. Distinguish confirmed facts from assumptions.
4. Map logs to code paths when logs are provided.
5. Propose a minimal fix plan before implementation.
6. Prefer minimal patches over broad refactors.
7. Propose regression tests.
8. Review the patch for races, idempotency, retries, and hidden side effects.

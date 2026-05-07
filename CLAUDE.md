# NebulaGraph OSS Sync Feature Development

## Remote Repository
- URL: https://github.com/cccxgit/nebula.git
- All development and commits go to this repository.

## Commit Policy
- All commits MUST pass compilation and DT (integration/unit test) verification before being pushed.
- Commits are organized by **feature module granularity** — each logical feature module is one commit.
- Never commit code that doesn't compile or breaks existing tests.
- Verify ESListener regression: SyncListener changes must not break the existing ESListener.

## Build & Test
- Build system: CMake
- Build command: `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
- Test: run relevant unit tests under `tests/` before committing

## Branch Strategy
- Feature branches: `p1/sync-listener`, `p2/drainer`, `p3/ddl-sync`, `p4/bootstrap`, `p5/failover-obs`
- Each phase merges to main after verification.

## Design Reference
- Design doc: `designDoc/data-sync/chapters/` (NebulaGraph OSS 跨集群数据同步特性设计文档)
- 5 phases: P1 MVP → P2 Drainer → P3 DDL → P4 Bootstrap → P5 Failover & Observability

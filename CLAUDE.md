# NebulaGraph OSS Sync Feature Development

## Remote Repository
- URL: https://github.com/cccxgit/nebula.git
- All development and commits go to this repository.

## Commit Policy
- All commits MUST pass compilation and DT (integration/unit test) verification before being pushed.
- Commits are organized by **feature module granularity** — each logical feature module is one commit.
- Never commit code that doesn't compile or breaks existing tests.
- Verify ESListener regression: SyncListener changes must not break the existing ESListener.

## Build & Test Environment (Singapore ECS)
- **Build server**: `47.84.234.139` (Aliyun ECS, Linux)
- **SSH access**: `ssh -i ~/.ssh/sg_aps.pem root@47.84.234.139`
- **Build system**: CMake
- **Build command**: `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
- **Test**: run relevant unit tests under `tests/` before committing
- **All compilation MUST be done on the SG Linux server** — macOS cannot build nebula (missing third-party: RocksDB, folly, fbthrift)
- **Third-party deps**: install via `install-third-party.sh` or download pre-built from vesoft CDN to `/opt/vesoft/third-party/`
- **Note**: The server also runs artology services (pm2, ports 3000/8787/8090-8092) — do not interfere with those processes

## Branch Strategy
- Feature branches: `p1/sync-listener`, `p2/drainer`, `p3/ddl-sync`, `p4/bootstrap`, `p5/failover-obs`
- Each phase merges to main after verification.

## Design Reference
- Design doc: `designDoc/data-sync/chapters/` (NebulaGraph OSS 跨集群数据同步特性设计文档)
- 5 phases: P1 MVP → P2 Drainer → P3 DDL → P4 Bootstrap → P5 Failover & Observability

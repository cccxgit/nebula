# NebulaGraph OSS Sync Feature Development

## Remote Repository
- URL: https://github.com/cccxgit/nebula.git
- All development and commits go to this repository.

## Commit Policy
- All commits MUST pass compilation and DT (integration/unit test) verification before being pushed.
- Commits are organized by **feature module granularity** — each logical feature module is one commit.
- Never commit code that doesn't compile or breaks existing tests.
- Verify ESListener regression: SyncListener changes must not break the existing ESListener.

## Build & Test Environment (Main PC WSL2 — Primary)
- **Build server**: `192.204.57.251:51002` (WSL2 Ubuntu 22.04, Ryzen 5800X 8C/16T, 48GB RAM)
- **SSH access**: `SSHPASS='K1=3SBNPLLOKj3%o-H2vkTYRzR=7HXutn' sshpass -e ssh -o PubkeyAuthentication=no -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 -p 51002 root@192.204.57.251`
- **Build system**: CMake
- **Build command**: `mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF && make -j8`
- **Source path**: `/root/nebula-build/nebula`
- **Third-party deps**: installed at `/opt/vesoft/third-party/` via `third-party/install-third-party.sh`
- **All compilation MUST be done on a Linux server** — macOS cannot build nebula (missing third-party: RocksDB, folly, fbthrift)

## Build & Test Environment (Singapore ECS — Backup, OOM risk)
- **Build server**: `47.84.234.139` (Aliyun ECS, Linux) — may OOM during compilation
- **SSH access**: `ssh -i ~/.ssh/sg_aps.pem root@47.84.234.139`
- **Note**: The server also runs artology services (pm2, ports 3000/8787/8090-8092) — do not interfere with those processes

## Branch Strategy
- Feature branches: `p1/sync-listener`, `p2/drainer`, `p3/ddl-sync`, `p4/bootstrap`, `p5/failover-obs`
- Each phase merges to main after verification.

## Design Reference
- Design doc: `designDoc/data-sync/chapters/` (NebulaGraph OSS 跨集群数据同步特性设计文档)
- 5 phases: P1 MVP → P2 Drainer → P3 DDL → P4 Bootstrap → P5 Failover & Observability

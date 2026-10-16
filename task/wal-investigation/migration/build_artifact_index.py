#!/usr/bin/env python3
"""Generate a GitHub-readable catalog from the explicit publication selection.

Only ARTIFACT-INDEX.md is regenerated. No experiment or product file is changed.
Run build_delivery, this generator, then build_delivery again to freeze hashes.
"""
from collections import defaultdict
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TASK = ROOT / 'task/wal-investigation'
OUTPUT = TASK / 'ARTIFACT-INDEX.md'

ROLES = {
 'DEVELOPER-HANDOFF.md': '接手第一入口：阅读、离线复查、评审、开发与验收顺序',
 'ARTIFACT-INDEX.md': '完整交付导航，本文件',
 'README.md': '研究总览和阅读入口',
 'REPORT.md': '通俗因果解释与服务实验关键证据',
 'EXPERIMENT-DETAILS.md': '原始实验时间窗口、操作、复现和限制',
 'WRITE-VS-IDLE.md': '空闲与持续 payload 的对比及整机待验边界',
 'COMPONENT-ANALYSIS.md': '结构开销、容量模型与独立组件机制',
 'CODE-WALKTHROUGH-RAFT.md': 'Raft 对象、心跳、复制、提交和读者',
 'CODE-WALKTHROUGH-WAL.md': '磁盘 WAL、缓存节点、标删、GC 和文件回退',
 'CODE-WALKTHROUGH-ROCKSDB.md': '提交位点、写缓冲、共享缓存和内存统计',
 'PRODUCTION-PARAMETER-TUNING.md': '生产空间分级、参数取舍和验证策略',
 'SPACE-LEVEL-KERNEL-PARAMETERS.md': '空间级配置可行性和方案演进',
 'SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md': '0.9 需求设计评审稿，首期开发与验收契约',
 'SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx': '需求设计 Word 评审版',
 'DELIVERY-AND-MIGRATION.md': '精确提交范围、目标环境依赖和迁移步骤',
 'sample_runtime.py': '只读采样进程、RocksDB HTTP 与磁盘 WAL，需显式目标',
 'parse_wal.py': '逐条读取 WAL 并验证结构，输出使用新位置',
 'test_parse_wal.py': '10 项合成边界检查，复制到新目录后运行',
 'cluster.py': '历史完整集群编排，会启停服务/建空间/改心跳/FLUSH，先适配',
 'analyze_results.py': '重算汇总和绘图，会写同目录输出，先复制证据',
 'analyze_runtime_rocks.py': '复算有效窗口、RSS 与 FLUSH，会写同目录输出',
 'atomic-buffer-probe.cpp': '原始空日志缓存组件源码，目标机器重建',
 'rocks-commit-probe.cpp': 'RocksDB 系统位点覆盖写组件，会创建新临时 DB',
 'payload-probe.cpp': '原始缓存 payload 和停写转空日志组件源码',
 'observation.patch': '124 行产品观测补丁；已含补丁的源码不要重复应用',
 'instrumentation-notes.txt': '观测开关、计数口径与开销边界',
 'preflight.py': '现有材料静态预检，区别于未实现的策略身份预检',
 'run_components.py': '参数化串行重建 Atomic/payload 探针，支持 smoke/full',
 'build_delivery.py': '生成明确发布清单与摘要，可选生成覆盖包',
 'verify_delivery.py': '只读核对文件摘要、类型和安全相对链接',
 'build_artifact_index.py': '按发布清单重新生成本索引；随后刷新摘要',
 'DELIVERY-MANIFEST.json': '本次文件、类型、字节和摘要；自身摘要由 Git 锚定',
 'PUBLISH-SHA256SUMS': '本次普通文件摘要；链接另由 JSON 核验',
 'publish-files.txt': '逐行列出本次交付的仓库相对路径',
 'recommended-files.txt': '初审 299 项候选历史快照，不是最终清单',
 'local-only-files.txt': '初审排除项快照，13 个必要链接后续已补回',
 'delivery-verification.json': '发布准备阶段的本地验证结果及未验边界',
 'source-link-navigation.json': '本轮将历史源码引用转换为 GitHub 导航的记录',
 'publication-validation.json': '发布前探针规范整理、当前源码重编译与完整运行输出',
 'wal-evidence.tar.gz': '停机 WAL 原始归档：3000 文件、6730574 条空 payload 记录',
 'wal-full.json': '逐条解析原始 WAL 的完整结构与计数报告',
 'wal-full.csv': '原始 WAL 按文件/分片/实例聚合表',
 'events.jsonl': '原完整服务实验的命令和阶段时间线',
 'build-j5.log': '遵守最大 make -j5 约束后的工程编译记录',
 'SHA256SUMS': '所在实验批次的历史摘要，不能替代本次发布清单',
}


def category(name):
    path = Path(name)
    if name == 'src/kvstore/wal/AtomicLogBuffer.cpp':
        return '产品观测代码与原始基线'
    if not name.startswith('task/wal-investigation/'):
        return '工程说明与协作约束'
    rel = path.relative_to('task/wal-investigation')
    top = rel.parts[0]
    if top == 'baseline' or path.name in ('observation.patch', 'instrumentation-notes.txt'):
        return '产品观测代码与原始基线'
    if len(rel.parts) == 1 and path.suffix in ('.md', '.docx', '.png', '.svg'):
        return '研究文章 需求设计与图表'
    if top == 'migration':
        return '迁移工具 审计与重新构建记录'
    if top == 'design-evidence':
        return '需求设计独立审查与出版记录'
    if path.suffix in ('.py', '.sh', '.cpp'):
        return '复现 采样 解析与组件工具源码'
    if top == 'configs' or path.suffix == '.ngql' or path.name == 'spaces.txt':
        return '历史配置与 nGQL 模板'
    if top.startswith('phase-'):
        return '原始服务分阶段采样'
    if top == 'service-logs':
        return '原始服务日志与必要别名'
    if top == 'production-tuning-evidence':
        return '容量对照与参数方案证据'
    if top == 'write-idle-comparison-20261011':
        return '空闲与 payload 对照证据'
    if top == 'wal-parser-selftest':
        return '解析器合成测试固件'
    if path.name.startswith('wal-'):
        return '原始 WAL 与解析观测'
    if path.name in ('.gitignore', '.gitattributes'):
        return '发布文件规则'
    return '实验构建 环境 时间线与汇总'


def role(name):
    p = Path(name)
    if name == 'src/kvstore/wal/AtomicLogBuffer.cpp':
        return '唯一产品源码改动，124 行观测；缓存策略未改'
    if p.name in ROLES:
        return ROLES[p.name]
    if '/baseline/' in name:
        return '固定提交 de9b3ed 的未改动源码，供原算法组件对照'
    if (ROOT / p).is_symlink():
        return '相对日志别名，目标随包交付，统计程序依赖'
    if '/phase-' in name:
        return '历史阶段采样/命令记录；保留过渡、缺口和失败样本'
    if '/service-logs/' in name:
        return '历史服务原始输出，保留真实字节'
    if p.suffix in ('.py', '.sh'):
        return '工具源码；执行前按迁移说明核对副作用与路径'
    if p.suffix == '.conf' or p.suffix == '.ngql':
        return '原实验模板，路径/端口/账号/空间名需按目标环境适配'
    if p.suffix in ('.csv', '.meta'):
        return '组件或服务观测原始样本/结构与分配器元数据'
    if p.name == 'run.json':
        return '本轮组件重编译和运行命令、源码/二进制摘要及结果'
    if p.suffix == '.log':
        return '对应步骤的实际运行/编译记录，非可执行入口'
    if p.suffix in ('.json', '.jsonl'):
        return '结构化证据或阶段性审查结果，按其时间和范围解释'
    if p.suffix in ('.png', '.svg'):
        return '由采样生成的研究图表，核对数字时回到原始数据'
    return '配套说明、环境记录或分析材料'


def main():
    selection = TASK / 'migration/publish-files.txt'
    files = selection.read_text().splitlines()
    assert len(files) == len(set(files))
    groups = defaultdict(list)
    for name in files:
        assert (ROOT / name).exists(), name
        groups[category(name)].append(name)
    order = ['研究文章 需求设计与图表', '产品观测代码与原始基线',
             '复现 采样 解析与组件工具源码', '迁移工具 审计与重新构建记录',
             '历史配置与 nGQL 模板', '原始服务分阶段采样', '原始服务日志与必要别名',
             '原始 WAL 与解析观测', '容量对照与参数方案证据', '空闲与 payload 对照证据',
             '解析器合成测试固件', '需求设计独立审查与出版记录',
             '实验构建 环境 时间线与汇总', '工程说明与协作约束', '发布文件规则']
    text = ['# WAL 研究成果索引与开发交接材料', '',
      '交付日期：2026 年 10 月 16 日。目标仓库：`cccxgit/nebula`，分支：`3.6-memory`。', '',
      '**接手开发先读 [开发接手指南](DEVELOPER-HANDOFF.md)。** 本索引回答每份材料在哪里、用于什么；完整开发契约在 [0.9 需求设计评审稿](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)，异机操作在 [迁移说明](DELIVERY-AND-MIGRATION.md)。', '',
      '## 交付状态和阅读口径', '',
      '源码和本地实验确认了空闲内部日志、WAL 缓存节点与 RocksDB 提交位点写缓冲的增长路径。当前产品源码只有 124 行观测改动，空间级配置仍待开发，建议容量范围和性能门槛待责任人评审；完整 50 空间真实业务写入和生产收益尚未验收。', '',
      '历史文章或审计中的“只保留本机”“未提交”描述其生成时刻；本次交付补齐下列清单中的证据与工具。Word 设计稿保留原评审版本，状态更新以本指南和 Git 为准。源码行号链接已尽量固定到分析基线；原机 `/opt/vesoft/...` 头文件位置是依赖记录，不能当成仓库内文件。', '',
      f'本索引逐项覆盖 **{len(files)} 个交付路径**，与 [publish-files.txt](migration/publish-files.txt) 一一对应。分类是阅读用途，不代表每个文件都是本次新增或都可执行。', '',
      '## 按目的找入口', '',
      '| 你要做什么 | 先看哪里 |', '| --- | --- |',
      '| 接手需求和拆分开发 | [接手指南](DEVELOPER-HANDOFF.md)、[需求设计](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md) |',
      '| 理解问题和证据强度 | [通俗报告](REPORT.md)、[实验明细](EXPERIMENT-DETAILS.md)、[写入对比](WRITE-VS-IDLE.md) |',
      '| 在另一环境复核 | [迁移说明](DELIVERY-AND-MIGRATION.md)、[脚本审计](migration/SCRIPT-PORTABILITY-AUDIT.md) |',
      '| 核对产品代码改动 | [代码审计](migration/CODE-CHANGE-AUDIT.md)、[完整观测补丁](observation.patch) |',
      '| 确认文件完整性 | [交付 JSON 清单](migration/DELIVERY-MANIFEST.json)、[校验工具](migration/verify_delivery.py) |', '',
      '## 分类目录', '', '| 材料类别 | 路径数 |', '| --- | ---: |']
    text.extend(f'| {g} | {len(groups[g])} |' for g in order if groups[g])
    for g in order:
        if not groups[g]:
            continue
        text.extend(['', '## ' + g, '', '| 仓库相对路径 | 用途和注意点 |', '| --- | --- |'])
        for name in groups[g]:
            target = os.path.relpath(ROOT / name, TASK).replace(os.sep, '/')
            text.append(f'| [{name}]({target}) | {role(name)} |')
    text.extend(['', '## 后续维护这份索引', '',
      '新增/删除交付材料后，按顺序更新发布清单、生成索引、最后冻结摘要。日志写到不纳入交付的 runs 目录；不要修改旧实验原件或旧校验快照。', '',
      '```bash', 'mkdir -p task/wal-investigation/migration/runs',
      'python3 task/wal-investigation/migration/build_delivery.py',
      'python3 task/wal-investigation/migration/build_artifact_index.py',
      'python3 task/wal-investigation/migration/build_delivery.py',
      'python3 task/wal-investigation/migration/verify_delivery.py', '```', '',
      'ELF、临时数据库和 Word QA 缓存按忽略规则留在本机；目标机器从源码重建。Git 不迁移 Codex 本机会话、安装目录或运行服务。', ''])
    OUTPUT.write_text('\n'.join(text))
    print(f'Indexed {len(files)} selected paths in {len(groups)} categories')


if __name__ == '__main__':
    main()

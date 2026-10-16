# 需求设计的证据复核与交付记录

本目录记录 2026 年 10 月 11 日需求设计编写过程。主文为 [图空间级 WAL 缓存配置需求设计](../SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)，另有同内容的 [Word 版](../SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx)。文档面向开发启动，尚未实施参数机制或运行生产性能测试。

- `reproduction-audit.md`：独立核查原始 CSV、WAL 归档、脚本参数、时间窗口以及复跑覆盖风险。
- `reproduction-audit-verification.json`、`reproduction-audit-run.log`：归档散列、数字复算和链接验证记录。
- `contract-audit.md`：独立审查配置、身份、启动、更新入口、生命周期及真实生效接口；它是供主文吸收的审查建议，取值和命名以主文为准。
- `build_design_docx.py`、`docx-build.json`：由 Markdown 构建 Word 的脚本、版本摘要与构建记录。
- `word-render.log`：打包 LibreOffice 在当前主机因 GLIBC 版本不兼容而失败的原始诊断。
- `render_with_host_libreoffice.py`、`word-render-host.log`：使用同一技能渲染器及打包 Python，显式改用主机 LibreOffice 7.3.7.2 的记录；没有修改托管依赖。
- `word-qa-delivery/`、`word-render-delivery.log`：最终 25 页逐页视觉检查材料；其余 `word-qa*` 和渲染日志为排版迭代记录。页面图片及中间 PDF 仅供内部校验，不作为新增交付格式。
- `verification.json`、`SHA256SUMS`：最终内容、链接、源码差异和文档交付核验。

独立审查已纳入主文的关键澄清包括：WAL 加速统计是每实例而非三实例合计；非空日志组件比较包含字符串分配；正常提交也可能回退读磁盘；Meta 首次加载前就保护冻结默认；集群 ID 用精确十进制字符串；HTTP 混合更新整单拒绝；每次构造实际容量检查；只读预检查无法通过现有接口权威确认集群 ID。

本轮没有启动数据库、没有修改现网参数，也没有新增产品源码改动。原始观测补丁和旧实验文件保持原状。文档中的性能门槛、覆盖值范围和输入规模上限是建议评审值，不是已测生产结论。证据文件主要保存在本机，旧 GitHub 文章不等于已经发布完整证据包。

# 公司电脑迁移与无 Rust 构建

本分支交付 C++/Rust 源码、Rust 依赖锁、已校验的预编译 Rust 静态库、部署和功能验证脚本、可编辑答辩 PPT、架构及性能材料。运行数据、完整 MS MARCO 归档、Nebula 第三方依赖包和历史 `build/` 目录不在 Git 中。请从 Git 克隆分支后在公司电脑新建构建目录，不复制本机的 CMake 缓存或数据库数据目录。

## 目标机器条件

预编译库由本机 Ubuntu 22.04、x86_64、glibc 2.35、GCC 11.4 和 Rust release 构建，已压缩保存在 `src/common/plugin/fulltext/tantivy/prebuilt/`。目标机器必须是 **Linux x86_64，glibc 不低于 2.35**；构建时用 Python 标准库核对目标平台、Rust 源码与压缩包 SHA256，再将 153 MiB 的 `.a` 解压到构建目录。它不调用 Cargo。目标机还要具备 Nebula 3.6 所需的 C++/CMake/third-party 3.3、Python **3.9 或更新版本**和足够的磁盘空间。glibc、系统 C++ 库及第三方依赖包的兼容性仍须由目标机实际编译、链接和运行验证。

先在公司电脑执行 `uname -m` 和 `ldd --version`，核对系统版本及上述条件。如果是 aarch64、macOS、musl 或 glibc 低于 2.35，这个预编译包不能直接使用；需要在与目标机兼容的构建环境单独编译 Rust 静态库，或为目标平台准备同样带校验信息的新包。此时不要强行跳过平台检查。

## 克隆、编译与安装

以下示例把运行目录放在源码目录的同级目录。`git clone` 只带来源码和已提交的材料；构建不会下载 MS MARCO 语料。若公司已有该仓库，请进入已有检出并切到 `3.6-tantivy` 分支。

```bash
git clone --branch 3.6-tantivy https://github.com/cccxgit/nebula.git nebula-3.6-tantivy
cd nebula-3.6-tantivy
mkdir -p task-logs/company-migration
set -o pipefail
task_runtime="$(pwd)/../nebula-run-tantivy"
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$task_runtime" \
  -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DNEBULA_TANTIVY_USE_PREBUILT=ON \
  2>&1 | tee task-logs/company-migration/configure.log
cmake --build build --parallel 5 \
  2>&1 | tee task-logs/company-migration/build.log
cmake --install build \
  2>&1 | tee task-logs/company-migration/install.log
```

上例最多 **5** 路 C++ 构建，遵循本次用户最新要求。安装前请确认这一个目标运行目录中的数据库进程已停止。`CMake` 会在 `build/tantivy-rust-target/release/libnebula_tantivy.a` 生成并链接经校验的静态库；仓库里的压缩包约 28 MiB，不必在公司电脑装 Rust。如果将来修改 Rust 实现，原压缩包的源码校验会失败；需在有 Rust 的兼容环境重建并更新包与清单。需要从源码构建 Rust 时设 `-DNEBULA_TANTIVY_USE_PREBUILT=OFF`，并提供 Cargo，内部 Rust 并行度固定为 1。

## 三个 storage 的实操验证

`cluster.py` 的默认运行目录是原开发机的绝对路径；在公司电脑上每次调用都显式传入 `--runtime`。它只管理隔离的五个服务，不使用另一套默认 `nebula.service`。首先用人工生成的中英混合数据做正确性验证，不依赖外部语料。

```bash
task_runtime="$(pwd)/../nebula-run-tantivy"
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r scripts/tantivy-benchmark/requirements.txt
ulimit -n 65536
python3 scripts/tantivy-demo/cluster.py init --runtime "$task_runtime"
python3 scripts/tantivy-demo/cluster.py start --runtime "$task_runtime"
python3 scripts/tantivy-demo/cluster.py status --runtime "$task_runtime"
python3 scripts/tantivy-demo/demo.py --space tantivy_company_demo \
  --output task-logs/company-migration/demo
python3 scripts/tantivy-benchmark/tantivy_regression_setup.py \
  --space ft_regression_company --index ftcase_native_company \
  --output task-logs/company-migration/grammar
```

主演示脚本验证 20 分片、3 副本、普通字符串谓词下推、词法检索、更新删除和索引生命周期；语法脚本再检查 8 条 fixture、18 项查询语法与计划。运行与报告路径保持在 `task-logs/company-migration/`。若运行目录已有进程或空间，请先检查现状，用新的空间名和输出目录；不要覆盖旧报告。现场操作顺序见 [演示指南](demo-guide.md)。

## 基准和参赛材料

公开源 MS MARCO passage v2 超过 1.38 亿条，但本项目目前仅在开发机上实际导入、测量到 500 万条且只保留前 128 个字符。语料条款限制非商业研究使用，因此 Git 中没有原始语料和投影文件。公司电脑复测需要按 [数据准备方法](benchmark-data.md)自行取得使用许可并重新准备语料；先做小样本与完整结果集合校验，再按 [基准操作手册](benchmark-runbook.md)执行相同负载。不要把旧机器上的 QPS 当作公司电脑测量结果。

最终交付入口为 [README](README.md)，含 [架构图](architecture.md)、[兼容矩阵](fulltext-compatibility.md)、[性能报告](benchmark-report.md)、[现场演示说明](demo-guide.md)与[答辩讲稿](speaker-notes-and-qa.md)。PPT 是可编辑的 `.pptx`，可在公司电脑修改公司实际复测结果；旧图表和旧机资源值必须保留明确的机器与样本口径。

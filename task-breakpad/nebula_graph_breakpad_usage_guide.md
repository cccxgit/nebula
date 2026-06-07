# Nebula Graph Breakpad 使用指导手册

## 1. 适用范围

本文基于 Nebula Graph 3.6 开源版本，说明如何启用 Breakpad、验证 daemon 在段错误等 fatal signal 下生成 minidump，以及如何解析 minidump 辅助定位崩溃根因。

本仓库当前 Breakpad 接入点在 `src/daemons/SetupBreakpad.cpp`：

- 仅在 `ENABLE_BREAKPAD` 编译宏开启时生效。
- `setupBreakpad()` 使用 `FLAGS_log_dir` 作为 minidump 输出目录。
- `google_breakpad::ExceptionHandler` 的 `install_handler=true`，因此 `SIGSEGV`、`SIGABRT` 等 fatal signal 会自动触发 minidump。
- graphd、metad、storaged 都在 daemon 启动流程中编译并调用该逻辑。

## 2. 编译安装

推荐 Debug 构建，保留调试信息：

```bash
mkdir -p build-breakpad
cd build-breakpad
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula \
      -DENABLE_TESTING=OFF \
      -DENABLE_BREAKPAD=ON \
      -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

如果后续需要用 `/opt/vesoft/third-party/3.3/bin/dump_syms` 生成 Breakpad 符号，建议关闭压缩 debug section：

```bash
cmake -S . -B build-breakpad \
      -DCMAKE_INSTALL_PREFIX=/usr/local/nebula \
      -DENABLE_TESTING=OFF \
      -DENABLE_BREAKPAD=ON \
      -DENABLE_COMPRESSED_DEBUG_INFO=OFF \
      -DCMAKE_BUILD_TYPE=Debug
make -C build-breakpad -j10
```

本次验证中，`ENABLE_COMPRESSED_DEBUG_INFO=ON` 时 `dump_syms` 对大体积 `nebula-graphd` abort；关闭压缩后可正常生成 `.sym` 文件。

## 3. 运行前检查

启动 Nebula 前执行：

```bash
ulimit -n 65536
cd /usr/local/nebula
```

确认 dump 目录存在。Breakpad 默认写入 daemon 的 `--log_dir`：

```bash
mkdir -p /usr/local/nebula/logs
```

确认二进制包含 Breakpad：

```bash
strings /usr/local/nebula/bin/nebula-graphd | grep -i breakpad | head
```

## 4. 崩溃后 dump 位置

daemon 发生 fatal signal 后，Breakpad 在 `--log_dir` 下生成 UUID 命名的 `.dmp` 文件，例如：

```text
/usr/local/nebula/logs/fbcdce7f-70a4-40ad-3554f4a6-2f943f14.dmp
```

查找最近 dump：

```bash
find /usr/local/nebula/logs -maxdepth 1 -name '*.dmp' \
  -printf '%TY-%Tm-%Td %TT %p %s bytes\n' | sort | tail
```

## 5. 生成 Breakpad 符号

Breakpad 的 `minidump_stackwalk` 需要按模块名和 debug id 组织符号目录。

```bash
BIN=/usr/local/nebula/bin/nebula-graphd
SYM_ROOT=/data/nebula-breakpad-symbols
TMP=/tmp/nebula-graphd.sym

/opt/vesoft/third-party/3.3/bin/dump_syms "${BIN}" > "${TMP}"
MODULE_LINE=$(head -1 "${TMP}")
MODULE_ID=$(echo "${MODULE_LINE}" | awk '{print $4}')
MODULE_NAME=$(echo "${MODULE_LINE}" | awk '{print $5}')

mkdir -p "${SYM_ROOT}/${MODULE_NAME}/${MODULE_ID}"
mv "${TMP}" "${SYM_ROOT}/${MODULE_NAME}/${MODULE_ID}/${MODULE_NAME}.sym"
```

符号文件必须与产生 dump 的二进制一一匹配；二进制重编译后 debug id 会变化，需要重新生成符号。

## 6. 解析 minidump

```bash
DUMP=/usr/local/nebula/logs/<uuid>.dmp
SYM_ROOT=/data/nebula-breakpad-symbols

/opt/vesoft/third-party/3.3/bin/minidump_stackwalk "${DUMP}" "${SYM_ROOT}" \
  > /tmp/nebula-minidump-stackwalk.txt \
  2> /tmp/nebula-minidump-stackwalk.stderr
```

重点看：

- `Crash reason`：崩溃信号，例如 `SIGSEGV`。
- `Crash address`：非法访问地址，空指针通常接近 `0x0`。
- `Thread N (crashed)`：崩溃线程。
- 栈顶第一帧：通常是最直接的根因位置。

本次空指针验证的关键输出：

```text
Crash reason: SIGSEGV
Thread 0 (crashed)
 0  nebula-graphd!main [GraphDaemon.cpp : 85 + 0x7]
```

这说明 minidump 已经能把段错误定位到具体源码文件和行号。

## 7. 无法生成 `.sym` 时的降级定位

如果 `dump_syms` 不可用或符号生成失败，`minidump_stackwalk` 仍能输出模块偏移：

```text
nebula-graphd + 0x535bd96
```

可用同版本未 strip 的二进制做降级解析：

```bash
addr2line -f -C -p -e /usr/local/nebula/bin/nebula-graphd 0x535bd96
```

这种方式不如完整 Breakpad 符号稳定，但在紧急现场仍可辅助判断大致函数。

## 8. 本次验证记录

关键日志位于 `task-breakpad/logs/`：

- `01_cmake_build_breakpad.log`：Breakpad 编译配置，确认 `ENABLE_BREAKPAD=ON`。
- `02_make.log`：完整 `make -j10` 编译日志。
- `03_make_install.log`、`03b_make_install_after_storaged_relink.log`：安装日志。二进制安装完成，conf 权限设置在本机失败。
- `04b_graphd_sigsegv_minidump_usr_local.log`：从 `/usr/local/nebula` 启动 graphd 后发送 `SIGSEGV`，成功生成 dump。
- `10_null_deref_stackwalk.log`：临时空指针崩溃验证，`dump_syms` 和 `minidump_stackwalk` 成功定位到源码行。
- `11_minidump_dump.log`：`minidump_dump` 格式解析输出，确认 dump header 和 stream 可读。

本次生成的关键文件：

```text
task-breakpad/runtime/graphd-logs/fbcdce7f-70a4-40ad-3554f4a6-2f943f14.dmp
task-breakpad/runtime/graphd-null-logs/d77641d7-3a71-471d-9b78cca1-2c9e81b3.dmp
task-breakpad/runtime/graphd_null_stackwalk.txt
task-breakpad/runtime/symbols-null/nebula-graphd/E557D6D5E7C471B1DB02DCBA4169833F0/nebula-graphd.sym
```

## 9. 生产建议

- 生产构建保留每个发布版本对应的未 strip 二进制和 `.sym` 符号目录。
- dump 目录建议单独规划磁盘空间，权限控制为仅运维/研发可读。
- minidump 可能包含内存片段和环境变量，外发前按公司安全要求脱敏。
- 如果需要在“不崩溃、不重启”的死锁现场主动采集 dump，应按 `task-breakpad/breakpad_signal_minidump_design.md` 的方案扩展 `pthread_sigmask + sigwaitinfo + WriteMinidump()`，不要在异步 signal handler 中直接写 dump。
- `make install` 如果在 conf 权限设置处失败，先确认 `/usr/local/nebula/etc` 下既有文件属主和权限；本次失败不影响已安装的 daemon 二进制，但需要修复权限后再做完整安装验收。

# 阶段 43：macOS 版本（在开发者的 Mac 上构建和测试）

日期：2026-11-02　版本：0.42.0（平台支持，没有功能变化）

## 1. 开发策略的变化

- 不再用 GitHub 构建 Windows 版本：CI 删除了 Windows 作业，其余 Linux 作业只在手动启动时运行。
- 开发版本在开发者的 Mac（Apple Silicon）上构建和测试，最终版完成后再上传到 GitHub。
- 上传前保留了完整的 git 历史备份：`backup/opencdc-full-history.bundle`。

## 2. 完成的功能

### 2.1 macOS 构建与打包

- **CMake 预设**：`macos`、`macos-ci`（AppleClang、Ninja Multi-Config、最低系统版本 macOS 14.0，与 Homebrew 的 Qt 一致）。
- **应用程序包** `OpenCDC.app`：
  - 包信息：名称、标识 `org.opencdc.analyzer`、版本；
  - 命令行程序仍是 `bin/opencdc`。
- **安装包**：CPack 生成 `.dmg` 和 `.tar.gz`（`OpenCDC-<版本>-macos-<架构>`），包含 `OpenCDC.app`、`bin/opencdc` 和 `share/opencdc`（文档、示例）。
- **Qt 部署后精简**（`cmake/MacDeployPrune.cmake`）：
  - macdeployqt 会部署 Homebrew Qt 的全部插件，其中虚拟键盘输入法插件带进了 QtQuick 和 QtQml，有的图片插件的依赖框架没有安装。
  - 现在只保留 Cocoa 平台插件、macOS 风格和 4 种图片格式插件，按 `otool -L` 计算依赖闭包，删除没有被引用的框架和库。框架从 12 个减为 5 个，安装包从 38 MB 减为 30 MB。
  - 清除扩展属性，最后重新做 ad hoc 签名。
- **安装测试** `package.install_smoke` 在 macOS 上增加检查：
  - Qt 已部署进包内，可执行文件不再引用 Homebrew 的 Qt；
  - 不需要的插件和框架不在包内；
  - `codesign --verify --deep --strict` 通过；
  - 在没有 Qt 环境变量的情况下运行界面自检。

### 2.2 构建脚本 `tools/macos/build-macos.sh`

一条命令完成全部步骤：

1. **检查环境**：Xcode 命令行工具。Apple clang 低于 16 时（slang 需要 C++20 的 `std::views::join`），用 `softwareupdate` 安装新的命令行工具。
2. **准备依赖**：用 Homebrew 安装 cmake、ninja、qt、bash。
3. **构建、测试与打包**：编译 Release，用 ctest 运行全部测试，用 cpack 打包；`.tar.gz` 改用系统 tar 重新打包，不带扩展属性。
4. **真实工程语料库**：运行两套语料库（可用 `--quick` 跳过）。
5. **记录**：日志写入 `build-logs/`，摘要为 `macos-summary.txt`。

编译器或最低系统版本变化时，自动重建构建目录。

### 2.3 在 Mac 上发现并修正的问题

| 问题 | 原因 | 修正 |
|---|---|---|
| `Sdc.cpp` 编译失败 | 浮点数 `std::to_chars` 需要 macOS 13.3 | 最低系统版本改为 14.0 |
| 3 个文档测试失败 | CMake 4 不再接受 `"\x1f"` 转义 | 分隔符改用 `string(ASCII 31 ...)` 生成 |
| 语料库无法下载 | 用户的全局配置启用了 Git LFS，但没有安装 `git-lfs` | 语料库的仓库在本地关闭 LFS 过滤器 |
| 语料库无法检出 | 上一次中断的检出留下了文件 | 改为 `checkout -f` |
| 2 个 VHDL 测试偶发失败 | CTest 并行运行时，两个测试进程用时间戳生成了同名临时目录 | 改为随机且保证不重复的临时目录 |
| 同类隐患 | 并行分析时，混合语言的临时目录、原语模型的临时文件可能被两个进程同时写坏 | 同样改为不重复的名字（`core/PathUtil.h`：`uniqueToken`、`createUniqueTempDir`） |
| `vhdl-corpus.sh` 不能在旧 bash 上运行 | 用到了 macOS 自带 bash 3.2 没有的 `mapfile` | 改为 `while read` 循环 |
| 链接警告 | 静态库在链接命令中重复出现（CMake 有意为之） | 加 `-no_warn_duplicate_libraries` |
| `.tar.gz` 中有 `._` 文件 | 扩展属性被打包进去 | 用系统 tar 重新打包，不带扩展属性 |

## 3. 构建与测试结果（Mac：macOS 26.6.2、arm64、Apple clang 21、Qt 6.11.2、CMake 4.4.3）

- 构建通过，编译警告 0 条。
- 491 个测试全部通过（包括 7 个界面自检和安装测试）。
- 语料库：SystemVerilog 9 项、VHDL 33 项全部通过，结果与 Linux 完全一致，包括 SURF 的 GTH/GTX/GTP 收发器工程、30 个收发器 `.xci` 和 13 个 FIFO `.xci`。
- 安装包：`OpenCDC-0.42.0-macos-arm64.dmg`（30 MB）、`.tar.gz`（31 MB）。
- 修改后在 Linux 上的验证：491 个测试、两套语料库和 Clang 构建全部通过。

## 4. 修改的文件

- `CMakePresets.json`、`CMakeLists.txt`、`cmake/Packaging.cmake`、`cmake/MacDeployPrune.cmake`（新）、`src/gui/CMakeLists.txt`。
- `src/core/PathUtil.h`、`src/frontend/AutoFrontend.cpp`、`tests/TestUtil.h`。
- `tests/package/InstallSmoke.cmake`、`tests/cli/RunDocConsole.cmake`。
- `tools/macos/build-macos.sh`（新）、`tools/ci/sv-corpus.sh`、`tools/ci/vhdl-corpus.sh`。
- `.github/workflows/ci.yml`、`README.md`。

## 5. 已知限制

- 应用只做了 ad hoc 签名，没有 Apple 开发者证书签名和公证。从网上下载的 dmg 第一次打开时，需要在"系统设置 > 隐私与安全性"中允许。
- 安装包只有 arm64 版本，没有构建 Intel / Universal 版本。
- 打包日志中仍有 macdeployqt 的 "Cannot resolve rpath" 提示。这些插件随后被删除，不影响运行。

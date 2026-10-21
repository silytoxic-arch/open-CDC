# 阶段 1 完成报告：工程基础

日期：2026-10-20　版本：0.1.0

## 1. 已完成的功能

| 功能 | 说明 |
|---|---|
| CMake 工程 | C++20；`CMakePresets.json` 提供 Windows/VS2022、Linux GCC/Clang 预设；Debug/Release；可选 `-Werror`/`/WX` |
| 依赖管理 | FetchContent 固定版本：slang v12.0、nlohmann/json v3.12.0（SHA-256 校验）、CLI11 v2.5.0、Catch2 v3.16.0；Qt 6 用 `find_package` |
| 架构约束检查 | `cmake/NoQtInCore.cmake` 在配置期遍历链接图，core/frontend/engine/CLI 依赖 Qt 时直接报错（已用反例验证） |
| core | 强类型 `Id<Tag>`、`SourceLocation`、`Diagnostic` + `DiagnosticSink`、`Result<T>`、`ProgressSink`、`CancellationToken`、UTF-8 路径工具 |
| frontend | `IFrontend` 接口 + slang 适配层（只有这一个 .cpp 引用 slang）：预处理、include 目录、宏定义、条件编译、参数、generate、实例数组、层次展开；slang 诊断转换为 OpenCDC 诊断，带文件、行、列 |
| Verilog-2001 关键字 | 按源文件把关键字集限制为 1364-2001/2005，`logic`、`bit` 等可以作为 Verilog 信号名 |
| engine | `.ocdc.json` 工程文件读取与校验（错误码 `PRJ-*`）；`AnalysisSession` 门面；不可变的 `AnalysisResult`；阶段状态报告；JSON 结果输出 |
| CLI | `opencdc --version`，`opencdc analyze <project> [--json out] [--quiet]`；退出码 0/1/2 |
| GUI | Qt6 Widgets 主窗口：Project Explorer、Design Hierarchy、Messages、Report、带行号的源码查看器；双击模块、实例或诊断可跳转到源码；分析在后台线程运行，可取消；未实现的视图（Clock Domains、CDC Paths、Rule Violations、CDC Schematic）都标注了计划阶段 |
| 示例 | `examples/smoke`：真实的 Verilog-2001 双时钟设计，包含 2FF 同步器、多比特直接跨域和 generate 逐位同步，为阶段 4/5 预留 |
| CI | `.github/workflows/ci.yml`：Windows 2022 + MSVC（Debug/Release，`/WX`，含 windeployqt 打包产物），以及 Ubuntu 24.04 + GCC |

所有尚未实现的阶段都会明确输出为 `not-implemented`：CLI 文本、JSON 中的 `"cdcAnalysisAvailable": false`、GUI 报告和占位视图都是如此。

## 2. 新建文件

```
CMakeLists.txt  CMakePresets.json  README.md  .gitignore  .github/workflows/ci.yml
cmake/{CompilerWarnings,Dependencies,NoQtInCore}.cmake
docs/architecture.md（更新：slang 版本、关键字风险已解决） docs/progress/stage-1.md
src/core/{Id.h, SourceLocation.h, Diagnostic.h/.cpp, Result.h, Progress.h, PathUtil.h, Version.h.in, CMakeLists.txt}
src/frontend/{Frontend.h, slang_adapter/SlangFrontend.cpp, CMakeLists.txt}
src/engine/{ProjectConfig.h/.cpp, AnalysisSession.h/.cpp, ResultJson.h/.cpp, CMakeLists.txt}
src/cli/{main.cpp, CMakeLists.txt}
src/gui/{main.cpp, MainWindow.h/.cpp, AnalysisController.h/.cpp, SourceViewer.h/.cpp, CMakeLists.txt}
tests/{CMakeLists.txt, TestUtil.h, cli/RunCli.cmake}
tests/core/{test_diagnostic.cpp, test_id_result.cpp}
tests/engine/{test_project_config.cpp, test_analysis_session.cpp}
tests/frontend/test_slang_frontend.cpp
examples/smoke/{smoke.ocdc.json, README.md, rtl/*.v, rtl/include/defs.vh}
third_party/README.md
```

与架构文档第 11.1 节的差异：
- `Cancellation.h` 合并进了 `Progress.h`；
- `SlangSmoke.cpp` 改名为 `SlangFrontend.cpp`，接口从 `smoke()` 改为 `elaborate()`，返回真实的实例层次；
- 新增 `PathUtil.h`，用于中文路径的 UTF-8 处理；
- 新增 `ResultJson`；
- `SourceLocation` 暂时直接保存文件路径字符串，阶段 2 引入文件表后改为 `FileId`。

## 3. 关键算法与设计点

- **层次收集**：递归遍历 slang 的已展开语义树，处理 `Instance`、`InstanceArray`、`GenerateBlock`（跳过未实例化分支）和 `GenerateBlockArray`。实例显示名取相对父实例的层次路径（如 `g_status[2].u_sync`），保留 generate 作用域信息。
- **诊断位置**：使用宏展开后的使用点（`getFullyExpandedLoc`），所以宏内部出错时定位到用户代码中使用宏的位置。
- **编译单元**：所有源文件按顺序组成一个编译单元，宏可以跨文件传递，与 Vivado/Quartus 行为一致。
- **线程模型**：GUI 通过 `QtConcurrent::run` 调用引擎，进度经 `QueuedConnection` 回到 GUI 线程；结果以 `shared_ptr<const AnalysisResult>` 只读共享；控制器析构时先取消再等待工作线程结束。

## 4. 编译与测试结果

| 环境 | 构建 | 测试 |
|---|---|---|
| Linux x64，GCC 13.3，Ninja Multi-Config，Release，`-Werror` | ✅ 本项目代码零警告 | ✅ 41/41（35 个单元测试 + 5 个 CLI 端到端 + 1 个 GUI 无头冒烟） |
| Linux x64，GCC 13.3，Ninja Multi-Config，Debug，`-Werror` | ✅ 零警告 | ✅ 41/41 |
| **Windows x64 + VS2022** | ⚠️ **未验证**：开发环境是 Linux 容器，无法运行 MSVC | 需推送到 GitHub 由 CI 验证，或本地构建 |

Qt：开发环境不能访问 Qt 官方下载源，因此从 GitHub 上 qtbase v6.8.3 源码编译了精简版（Widgets + Concurrent，offscreen 平台）用于验证。

已验证的反例：
- 测试脚本对错误的期望值会失败；
- 带语法错误的工程使 CLI 返回 1，并报告 `top.v:2:9: error[FE-SLANG-ExpectedToken]`；
- 给 engine 加上 Qt 依赖会导致配置失败。

## 5. 已知限制

1. MSVC/Windows 构建尚未实际运行过（见上表）。已针对 MSVC 做了预防：`/permissive- /utf-8 /Zc:__cplusplus`、`gmtime_s`、控制台 UTF-8、测试环境自动设置 Qt DLL 的 PATH，但仍可能出现只在 MSVC 下才有的编译错误。
2. 源文件路径含当前系统代码页无法表示的字符时，该文件的 Verilog 关键字限制会退化为 1364-2005，并给出警告 `FE-KEYWORD-MAPPING`。
3. `include 进来的头文件使用 1364-2005 关键字集（slang 的关键字映射按文件生效），与 2001 只差 `uwire` 一个关键字。
4. 约束文件只记录，不读取（`CON-NOT-IMPLEMENTED` 警告）。
5. 尚无 RTL 抽取、时钟和 CDC 分析，这些属于阶段 2–5。
6. 源码查看器按 UTF-8 读取文件，GBK 编码的中文注释会显示为乱码（不影响解析）。
7. 首次配置需要访问 GitHub 下载依赖；离线构建方法见 `third_party/README.md`。

## 6. 下一阶段建议（阶段 2：Verilog 前端 + Design IR）

建议拆成以下可分别验证的任务：

1. `ir` 模块：`Net`、`SigSpec`、`Cell`、`RegisterInfo`、`ModuleBody`、`InstanceNode`、`Design` 的数据结构与构建器，加 IR 文本 dump，用于 golden 文件比对。
2. 连续赋值与端口连接：生成别名和组合单元，处理位选择、拼接和常量。
3. always 块符号执行：组合块到 Mux 树；时序块识别时钟、异步复位、使能和同步复位；推断锁存器；检测多驱动。
4. 明确的"不支持清单"诊断（function/task、存储器数组、非常量 for 循环等）。
5. 解析回归测试：不少于 30 个 Verilog 片段，配 golden IR；在 Linux CI 上与 Yosys 做寄存器集合的差分对比（可选）。

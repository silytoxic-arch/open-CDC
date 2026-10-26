# 补丁 0.20.1：非 UTF-8 源文件、未知模块、消息窗口列宽

日期：2026-10-26　版本：0.20.1

本补丁处理用户试用中反馈的两个问题，并修正同时发现的几处兼容性问题。

## 1. 用户反馈与处理

| 反馈 | 原因 | 处理 |
|---|---|---|
| 源码中有（中文）注释时软件报错 | 源文件是 GBK 编码，这是中文 Windows 编辑器和 Vivado/Quartus 工程的默认编码。工具只按 UTF-8 读取：每处注释都报 `FE-SLANG-InvalidUTF8Seq`，源码窗口显示乱码。同一截图中，真正使分析停止的是另一条错误 `unknown module 'fifo_8X4K'`：它是 Vivado FIFO Generator IP，工程源文件中没有它的 HDL。 | 源文件统一按"UTF-8 → 带 BOM 的 UTF-16 → GB18030（GBK 的超集）→ Windows-1252"的顺序识别编码，转为 UTF-8 后再分析。被 `` `include `` 的文件、VHDL 文件、SDC/XDC 文件、GUI 源码窗口都使用同一套识别。转码的文件各有一条提示（`FE-ENCODING`/`CON-ENCODING`），说明按什么编码读入。未知模块见第 2 节。 |
| Messages 窗口中 Message 与 Location 之间的分隔线无法拖动 | Message 列设为 `Stretch`，这种模式下列宽不能拖动。其他几个表格和树（工程浏览、层次、时钟、违规、工程编辑器）也有 `Stretch`/`ResizeToContents` 列。 | 所有数据表格的列都改为可拖动，最后一列填满剩余宽度。数据加载后按内容设定一次初始宽度（有上限）。GUI 冒烟测试增加检查：任何表格中都不能有不可拖动的列。 |

## 2. 未知模块（IP 核）

- 缺省行为不变：未知模块是错误，CDC 分析不运行。错误后附一条说明：如果它是厂商 IP 核，请把 IP 生成的 HDL 加入源文件（Vivado：`<ip>/synth/<ip>.v` 或 `.vhd`，或仿真网表）；或者把工程选项 `unknownModules` 设为 `blackbox`。
- 新选项 `options.unknownModules`：`"error"`（缺省）或 `"blackbox"`。GUI 工程编辑器的"General"页对应"Unknown modules"，命令行对应 `--blackbox-unknown`。设为 `blackbox` 时，未知模块的实例按黑盒分析，每个实例一条警告 `FE-BLACKBOX-MODULE`，其余部分照常分析，内置的厂商原语模型仍然生效。黑盒的输出没有时钟域，经过它的跨域不会被检查，警告中写明了这一点。

## 3. 同时修正的兼容性问题

- 先使用后声明的信号（Vivado/Quartus 接受）不再报错。ADI 的 `axi_ad9361_tx.v` 就是这样写的。
- 新增 Xilinx UNIMACRO 模型：`MULT_MACRO`、`ADDSUB_MACRO`、`MACC_MACRO`、`FIFO_DUALCLOCK_MACRO`。双时钟 FIFO 用 Gray 指针加两级同步实现，能识别为异步 FIFO。

## 4. 修改的文件

- 新增 `src/core/TextEncoding.{h,cpp}`（编码识别与转换；Windows 用代码页 54936，POSIX 用 iconv）、`src/gui/ColumnSizing.h`、`src/frontend/primitives/xilinx_unimacro.v`
- 修改：
  - 前端：`src/frontend/slang_adapter/SlangFrontend.cpp`（非 UTF-8 文件与被包含文件预先转码；先用后声明；未知模块说明）、`src/frontend/slang_adapter/IrBuilder.{h,cpp}`、`src/frontend/{Frontend.h,MixedSupport.h,AutoFrontend.cpp,CMakeLists.txt}`、`src/frontend/vhdl/VhdlFrontend.cpp`
  - 约束：`src/constraints/Sdc.cpp`
  - 引擎与命令行：`src/engine/ProjectConfig.{h,cpp}`、`src/cli/main.cpp`
  - GUI：`src/gui/{MainWindow,CdcViews,ClockView,ProjectEditor,SourceViewer}.cpp`、`src/gui/ProjectEditor.h`、`src/gui/main.cpp`（冒烟测试）
  - 构建：`src/core/CMakeLists.txt`
- 测试：
  - `tests/core/test_text_encoding.cpp`（新）
  - `tests/frontend/test_encoding.cpp`（新）
  - `tests/frontend/test_primitives.cpp`
  - `tests/engine/test_project_config.cpp`

## 5. 测试

- **编码**：
  - 正例：UTF-8 校验（含超长编码、代理项、截断等负例）；GB18030、带 BOM 的 UTF-8、UTF-16LE、Windows-1252 解码；GBK 注释的 Verilog 文件及其头文件、VHDL 文件、SDC 文件都能分析，且不再报 `InvalidUTF8Seq`。
  - 负例：UTF-8 文件不提示。
- **未知模块**：
  - 正例：默认模式下报错并附说明（slang 和链接器两条路径都覆盖）；黑盒模式下有警告、分析照常运行、原语模型仍然生效。
  - 负例：工程文件中的非法取值会被拒绝。
- **其他**：
  - UNIMACRO 双时钟 FIFO 识别为异步 FIFO；
  - 先用后声明的写法被接受；
  - GUI 冒烟测试检查所有表格列都可拖动。

## 6. 已知限制

- 编码只做自动识别，没有手动指定编码的选项。一个 Windows-1252 文件如果恰好也是合法的 GB18030 字节序列，会按 GB18030 读入；这只影响注释的显示，不影响分析。
- 对 Vivado IP 核（.xci）还没有建模，只能加入 IP 生成的 HDL，或者按黑盒处理。下一步可以考虑：读取 FIFO Generator 的 .xci，按独立时钟或公共时钟的配置建立 FIFO 模型。

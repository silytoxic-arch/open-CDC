# 阶段 77：Gowin EDA 工程（.gprj）—— 导入、Gowin 原语、自动选择顶层

日期：2026-11-09　版本：0.77.0

阶段 76 的下一步是验证一个 Lattice 或 Gowin 工程。这一阶段选择 **NESTang**，即 Tang Nano 20K（Gowin GW2AR-18C）上的 NES 核心，取自 sipeed/TangNano-20K-example，提交 e23949a。它有以下特点：
- 用 Gowin EDA 的工程文件 `nes.gprj` 管理，以前 OpenCDC 不能读取这种工程；
- 没有指定顶层模块；
- 工程设置为 SystemVerilog 2017，`.v` 文件中也使用 `typedef`；
- 使用 Gowin 原语：rPLL、CLKDIV、OSER10、ELVDS_OBUF；
- SDC 文件使用 `//` 注释。

最初 OpenCDC 无法打开这个工程。加入导入器后，第一次分析有 9 个错误。依次处理后结果是：
- 0 个错误；
- 0 个未分析的构造；
- 638/638 个寄存器的时钟已解析。

工程已加入语料库。

## 1. 完成的功能

### 1.1 Gowin 工程导入（`src/engine/GowinProject.{h,cpp}`）

- `.gprj` 可以用在任何需要工程的地方：`opencdc analyze`、`opencdc import`、GUI 的 *Open Project*。
- 从 `.gprj` 的 XML 中读取：
  - 文件列表中启用的设计文件，按顺序：`file.verilog`（`.v`、`.sv`）、`file.vhdl`；
  - 时序约束：`file.sdc`；
  - 器件型号，用在导入摘要中。
- 综合设置的读取顺序：先找 `impl/<工程名>_process_config.json`（新版 Gowin EDA），没有时找 `impl/project_process_config.json`（旧版）。从中读取：
  - `TopModule`；
  - `Verilog_Standard`：`Vlg_Std_Sysv2017` 表示所有 Verilog 文件都按 SystemVerilog 2017 读取，包括 `.v` 文件；
  - `IncludePath`。
- 没有读取的内容都会报告：
  - 缺失的文件：`PRJ-GPRJ-MISSING`；
  - 禁用的文件、物理约束 `.cst`、逻辑分析仪配置 `.gao`：`PRJ-GPRJ-SKIPPED`；
  - 无法解析的设置：`PRJ-GPRJ-CONFIG`。
- 原来 Vivado 导入器中的 XML 读取代码移到 `src/engine/Xml.{h,cpp}`，两个导入器共用。

### 1.2 新的工程选项

| 选项 | 作用 | 设置者 |
|---|---|---|
| `verilogFilesSystemVerilog` | 与 `systemverilog-2017` 一起使用：`.v` 文件也使用 SystemVerilog 关键字。默认仍按 Vivado 的做法，`.v` 文件保留 Verilog-2005 关键字 | 导入时 `Verilog_Standard` 为 `Vlg_Std_Sysv2017` |
| `options.topSelection: deepest` | 未指定顶层且有多个根模块时，只分析层次最深的根模块，深度相同时比较实例数。报告 `FE-AUTO-TOP`，并列出未分析的根模块 | 导入时 `TopModule` 为空 |
| `options.gowinPrimitives` | 启用名称通用的 Gowin 原语模型（见 1.3） | 导入 Gowin 工程时 |

关于 `topSelection: deepest`：
- Gowin EDA 会自己选择顶层，具体规则没有找到文档。OpenCDC 采用“层次最深的根模块”，这是 Yosys `-auto-top` 的规则。
- 在 NESTang 中，`dsp.v` 的 `FirFilter` 是未使用的根模块，它实例化了 Xilinx 的 `DSP48A1`。如果不选择顶层，所有根模块都会被分析，`DSP48A1` 会作为未知模块报错。
- 为了选择顶层，需要先展开一次全部根模块。混合语言路径在设计有错误时，以前不输出层次结构，现在输出最后一轮收敛的层次（与纯 Verilog 路径一致），生成的包装模块不计入。

### 1.3 Gowin 原语模型（`src/frontend/primitives/gowin.v`）

以前没有为 `CLKDIV` 这类通用名称建模：用户自己的模块如果缺失，会被模型掩盖。现在这些模型**只在 Gowin 工程中使用**，其他工程中这些名称仍是未知模块。

| 原语 | 模型 |
|---|---|
| `CLKDIV`、`CLKDIV2` | 由 `HCLKIN` 分频得到的派生时钟，分频比为 `DIV_MODE`（"2"、"3.5"、"4"、"5"、"8"） |
| `DHCEN` | 时钟直通 |
| `OSER4`、`OSER8`、`OSER10` | 在 `PCLK` 上锁存并行字，在 `FCLK` 上移位输出 |
| `IDES4`、`IDES8`、`IDES10` | 在 `FCLK` 上移位输入，在 `PCLK` 上输出并行字 |
| `ELVDS_OBUF`、`TLVDS_OBUF`、`ELVDS_IBUF`、`TLVDS_IBUF` | 差分缓冲（连线） |

`docs/primitives.md` 已更新。

### 1.4 Gowin EDA 接受的写法

- **由模块自己驱动的 `input reg` 端口**（`autofire.v`：`input reg out`，在 `always` 中赋值）：读作 `output reg`，报告告警 `FE-NET-PROCEDURAL-ASSIGN`。
  - 反例：没有 `reg` 的 `input x` 被赋值时仍然是错误。
  - 以前的修正规则会先把它改成 `input reg x`，但结果仍是错误。现在这条规则不再作用于输入端口。
- **SDC 中的 `//` 注释**：可以单独一行，也可以写在命令之后。
  - 只有以 `//` 开头的词才算注释；词内部或花括号内的 `//` 不受影响（反例测试）。
- **SDC 中的报告命令**：`report_timing` 等报告命令被忽略，不再报告“未知命令”。

### 1.5 RTL 提取

- **流式拼接**：`{>>{a, b}}` 与 `{<<N{x}}`。
  - 赋给更宽的目标时左对齐（IEEE 1800 11.4.14）；
  - `<<` 从右端按 N 位分块，第一块成为最高位；
  - 带 `with` 子句时不分析（反例测试）。
  - 来源：NESTang `print.v` 的 `int_print({>>{"text"}}, STR)`。
- **字符串字面量作为值**：每个字符 8 位，第一个字符在最高位。
- **对子实例输出端口的层次引用**：`uart_demux.write`，该端口以位置方式连接到 `uart_write`。
  - 只要端口完整连接，就读取它在本模块中驱动的信号；
  - 未连接的输出仍是层次引用，不分析（反例测试）。
- **`mem[i][j][0] <= ...`**：两个变量下标后再接一个常量下标（`packet_picker.sv`）。处理方式是对 `j` 选中的部分做读-改-写。

### 1.6 时钟：被同域低有效使能门控的选通寄存器

```verilog
assign O_RXWT = ~psSEL & RXWT;      // dualshock_controller.v
always @(negedge W_RXWT) ...
```

- 以前：两个输入都是普通寄存器，无法判断哪个是时钟，28 个寄存器的时钟未解析。
- 现在的条件：
  - 两个输入是同一时钟、同一边沿的寄存器；
  - 其中恰好一个经过取反。
- 满足条件时，未取反的寄存器（选通）是时钟源，形式为派生时钟；取反的寄存器是使能，记为门控。
- 反例测试：两个都未取反时仍然无法判断。

## 2. 真实工程验证

### NESTang（Tang Nano 20K，直接分析 `nes.gprj`，加入语料库）

| | 第一次分析（加入导入器后） | 修复后 |
|---|---|---|
| 错误 | 9（`.v` 中的 `typedef`、`input reg` 被赋值），无法分析 | 0 |
| 顶层 | 所有根模块，包括 `FirFilter` → `DSP48A1` | `NES_Tang20k`（`FE-AUTO-TOP`） |
| 未知模块 | `CLKDIV`、`OSER10`×3、`ELVDS_OBUF`、`DSP48A1` | 0 |
| 实例 | 0 | 122 |
| 寄存器 | — | 638，全部时钟已解析 |
| 未分析的构造 | — | 0（中间一度为 6：流式拼接 3 处、层次引用 1 处、`mem[i][j][0]` 2 处） |
| 时钟 | — | 12 |
| 跨时钟路径 | — | 27 |

时钟结构：
- NES 时钟 `clk` 和 HDMI 的 `clk_p5` 都是 rPLL 输出，`clk_p5` 经 `CLKDIV` 5 分频得到像素时钟。
- SDC 用 `create_clock` 直接在这两个网络上定义时钟，它们因此成为两个独立的约束时钟，关系未知。按约束语义，这是正确的结果。
- 27 条跨时钟路径都是 NES 时钟 → HDMI 像素时钟，需要复核（CDC-010）。

### 其他语料

- 其余 30 个 SV 项目的 CDC 结果与修改前逐行相同。
- VHDL 语料 48 个全部匹配。

## 3. 修改的文件

- **新文件**：
  - `src/engine/GowinProject.{h,cpp}`；
  - `src/engine/Xml.{h,cpp}`（从 `VivadoProject.cpp` 移出）；
  - `src/frontend/primitives/gowin.v`。
- **工程与界面**：
  - `src/engine/ProjectConfig.{h,cpp}`：`.gprj` 分派、`verilogFilesSystemVerilog`、`topSelection`、`gowinPrimitives`；
  - `src/cli/main.cpp`、`src/gui/MainWindow.cpp`：提示文字与文件过滤器。
- **前端**：
  - `src/frontend/Frontend.h`：`verilogFilesSystemVerilog`、`deepestRootTop`、`gowinPrimitives`；
  - `src/frontend/AutoFrontend.cpp`：选择顶层、只在 Gowin 工程中使用 Gowin 模型、有错误时也输出层次结构；
  - `src/frontend/Primitives.{h,cpp}`、`src/frontend/CMakeLists.txt`：`gowinOnly`；
  - `src/frontend/slang_adapter/SlangFrontend.cpp`：`.v` 文件的关键字版本、`input reg` 的读法；
  - `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：流式拼接、字符串字面量、子实例输出的层次引用、`mem[i][j][k]`；
  - `src/frontend/slang_adapter/ProcessLowering.cpp`：`subInner` 的读-改-写。
- **约束与时钟**：
  - `src/constraints/Sdc.cpp`：`//` 注释、报告命令；
  - `src/clock/ClockAnalyzer.cpp`：选通寄存器与使能。
- **测试**（每个用例都含反例）：
  - `tests/engine/test_gowin_project.cpp`：新增，5 个用例；
  - `tests/ir_extract/test_ir_extract.cpp`：4 个用例；
  - `tests/constraints/test_sdc.cpp`：1 个用例；
  - `tests/clock/test_clock.cpp`：1 个用例；
  - `tests/cli/RunImport.cmake`：提示文字。
- **文档与版本**：
  - `README.md`、`docs/primitives.md`；
  - `tools/ci/sv-corpus.sh`：NESTang；
  - `CMakeLists.txt`：版本 0.77.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：639 个测试全部通过。
- 第一次运行时有两个失败，都已修正：
  - `docs/primitives.md` 中缺少新模型的说明；
  - CLI 导入测试仍匹配旧的提示文字。
- 新测试中我自己的两处预期错误，都已改正：
  - 没有 `reg` 的 `input` 端口被旧规则改成了 `input reg`，进而被新规则改为输出。改为旧规则不再作用于输入端口。
  - 时钟反例中的寄存器已被另一个门识别为时钟，于是被当作已知时钟。反例改用两个与其他逻辑无关的寄存器。
- SV 语料 31 个项目全部匹配（新增 NESTang），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 76 的 Windows CI：
  - 提交 59afc31，运行 42555357003；
  - 报告提交 ac9666b，运行 42556680662；
  - 均成功。
- 本阶段的 Windows CI（提交 9d94ee5，运行 42571705121）：
  - Release：639/639 通过，生成 `OpenCDC-0.77.0-windows-x64.zip` 与 `.exe`；
  - Debug：在配置步骤失败。原因是下载 nlohmann_json 的发布包时 GitHub 返回 HTTP 504（网络问题，与代码无关），没有进入编译。
  - 本报告的提交会重新运行两个构建，结果记录在阶段 78 的报告中。

## 5. 已知限制

- **未指定顶层时的选择规则**：Gowin EDA 的规则没有文档，OpenCDC 选择层次最深的根模块。结果不同时，请在工程中设置顶层。
- **Gowin 原语覆盖不全**：`IDDR`/`ODDR`（与 Xilinx 同名）、`SP`、`pROM`、`DFF` 系列、SERDES 等仍没有模型。
- **只对子实例的输出端口支持层次引用**：要求端口完整连接；更深层的引用或未连接端口的引用仍不分析。
- **选通寄存器规则的范围**：只处理“恰好一个取反”的两输入与门。

## 6. 下一步

阶段 78：继续用真实工程验证。候选：
- 一个 Lattice（iCE40/ECP5）工程，例如 Diamond 的 `.ldf` 或 Radiant 的 `.rdf` 工程，或 yosys/nextpnr 流程；
- 另一个 Gowin 工程（例如 Tang Nano 9K、Tang Primer 20K 上的设计），检查更多 Gowin 原语。

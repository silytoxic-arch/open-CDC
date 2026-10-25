# 阶段 14 完成报告：约束一致性检查、Vivado 时钟报告校验、Lattice/Gowin 扩展、microwatt

日期：2026-10-25　版本：0.14.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T14.1 Vivado 时钟报告校验 | 读取 Vivado `report_clocks` 的输出（新旧两种表格格式；`report_timing_summary` 的 Clock Summary 也能读），按来源引脚或时钟名与 OpenCDC 的时钟模型匹配：<br>- 周期差异超过 0.1% 报 `VIV-PERIOD-MISMATCH`（warning），附 OpenCDC 的推导式；<br>- Vivado 有周期而 OpenCDC 没有：`VIV-PERIOD-UNKNOWN`；<br>- 只有一方有的时钟：`VIV-CLOCK-UNMATCHED`、`VIV-CLOCK-MISSING`（info）。逻辑分频缺失说明 Vivado 不对它做时序分析。<br>入口：工程字段 `vivadoClockReports`、CLI `--vivado-clocks`（可重复）、GUI 工程编辑器的"Vivado 时钟报告"列表。分析阶段列表中新增"Vivado clock report check"（只在配置了报告时出现）。 |
| T14.2 约束一致性检查 | `CON-INPUT-PERIOD`：生成器配置的输入周期与实际输入时钟不符。<br>`CON-PERIOD-MISMATCH`：生成器输出上的约束与模型推导的周期不符。<br>`CON-GROUP-SINGLE`、`CON-GROUP-OVERLAP`：时钟组约束只有一个组，或同一时钟出现在多个组。<br>`CON-GROUP-RELATED`：同一时钟族的时钟被声明为异步。 |
| T14.3 Lattice/Gowin 扩展 | 新模型：ECP5 `IDDRX1F`/`ODDRX1F`/`DP16KD`，iCE40 `SB_RAM40_4K` 及三种下降沿变体，Gowin `SDPB`/`DPB`。<br>VHDL 组件包：Lattice（`ecp5u`、`ecp5um`、`ecp5um5g`、`sb_ice40_components_syn`、`iCE40UP`）和 Gowin（`gw1n`、`gw2a` 等 8 个库），写法为 `use <lib>.components.all`。<br>`EHXPLLL` 的输出周期按 `FEEDBK_PATH` 计算。 |
| T14.4 真实工程 microwatt | 完整的 microwatt POWER SoC（ECP5-EVN 配置：34 个核心 VHDL 文件、SoC、Verilog UART、ECP5 PLL）从 55 个错误修到 0 个错误、1 个警告，加入 VHDL 语料回归。修正的前端问题见 3.3 节。 |

## 2. 修改的文件

**新建**
- `src/engine/VivadoClocks.{h,cpp}`、`src/engine/ClockNaming.h`（从 SdcSuggest 中提取的命名规则，两者共用）
- `src/frontend/primitives/lattice_components.vhd`、`src/frontend/primitives/gowin_components.vhd`
- `tests/engine/test_vivado_clocks.cpp`
- `examples/primitives/vivado_clocks.rpt`
- `docs/progress/stage-14.md`

**修改**
- `src/clock/ClockAnalyzer.cpp`：约束一致性检查
- `src/engine/ProjectConfig.{h,cpp}`（`vivadoClockReports`）、`src/engine/AnalysisSession.cpp`（新阶段）、`src/engine/SdcSuggest.cpp`（改用 ClockNaming）、`src/engine/CMakeLists.txt`
- `src/cli/main.cpp`：`--vivado-clocks`
- `src/gui/ProjectEditor.{h,cpp}`、`src/gui/main.cpp`（自测检查新字段不被编辑器丢失）
- `src/frontend/Primitives.cpp`（`OPENCDC-LIBRARIES` 头注释）、`src/frontend/AutoFrontend.cpp`（厂商库列表改为从包列表生成；纯 VHDL 设计中的本地原语组件）、`src/frontend/CMakeLists.txt`
- `src/frontend/primitives/lattice_gowin.v`：新模型，`EHXPLLL` 的 `FEEDBK_PATH`
- `src/frontend/vhdl/VhdlNames.cpp`、`VhdlElab.h`、`VhdlBuiltins.cpp`、`VhdlStmt.cpp`：见 3.3 节
- `src/frontend/slang_adapter/ProcessLowering.cpp`：完整 case 的判断
- `tests/CMakeLists.txt`（`cli.vivado_clocks`）、`tests/frontend/test_primitives.cpp`、`tests/clock/test_clock.cpp`、`tests/vhdl/test_vhdl_elab.cpp`、`tests/ir_extract/test_ir_extract.cpp`
- `tools/ci/vhdl-corpus.sh`：microwatt（解析全部 111 个文件，并分析 ECP5-EVN SoC）
- `README.md`、`docs/architecture.md`（第 21 节）、`docs/primitives.md`、`CMakeLists.txt`（版本 0.14.0）

已有接口没有删除。一处行为有变化：原来"两个变量索引的赋值目标"报 `IR-UNSUPPORTED-TARGET`，现在能正确建模。原来的负例测试改为仍不支持的形式 `m(i).f(j)`。

## 3. 关键算法

### 3.1 Vivado 报告的解析与匹配
- 表头以 `Clock` 开头，并含 `Period…` 和 `Waveform…` 两列。`Period` 在前为新格式，否则为旧格式。`{…}` 作为一个记号，表在空行处结束。
- 用与约束建议相同的规则得到 Vivado 对象名：端口名、`inst/CLKOUT0`、`cnt_reg[2]/Q`。比较时忽略大小写，`.` 与 `/` 视为相同（Vivado 在 generate 作用域名中保留点）。
- 先按来源引脚匹配，再按时钟名匹配（SDC 名、去掉顶层的层次名、Tcl 安全名）。没有来源的虚拟时钟和 MMCM/PLL 反馈时钟（`CLKFBOUT`）不报未匹配。

### 3.2 一个嵌入文件提供多个库
VHDL 组件包首行若有 `-- OPENCDC-LIBRARIES: a b c`，就为每个库登记一份，写入临时目录时文件名加库名前缀。只有 VHDL 代码引用、而工程本身没有提供的库才会加入。

### 3.3 microwatt 暴露的前端问题
| 问题 | 修正 |
|---|---|
| `data(f + 63 downto f)` 报"范围边界必须静态" | 把左右边界分解为"同一表达式 + 常数"。长度静态时生成一个 `DynSelect`，宽度为长度乘元素宽度，索引取右边界（右边界的元素总在最低位，升序、降序数组都一样） |
| `std_match(std_ulogic_vector)`、`std_match(std_ulogic)`、`is_x(unsigned)` 不存在 | `numeric_std` 补齐这些重载 |
| `(39 downto 0 => vec, others => '0')` | VHDL-2008：范围选择对应一个数组值时逐元素展开，长度不符报 `VHDL-LENGTH` |
| `tab(i)(j) <= x` | 合并成扁平行上的一个索引：offset(i) × 行长 + offset(j)。offset 从最低位起计元素数：降序为 `i − right`，升序为 `right − i` |
| 覆盖枚举全部三个字面量的 case 被判为锁存器 | VHDL 要求 case 覆盖子类型的全部值，所以最后一个分支的条件恒为真；子类型以外的编码（两位编码的第四个值）不会出现 |
| `endfile(f)`、`readline(f, l)` 无匹配重载 | 无参数声明的 textio 和仿真例程接受任意参数，不求值 |
| Verilog：16 个常数项覆盖 4 位选择信号、没有 default（uart16550）被判为锁存器 | 选择信号不超过 12 位时检查覆盖：casez 的 z 位、casex 的 x/z 位展开为两个值，普通 case 中含 x 的项不覆盖任何值。全覆盖时最后一项不再需要条件 |
| 纯 VHDL 设计中本地声明的 `EHXPLLL` 组件被当作黑盒 | 先用 VHDL 前端分析，若有黑盒实例的名字是已建模原语，则改用混合链接器重新分析 |
| CLKOS 反馈的 ECP5 PLL 周期错误 | VCO 比例 = `CLKI_DIV / (CLKFB_DIV × 反馈输出的分频)`，各输出再乘自己的分频 |

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 370 个 CTest 用例全部通过（阶段 13 是 344 个）。新增：
  - Vivado 报告：新格式、旧格式和 `report_timing_summary` 的解析（后者用 01signal.com 上公开的一段 Vivado 输出）；一致的报告；周期不符、未匹配和缺失的时钟；只有 Vivado 知道的周期；按时钟名匹配；非报告文本报错（负例）；未配置时不增加阶段（负例）；`cli.vivado_clocks`。
  - 约束一致性：五种检查，各有正例和负例。
  - Lattice/Gowin：存储器与 DDR 的跨域路径（只读端口不产生跨域，负例）；三个厂商库的 VHDL 组件包；工程自带同名库时不替换（负例）；本地声明的原语组件；非原语的本地组件仍是黑盒（负例）。
  - VHDL：变量边界切片（降序与升序数组）及长度可变时报错（负例）；`std_match`/`is_x` 重载；范围选择中的切片及长度不符报错（负例）；两个变量索引；枚举 case 不推断锁存器；textio 例程。
  - Verilog：全覆盖 case 不推断锁存器；缺值时推断锁存器（负例）；casez 通配位展开，及缺值时的负例；普通 case 中含 x 的项不算覆盖（负例）。
- **真实工程**：
  - microwatt（ECP5-EVN）：0 个错误、1 个警告（RAM 初始化从文件读取，没有建模）、0 个锁存器、1217/1217 个寄存器解析出时钟、系统时钟 40 MHz（12 MHz × 20 × 2 / 12，与 Makefile 中的 `CLK_FREQUENCY` 一致）。
  - microwatt 全部 112 个 VHDL 文件中 111 个解析通过；唯一被拒绝的 `no-init-mem.vhdl` 缺少 `begin`，本身不是合法的 VHDL。
  - open-logic、neorv32、PoC、OSVVM、surf 的结果与阶段 13 相同。

## 5. 已知限制
- 没有真实的 Vivado 实现结果可用。报告解析按 UG835 的格式和一段公开的 Vivado 输出编写；样例报告 `examples/primitives/vivado_clocks.rpt` 是按这种格式手写的，不是 Vivado 生成的。
- 时钟按名字匹配。综合改名后（例如 `clk_IBUF_BUFG`），只能靠 SDC 时钟名匹配上。
- 从文件读取的存储器初始化（textio）不求值，报 `IR-UNSUPPORTED-INIT`。它不影响 CDC 结论。
- Gowin `IDDR`/`ODDR` 与 Xilinx 同名，没有建模；Lattice SERDES、`PDPW16KD`、iCE40 `SB_IO` 也没有建模。
- 判断 Verilog case 是否全覆盖只适用于不超过 12 位的选择信号。

## 6. 下一步建议
1. microwatt 的 Xilinx 配置（Arty：`clk_gen_plle2` 和 DRAM 控制器 litedram），覆盖多时钟域的真实 SoC。
2. 读取 Vivado 的 `report_cdc` 输出，与 OpenCDC 的 CDC 结论对照。
3. iCE40 `SB_IO` 的寄存器化 I/O 和 Lattice SERDES 模型。

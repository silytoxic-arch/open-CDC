# 阶段 78：Lattice Diamond 工程（.ldf）与 LPF 时序约束 —— f32c（ULX3S，ECP5）

日期：2026-11-09　版本：0.78.0

阶段 77 的下一步是验证一个 Lattice 工程。这一阶段选择 **f32c**（BSD-2-Clause）：一个 MIPS/RISC-V 兼容的 SoC，几乎全部用 VHDL 编写。验证的配置是它在 ULX3S（ECP5）上的 Lattice Diamond 工程 `ulx3s_sdram_dv.ldf`，取自 f32c/f32c，提交 546d817。

这个工程覆盖了几项 OpenCDC 以前没有的功能：
- Diamond 工程文件 `.ldf`；
- Lattice 的偏好文件 `.lpf`：用 `FREQUENCY PORT` 定义时钟，用 `BLOCK PATH FROM CLKNET ... TO CLKNET ...` 声明不定时的时钟对；
- 几种 VHDL 写法：时钟沿与多个使能相与、一个进程中有多个时钟 if、两个进程写同一个共享变量（真双口 RAM）、在细化时读取文件的 ROM 初始化函数；
- ECP5 原语：两个 EHXPLLL，以及 VHI/VLO/USRMCLK。

加入导入器后，第一次分析的情况：
- 2 个错误，整个设计无法分析；
- 至少 23 个锁存器推断；
- 7 处“时钟沿不在时钟进程条件中”；
- 若干未分析的构造。

修复后的结果：
- 0 个错误，0 个锁存器；
- 1 个未分析的构造，是从文件读取的 ROM 初值；
- 350/350 个寄存器的时钟已解析；
- 15 条跨时钟路径。

工程已加入语料库。

## 1. 完成的功能

### 1.1 Diamond 工程导入（`src/engine/DiamondProject.{h,cpp}`）

- 导入 `.ldf` 的默认实现（`default_implementation`）：
  - 设计文件按顺序导入：Verilog、SystemVerilog，以及 VHDL（库取自 `<Options lib="..."/>`）；标记为 `excluded="TRUE"` 的不导入；
  - IPexpress 模块（`.ipx`）：读取它生成的 HDL，即 `top_level_verilog` 或 `top_level_vhdl` 文件；
  - 约束：偏好文件 `.lpf`，以及 Synplify 约束 `.sdc`、`.fdc`；
  - 顶层：依次取实现的 `top`、`def_top`、源文件上标记的 `top_module`。
- 没有读取的内容都会报告：
  - `PRJ-LDF-MISSING`：缺失的文件；
  - `PRJ-LDF-SKIPPED`：被排除的文件，以及编程文件、功耗文件等其他文件；
  - `PRJ-LDF-IPX`：没有生成 HDL 的 IP 模块；
  - `PRJ-LDF-IMPLEMENTATION`：未导入的其他实现。
- CLI、GUI 的打开对话框和 `opencdc import` 都支持 `.ldf`。

### 1.2 LPF 时序偏好（`src/constraints/Lpf.{h,cpp}`）

`.lpf` 中与时钟分析有关的偏好会被翻译成 SDC 命令，再交给现有的 SDC 读取器。每条命令写在原偏好所在的行，所以诊断信息指向 `.lpf` 的原始行号。

| LPF | SDC |
|---|---|
| `FREQUENCY PORT "clk" 25 MHZ;`（也支持 NET、KHZ/HZ/GHZ） | `create_clock -name {clk} -period 40 [get_ports {clk}]` |
| `PERIOD PORT\|NET "x" 8 NS;` | 同上 |
| `BLOCK PATH FROM CLKNET "a" TO CLKNET "b";`（端点也可以是 PORT、NET、CELL） | `set_false_path -from [get_clocks -of_objects [get_nets {a}]] -to ...` |

其余偏好的处理：
- `BLOCK RESETPATHS/ASYNCPATHS/JTAGPATHS`：这些路径 CDC 分析本来就不定时，不需要处理。
- `MULTICYCLE`、`MAXDELAY`、`INPUT_SETUP`、全局 `FREQUENCY` 等其他时序偏好：汇总为一条 `CON-LPF-IGNORED` 提示。
- `LOCATE`、`IOBUF`、`SYSCONFIG` 等物理偏好：直接跳过。
- `BLOCK INTERCLOCKDOMAIN PATHS`：暂不支持，报告 `CON-LPF-PARSE` 并建议改用 `BLOCK PATH`。
- 格式不对的偏好：同样报告 `CON-LPF-PARSE`。

### 1.3 相关时钟之间的时钟级例外

- 以前的规则：时钟级的 `set_false_path` 或 `set_max_delay -datapath_only` 只在两个时钟关系未知时，把它们声明为异步。两个派生自同一时钟源的时钟（例如同一 PLL 的两个输出）仍按同步处理，它们之间的跨越不检查。
- 问题：这些路径既不定时，也不检查。
- 现在：只要有时钟级例外，即使两个时钟相关，也按异步检查。Vivado 的 `report_cdc` 也是这样处理的。原因说明中注明“the clocks are related, but these paths are not timed”。
- 寄存器级例外仍不改变时钟关系（反例测试）。
- 这条修改改变了语料库中两个项目的结果，都是设计者用时钟级 false path 声明的跨越，现在被检查：

| 项目 | 修改前 | 修改后 | 涉及的约束 |
|---|---|---|---|
| ao486_MiSTer | 605 条跨越路径（80 条已同步） | 1556 条（704 条已同步） | `ao486.sdc` 在 CPU 时钟与 VGA/UART/MPU/音频时钟之间设置 `set_false_path` |
| chiplab | 132 条（72 条已同步） | 138 条（78 条已同步） | `soc_up.xdc` 的两条 `set_false_path` |

### 1.4 VHDL

- **时钟沿与多个使能**：`if rising_edge(clk) and clk_enable = '1' and (not C_cache or dmem_cache_wait = '0') then`（`f32c_core.vhd`）。
  - 以前只接受“沿 and 一个使能”，这种写法被当作组合进程，产生锁存器和 `IR-UNSUPPORTED-EDGE`。
  - 现在 `clockEdge` 返回全部使能项，寄存器的使能是它们的与。
- **只由时钟 if 组成的进程**，按时钟沿分组：
  - 每个时钟沿生成一个时钟进程；
  - 同一时钟沿的多个 if 按原顺序放进同一个时钟体，各自在自己的使能下执行，所以后面的赋值仍然优先；
  - 例子：`f32c_cache.vhd` 中一个下降沿 if 加一个上升沿 if；`f32c_core.vhd` 和 `glue_sdram_dv.vhd` 中多个同沿 if，使能不同；
  - 反例测试：进程中只要有一条语句不是时钟 if，处理方式就不变（仍然报告 `VHDL-CLOCKED-TAIL`）。
- **两个进程写同一个共享变量数组**（`bram_true2p_1clk.vhd`，每个端口一个进程，两个端口在不同的 generate 分支中）：
  - 每个写进程写自己的一份副本，读操作看到所有副本，与厂商 RAM 模型的处理一致。
  - 细化之前，先按语法扫描架构中的进程，找出写共享变量的进程；未被选中的 generate 分支中的写进程也会计入。
  - 这些进程没有被细化，它们的副本在架构细化后接常量 0。
  - 反例测试：两个进程驱动同一个信号仍然是多驱动错误。
- **细化时读文件的 ROM 初始化函数**（`rom.vhd`）：
  - 函数体是 `while not endfile(f) loop readline(f, l); ...`。
  - 以前：`endfile` 的类型推断失败，报告 `VHDL-NO-OVERLOAD` 错误。
  - 现在：`endfile` 与 `readline` 一样，使函数在第一次读文件时停止，结果未知，报告 `IR-UNSUPPORTED-INIT`。文件内容本来就不读取。
- **部分端口开路**（`glue_sdram_dv.vhd`：`spi_cen(0) => spi_ss(i), spi_cen(3 downto 1) => open`）：开路的位是 X 常量，不驱动任何东西。以前整个端口被作为未分析的构造。

### 1.5 原语

| 原语 | 模型 |
|---|---|
| `VHI`、`VLO` | 常量 1 和 0（IPexpress/Clarity 模块中的 scuba 常量） |
| `USRMCLK`（ECP5） | 由逻辑驱动配置 SPI 时钟引脚，只有输入 |

这三个原语都加入了 VHDL 组件包 `ecp5u.components`。

## 2. 真实工程验证

### f32c ULX3S `sdram_dv`（直接分析 `.ldf`，加入语料库）

| | 第一次分析（加入导入器后） | 修复后 |
|---|---|---|
| 错误 | 2：共享变量 RAM 多驱动、`not endfile` 重载失败；无法分析 | 0 |
| 锁存器 | 至少 23 | 0 |
| 未分析的构造 | 时钟沿 7 处、部分端口 3 处、未知原语 5 处等 | 1（文件初始化的 ROM） |
| 寄存器 | — | 350，全部时钟已解析 |
| 时钟 | — | 5：`clk_25m`（LPF）、CPU 90 MHz、像素 74.25 MHz、5 倍像素 371.25 MHz、112.5 MHz |
| 跨时钟路径 | — | 15（3 条已同步，12 条未同步） |

结果说明：
- 15 条路径都在 DVI 帧缓冲 `dvi_fb.vhd` 中。设计者用 LPF 的 `BLOCK PATH` 声明 CPU 时钟与像素时钟之间不定时（见 1.3）。
- 像素时钟 → CPU 时钟：FIFO 指针用两级/三级同步器同步，识别为已同步，另有一处重新汇合（CDC-005）。
- CPU 时钟 → 像素时钟：
  - 视频时序寄存器 `R_hdisp` 等只经过一级寄存器（CDC-003/002）。源码注释称它们“基本静态，从主时钟同步过来”，需要设计者确认或豁免。
  - 像素 FIFO 的 RAM 读出（CDC-004/008）是自定义的异步 FIFO，需要设计者复核。

### 其他语料

- SV 语料共 32 个项目，全部匹配，新增 f32c。
  - ao486_MiSTer 与 chiplab 的预期值已按 1.3 更新；
  - 其余项目的 CDC 结果与修改前逐行相同。
- VHDL 语料 48 个全部匹配。

## 3. 修改的文件

- **新文件**：
  - `src/engine/DiamondProject.{h,cpp}`；
  - `src/constraints/Lpf.{h,cpp}`。
- **工程与界面**：
  - `src/engine/ProjectConfig.cpp`、`src/engine/CMakeLists.txt`；
  - `src/cli/main.cpp`、`src/gui/MainWindow.cpp`。
- **约束与时钟**：
  - `src/constraints/Sdc.cpp`、`src/constraints/CMakeLists.txt`：读取 `.lpf`；
  - `src/clock/ClockAnalyzer.cpp`：相关时钟之间的时钟级例外。
- **VHDL 前端**：
  - `src/frontend/vhdl/VhdlStmt.cpp`：多使能、按时钟沿分组、共享变量副本、文件读取的循环；
  - `src/frontend/vhdl/VhdlElab.{h,cpp}`：共享内存、部分开路的输出；
  - `src/frontend/vhdl/VhdlExpr.cpp`：共享内存的读、`endfile` 的类型；
  - `src/frontend/vhdl/VhdlNames.cpp`：`endfile`。
- **原语模型**：`src/frontend/primitives/lattice_gowin.v`、`lattice_components.vhd`。
- **测试**（每个用例都含反例）：
  - `tests/engine/test_diamond_project.cpp`：新增，3 个用例；
  - `tests/constraints/test_sdc.cpp`：2 个用例；
  - `tests/vhdl/test_vhdl_elab.cpp`：4 个用例；
  - `tests/clock/test_clock.cpp`：1 个用例；
  - `tests/cli/RunImport.cmake`：提示文字。
- **文档与语料**：
  - `README.md`、`docs/primitives.md`；
  - `tools/ci/sv-corpus.sh`：f32c，ao486/chiplab 的预期值。
- **版本**：`CMakeLists.txt`，0.78.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：649 个测试全部通过。
- 第一次运行时有两类失败，都已修正：
  - CLI 导入测试的匹配文字含括号，被当作正则表达式；
  - 我的 VHDL 测试把开路位写成了 `{1'bx, 1'bx, 1'bx, s}`，实际的转储格式是 `{3'bxxx, s}`。
- 语料回归中发现我自己引入的一个错误，已修正：
  - 原因：共享变量的预扫描最初对 if-generate 条件求值，在 for-generate 内求值时循环变量尚未声明，于是报告了错误（microwatt、neorv32、mipi_csi2_rx 失败）。
  - 修正：改为扫描全部分支，不求值；未细化的写进程的副本接 0。
- SV 语料 32 个、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 77 的 Windows CI（报告提交 759531e，运行 42573129655）：Release 与 Debug 均 639/639 通过，生成 `OpenCDC-0.77.0-windows-x64.zip` 与 `.exe`。上一次运行中 Debug 的失败是下载依赖时的 HTTP 504，与代码无关。
- 本阶段的 Windows CI：见提交后的补充。

## 5. 已知限制

- **LPF**：`BLOCK INTERCLOCKDOMAIN PATHS`、`MULTICYCLE` 等偏好不读；Diamond 策略文件（`.sty`）中的 include 路径和宏不读。
- **共享变量 RAM**：
  - 按进程语句识别写进程。for-generate 中同一个进程语句的多个实例算作一个写者，此时仍会报告多驱动。
  - 未选中的 generate 分支中的写者留下接 0 的副本：读出端多一个与常量 0 的异或。
- **ROM 初值**：从文件读取的 ROM 初值未知，不影响 CDC 结果。
- **MachXO2 原语**：`DP8KC`、`EFB`、`BB` 等没有模型。
- **INTERCLOCKDOMAIN**：要支持它，需要一种“所有时钟两两异步”的约束形式。

## 6. 下一步

阶段 79：继续用真实工程验证。候选：
- 一个 MachXO2/MachXO3 的 Diamond 工程，为 `DP8KC`、`EFB` 等原语建模；
- 一个 Radiant 工程（`.rdf` 与 `.pdc`），用于 CrossLink-NX/Certus-NX。

# 阶段 16 完成报告：经译码逻辑的限定数据、按时钟对汇总

日期：2026-10-25　版本：0.16.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T16.1 限定数据识别 | 以下几类路径以前被误报为未同步或 CDC-004，现在识别为限定数据：<br>- 源数据参与计算使能；<br>- 源数据只作为选择端，被选的是同步后的请求；<br>- 源数据与同步后的请求相与；<br>- `req_sync ? f(src) : 0`（被降低为同步清零）。<br>经译码后又连续打两拍的源数据，在有限定条件时不再当作"逻辑后面的两级同步器"。 |
| T16.2 时钟对汇总 | CLI、GUI 运行报告、HTML 报告和 JSON（`clockPairs`）中，按（源时钟, 目的时钟）列出路径数、已同步、违例、潜在风险和需复核数。需复核路径来自未知时钟关系时，CLI 提示使用 `--suggest-sdc`。 |
| T16.3 约束 | `set_input_delay -clock [get_clocks -of_objects …]`。BSCANE2 的 TAP 寄存器改为经 `TCK` 引脚取时钟，这样约束建议中的 `create_clock … [get_pins {…/bscan/TCK}]` 能覆盖整个 JTAG 时钟域（阶段 15 中模型内部还剩一个寄存器挂在黑盒时钟上）。 |

## 2. 修改的文件

**新建**
- `src/engine/ClockPairs.{h,cpp}`
- `tests/engine/test_clock_pairs.cpp`
- `docs/progress/stage-16.md`

**修改**
- `src/cdc/CdcAnalyzer.cpp`：使能路径的限定、`andQualifier`（与门与同步清零）、选择端为源时的限定候选；同步链按"源直接驱动的先处理"的顺序识别
- `src/constraints/Sdc.{h,cpp}`、`src/clock/ClockModel.h`、`src/clock/ClockAnalyzer.cpp`：`set_input_delay` 的 `-of_objects`
- `src/frontend/primitives/xilinx_unisim.v`：BSCANE2
- `src/engine/ResultJson.cpp`、`src/engine/Report.cpp`、`src/engine/CMakeLists.txt`、`src/cli/main.cpp`、`src/gui/MainWindow.cpp`：时钟对汇总
- `tests/cdc/test_cdc.cpp`、`tests/CMakeLists.txt`（`cli.clock_pairs`）
- `tools/ci/vhdl-corpus.sh`：microwatt Arty 的期望值；新增"加 JTAG 约束"的版本
- `README.md`、`docs/architecture.md`（第 23 节）、`CMakeLists.txt`（版本 0.16.0）

JSON 只新增字段 `cdc.clockPairs`，已有字段不变。

## 3. 关键算法

### 3.1 与门限定（`andQualifier`）
对目的寄存器的每个数据位，沿别名找到驱动它的单元。若该单元是与门，并且它的某个输入由一个寄存器驱动，而这个寄存器是来自源时钟（或与之相关的时钟）的同步器输出，则该位受限定。所有位都受限定时，路径为限定数据。

`q <= req_sync ? f(src) : 0` 在 IR 中是"数据 f(src)，由 `!req_sync` 同步清零"，所以同步复位引脚只有一个来源，且来源是同步器输出时，也视为限定。

### 3.2 同步链的识别顺序
第一遍先处理数据由源寄存器直接驱动（没有组合逻辑）的路径。它们识别出的同步链末级被登记为限定信号，供第二遍使用。第二遍处理经过逻辑的路径：若此时能找到限定条件，就交给"限定数据"处理，不再按"逻辑后面的同步链"报 CDC-004。没有限定条件时仍报 CDC-004（负例测试覆盖）。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 381 个 CTest 用例全部通过（阶段 15 是 377 个）。新增：
  - 使能中的译码，及被选的限定信号；无限定的译码、源数据与限定信号异或（负例）。
  - 与门限定、同步清零形式；或运算（负例）。
  - 无限定的"译码后两拍"仍报 CDC-004（负例）。
  - 时钟对汇总：计数、排序，HTML 与 JSON 中的输出；没有 CDC 结果时为空（负例）。`cli.clock_pairs`。
  - `set_input_delay -clock [get_clocks -of_objects …]`，及网线上没有时钟时报 `CON-UNMATCHED`（负例）。
- **真实工程 microwatt Arty A7**：
  - 173 条跨域路径中 170 条已同步（阶段 15 为 145 条）；同步器结构从 32 个降到 15 个，因为原来被误认作两级同步链的 DMI 数据路径现在归为限定数据。
  - 按时钟对汇总：TCK→系统时钟 120 条、系统时钟→TCK 36 条，它们就是全部的需复核路径。
  - 加上约束建议给出的 TCK 时钟和时钟组：需复核路径为 0，剩下 2 个错误，都是设计问题：
    - CDC-006：`dmi_dtm_xilinx.vhdl` 用系统时钟域的 `sys_reset` 异步复位 JTAG 域的寄存器，释放没有同步；
    - CDC-004：DMI 应答在系统时钟域先经地址多路选择，再进入 JTAG 域的两级同步器。
  - 其他语料（open-logic、neorv32、PoC、OSVVM、surf、microwatt ECP5、PULP axi_cdc）结果不变。

## 5. 已知限制
- 与门限定只看目的寄存器数据输入的第一个单元：限定信号必须是这个与门的直接输入，不跟随多级与门或反相器。
- 同步清零形式要求同步复位引脚只有一个来源（同步器输出本身或其反相）。
- 限定数据的状态是"协议未验证"：工具不检查源数据在限定信号有效期间是否保持稳定。

## 6. 下一步建议
1. 限定识别沿多级与门、反相器展开，并检查源数据在限定信号有效期间不变的握手协议（请求保持到应答返回）。
2. 复位路径汇总：按复位源列出受影响的时钟域和寄存器数，方便处理 CDC-006。
3. 更多真实工程：带 Xilinx MIG 或 PCIe 的设计（需要更多黑盒原语模型）。

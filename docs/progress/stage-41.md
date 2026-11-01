# 阶段 41：Transceivers Wizard（gtwizard_ultrascale）模型，SURF 收发器工程

日期：2026-11-01　版本：0.41.0

## 1. 完成的功能

### 1.1 `.xci` 模型：`xilinx.com:ip:gtwizard_ultrascale`

UltraScale/UltraScale+ 的 GTH、GTY 收发器向导以前不被建模，`.xci` 留作未知模块，收发器的用户时钟和它们驱动的逻辑都无法分析。

- **端口**：
  - JSON `.xci`：使用其中的端口列表；
  - XML `.xci`：使用配置启用的端口（`INTERNAL_PORT_ENABLED_*` 大于 0，-1 表示禁用；以及 `ENABLE_OPTIONAL_PORTS`）。
- **位宽**：按通道的位宽乘以 `C_TOTAL_NUM_CHANNELS`：
  - 用户数据为 `C_TX_USER_DATA_WIDTH`/`C_RX_USER_DATA_WIDTH`；
  - `drpaddr_in`：GTHE3 为 9 位，其余为 10 位；`txdiffctrl_in`：GTHE3 为 4 位，其余为 5 位；
  - 已与 SURF 中 23 个组件声明核对。
- **时钟**：
  - TX/RX 源时钟（`gtwiz_userclk_*_srcclk_out`，或 `txoutclk_out`/`rxoutclk_out`）是独立的时钟，周期为 `C_*_USRCLK_FREQUENCY` × `C_*_USER_CLOCKING_RATIO_FSRC_FUSRCLK`；
  - `usrclk`、`usrclk2` 由源时钟派生（`OPENCDC_CLOCK_RATIO`），并由源时钟端口驱动，所以 `get_clocks -of_objects [get_pins …/gtwiz_userclk_rx_srcclk_out]` 与 Vivado 一样能找到这三个时钟；
  - 用户时钟在 IP 外部时（`txusrclk2_in`/`rxusrclk2_in`），使用用户的时钟。
- **时钟域**：
  - TX/RX `usrclk2`：用户数据、8b/10b、gearbox、逗号对齐、极性、缓冲状态、reset-done，以及缓冲旁路辅助模块（`gtwiz_buffbypass_*_start_user_in`、`_done_out`、`_error_out`）；
  - `drpclk_in`：`drp*`；
  - `gtwiz_reset_clk_freerun_in`：`gtwiz_reset_rx_cdr_stable_out` 和 QPLL 复位输出；
  - 异步输出：PMA/PRGDIV reset done、power good、CDR 与 CPLL lock、`rxsyncdone_out`、`rxphaligndone_out`、`rxdlysresetdone_out`、`cpll*clklost_out`；
  - 不读取：在 IP 内同步的复位、通道的 PMA/PCS/CDR 复位、静态与模拟控制（`loopback_in`、TX 摆幅与预加重、`txinhibit_in`、CPLL 控制）、参考时钟、串行引脚。
- **不建模时明确报告**（`FE-XCI-UNSUPPORTED`）：列出不认识的端口；没有配置用户时钟频率时不编造周期；配置中没有任何端口。

### 1.2 只有 Verilog 时的模型内置黑盒

IP 模型（以及原语模型）使用内置黑盒 `opencdc_async_source` 表示没有时钟域的信号。工程只有 Verilog（例如 `.xci` 本身作为顶层，或 Verilog 顶层加 `.xci`）时，第一次 slang 精化报 "unknown module 'opencdc_async_source'"，以前没有重新用模型精化。现在这种情况与缺少原语一样，转到带模型的精化。

### 1.3 VHDL：用子类型名做切片（在 SURF 中发现）

- `d(FIELD_C)`：`FIELD_C` 是离散子类型名时是切片，不是下标。以前在赋值目标中得到类型为空的下标而崩溃（段错误），在表达式中产生 19 个重载错误。新函数 `isSliceRange` 同时识别范围和离散子类型名，用于取值、赋值目标和类型推导。
- 赋值目标的下标无法求值时报 `VHDL-INDEX`，不再崩溃。
- 只有一个字段的记录：先整体赋值、再部分赋值时，以前误报 `VHDL-NO-HARDWARE`。已修正已知值的整体赋值捷径（类型不同时转换为目标类型的位）。

### 1.4 `unisim.vcomponents` 补全

`BUFG_GT`、`IBUFDS_GTE2`、`IOBUF`、`LUT6_2` 有 Verilog 模型，但 VHDL 组件包中缺少声明。SURF 的 `Pgp2fcGtyCoreWrapper` 因此报 `VHDL-COMPONENT`。已补全，并加测试保证每个 Xilinx unisim 模型都有组件声明。

## 2. 真实工程验证（SURF，slaclab/surf）

- **30 个 Transceivers Wizard `.xci` 单独分析**（PGPv2b、PGPv2fc、PGPv3，GTHE3/GTHE4/GTYE4，XML 与 JSON）：全部建模，0 错误。以前 0/30（23 个因为内置黑盒，7 个因为缓冲旁路和 CPLL 端口）。
- **`Pgp3GthUsIpWrapper`**（5 个线速率的 `.xci`）：6 个时钟，195/195 个寄存器已解析，0 错误 0 警告，32 条跨域路径，22 条已同步。
- **完整的 PGPv3 核心 `Pgp3GthUs`**（57 个文件，DRP 与监视打开）：
  - 2158/2158 个寄存器已解析，209 条路径；
  - 用 Vivado 风格的约束（`get_clocks -include_generated_clocks -of_objects` 选择收发器源时钟）后，没有未知关系；
  - 9 个错误违例：已对照 SURF 源码确认在该配置下是真实的（`pgpRxClk` 接到 TX `usrclk2`，而 `gearboxAligned` 和 `rx_done` 在 RX `usrclk2` 域；9 位 FIFO 计数经 `SynchronizerVector` 逐位同步）。
- **`Pgp2fcGtyCoreWrapper`**（用户时钟在 IP 外部、缓冲旁路、CPLL 端口、`BUFG_GT`）：5 个时钟，458/458 个寄存器已解析，0 错误 0 警告，52 条路径，36 条已同步；`gtwiz_buffbypass_rx_done_out`（RX 用户时钟）经两级同步进入 `axilClk`。

这些都加入了 VHDL 语料库（`tools/ci/vhdl-corpus.sh`）：3 个工程的结果检查，以及 30 个 `.xci` 各自必须建模成功。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`gtWizard` 及分派。
- `src/frontend/AutoFrontend.cpp`：缺少 `opencdc_async_source` 时用模型重新精化。
- `src/clock/ClockAnalyzer.cpp`：黑盒时钟读取 `OPENCDC_CLOCK_PERIOD`。
- `src/frontend/vhdl/VhdlElab.h`、`VhdlNames.cpp`、`VhdlExpr.cpp`、`VhdlStmt.cpp`：子类型名切片、`VHDL-INDEX`、单字段记录。
- `src/frontend/primitives/unisim_vcomponents.vhd`：4 个组件。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 端口与位宽（2 通道 GTHE4、GTHE3、GTYE4），时钟周期与比例，各时钟域；负例：禁用的端口、未启用的端口不出现；
    - 用户时钟在 IP 外部、缓冲旁路、CPLL：域寄存器只读取同步端口，负例为复位与静态控制不被读取；
    - 负例：未知端口（消息中列出）、没有频率、没有端口；
    - 端到端：同一 `usrclk2` 内没有路径；用 `get_clocks -of_objects` 设为异步后 1 条未同步路径，无未知关系，无 `CON-UNMATCHED`；时钟周期 6.4 ns。
  - `tests/frontend/test_primitives.cpp`：每个 unisim 模型都有 VHDL 组件。
  - `tests/vhdl/test_vhdl_records.cpp`：子类型名切片，单字段记录。
- `tools/ci/vhdl-corpus.sh`：SURF 收发器工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.41.0。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 只建模列出的端口；其他端口（例如眼图扫描、PRBS、`rxosintdone_out`）会使 IP 不被建模，并列出这些端口。
- 收发器内部的复位顺序（reset helper 的状态机）不建模：复位输入在 IP 内同步，不检查。
- 动态重配置（DRP 改变线速率）不建模，周期为配置值。
- 7 系列收发器向导（`gtwizard`，GTX/GTP）没有建模。

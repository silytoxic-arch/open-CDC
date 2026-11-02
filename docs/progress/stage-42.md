# 阶段 42：7 系列 Transceivers Wizard；VHDL 组件声明给出 IP 端口；实例引脚上的 get_clocks -of_objects

日期：2026-11-02　版本：0.42.0

## 1. 完成的功能

### 1.1 VHDL 组件声明给出 XML `.xci` 的端口

XML `.xci` 中没有端口列表。以前只能从旁边的组件文件（`<instance>.xml`）读取，没有这个文件时，很多 IP 无法建模。SURF 等工程只提交 `.xci`，但设计中有 IP 的 VHDL 组件声明。

- 工程中有 `.xci` 时，扫描所有 VHDL 源文件中的组件声明（结构体或包中都可以）。
- XML `.xci` 没有组件文件时，用同名（不区分大小写）的组件声明作为端口列表。
- **位宽必须是字面值**：`std_logic`、`std_logic_vector(8 downto 0)`、`(16-1 downto 0)`、`(0 to 3)`、`unsigned`/`signed`、带库前缀的类型名。
  - 位宽依赖 generic 或常量时，不猜测，该组件不使用；
  - 同名组件有两个不同的声明时，不使用；完全相同的重复声明可以使用。
- 模型的说明中写出使用的声明位置（"ports from the VHDL component declaration at 文件:行"）。
- 这是通用机制，对所有从端口列表建模的 IP 都有效。

### 1.2 `.xci` 模型：7 系列 Transceivers Wizard（`xilinx.com:ip:gtwizard` 3.x，GTP/GTX/GTH）

7 系列向导的端口不能只由几个参数确定，所以必须有实例的端口（JSON、组件文件或 VHDL 组件声明）。

- **端口**：按通道为 `gt<N>_<端口>`，去掉前缀后分类，不区分大小写。
- **时钟**：
  - `gt<N>_txoutclk_out`/`rxoutclk_out` 是独立时钟，周期 = 线速率 / 内部数据位宽（`gt<N>_val_*_line_rate`、`gt<N>_val_*_int_datawidth`，输出时钟源为 AUTO 或 TXOUTCLKPMA 时）；
  - `*outclkfabric_out` 使用参考时钟频率；`txoutclkpcs_out` 没有周期；
  - 用户时钟是设计接到 `gt<N>_txusrclk2_in`/`rxusrclk2_in` 的时钟。
- **时钟域**：
  - 本通道的 `usrclk2`：用户数据、gearbox、8b/10b、逗号对齐、极性、PRBS、reset-done；
  - `gt<N>_drpclk_in`：`drp*`；
  - `sysclk_in`：复位状态机的输出（`*_fsm_reset_done_out`、`*_mmcm_reset_out`、`qpllreset_out`、`pll0/1reset_out`）；
  - 异步：眼图扫描、数字与 RX 监视、CPLL lock 与 clock lost、PMA reset done、相位对齐状态、`rxelecidle_out`、GTP 的 `rxphmonitor_out`/`rxphslipmonitor_out`；
  - 不读取：在 IP 内同步的复位和 lock 输入、模拟和静态控制、参考时钟与 QPLL/PLL 时钟、串行引脚。
- **不建模时明确报告**（`FE-XCI-UNSUPPORTED`）：
  - 没有端口：说明三种给出端口的方式；
  - 不认识的端口：列出这些端口；
  - 某个时钟域的端口存在，但没有该域的时钟端口：列出缺少的时钟端口。

### 1.3 `get_clocks -of_objects` 作用在时钟网络的另一个名字上

`get_clocks -of_objects [get_pins u/gt0_rxoutclk_out]` 以前报 `CON-UNMATCHED`（"no register is clocked through it"），尽管寄存器正是经过连在这个引脚上的父模块网络打的时钟。原因是时钟追踪只记录了寄存器的时钟输入和经过的单元，没有记录同一网络的别名（实例端口、assign）。

现在没有直接记录的名字会按驱动源查找：驱动源相同（只经过别名，不经过逻辑）的网络带有相同的时钟。数据网络即使经过别名，也仍然不会被当作时钟。该修正适用于 `set_clock_groups`、时钟级例外和 `set_input_delay -clock` 中的 `-of_objects`。

## 2. 真实工程验证（SURF，slaclab/surf）

| 工程 | `.xci` | 结果 |
|---|---|---|
| `Pgp3Gtx7IpWrapper`（GTX） | 3、6、10 Gb/s 共 3 个 | 6 个时钟，200/216 个寄存器已解析，34 条路径，24 条已同步；无未知关系；9 条潜在风险是 AXI-Lite 异步 FIFO 的 Gray 指针（与阶段 41 的 GTH 相同），1 条需复查 |
| `Pgp3Gtp7IpWrapper`（GTP，含 TX/RX gearbox） | 3、6 Gb/s 共 2 个 | 6 个时钟，269/269 个寄存器已解析，0 错误 0 警告，43 条路径，31 条已同步；无未知关系 |

- **时钟周期与 SURF 一致**：
  - GTX 10G 的 RXOUTCLK：10.3125 / 32 = 322.27 MHz，与 SURF 注释"322.26 MHz (3.103 ns)"一致；
  - GTX 6G：195.31 MHz，与注释"5.12 ns"一致；
  - GTP 6.25G：6.25 / 16 = 390.625 MHz，与 SURF 中 RX MMCM 的 `CLKIN_PERIOD_C = 2.56`（6.25 Gbps）一致。
- **GTX 的 1 个警告是真实的**：
  - `ClockManager7` 内部有一个 `AxiLiteToDrp`，它的 `drpClk` 接 `ClockManager7` 的 `axilClk`；
  - SURF 的封装没有连接这个端口，该端口保持默认值 '0'；
  - 所以那 16 个寄存器确实不会翻转（`CLK-CONSTANT`）。
- **`get_clocks -of_objects [get_pins -hier *U_Pgp3Gtx7Ip*/gt0_rxoutclk_out]`** 加上 `-include_generated_clocks` 后，得到 RXOUTCLK 和它经 MMCM 派生的两个时钟。修正前，这一组为空，9 对时钟关系未知。
- 这两个工程加入 VHDL 语料库。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：`XciPort`、`XciComponent`、`XciContext::vhdlComponents`、`vhdlComponentPorts`、`gtWizard7`，以及分派时使用组件声明。
- `src/frontend/AutoFrontend.cpp`：工程中有 `.xci` 时扫描 VHDL 组件声明。
- `src/clock/ClockAnalyzer.cpp`：`-of_objects` 按驱动源查找别名网络。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 组件端口：标量、`16-1 downto 0`、`0 to 3`、inout、buffer、带库前缀的类型、重复的相同声明；负例：generic 位宽、两个不同的同名声明；
    - 7 系列模型：位宽、RXOUTCLK 周期 5.12 ns、fabric 时钟 6.4 ns、各时钟域、异步输出、说明中的声明位置；负例：复位、loopback、串行引脚、`rxusrclk_in` 不被读取；
    - 负例：没有端口、未知端口（列出）、缺少 TX 用户时钟端口；
    - VHDL 端到端：同一时钟内没有路径；用 `get_clocks -of_objects [get_pins u/gt0_rxoutclk_out]` 分组后 1 条未同步路径，无未知关系，无 `CON-UNMATCHED`。
  - `tests/clock/test_clock.cpp`：实例引脚是时钟网络的别名时 `-of_objects` 能找到时钟；负例：经过 assign 别名的数据网络仍然报 `CON-UNMATCHED`。
  - 已确认：撤掉 `ClockAnalyzer` 的修正后，这两个测试共 4 个断言失败。
- `tools/ci/vhdl-corpus.sh`：两个 SURF 7 系列工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.42.0。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 7 系列向导的端口只能来自实例：只有 XML `.xci`、没有组件文件或 VHDL 组件声明时不建模（例如 Verilog 设计）。
- Verilog 实例化不提供端口位宽，所以只扫描 VHDL 组件声明。
- TXOUTCLK 选择参考时钟或 PCS 时钟作为源时，不给周期。
- 收发器向导内部的复位状态机不建模。

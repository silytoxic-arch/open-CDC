# 阶段 47：XAUI IP 模型；xpm_fifo_sync；修正语料库错误加载 SURF dummy 实体的问题

日期：2026-11-02　版本：0.47.0

## 1. 完成的功能

### 1.1 `xaui`（XAUI，PG053），含共享逻辑的核

端口来自实例（JSON、组件文件或 VHDL 组件声明）。

- **输出时钟**：`clk156_out` 是独立的时钟，也是全部 4 条 lane 的用户时钟。它来自 lane 0 的 TXOUTCLK，频率 = lane 速率 / 20：XAUI（3.125 Gb/s）为 156.25 MHz，DXAUI（6.25 Gb/s）为 312.5 MHz。RX 弹性缓冲把接收数据转到这个时钟。
- **`clk156_out` 时钟域**：XGMII、`configuration_vector`、`status_vector`、`debug`；收发器状态与控制（`gt_txresetdone`、`gt_rxresetdone`、`gt_rxbufstatus`、`gt_rxdisperr`、`gt_rxnotintable`、`gt_rxcommadet`、极性、PRBS）。
- **`dclk` 时钟域**：各 lane 的 DRP（`gt<N>_drp*`）。
- **异步输出**：`clk156_lock`、PMA reset done、相位对齐状态、QPLL lock、眼图扫描、数字监视、power good。
- **不读取**：`reset`、`signal_detect`、收发器复位、静态控制（`loopback`、`txdiffctrl` 等）、串行引脚。
- **不建模时明确报告**：
  - 不含共享逻辑的核（没有 `clk156_out`），这时用户时钟由设计提供；
  - MDIO 端口（作为未分类端口列出）。

### 1.2 `xpm_fifo_sync` 原语模型

SURF 的 `FifoXpm` 在同步模式下实例化 `xpm_fifo_sync`。以前没有这个模型，实例被当作黑盒（`IR-UNRESOLVED-INSTANCE`）。

- 新增 Verilog 模型：存储器、指针和输出寄存器都在 `wr_clk` 中（UG974：读写都使用 `wr_clk`）。
- 新增 `xpm.vcomponents` 中的组件声明。
- 经过它的数据不跨时钟；它的输出在别的时钟中读取时，仍然是跨时钟路径。

### 1.3 语料库修正：SURF 实体选择真实实现，不选 dummy

**问题**：SURF 的依赖解析器在一个实体有多个文件时，选了按路径排序的第一个。`base/fifo/rtl/dummy/FifoXpmDummy.vhd` 排在 `rtl/xilinx/FifoXpm.vhd` 前面，所以选中了 dummy。

**影响**：dummy 是空实现，没有 FIFO，只把输出接到常量。阶段 44 和 46 的 UltraScale+ 10G 以太网（MAC 使用 `SYNTH_MODE_G => "xpm"`）因此少分析了 MAC 的 TX/RX FIFO。

**修正**：

- 解析器按 ruckus 的做法（Vivado 2019.1 及以后）选择：有真实实现时用真实实现，只有 dummy 时才用 dummy（例如 Altera 的 `FifoAlteraMfDummy`）。
- 语料库脚本和本地工程脚本都已修正。
- 只有使用 XPM FIFO 的工程结果改变；7 系列、GTH UltraScale 和 1G 以太网使用推断的 FIFO，结果不变。

## 2. 真实工程验证（SURF）

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `XauiGthUltraScaleWrapper`（GTH UltraScale，xaui 12.2，推断 FIFO） | 5（578/578） | 0 错误 0 警告，16 条路径，12 条已同步，0 个违例，4 条需复查 |
| `XauiGthUltraScaleWrapper`（GTH UltraScale+，xaui 12.3，FifoXpm） | 5（485/485） | 0 错误 0 警告，30 条路径，28 条已同步，2 个 CDC 错误 |
| `XauiGtyUltraScaleWrapper`（GTY UltraScale+） | 5（485/485） | 同上 |
| `TenGigEthGthUltraScale`（US+，xxv_ethernet）重新分析 | 5（649/649） | 64 条路径（以前 35），50 条已同步，4 个 CDC 错误（以前 2 个） |
| `TenGigEthGtyUltraScale`（US+）重新分析 | 5（649/649） | 同上 |

- **新出现的 CDC 错误是真实的**（每个工程 2 个：CDC-001 和 CDC-003）：
  - 在 SURF 的 `FifoXpm` 中，`xpm_fifo_async` 后面的 `FifoOutputPipeline` 工作在 `rd_clk`，生成参数为 `RST_ASYNC_G => false`，复位端口接的是 FIFO 的 `rst`。
  - 在 `AxiStreamFifoV2` 中，这个 `rst` 是写侧的 `sAxisRst`。也就是说，写时钟域的复位没有经过同步，就在读时钟域作为同步复位使用。
  - 在 MAC 的 RX FIFO 中，这对应从 PHY 时钟（`clk156_out` 或 `tx_mii_clk`）到 DMA 时钟的路径。
  - 相比之下，`xpm_fifo_async` 本身在内部把复位同步到读侧，SURF 推断的 FIFO 也是如此。
- **10G US+ 原有的 2 个 CDC-004** 仍在：`txRstdone and rxRstdone` 先组合再同步（阶段 44）。
- **XAUI GTH UltraScale 的 4 条需复查路径**都是 MAC 异步 FIFO 的 Gray 指针。
- 3 个 XAUI 工程加入 VHDL 语料库；两个 10G US+ 工程的期望结果已按上表更新。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`xaui` 模型和分派。
- `src/frontend/primitives/xilinx_xpm.v`、`xpm_vcomponents.vhd`：`xpm_fifo_sync`。
- 测试：
  - `tests/frontend/test_xci.cpp`：XAUI 各时钟域、156.25 MHz、DRP、异步输出、串行引脚为常量；负例：复位、`signal_detect`、收发器复位、静态控制不被读取；DXAUI 312.5 MHz；负例：不含共享逻辑的核、MDIO 端口。
  - `tests/frontend/test_primitives.cpp`：
    - `xpm_fifo_sync` 已内置；
    - VHDL 中同一时钟读写，没有跨时钟路径，没有未解析实例；负例：在另一个时钟读取输出时报跨时钟路径；
    - 已确认去掉模型后这个测试失败（4 个断言）。
- `tools/ci/vhdl-corpus.sh`：dummy 实体的选择、3 个 XAUI 工程、10G US+ 的新期望。
- 文档：`docs/vivado-ip.md`、`docs/primitives.md`、`README.md`。
- `CMakeLists.txt`：0.47.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- XAUI 不含共享逻辑的核不建模；7 系列 XAUI 在 SURF 中只有 RTL（没有 `.xci`），没有用真实工程验证。
- `xpm_fifo_sync` 模型不区分 `READ_MODE`/`FIFO_READ_LATENCY`，因为它们不影响跨时钟分析。
- 阶段 44 的 10G US+ 结果（35 条路径）是在加载 dummy FIFO 时得到的。阶段 44 文档保留原样，这里给出更正后的结果。

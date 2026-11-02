# 阶段 48：100G 以太网（cmac_usplus）IP 模型

日期：2026-11-02　版本：0.48.0

## 1. 完成的功能

### 1.1 `cmac_usplus`（100G Ethernet，PG203），CAUI-4 与 AXI4-Stream 用户接口

端口来自实例（JSON、组件文件或 VHDL 组件声明）。

- **输出时钟**：
  - `gt_txusrclk2` 和 `gt_rxusrclk2` 是独立的时钟。GTY 内部数据通路为 80 位，频率 = 线速率 / 80，25.78125 Gb/s 时为 322.27 MHz。SURF 在 PLL 上写的 `CLKIN_PERIOD_G => 3.1`（322.58 MHz）与此一致。
  - `gt_rxrecclkout` 也是独立的时钟，不给周期。
  - `gt_ref_clk_out` 是经过 IP 的参考时钟。它用阶段 46 的派生时钟表示：从 `gt_ref_clk_p` 派生，比例 1，输入周期取自 `C_GT_REF_CLK_FREQ`。
- **`gt_txusrclk2` 时钟域**：TX AXI4-Stream、`tx_preamblein`、`tx_ovfout`/`tx_unfout`、`ctl_tx_*`、`stat_tx_*`、`usr_tx_reset`、TX 收发器调试端口。
- **`rx_clk` 时钟域**：RX AXI4-Stream、`rx_preambleout`、`rx_otn_*`、`ctl_rx_*`、`stat_rx_*`（含 RS-FEC）、`usr_rx_reset`。`rx_clk` 由设计提供：同步模式下接 `gt_txusrclk2`，异步模式下由 RX FIFO 跨域。
- **其他时钟域**：RX 收发器调试端口在 `gt_rxusrclk2`；核的 DRP 在 `drp_clk`；收发器 DRP 和 common DRP 在 `gt_drpclk`。
- **异步输出**：眼图扫描、power good。
- **不读取**：`sys_reset`、`core_*_reset`、`gtwiz_reset_*`（在 IP 内同步）；`gt_ref_clk_n`；静态收发器控制；串行引脚。
- **不建模时明确报告**：
  - LBUS 用户接口；
  - CAUI-10；
  - 自协商/链路训练、AXI4-Lite 接口的端口（作为未分类端口列出）。

## 2. 真实工程验证（SURF `Caui4GtyIpWrapper`）

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| 161.13 MHz 参考时钟（`Caui4GtyIpCore161MHz`） | 5（389/405） | 0 错误，1 个警告，2 条路径（复位同步器），0 个违例 |
| 156.25 MHz 参考时钟（generic `REFCLK_TYPE_G`，`Caui4GtyIpCore156MHz`） | 5（389/405） | 同上 |

- **时钟结构与 SURF 一致**：
  - 封装把 `rx_clk` 接到 `txusrclk2`，所以用户逻辑（RX 控制状态机、`stat_rx_aligned` 的使用）都在 `gt_txusrclk2`。
  - `phyClk` 由 `ClockManagerUltraScale` 的 PLL 从 `gt_txusrclk2` 产生（195.7 MHz = 322.27 × 17 / 4 / 7）。OpenCDC 解析为派生时钟，所以封装中 `txusrclk2` 与 `phyClk` 之间的 AXI-Stream FIFO 是同步时钟之间的路径，不是跨时钟路径。
- **1 个警告是真实的**：16 个寄存器的时钟是常量 0（`CLK-CONSTANT`）。它们属于 `ClockManagerUltraScale` 内部的 `AxiLiteToDrp`，SURF 没有连接它的 `axilClk`。与阶段 42、46 中的 `ClockManager7` 情况相同。
- **2 条跨时钟路径**都是复位同步器：`stableRst` 和 PLL 的 `LOCKED`。
- 2 个工程加入 VHDL 语料库。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`cmacUsplus` 模型和分派。
- `tests/frontend/test_xci.cpp`：
  - 各时钟域、322.27 MHz、`gt_ref_clk_out` 派生（比例 1、输入周期 6.206 ns）、DRP、异步输出；
  - 负例：复位、`gt_ref_clk_n`、静态控制不被读取；LBUS、CAUI-10、自协商端口不建模。
- `tools/ci/vhdl-corpus.sh`：2 个 SURF CAUI-4 工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.48.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 只支持 CAUI-4 和 AXI4-Stream 接口；LBUS、CAUI-10、自协商/链路训练、AXI4-Lite、时间戳不建模。
- 在 SURF 封装中 `rx_clk` 就是 `gt_txusrclk2`，所以异步模式（`rx_clk` 是另一个时钟）只用单元测试验证过。

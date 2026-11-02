# 阶段 46：1G/2.5G 以太网 PCS/PMA（gig_ethernet_pcs_pma）；IP 内部派生时钟；单元素可变偏移切片

日期：2026-11-02　版本：0.46.0

## 1. 完成的功能

### 1.1 `portDomainModel` 支持派生输出时钟

以前，IP 的输出时钟只能作为独立时钟（自带周期）。现在，输出时钟可以声明为"由某个时钟派生"，并给出周期比。这里的源时钟可以是输入时钟端口、另一个输出时钟或 IP 内部时钟。

- 生成的模型中，派生时钟是挂在源时钟上的分频寄存器，带 `OPENCDC_CLOCK_RATIO` 和 `OPENCDC_CLOCK_INPUT_PERIOD`。
- 时钟分析据此做两件事：
  - 把它当作源时钟的派生时钟，两者是同步关系；
  - 源时钟有周期时按比例计算周期；源时钟没有约束时，使用 IP 配置给出的输入周期。
- 缺少源时钟端口时，明确报 `FE-XCI-UNSUPPORTED`，并列出端口名和它的源（例如 `clk125_out (from refclk625_p)`）。

### 1.2 `gig_ethernet_pcs_pma`（1G/2.5G Ethernet PCS/PMA or SGMII，PG047）

端口来自实例（JSON、组件文件或 VHDL 组件声明）。

**使用收发器、不含共享逻辑**（`c_use_transceiver`，`c_support_level` 为 false）：

- **GMII 时钟域 `userclk2`**：GMII、`configuration_vector`、`status_vector`、自协商（`an_*`）、`speed_is_*`、`gmii_isolate`。`c_rx_gmii_clk_src` 为 `RXOUTCLK` 时，GMII RX 在 `rxuserclk2`。
- **输出时钟**：`txoutclk` 和 `rxoutclk` 是独立的时钟。收发器内部数据通路为 2 字节，线路使用 8b/10b 编码，所以频率 = 线速率 / 20：1G 时为 62.5 MHz，2.5G 时为 156.25 MHz。
- **收发器调试端口**：TX 侧在 `userclk`（收发器的 TXUSRCLK2），RX 侧（`rxcharisk`、`rxbufstatus`、`rxdisperr` 等）在 `rxuserclk2`；DRP 在 `gt0_drpclk_in`。
- **异步输出**：`resetdone`、`cplllock`、`mmcm_reset`、眼图扫描、数字监视、PMA reset done、PLL 复位输出。
- **不读取**：`reset`、`pma_reset`、`mmcm_locked`、`signal_detect`（都在 IP 内部同步）；静态的收发器控制（`txdiffctrl`、`loopback` 等）；PLL 时钟和 lock 输入；串行引脚。

**UltraScale LVDS 上的同步 SGMII，含共享逻辑**：

- **派生时钟**：`clk625_out`、`clk312_out`、`clk125_out` 由 IP 内的 MMCM 从 625 MHz 的 `refclk625_p` 产生，用 1.1 节的派生时钟表示，比例分别为 1、2、5。它们与参考时钟同步，与参考时钟的约束一起变化。
- **GMII 时钟域 `clk125_out`**：GMII、配置和状态，以及 `sgmii_clk_r/f/en`、`rst_125_out`。
- **异步输出**：`mmcm_locked_out`、`idelay_rdy_out`。

**不建模时明确报告原因**：

- TBI 接口（外部 SerDes）；
- 收发器 + 共享逻辑（用户时钟来自 IP 内部的 MMCM，具体结构未核实，不猜测）；
- 异步 SGMII over LVDS；
- 7 系列 LVDS 或不含共享逻辑的 LVDS；
- 多条 lane；
- MDIO 端口（作为未分类端口列出）。

### 1.3 VHDL：宽度为一个元素的可变偏移切片目标

在 SURF 的 `AxiStreamResize` 中发现：从 GMII 的 1 字节转换到 MAC 的多字节接口时，有下面的赋值，`SLV_BYTES_C = 1`：

```vhdl
v.obMaster.tKeep((SLV_BYTES_C*idx)+(SLV_BYTES_C-1) downto (SLV_BYTES_C*idx)) := ibM.tKeep(SLV_BYTES_C-1 downto 0);
```

- **以前**：报错 `VHDL-TYPE: cannot convert a value of type 'slv' to 'std_ulogic'`，整个设计无法分析。这一行影响所有经过 GMII 的 SURF 以太网 MAC。
- **原因**：可变偏移切片被拆成逐个元素的目标，再按位分配右值。只有一个元素时，原来的代码把它当作普通的单个目标，于是要把 1 位数组转换成元素类型。
- **现在**：这些元素目标带有标记，即使只有一个元素，也按位分配右值。宽度不符时仍报 `VHDL-LENGTH`。

## 2. 真实工程验证（SURF 1G 以太网：MAC + PCS/PMA）

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `GigEthGtp7Wrapper`（GTP7 核 + `ClockManager7` MMCM + `Gtp7QuadPll`） | 5（690/724） | 0 错误，1 个警告（见下），50 条路径，36 条已同步，0 个违例 |
| `GigEthGtp7`（核本身，`sysClk62` 用 `create_generated_clock -divide_by 2` 约束） | 4（668/668） | 0 错误 0 警告，50 条路径，36 条已同步，0 个违例 |
| `GigEthLvdsUltraScale`（SGMII over LVDS） | 4（653/653） | 0 错误 0 警告，47 条路径，33 条已同步，0 个违例 |

- **MMCM 时钟与 SURF 一致**：在封装中，`ClockManager7` 由 125 MHz 参考时钟产生 125 MHz（`userclk2`）和 62.5 MHz（`userclk`/`rxuserclk`）。OpenCDC 解析为 `CLKOUT0` 8 ns、`CLKOUT1` 16 ns，两者都派生自参考时钟，彼此同步。GT 的 TX 调试端口 `gt0_txpolarity_in`（在 62.5 MHz 的 `userclk` 中）由 125 MHz 域驱动，因为两个时钟相关，所以不是违例。
- **LVDS 的 `clk125_out`** 解析为派生自 `sgmiiClkP`（1.6 ns）的 8 ns 时钟，别名为 SURF 中的 `sysClk125`。
- **14 条未同步路径**（5 条潜在风险、9 条需复查）都是 AXI-Lite 异步桥和 MAC 异步 FIFO 的 Gray 指针，与以前的 SURF 工程相同。
- **封装中的 1 个警告是真实的**：34 个寄存器的时钟是常量 0（`CLK-CONSTANT`）。它们属于 `ClockManager7` 内部的 `AxiLiteToDrp`，SURF 没有连接它的 `axilClk`，这个端口保持默认值 '0'。阶段 42 的 GTX 工程中也有同样的情况。
- 3 个工程加入 VHDL 语料库。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：
  - `PortClass` 增加 `source`、`ratio`；
  - `portDomainModel` 生成派生时钟；
  - `gigEthernetPcsPma` 和分派。
- `src/frontend/vhdl/VhdlElab.h`、`VhdlNames.cpp`、`VhdlStmt.cpp`：`Target::sliceElement`，单元素切片按位赋值。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 收发器：各时钟域、62.5 MHz、DRP、调试端口、异步输出、`txp` 为常量；负例：复位、`signal_detect`、静态控制不被读取；RXOUTCLK 作为 GMII RX 时钟源、2.5G 156.25 MHz；
    - LVDS：三个派生时钟的比例和输入周期、`clk125_out` 时钟域；负例：`refclk625_n`、复位不被读取，`clk125_out` 不是独立时钟；
    - 负例：TBI、收发器 + 共享逻辑、异步 SGMII、多 lane、7 系列 LVDS、MDIO 端口、缺少参考时钟端口；
    - VHDL 端到端：`clk125_out` 到 `clk312_out` 是相关时钟，周期分别为 8 ns 和 3.2 ns，没有未同步路径；负例：到与参考时钟异步的时钟时，报 1 条未同步路径。
  - `tests/vhdl/test_vhdl_elab.cpp`：单元素可变偏移切片（生成 `dyn_insert`，没有 `VHDL-TYPE`）；负例：2 位值赋给 1 个元素时报 `VHDL-LENGTH`。
- `tools/ci/vhdl-corpus.sh`：3 个 SURF 1G 以太网工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.46.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 收发器 + 共享逻辑的变体不建模，因为用户时钟来自 IP 内部的 MMCM，具体结构未核实。
- 7 系列 LVDS、异步 SGMII、TBI、多 lane、MDIO 不建模。
- SURF 中 GTH7、GTX7、GTH/GTY UltraScale(+) 的 1G 以太网只有 RTL，没有 `.xci`，所以没有用真实工程验证这些变体。
- 派生时钟的分频寄存器计入寄存器数。没有使用的派生时钟（例如 SURF 中悬空的 `clk312_out`）也是这样。

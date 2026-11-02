# 阶段 44：10G/25G 以太网 IP 模型；在 SURF 以太网 MAC 中发现的三种 VHDL 写法

日期：2026-11-02　版本：0.44.0

## 1. 完成的功能

### 1.1 通用的"逐端口分类"IP 模型

新增 `portDomainModel`：给出实例的端口和每个端口的分类（同步于某个时钟、异步输出、不读取、输入时钟、输出时钟），生成 CDC 模型：

- 每个时钟域一个寄存器，汇合该域的所有输入；
- 输出是其时钟的寄存器；
- 输出时钟是独立的时钟，可以带周期；
- IP 内部的时钟（例如没有引出端口的恢复时钟）用名字声明；
- 未分类的端口、缺少时钟端口的时钟域，都明确报 `FE-XCI-UNSUPPORTED` 并列出名字。

下面两个 IP 都基于它实现。

### 1.2 `ten_gig_eth_pcs_pma`（10G/25G Ethernet PCS/PMA，PG068）

端口来自实例（JSON、组件文件或 VHDL 组件声明）。

- **TX clock source**（PG068）：
  - 包括 XGMII、`configuration_vector`、`status_vector`、`core_status`、`pma_pmd_type`、`tx_disable`、reset-done；
  - 7 系列（以及 UltraScale 10GBASE-KR）为 `coreclk`；
  - UltraScale 10GBASE-R 为 `txusrclk2`，此时 `coreclk` 只用于收发器复位逻辑。
- **XGMII RX**：有弹性缓冲时与 TX 相同；没有弹性缓冲时（`c_no_ebuff`）在恢复时钟中。
- **其他时钟域**：DRP 仲裁在 `dclk`；收发器调试端口在 `txusrclk2`（TX）或恢复时钟（RX）。
- **输出时钟**：`txoutclk` 和 `rxrecclk_out` 是独立的时钟，10G 时为 322.27 MHz。收发器内部数据通路为 32 位，与 64 位 fabric 接口无关；SURF 中的信号名 `txClk322` 也印证了这一点。
- **不读取**：`signal_detect`、`tx_fault`、各复位和 QPLL 输入。
- **异步输出**：眼图扫描、数字监视、PMA reset done、`qpll0reset`、`reset_tx_bufg_gt`。
- **不建模时明确报告**：例如 MDIO 端口（`mdc`、`mdio_in`），列出端口名。

### 1.3 `xxv_ethernet`（10G/25G High Speed Ethernet，PG210），PCS/PMA 64 位 + MII，单核

- **输出时钟**：`tx_mii_clk` 和 `rx_clk_out` 是独立的时钟，10G 时 156.25 MHz，25G 时 390.625 MHz；`rxrecclkout` 也是，但不给周期。
- **各时钟域**：
  - TX MII、`ctl_tx_*`、`stat_tx_*` 和 TX 调试端口在 `tx_mii_clk`；
  - RX MII 在 `rx_core_clk`（异步时钟模式下由 RX FIFO 跨域）；
  - `ctl_rx_*`、`stat_rx_*` 和 RX 调试端口在 `rx_clk_out`；
  - 收发器复位辅助模块的输出在 `dclk`；DRP 在 `gt_drpclk`。
- **不建模**：MAC 变体和多核配置，明确报告原因。

### 1.4 在 SURF 以太网 MAC 中发现的 VHDL 写法（以前会出错或丢失逻辑）

| 写法 | 以前 | 现在 |
|---|---|---|
| `v.tKeep(conv_integer(n) downto 0) := (others => '1')`：可变边界切片 | 错误 `VHDL-NOT-STATIC`，整个设计无法分析 | 值为 `(others => x)` 时支持：每个元素按下标是否在 [下界, 上界] 内选择新值或原值。其他值仍报错 |
| 时钟 if 之后的 `macMaster <= varMaster;` | 警告并忽略，信号没有驱动 | 变量在时钟沿更新并保持，信号是变量寄存器的组合函数。尾部只读进程变量和常量时才这样处理；读信号时仍警告，因为那样会在两个沿都采样 |
| `if FIXED_G then q <= 组合; elsif rising_edge(clk) then ...`，generic 为真 | 当作恒定有效的异步分支（`VHDL-ASYNC-LOAD`），生成一个错误的寄存器 | 第一个分支总是成立，进程是组合逻辑。generic 为假时与以前相同 |

## 2. 真实工程验证（SURF 10G 以太网：MAC + PCS/PMA）

| 工程 | IP | 时钟（已解析寄存器） | 结果 |
|---|---|---|---|
| `TenGigEthGtx7` | ten_gig（kintex7，32 位） | 5（738/738） | 0 错误 0 警告，51 条路径，37 条已同步，0 个违例 |
| `TenGigEthGth7` | ten_gig（7 系列 GTH） | 4（731/731） | 同上 |
| `TenGigEthGthUltraScale` | ten_gig（UltraScale，64 位） | 5（765/765） | 53 条路径；3 个 CDC-002 警告违例 |
| `TenGigEthGthUltraScale`（US+） | xxv_ethernet（GTH） | 5（580/580） | 35 条路径；2 个 CDC-004 错误违例 |
| `TenGigEthGtyUltraScale`（US+） | xxv_ethernet（GTY） | 5（580/580） | 同上 |

- **UltraScale 版本的 3 个 CDC-002 是真实情况**：SURF 在 `statusReg`（`phyClock` = `txusrclk2`）中用一个寄存器采样 `U_TenGigEthRst` 在 `coreClk` 域产生的 `rstCnt`、`rstPulse`。这些是准静态的复位标志，只有一级。
- **UltraScale+ 版本的 2 个 CDC-004 是真实情况**：`TenGigEthGthUltraScaleRst` 中 `ready <= txRstdone and rxRstdone;` 先组合再送入 `Synchronizer`。两个复位完成信号来自 `dclk` 域，组合后才同步。
- **XGMII 时钟域最初分错**：起初按 `coreclk` 建模 UltraScale 版本，得到 6 个错误违例（XGMII 在 `coreClk` 与 `phyClock` 之间）。核对 PG068 后改为"UltraScale 10GBASE-R 在 `txusrclk2`"，这些违例消失，与 SURF 的用法一致（XGMII 直接在 `phyClock` 中读写）。
- 5 个工程都加入了 VHDL 语料库。语料库中带有一个按 `entity surf.X`、组件实例和 `use surf.pkg` 收集文件的依赖解析器。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`PortClass`、`InternalClock`、`portDomainModel`、`tenGigEthPcsPma`、`xxvEthernet`，以及分派。
- `src/frontend/vhdl/VhdlElab.h`、`VhdlNames.cpp`、`VhdlStmt.cpp`：`Target::masked`、`othersValue_`、尾部赋值（`tailReadsOnlyVariables`）、generic 为真的组合分支（`emitCombinational`）。
- 测试：
  - `tests/vhdl/test_vhdl_elab.cpp`：
    - 可变边界切片：每位与下标比较，生成 8 位寄存器；负例为非 others 值时仍报 `VHDL-NOT-STATIC`；
    - 尾部赋值：变量成为寄存器，信号为组合连接，没有警告；负例为尾部读信号时仍警告；
    - generic 为真的组合分支：没有寄存器、没有 `VHDL-ASYNC-LOAD`；负例为 generic 为假时是寄存器。
  - `tests/frontend/test_xci.cpp`：
    - ten_gig 7 系列（`coreclk` 域、322.27 MHz 时钟、DRP、调试端口、异步输出）、UltraScale（`txusrclk2`，没有 `coreclk` 寄存器）、无弹性缓冲（恢复时钟）；负例：复位不被读取、没有端口、MDIO 端口；
    - xxv：各时钟域、156.25 MHz；负例：复位不被读取、MAC 变体、4 个核。
- `tools/ci/vhdl-corpus.sh`：5 个 SURF 以太网工程和依赖解析器。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.44.0。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建；Mac 版本由开发者在本机运行 `tools/macos/build-macos.sh` 确认。

## 5. 已知限制

- `BUFG_GT` 的 `DIV` 端口（分频）不建模。SURF UltraScale 版本中经 `DIV="001"` 得到的 156.25 MHz `phyClock` 被看成 `txoutclk`（322 MHz）本身：关系正确（同源），但周期不对。原因是 `DIV` 是端口，原语模型在模块内部看不到它的常量值。下一阶段处理。
- ten_gig 在 25G 时不给 `txoutclk` 的周期；xxv 的 `rxrecclkout` 不给周期。
- xxv 只支持 PCS/PMA + MII 的单核配置。

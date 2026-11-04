# 阶段 50：AXI DMA（axi_dma）IP 模型；长度不符的 case 选择项

日期：2026-11-04　版本：0.50.0

## 1. 完成的功能

### 1.1 `axi_dma`（AXI DMA，PG021），直接寄存器模式

端口来自实例（JSON、组件文件或 VHDL 组件声明）。

- **`s_axi_lite_aclk` 时钟域**：AXI4-Lite 寄存器接口、`axi_resetn`、中断（`mm2s_introut`、`s2mm_introut`）。
- **异步时钟模式**（`C_PRMRY_IS_ACLK_ASYNC = 1`）：
  - MM2S 通道（`m_axi_mm2s_*`、`m_axis_mm2s_*`、`mm2s_prmry_reset_out_n`）在 `m_axi_mm2s_aclk`；
  - S2MM 通道（`m_axi_s2mm_*`、`s_axis_s2mm_*`、`s2mm_prmry_reset_out_n`）在 `m_axi_s2mm_aclk`；
  - IP 在内部完成这些时钟之间的跨域。
- **同步时钟模式**（`C_PRMRY_IS_ACLK_ASYNC = 0`）：
  - PG021 要求所有时钟端口接同一个时钟，所以模型把所有端口放在 `s_axi_lite_aclk`。
  - 如果设计在 `m_axi_mm2s_aclk` 或 `m_axi_s2mm_aclk` 上接了别的时钟，进入 IP 的路径会报告为跨时钟路径。这样的连接违反了 IP 的要求，报告出来是有意的。
- **`axi_dma_tstvec`**：调试输出，没有文档说明它的时钟，按异步输出处理。
- **不建模时明确报告原因**：
  - Scatter Gather（`C_INCLUDE_SG = 1`）；
  - 多通道 DMA。

### 1.2 VHDL：长度与选择表达式不同的 case 选择项

在 Digilent `usb2device` 的 `Control_Registers.vhd` 中发现：选择表达式 `loc_addr` 是 7 位，65 个选择项中有 4 个只有 6 位，例如 `when b"011100" =>`。

- **以前**：报错 `VHDL-LENGTH`，整个设计无法分析。
- **现在**：报警告 `VHDL-LENGTH`，并说明该选择项永不匹配。
  - 在 IEEE 1076 中，这种写法是错误。
  - 长度不同的数组永远不相等，所以把它当作永不匹配，语义上是确定的。
  - Quartus 对类似情况也只给警告（"choice is outside the range of the select"）。
- 对 `usb2device` 的影响：
  - 地址 `011100`、`011101`（`ENDPTCTRL7`、`ENDPTCTRL8`）的写入永远不会发生；
  - 读回地址 `000100`、`000101`（`HWTXBUF`、`HWRXBUF`）的分支也永远不会选中。
  - 这是该 IP 源代码中的真实缺陷。

## 2. 真实工程验证（Digilent `usb2device`）

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `usb2device`（`axi_dma` 同步模式，4 个 FIFO、1 个块 RAM IP，ULPI 时钟经 MMCM） | 3（750/750） | 0 错误，5 个警告，135 条路径，127 条已同步，6 个 CDC 错误 |

- **时钟约束**：3 个 AXI 时钟端口用一条 `create_clock` 约束为同一个时钟，与 PG021 的要求一致。DMA 的所有寄存器在这个时钟中，经过 DMA 的路径没有跨时钟。
- **5 个警告**：4 个是上面的 case 选择项，1 个是长度不同的数组比较（`DMA_Transfer_Manager.vhd:1310`），都在源代码中。
- **6 个 CDC-006 错误是真实的**：
  - Digilent 的 `HandshakeData` 中，`aReset` 在一个时钟中产生，在另一个时钟中用作异步复位，没有复位同步器。
  - 模块内部只把 `aReset` 同步到 `InClk`，用来保持 `iRdy`；`OutClk` 侧直接用 `aReset` 做异步复位，释放与 `OutClk` 异步。
- **42 条潜在风险**：都是握手模块之间的重汇聚（CDC-005）。
- 加入 VHDL 语料库，同时使用 Digilent 自带的 `constrs_1.xdc`。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`axiDma` 模型和分派。
- `src/frontend/vhdl/VhdlStmt.cpp`：长度不符的 case 选择项。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 异步模式各通道的时钟域、中断、复位输出、调试输出；
    - 同步模式所有端口在 `s_axi_lite_aclk`；负例：同步模式没有 MM2S/S2MM 时钟中的寄存器；
    - 负例：Scatter Gather 不建模。
  - `tests/vhdl/test_vhdl_elab.cpp`：6 位选择项、7 位选择表达式时报警告，该分支的寄存器不存在；负例：7 位选择项正常匹配，没有警告。
- `tools/ci/vhdl-corpus.sh`：`usb2device` 工程。
- 文档：`docs/vivado-ip.md`、`docs/architecture.md`、`README.md`。
- `CMakeLists.txt`：0.50.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- AXI DMA 的 Scatter Gather 和多通道模式不建模。
- Vivado 对长度不符的 case 选择项如何处理，没有找到文档。这里采用"永不匹配"的语义，并以警告报告。

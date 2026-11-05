# 阶段 55：Zynq-7000 Processing System、SmartConnect；Vivado 2020.2+ 的 .gen 目录

日期：2026-11-05　版本：0.55.0

真实工程取自 cteqeu/SoC（比利时 CTEQ 课程的 Zynq 工程）中的 4 个块设计，覆盖 Vivado 2019.2 到 2021.2：

| 工程 | 板子 | 内容 |
|---|---|---|
| `SoC_7_Timers_GIC` | Pynq-Z2 | PS7 + 两个 AXI GPIO，经 xlconcat 接到 `IRQ_F2P` |
| `SoC_Multiple_IRQ_F2P` | MiniZed | 同上，带工程自己的 xdc |
| `eFPGA_XADC` | MiniZed | PS7 + XADC Wizard（AXI） |
| `eFPGA_BRAM` | MiniZed | PS7 + SmartConnect + AXI BRAM Controller + Block Memory |

该仓库没有许可证文件，所以只在校验时下载，不随 OpenCDC 分发。

## 1. 完成的功能

### 1.1 `processing_system7`（PG082、UG585）

端口来自实例（组件文件或 JSON `.xci`）。

- **FCLK_CLK0..3**：PS PLL 产生的独立时钟。
  - 周期取自 `PCW_ACT_FPGAn_PERIPHERAL_FREQMHZ`（实际频率）。
  - 别名为 Vivado 的名字 `clk_fpga_0..3`，与 IP 自带 xdc 中的 `create_clock -name clk_fpga_0` 一致，用户约束中可以写 `get_clocks clk_fpga_0`。
- **AXI 端口**：各在自己接口的时钟（`M_AXI_GPn_ACLK`、`S_AXI_GPn_ACLK`、`S_AXI_HPn_ACLK`、`S_AXI_ACP_ACLK`）。
- **DMA 与 GMII**：DMA 请求接口在 `DMAn_ACLK`；EMIO GMII 在 `ENETn_GMII_TX_CLK`/`RX_CLK`。
- **EMIO GPIO 输出**：是 PS 时钟 `cpu_1x` 的寄存器。
  - `cpu_1x` 是 CPU 时钟的 1/6（6:2:1）或 1/4（4:2:1），作为一个独立时钟。
  - 所以 fabric 逻辑不经同步使用 EMIO GPIO 时会被报告。
  - 如果把它当作"没有时钟域"，这类路径就不会被检查。
- **没有 fabric 时钟域的输出**：`FCLK_RESETn_N`、到 fabric 的中断和其他 EMIO 信号。
- **在 PS 内同步、模型不读取的输入**：`IRQ_F2P` 和 EMIO 输入。
- **不读取的引脚**：MIO、DDR 和 PS 引脚（inout）。

### 1.2 `smartconnect`（PG247）

- 每个 AXI 接口在与它关联的时钟中。关联关系来自组件文件或 JSON `.xci` 中时钟接口的 `ASSOCIATED_BUSIF`（`aclk`、`aclk1`……）。
- `aresetn` 在 `aclk`；IP 在内部跨域。
- 没有关联信息时不建模，并说明原因。
- 为此 `XciInfo` 新增 `busClock`（接口 → 时钟端口），从组件文件或 JSON boundary 读取。

### 1.3 Vivado 2020.2+ 的 `.gen` 目录

从 2020.2 起，Vivado 把 output products（包括组件文件 `<instance>.xml`）放在 `<project>.gen`，`.xci` 仍在 `<project>.srcs`。以前这样的 XML `.xci` 找不到端口，无法建模。

- 块设计和 XML `.xci` 都会到对应的 `.gen` 目录中查找组件文件（`generatedProductsDir`）。

### 1.4 其他

- **inout 端口**：`portDomainModel` 支持 inout 端口，只能归为忽略，生成 `inout wire` 端口且不驱动。
- **黑盒时钟的别名**：时钟分析读取黑盒时钟（模型中的 `opencdc_async_source`）上的 `OPENCDC_CLOCK_ALIAS`。以前只有分频时钟读取它。

## 2. 真实工程验证

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `SoC_7_Timers_GIC`（2021.2，组件文件在 `.gen`） | 1（99/99）：`clk_fpga_0` 10 ns | 0 错误，0 警告，0 条跨时钟路径 |
| `SoC_Multiple_IRQ_F2P`（2020.2） | 2（98/98）：`clk_fpga_0` 20 ns、`cpu_1x` | 0 错误，0 警告，0 条跨时钟路径 |
| `eFPGA_XADC`（2019.2） | 2（82/82） | 0 错误，0 警告，0 条跨时钟路径 |
| `eFPGA_BRAM`（2019.2，SmartConnect） | 2（118/118） | 0 错误，0 警告，0 条跨时钟路径 |

- `cpu_1x` 中的 3 个寄存器是模型中的 EMIO GPIO 输出，它们直接连到引脚，不进入 fabric 逻辑。

- 这 4 个设计都只用一个 fabric 时钟（`clk_fpga_0`），所以没有跨时钟路径。这与设计一致。
- 跨时钟的情况由单元测试覆盖：两个 FCLK 之间未同步的路径，以及 EMIO GPIO 进入 FCLK 域的路径。
- 修正前，这 4 个工程都无法分析：PS7 没有模型，2020.2+ 的组件文件也找不到。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：
  - `processingSystem7`、`smartConnect`；
  - `clockAssociations`、`busClock`；
  - `generatedProductsDir`；
  - `portDomainModel` 的别名和 inout 支持。
- `src/frontend/BlockDesign.cpp`：在 `.gen` 中查找组件文件。
- `src/clock/ClockAnalyzer.cpp`：黑盒时钟的别名。
- 测试 `tests/frontend/test_xci.cpp`：
  - PS7 模型：正例为时钟别名和周期、inout、各接口的时钟、`cpu_1x`；负例为中断、EMIO 输入和引脚不被读取。
  - 设计中的 PS7：`get_clocks clk_fpga_0` 能匹配；两个 FCLK 之间未同步的路径被报告；EMIO GPIO 进入 FCLK 域的路径被报告。
  - SmartConnect：按关联划分时钟；负例为主接口不在 `aclk`、没有关联时不建模。
  - `.gen` 目录：XML `.xci` 在 `.srcs`、组件文件在 `.gen` 时也能建模。
- `tools/ci/sv-corpus.sh`：cteqeu/SoC 的 4 个工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.55.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- PS7 中除 GPIO 外的 EMIO 外设（UART、SPI、I2C 等）和 `FCLK_RESETn_N` 没有 fabric 时钟域，经过它们的路径不检查。
- `cpu_1x` 与 FCLK 的关系是"未知"：它们都来自 PS PLL，但 Vivado 也把它们当作不相关的时钟。
- Zynq UltraScale+ 的 `zynq_ultra_ps_e` 没有模型。

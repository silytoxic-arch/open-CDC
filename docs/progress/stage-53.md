# 阶段 53：Vivado 块设计（.bd）；块设计中的基础 IP 模型；STARTUPE2/STARTUPE3

日期：2026-11-05　版本：0.53.0

真实工程是 Fuxi（MaxXSoft/Fuxi，GPL-3）的 SoC。它是一个 Vivado 块设计（`soc.bd`，Vivado 2018.3），由 `SoC.v` 实例化，共 45 个叶单元：

- **时钟与 CPU**：Clocking Wizard 产生 6 个时钟；CPU 在 `clk_cpu`（50 MHz），外设在 `clk_peripheral`（100 MHz）。
- **三个 AXI Interconnect**：内部有 crossbar、clock converter、data FIFO、protocol converter、width converter。
- **存储**：DDR3 MIG（AXI）、AXI BRAM Controller + Block Memory。
- **外设**：两个 Quad SPI（其中一个用 STARTUPE2 访问配置 flash）、UART 16550、Ethernet Lite、中断控制器、APB 桥、TFT 控制器、Processor System Reset 等。
- **调试**：VIO、ILA、System ILA。
- **工程自己的模块**（module reference）：CPU 包装、CLINT、confreg、复位同步器。

## 1. 完成的功能

### 1.1 块设计（`.bd`）

`.bd` 文件可以直接放进源文件列表，与 Vivado 工程的写法一致。OpenCDC 生成 Vivado 会生成的那个模块（以设计名命名，例如 `soc`），再按各 IP 单元的配置建模。

- **单元**：
  - IP 单元：实例化以配置名（`xci_name`）命名的模块，并把 `.bd` 旁边的 `ip/<xci_name>/<xci_name>.xci` 加入建模（`FE-XCI-MODEL`）。
  - module reference：实例化被引用的模块，传入组件文件中的 HDL 参数（long、bitString、string、bool 四种格式）。
- **层次**：层次单元一律展平，实例名是单元路径中的 `/` 换成 `_`。这包括 AXI Interconnect（Vivado 用 crossbar、couplers 等搭成）和只转发接口的 couplers（没有 `components` 的层次）。
- **网络**：按 `.bd` 中的端口列表连接，网络名沿用块设计的网络名。
- **接口网络**：逐个信号展开。
  - 端口映射来自单元的组件文件 `ip/<cell>/<cell>.xml`；也可以来自 JSON `.xci` 的 boundary，或 module reference 在 `.bd` 中的 `port_maps`。
  - 支持映射到端口的一段，例如 crossbar 各 slot 共用 `s_axi_awaddr[95:0]` 的不同段。
  - 信号网络命名为 `<接口网络>_<信号>`（`fuxi_inst_ARADDR`）。
  - 设计的接口端口展开为 `<接口>_<信号小写>`（`DDR3_addr`、`UART_txd`），与 Vivado 生成的模块一致，所以用户的顶层可以按原样连接。
- **位宽与默认值**：
  - 网络取驱动端的位宽；较窄的输入取低位，较宽的输入高位补 0。
  - 未连接的输入取组件文件中的默认值。
- **报告**：
  - `FE-BD-MODULE`（info）列出单元数、展平的层次数和 IP 配置数；多驱动或无驱动的网络写在说明中。
  - 单元的端口未知时（没有生成 output products）报错 `FE-BD-CELL`，并说明需要在 Vivado 中生成 output products。

### 1.2 组件文件的读取

- 新增公开接口 `cellInterfaceFromText`，读取 IP-XACT 组件文件或 JSON `.xci` 的 boundary，得到四类信息：端口、总线接口的端口映射（含位段）、输入的默认值、HDL 参数。
- **修正一个已有缺陷**：
  - 组件文件中 `<spirit:left ... spirit:dependency="... > 32 ...">` 这样的属性值含有 `>`，以前的正则在属性中途就结束了匹配，宽度读不出来，整个 IP 因此不能按端口建模。例如 axi_intc 的 `s_axi_awaddr`。
  - 现在允许属性值中出现 `>`。
- **修正一个崩溃**：大文件（MIG 的组件文件有 7552 个端口）用 `[\s\S]*?` 匹配大段文本时，std::regex 递归过深导致栈溢出。改为按标签查找元素。

### 1.3 新的 IP 模型

| IP | 模型 |
|---|---|
| `proc_sys_reset` | 外部复位、辅助复位、调试复位和 `dcm_locked` 在 IP 内同步到 `slowest_sync_clk`，模型不读取；5 个复位输出是 `slowest_sync_clk` 的寄存器 |
| `util_vector_logic` | 组合逻辑：and/or/xor/not |
| `axi_protocol_converter`、`axi_data_fifo`、`axi_register_slice` 及 AXI4-Stream 的 register slice、width converter 等 | 一个时钟 `aclk` |
| `axi_dwidth_converter` | 异步时钟（`C_AXI_IS_ACLK_ASYNC`）或比例 FIFO（`C_FIFO_MODE` 2）时两侧各在自己的时钟；否则全部在 `s_axi_aclk` |
| `axis_clock_converter` | 两侧各在自己的时钟，IP 内部跨域 |
| `axi_bram_ctrl` | 全部在 `s_axi_aclk`；`bram_clk_a/b` 直接传出 `s_axi_aclk`（新增端口类别 Through） |
| `axi_apb_bridge` | 一个时钟 `s_axi_aclk` |
| `axi_intc` | 寄存器和 `irq` 在 `s_axi_aclk`（`C_ENABLE_ASYNC` 时 `irq` 在 `processor_clk`）。`C_ASYNC_INTR` 标记的中断输入在 IP 内同步，模型只采样其余的位。例如 Fuxi 的 4 个输入中第 3 位异步，模型采样 `(intr & 4'h7)` |
| `axi_ethernetlite`（MII） | 寄存器、中断、PHY 复位和 MDIO 在 `s_axi_aclk`；接收数据在 `phy_rx_clk`，发送数据在 `phy_tx_clk`；`phy_crs`、`phy_col`、`phy_mdio_i` 在 IP 内同步 |
| `system_ila` | 只有输入 |
| `axi_quad_spi` 扩展 | AXI4 存储接口（`s_axi4_*`，含 XIP）在 `s_axi4_aclk`，与 SPI 侧之间另有一对异步 FIFO。使用 STARTUP 时，模型像 IP 一样实例化 `STARTUPE2`（UltraScale 为 `STARTUPE3`），SPI 时钟驱动 `USRCCLKO` |

### 1.4 原语 `STARTUPE2`、`STARTUPE3`

- `CFGCLK`、`CFGMCLK` 是配置振荡器的独立时钟；`EOS`、`PREQ`（以及 `STARTUPE3` 的 `DI`）没有时钟域；`USRCCLKO` 等输入接到器件引脚。
- Fuxi 的 `timing.xdc` 中有 `create_generated_clock ... [get_pins -hierarchical *USRCCLKO]`：以前找不到目标，报 `CON-PARSE` 错误；现在能匹配到模型中的引脚。

## 2. 真实工程验证（Fuxi SoC）

| 时钟（已解析寄存器） | 结果 |
|---|---|
| 8（1095/1095） | 0 错误，9 个警告，1 条跨时钟路径（已同步），0 违例 |

- **展开规模**：45 个单元，22 个层次展平，41 个 IP 配置。
  - 除 TFT 控制器外，所有 IP 都已建模；
  - 自定义 IP `nt35510_controller` 从工程的 IP 仓库读取。
- **时钟**：
  - Clocking Wizard 输出 `clk_cpu` 20 ns、`clk_peripheral` 10 ns、`clk_spi` 50 ns、`clk_timer` 10 ns，以及 Vivado 名（`clk_cpu_soc_clk_wiz_0_0` 等）；
  - MIG 的 `ui_clk` 为 10 ns（别名 `clk_pll_i`）；
  - 约束中的 `clk_sck` 生成时钟落在 STARTUPE2 的 `USRCCLKO` 上。
- **跨时钟只发生在 IP 内部**：clock converter、Quad SPI 的异步 FIFO、MIG 的复位同步器。唯一的一条跨时钟路径是 `locked` 进入 MIG 的 `rstdiv0_sync_r`，判为已同步。
- **9 个警告**：
  - TFT 控制器没有模型（PG095 中各端口的时钟域没有查到可靠的说明，所以不猜测）；
  - CPU 内核是黑盒：`Fuxi.v` 由 Chisel 生成，不在仓库中；
  - nt35510 控制器中的阻塞/非阻塞混用；
  - 6 个约束警告：工程 xdc 引用了 Vivado 综合后的内部寄存器名，或引用了未定义的时钟。
- **对比**：修正前 OpenCDC 不能读取 `.bd`，整个 SoC 只能当作黑盒。
- 加入 SystemVerilog 语料库：
  - 只检出 `soc`、`verilog` 两个目录（sparse checkout）；
  - 约束使用工程自带的 `io.xdc`、`timing.xdc`。

## 3. 修改的文件

- `src/frontend/BlockDesign.{h,cpp}`（新增）：块设计 → Verilog 模块。
- `src/frontend/XciModels.{h,cpp}`：
  - `cellInterfaceFromText`；
  - 新 IP 模型，以及 `PortClass::Kind::Through`；
  - Quad SPI 的 AXI4 接口和 STARTUP；
  - 含 `>` 的属性值；
  - 按标签查找元素，避免正则栈溢出。
- `src/frontend/AutoFrontend.cpp`：`.bd` 展开为生成的模块，并把各 IP 配置加入队列。
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：`STARTUPE2`、`STARTUPE3`。
- 测试：
  - `tests/frontend/test_block_design.cpp`（新增）：
    - 正例：接口网络按端口映射展开、网络命名、层次展平、未连接输入取默认值、设计接口端口展开、端口的段拼接、未连接的输出；
    - 负例：只连一个 slot 时另一段补 0；非 JSON、没有设计名、单元端口未知时报错；
    - 端到端：经过 AXI4-Stream clock converter 时没有跨时钟路径；负例：去掉 converter 直接相连时报告跨时钟路径。
  - `tests/frontend/test_xci.cpp`：
    - 正例：各新模型的时钟域；
    - 负例：异步输入不被采样、同步模式没有第二个时钟、未知端口不猜测、快速中断模式不建模、debug core 不能有输出；
    - 组件文件与 JSON boundary 的读取（含 `>` 的属性、位段、默认值、参数格式）。
  - `tests/frontend/test_primitives.cpp`：STARTUPE2/STARTUPE3 的 `CFGMCLK` 是独立时钟；负例：`USRCCLKO` 不是时钟。
- `tools/ci/sv-corpus.sh`：Fuxi 的下载与 `fuxi_soc` 工程。
- 文档：`docs/vivado-ip.md`（新增"块设计"一节）、`docs/primitives.md`、`docs/architecture.md`、`README.md`。
- `CMakeLists.txt`：0.53.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 层次展平后，实例名用 `_` 连接，与 Vivado 的层次路径不同。约束中用 `-hierarchical` 通配符引用的对象仍能匹配，写出完整层次路径的不能。
- AXI TFT 控制器（`axi_tft`）没有模型。
- 块设计需要 Vivado 生成的 output products（各 IP 的组件文件）。只提交 `.bd` 而没有 `ip/` 目录的工程（例如 SURF 的 MicroblazeBasicCore）会报 `FE-BD-CELL`。
- module reference 的端口映射只用整个端口，不处理位段（`.bd` 中 module reference 的映射没有位段）。

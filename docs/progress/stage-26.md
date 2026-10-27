# 阶段 26：IP 组件文件、AXI 外设与块设计连线 IP

日期：2026-10-27　版本：0.26.0

## 1. 完成的功能

### 1.1 端口来自 IP 的组件文件

XML 格式的 `.xci` 不含端口表，以前只有 JSON 格式（Vivado 2020.2+）能给出端口，XML 下单时钟 IP 只能推导 CORDIC。Vivado 在 XML `.xci` 旁边还会写一份组件文件 `<实例名>.xml`（IP-XACT），里面每个端口都有算好的位宽和启用标记。现在：

- 有组件文件时，端口表取自它；被配置禁用的端口（`isEnabled` 为 false）不出现。
- 位宽仍是表达式（没有算好）时不猜，IP 不按端口表建模。
- 启用标记的属性里常有 `>`（如 `ID_WIDTH >0`），解析时按标记内容而不是属性来判断。

### 1.2 新增模型

| IP | 模型 |
|---|---|
| axi_gpio、axi_iic、axi_timer、axi_uartlite、axi_uart16550 | 单时钟 `s_axi_aclk`：所有输出都是该时钟的寄存器，依赖全部输入。UART 16550 使用外部时钟（xin/rclk）时不建模 |
| axi_quad_spi（标准模式） | AXI 一侧在 `s_axi_aclk`，SPI 引脚（io*、sck_*、ss_*、spisel）在 `ext_spi_clk`。`Async_Clk` 时两侧经两个异步 FIFO 交换（按异步 FIFO 识别）；否则直接相连，此时两个时钟必须相同，不同就报未同步跨域。AXI4 存储器接口（XIP）和 STARTUP 端口不建模 |
| xlslice、xlconcat、xlconstant | 块设计中的连线：切片（反向范围按位反转）、拼接、常量（最多 62 位） |

## 2. 真实工程验证：Digilent Pmod IP

vivado-library 中的 Pmod IP 是 Vivado 块设计：`PmodX.v` 是块设计网表，各子 IP 以 `.xci` 给出。约束用各自的 OOC 约束文件。Digilent 自己的 `pmod_bridge` 等 IP 没有模型，按黑盒处理。

以前（0.25.0）：每个子 IP 都报 FE-XCI-UNSUPPORTED，分析到 0 个寄存器。现在 10 个 Pmod：

| 工程 | IP | 寄存器 | 跨域 |
|---|---|---|---|
| PmodGPIO | axi_gpio、xlconcat、xlslice | 10/10 | 0 |
| PmodSF3 | axi_quad_spi（Async_Clk） | 51/51 | 10 条，全部已同步（2 个异步 FIFO、4 个复位同步器）；6 条因 OOC 约束未声明两个时钟的关系而需复核（CDC-010） |
| PmodNAV | axi_gpio、axi_quad_spi、xlconcat、xlslice | 58/58 | 同 PmodSF3 |
| PmodCOLOR | axi_gpio、axi_iic、xlconcat、xlslice | 25/25 | 0 |
| PmodAD2 | axi_iic、xlconstant | 14/14 | 0 |
| PmodBT2 | axi_gpio、axi_uart16550 | 28/28 | 0 |
| PmodESP32 | axi_gpio、axi_uartlite | 23/23 | 0 |
| PmodAMP2 | axi_gpio、axi_timer、xlconcat、xlconstant（PWM 为黑盒） | 21/21 | 0 |
| PmodHYGRO | axi_iic、axi_timer、xlconstant | 26/26 | 0 |
| PmodWIFI | axi_gpio、axi_quad_spi、axi_timer | 52/52 | 0（Quad SPI 为 Async_Clk = 0，ext_spi_clk 接 s_axi_aclk） |

PmodHYGRO 报 FE-UNCONNECTED-PORTS 警告：块设计网表没有连接 axi_timer 的输入 `capturetrig0/1`、`freeze`。这是网表本身的情况。

PmodGPIO、PmodSF3、PmodCOLOR、PmodBT2 加入了 VHDL 语料回归（vivado-library 已在其中）。

已有的 `.xci` 工程（13 个 FIFO、ZmodScope、MIPI CSI-2、Cmod A7、Nexys A7）用组件文件后生成的模型文本与以前完全相同。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：
  - `parseComponentPorts`：读取组件文件；
  - `dispatch`：XML `.xci` 没有端口表时读同目录的组件文件；
  - `wiringIp`（xlslice/xlconcat/xlconstant）、`quadSpi`；
  - 单时钟模型的时钟增加 `s_axi_aclk`，并加入五种 AXI 外设；
  - inout 端口（JSON 和组件文件）按 inout 声明。
- `src/frontend/MixedSupport.h`：未知模块提示中的 IP 列表。
- `tests/frontend/test_xci.cpp`：
  - 组件文件：端口与位宽、被禁用的端口不出现（启用标记的属性里含 `>`）、没有组件文件或位宽未算好时不建模；端到端在另一时钟读 GPIO 输出才跨域；
  - 切片、拼接、常量：生成的连线、越界的切片不建模、拼接只取 NUM_PORTS 个输入；端到端只有切片的跨时钟读取报告；
  - Quad SPI：Async_Clk 时 2 个异步 FIFO、无未同步路径；SPI 引脚在 AXI 时钟读取会跨域；非 Async_Clk 时不同时钟报未同步跨域。
- 语料库：`tools/ci/vhdl-corpus.sh`（4 个 Pmod）。
- 文档：`docs/vivado-ip.md`、`README.md`。

## 4. 开发中发现并修正的问题

- 启用标记的属性含 `>` 时，最初的正则匹配不到标记内容，被禁用的端口被当成存在（FIFO 模型多出 AXI 端口）。改为按标记内容匹配。
- 曾尝试把只引用 `dummy_view` 的端口当作不存在，但有的组件文件所有端口都只引用 `dummy_view`（PmodESP32 的 GPIO 因此失去全部端口），已撤销，只按启用标记判断。

## 5. 已知限制

- AXI 外设内部对引脚输入的同步不建模：引脚输入若来自另一个时钟域，会被报告，需要人工确认。
- Digilent 自己的 IP（pmod_bridge、PWM 等）不从 `.xci` 建模：需要其 HDL 和包装，否则是黑盒。
- Quad SPI 的 XIP/AXI4 模式、STARTUP 端口不建模。

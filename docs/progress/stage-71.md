# 阶段 71：内置原语模型中未使用的接口

日期：2026-11-08　版本：0.71.0

阶段 70 之后，每个 MiSTer 核心仍有 50 个寄存器的时钟未解析，产生 5 条 `CLK-UNRESOLVED` 告警。这些寄存器都在 HPS 原语的模型内部：
- `f2sdram.cmd_port_clk_3..5`；
- `fpga2hps.clk`；
- `hps2fpga.clk`。

阶段 70 的计划是给这些时钟建模。检查 MiSTer 的 `sysmem.sv` 后发现，这个计划的前提不对：

```verilog
cyclonev_hps_interface_fpga2hps fpga2hps(.port_size_config({2'b11}));   // 只有配置端口，没有时钟
cyclonev_hps_interface_hps2fpga hps2fpga(.port_size_config({2'b11}));
cyclonev_hps_interface_fpga2sdram f2sdram(..., .cmd_port_clk_0(...), .cmd_port_clk_1(...), .cmd_port_clk_2(...));
```

- 这些接口在设计中**确实没有时钟**：
  - `fpga2hps` 和 `hps2fpga` 桥只连接了 `port_size_config = 2'b11`（未使用）；
  - `f2sdram` 只使用了端口 0..2。
- 硬件中这些接口不工作，没有可以建模的时钟。
- 模型内部为这些接口写的寄存器在这个设计中并不存在。

所以正确的处理不是给它们找一个时钟，而是如实说明：这是未使用的接口，不是设计中的问题。

## 1. 完成的功能

### 1.1 `CLK-UNUSED-INTERFACE`

- **判断条件**：一个寄存器的时钟追溯到一个未被驱动的网络，并且这个网络是 OpenCDC **内置原语模型**的输入端口（用户在实例上没有连接它）。这时，它所驱动的接口未被使用。
- **报告方式**：
  - 报告信息级的 `CLK-UNUSED-INTERFACE`，不再报告 `CLK-UNRESOLVED` 告警；
  - 信息中写明实例、端口和原语名称，例如：`clock input 'clk' of 'sys_top.sysmem.fpga_interfaces.hps2fpga' (vendor primitive cyclonev_hps_interface_hps2fpga) is not connected: the interface it clocks is not used`。
- **处理不变**：这些寄存器仍然不参与 CDC 分析，`Clocks` 行中“已解析时钟的寄存器”的计数也不变。只改变诊断的级别和说明。
- **实现**：
  - `ir::ModuleBody::vendorModel`：前端在设计完成后，把内置原语模型的模块体标记出来（与 `FE-PRIMITIVES` 使用同一个集合）；
  - `ClockAnalyzer`：在“时钟网络未被驱动”的分支中检查这个标记和端口方向。
- **仍然是告警**（反例测试）：
  - 用户自己的模块，时钟输入没有连接；
  - 原语模型的时钟连接到一个声明了但没有驱动的线网。这是用户设计中的错误，仍报告 `CLK-UNRESOLVED`，指出这个线网。
- 阶段 70 的双端口 altsyncram 反例（`clock1` 不连接）现在也按未使用的端口 B 报告为 `CLK-UNUSED-INTERFACE`，测试已相应更新。
- `docs/architecture.md` 的时钟追溯表已加入这一情况。

## 2. 真实工程验证

对 SV 语料的 26 个项目逐一统计：

| 工程 | `CLK-UNRESOLVED`（修复前 → 后） | `CLK-UNUSED-INTERFACE` |
|---|---|---|
| Template_MiSTer | 5 → 0 | 5（fpga2hps、hps2fpga、f2sdram 端口 3..5） |
| NES_MiSTer | 5 → 0 | 5 |
| Genesis_MiSTer | 5 → 0 | 5 |
| SNES_MiSTer | 5 → 0 | 5 |
| 其余 22 个项目 | 不变（都是 0） | 0 |

- 四个 MiSTer 核心现在没有任何时钟未解析的告警。
- 跨时钟路径、寄存器数和规则结果都不变。

## 3. 修改的文件

- `src/ir/Design.h`：`ModuleBody::vendorModel`。
- `src/frontend/AutoFrontend.cpp`：标记内置模型的模块体。
- `src/clock/ClockAnalyzer.cpp`：`CLK-UNUSED-INTERFACE`。
- `docs/architecture.md`。
- 测试：
  - `tests/frontend/test_primitives.cpp`：1 个新用例（HPS 接口的未连接时钟；用户模块和未驱动线网两个反例），并更新阶段 70 的单端口用例。
- `CMakeLists.txt`：版本 0.71.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：611 个测试全部通过。
- 新测试第一次失败：我预期 11 条信息，实际 27 条。
  - 原因是我漏算了 `f2sdram` 的 `rd_clk_0..3` 和 `wr_clk_0..3`，它们在测试中也没有连接；
  - 实际的 27 条（每个 `f2sdram` 13 条，两个实例，加上 `hps2fpga` 1 条）是正确的，已改正期望值。
- SV 语料 26 个项目全部匹配，VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）：见提交后的记录。

## 5. 已知限制

- 只认得 OpenCDC 自带的原语模型。用户自己提供的厂商仿真模型没有这个标记，它们的未连接时钟仍报告为 `CLK-UNRESOLVED`。
- SNES.sdc 中在 Quartus 内部引脚上定义的两个生成时钟仍不能匹配（见阶段 70）。

## 6. 下一步

阶段 72：继续用真实工程验证。候选：
- 另一个规模不同的 Quartus 工程，例如 Terasic DE10-Nano 的参考设计；
- 一个使用 Xilinx UltraScale+ 原语的较大 Vivado 工程。

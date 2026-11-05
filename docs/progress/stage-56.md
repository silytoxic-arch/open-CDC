# 阶段 56：没有模型的 IP 按其 output products 分析；网表原语

日期：2026-11-05　版本：0.56.0

## 1. 完成的功能

### 1.1 IP 的 output products

以前，没有模型、也不在 IP 仓库中的 IP 只能是未知模块，要么报错，要么当作黑盒。但 Vivado 工程中常常带有它为 IP 生成的文件，现在按以下顺序使用：

1. **生成的 HDL**：
   - 实例的组件文件 `<instance>.xml` 列出综合视图（IP 自己的源文件，通常在块设计的 `ipshared/` 中）和综合包装视图（`synth/<instance>.v/.vhd`，即设计实例化的模块）。
   - 这些文件按 Vivado 综合的方式分析，VHDL 文件使用组件文件给出的库名。
2. **仿真网表**：HDL 不能用时，使用 `<instance>_sim_netlist.v`，即 Vivado 写出的门级网表。

规则：

- **加密的文件不使用**：含 IEEE 1735 `protect` 块的文件不用，例如调试核和部分 Xilinx IP。原因写在 `FE-XCI-UNSUPPORTED` 警告中，例如"encrypted: …"。
- **报告**：使用时 `FE-XCI-MODEL` 的说明为"analyzed from its output products"，并写明用的是 HDL 还是网表。
- **优先级**：已有的模型仍然优先，因为模型更小，并带有 IP 的时钟名和周期。
- **查找位置**：组件文件也会到 `<project>.gen` 中查找（阶段 55）。
- **前端**：没有 Verilog 模型的结果只加入 IP 自己的文件。

### 1.2 网表中的原语

阶段中统计了 Vivado 仿真网表中实际出现的原语，补上缺少的：

| 原语 | 模型 |
|---|---|
| `CARRY4`、`CARRY8` | 组合的进位链 |
| `RAM16X1D`、`RAM32X1D`、`RAM64X1D`、`RAM128X1D`、`RAM32X1S`、`RAM64X1S`、`RAM32M`、`RAM64M` | 分布式 RAM：在 `WCLK` 写，读不经时钟，所以一个时钟写、另一个时钟的寄存器读，就是跨时钟 |
| `BIBUF` | PS 的焊盘，没有逻辑 |

VHDL 组件声明也一并加入。

## 2. 真实工程验证

两个 cteqeu/SoC 工程，其用户 IP 的仓库路径都在工程之外（`../../../../devWorks/...`），仓库中没有：

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| MiniZed `Vitis_Custom_AXI_IP2021`（Math_IP，按生成的 HDL 分析，4 个文件） | 2（74/74） | 0 错误，0 警告，0 条跨时钟路径 |
| Pynq-Z2 `SoC_AXI_NeoPixel`（AXI_NeoPixel，按生成的 HDL 分析，工程自带 xdc） | 2（179/179）：`clk_fpga_0` 10 ns、`clk_fpga_1` 20 ns | 0 错误，1 个警告，64 条跨时钟路径，均未同步，需要审查 |

- **AXI_NeoPixel 的发现**：
  - IP 的 64 个 AXI 寄存器 `slv_reg0..63` 在 `clk_fpga_0`（100 MHz），各 24 位；
  - 它们被 `clk_fpga_1`（50 MHz）中 NeoPixel 逻辑的 `GRB` 寄存器直接读取，没有同步；
  - 两个 PS 时钟的关系在约束中没有给出，所以结果是"需要审查"。如果两个时钟按异步处理，这就是 64 条多位跨时钟违例。
- **1 个警告**：IP 源代码中 `LEDS` 的非常量初值。
- **对比**：修正前，这两个工程中的用户 IP 都是未知模块，无法分析。
- 两个工程已加入 SystemVerilog 语料库。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`generatedIp`、`unusableFile`。在 `dispatch` 中，模型和 IP 仓库都不能用时尝试 output products，失败时把原因作为说明加入。
- `src/frontend/AutoFrontend.cpp`：没有 Verilog 模型时只加入 IP 的文件；`FE-XCI-MODEL` 的措辞。
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：新原语。
- 测试 `tests/frontend/test_xci.cpp`：
  - 正例：从生成的 HDL 分析，文件顺序和摘要正确，设计中能找到 IP 内部的跨时钟；
  - 正例：HDL 加密时改用仿真网表，网表含 FDRE 两级同步、CARRY4、RAM32M 和 `glbl`，没有错误，同步路径和未同步路径（RAM 被另一时钟读）都能识别；
  - 负例：网表也加密时不建模，并说明原因；没有 output products 时说明缺少组件文件。
- `tools/ci/sv-corpus.sh`：`zynq_math_ip`、`zynq_axi_neopixel`。
- 文档：`docs/vivado-ip.md`（新增"IP from its output products"一节）、`docs/primitives.md`、`README.md`。
- `CMakeLists.txt`：0.56.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 分布式 RAM 原语不处理 `IS_WCLK_INVERTED`。
- 仿真网表中 Vivado 内部的单元名与综合网表的名字可能不同，写给综合网表的约束不一定能匹配。
- 加密的 IP（ILA、VIO、JTAG-to-AXI 等）仍需要模型或作为黑盒。

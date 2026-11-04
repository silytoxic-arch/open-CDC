# 阶段 51：AXI Clock Converter、AXI Crossbar、AXI 接口的 Block Memory、XML 格式的 Floating Point；SystemVerilog 前端四处修正

日期：2026-11-04　版本：0.51.0

真实工程：Loongson NSCSCC（龙芯杯）的 AXI SoC（`soc_axi_func`），CPU 为 NonTrivialMIPS（MIT 许可，trivialmips/nontrivial-mips）。

- CPU 在 `cpu_clk`（50 MHz）中；
- 经 AXI Clock Converter 和 AXI Crossbar 连到 `sys_clk`（100 MHz）中的 AXI 块 RAM 和 confreg；
- CPU 内部有 6 个 Floating Point IP 和一个 AXI Crossbar。

## 1. 完成的功能

### 1.1 Vivado IP 模型

所有 `.xci` 都是 XML 格式，并且从 Verilog 中实例化，没有 VHDL 组件声明可以提供端口，所以端口从配置参数生成。

| IP | 时钟域 | 端口 |
|---|---|---|
| `axi_clock_converter`（PG164） | 从接口和 `s_axi_aresetn` 在 `s_axi_aclk`，主接口和 `m_axi_aresetn` 在 `m_axi_aclk`；IP 在内部跨域 | AXI4（8 位 len、region、QoS）、AXI3（4 位 len、2 位 lock、QoS、WID）或 AXI4-Lite；ID/地址/数据位宽和 user 信号来自参数 |
| `axi_crossbar`（PG059） | 全部在 `aclk` | 同上；`C_NUM_SLAVE_SLOTS`/`C_NUM_MASTER_SLOTS` 个接口拼接在每个端口中 |
| `blk_mem_gen` AXI4/AXI4-Lite 接口（PG058） | 全部在 `s_aclk` | 32 位地址；数据位宽 `C_WRITE_WIDTH_A`；`C_HAS_AXI_ID` 时有 ID；开启 ECC 时有 ECC 端口；没有 lock/cache/prot |
| `floating_point`，XML `.xci`（PG060） | 一个时钟的数据通路（与 JSON 格式相同） | A、B、C、operation 通道；非阻塞模式时没有 `tready`；按配置生成 `tuser`/`tlast` |

端口集合是精确的：与 Xilinx 的端口一致，只按 AXI 协议版本区分。

- 例如 AXI3 没有 region，AXI4 没有 WID。
- 第一次实现时用了超集，结果在真实工程中出现 `FE-UNCONNECTED-PORTS` 警告（`awregion`）。已改为精确集合，警告消失。

### 1.2 Clocking Wizard：过期的 PLL 参数

`clk_pll.xci` 的配置如下：

- `C_PRIMITIVE = PLL`，但 `C_PLL_*` 的倍频/分频值全是 1；
- `C_MMCM_*` 给出 9 / 18 / 9；
- 输出频率 `C_CLKOUTn_OUT_FREQ` 为 50 MHz 和 100 MHz。

以前按 `C_PLL_*` 建模，两个输出都是 100 MHz，与配置不符。现在先检查倍频/分频值能否得到配置的频率（误差 1% 以内）：

- PLL 的值不能得到、MMCM 的值能得到时，改用 MMCM 的值，并给出说明；
- 两组值都不能得到时，保留原值，并给出说明。

现在 `cpu_clk` 为 20 ns，`sys_clk` 为 10 ns，与配置一致。

### 1.3 SystemVerilog 前端

**(1) SystemVerilog 工程中的 `.v` 文件使用 Verilog 关键字**

- **问题**：`mycpu_top.v` 有一个名为 `int` 的端口，`soc_axi_lite_top.v` 用 `.int(6'd0)` 连接它。以前，工程中只要有 `.sv` 文件，所有文件都按 SystemVerilog 关键字解析，所以这里报错。
- **修正**：
  - `.v` 文件使用 1364-2005 关键字，与 Vivado 把 `.v` 当作 Verilog 文件一致；
  - `.sv` 文件仍使用 SystemVerilog 关键字。
- **随之发现的 slang 问题**：slang 的关键字映射使用一个栈。被 `include 的文件如果没有映射，并且它嵌套在另一个没有映射的 include 中，弹栈时会弹掉外层文件的条目，此后的文件就用错了关键字。在这个工程中，`soc_top.sv` → `common_defs.svh` → `compile_options.svh`，结果之后的 `single_port_ram.sv` 被当作 Verilog 解析。
- **对策**：
  - 预处理前先扫描所有 `include，为每个源文件和每个被包含的文件都建立映射；
  - 被包含的文件使用第一个包含它的文件的关键字。
  - 已用一个最小例子确认：修正前出错，修正后通过。

**(2) 设计没有实例化的模块中的语义错误降为警告**

- `cp0_forward.sv` 在 Vivado 工程中，但没有被任何模块实例化，内容已经过时（引用了不存在的结构体成员）。
- Vivado 从顶层展开，不检查这样的模块。现在 OpenCDC 也把这类语义错误报告为警告，并说明原因。
- 语法错误仍然是错误：slang 的语法诊断列表中的诊断都不降级。

**(3) 复合赋值（`a |= b`、`cnt += x`）**

- **以前**：
  - 报 96 个 `IR-UNSUPPORTED-EXPR`（`LValueReference`）；
  - 运算还被做了两次：slang 的右值已经是 `LValueReference op b`，代码又对左值再做一次运算。
- **现在**：`LValueReference` 取目标的当前值，运算只做一次。测试中，循环 `y |= {8{hit[i]}} & d[i]` 展开为 4 级 `or` 链。

**(4) 带局部声明的命名块中的异步复位**

- secworks aes 中有这样的写法：`always @(posedge clk or negedge reset_n) begin: reg_update integer i; if (!reset_n) ...`。
- 以前报 `IR-AMBIGUOUS-CLOCK`，这些寄存器没有被分析。
- 现在识别复位时，跳过没有初值的局部声明。带初值的声明是语句，仍然报告。

## 2. 真实工程验证

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `soc_axi_func` + NonTrivialMIPS（含 AES 子模块 secworks/aes，BSD-2） | 3（991/991） | 0 错误，6 个警告，0 条跨时钟路径 |

- **时钟**：`cpu_clk` 为 20 ns（50 MHz），`sys_clk` 为 10 ns，都派生自 `clk`。约束按工程自带的 xdc 的意图，把两者设为异步。
- **为什么没有跨时钟路径**：
  - CPU 与 RAM/confreg 之间只经过 AXI Clock Converter，跨域在 IP 内部完成；
  - 两个复位分别在各自时钟中用两级寄存器同步（`cpu_resetn`、`sys_resetn`）。
  - 这个结果与设计一致。
- **6 个警告**：
  - 4 个是 `cp0_forward.sv` 中的过时代码，在未实例化的模块中，已降为警告；
  - 1 个是 slang 的隐式命名端口类型不同（`instr_fetch.sv:107`，源代码本身的问题）；
  - 1 个是仿真时钟 `always #5.5`（不分析）。
- **修正前后对比**：
  - 修正前，这个工程无法分析（8 个错误）；
  - 修正后：寄存器 969 → 991（aes 的寄存器恢复），不支持的结构 99 → 1。
- 加入 SystemVerilog 语料库：
  - 只检出需要的目录（`loongson/soc_axi_func/rtl` 和 `src`，sparse checkout）；
  - AES 子模块按 nontrivial-mips 记录的提交检出。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：
  - `axiInterfacePorts`、`axiClockConverter`、`axiCrossbar`、`blockMemoryAxi`；
  - Floating Point 端口生成；
  - Clocking Wizard 频率一致性检查。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`.v` 关键字、include 的映射、未实例化模块中的语义错误。
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`、`ProcessLowering.cpp`：`LValueReference`、复合赋值、命名块中的局部声明。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - Clock Converter（AXI3/AXI4/AXI4-Lite 的端口、两个时钟域；负例：AXI3 没有 region，两侧没有混在一起）；
    - Crossbar（拼接的端口，一个时钟）；
    - AXI Block Memory（AXI4、无 ID、AXI4-Lite；负例：没有 lock/prot）；
    - Floating Point（XML；负例：没有 `tready`、没有 C 通道）；
    - Clocking Wizard（过期的 PLL 值改用 MMCM 值；负例：一致的 PLL 值照用，没有说明）。
  - `tests/frontend/test_slang_frontend.cpp`：
    - `.v` 在 SystemVerilog 工程中使用 Verilog 关键字；负例：同样的内容作为 `.sv` 文件时报错。原有测试的期望随之更新：以前 `.v` 在 SystemVerilog 工程中报错，现在不报错。
    - 嵌套 include 之后的文件关键字正确；
    - 未实例化模块中的语义错误是警告；负例：同一模块作为顶层时是错误，语法错误仍是错误。
  - `tests/ir_extract/test_ir_extract.cpp`：
    - 复合赋值展开为 `or`/`add` 链，没有 `opaque`；
    - 命名块中局部声明后的异步复位；负例：带初值的声明仍报 `IR-AMBIGUOUS-CLOCK`。
- `tools/ci/sv-corpus.sh`：`fetch_sparse`、`soc_axi_func` 工程。
- 文档：`docs/vivado-ip.md`、`docs/architecture.md`、`README.md`。
- `CMakeLists.txt`：0.51.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- AXI Clock Converter 的模型不包含它内部的跨域结构（与其他 IP 模型一样，IP 被当作正确完成跨域）。
- SystemVerilog 工程中，内容是 SystemVerilog 的 `.v` 文件现在会报错，这与 Vivado 一致。需要时可以把文件改为 `.sv`。
- Clocking Wizard 的两组倍频/分频值都与配置频率不符时，只给出说明，不推测频率。

# 阶段 83：SERV —— 一个 FuseSoC 核心，各厂商的开发板

日期：2026-11-09　版本：0.83.0

阶段 82 的下一步是再验证一个 FuseSoC 设计。这一阶段选择 **olofk/serv**（提交 f200eb2，ISC 许可）：位串行 RISC-V 处理器 SERV 和它的参考 SoC servant。`servant.core` 有 40 个目标，几乎每个目标是一块开发板，覆盖 Xilinx、Lattice、Intel、Gowin、Microchip、Cologne Chip 的器件。每块板都有自己的顶层和时钟生成方式，正好用来检查 FuseSoC 导入和各厂商时钟原语模型的覆盖面。

SERV 本身只有一个时钟域，所以这一阶段关注的是：每个目标能否导入、细化，时钟能否从板上输入经时钟生成器正确推出。

第一次分析：`servant.core` 无法读取（YAML 流式序列中的隐式键值对）。逐项修复后再按目标分析，发现的问题：

| 问题 | 涉及的目标 |
|---|---|
| 文件名是条件表达式：`"tool_quartus? (servant_ram_quartus.sv)" : {file_type: ...}`，被当作文件名 | 所有目标 |
| 生成器在目标中带参数：`generate: [ice40pll: {freq_out: 16}]`；`icepll` 生成器（调用 IceStorm 的 `icepll` 程序）不运行 | 7 个 iCE40 目标 |
| 新的 flow API：`flow: gowin`，没有 `default_tool` | tangnano9k |
| `datatype: file` 的参数值没有加引号（`memfile=blinky.hex`） | tangnano9k |
| Lattice `LPF` 约束未读取 | ulx3s、ecp5_evn |
| `output wire o_led2 = 1'b0`：输出线网端口带值，SystemVerilog 不允许 | go_board、polarfire_splashkit |
| 未建模的原语：Spartan-6 `PLL_BASE`、Spartan-3E `DCM_SP`、`cyclone10lp_pll` | ax309、lx9_microboard、pipistrello、nexys_2、cyc1000 |

修复后，40 个目标中 35 个 0 错误（见第 2 节）。

## 1. 完成的功能

### 1.1 FuseSoC 导入的补充（`FuseSocProject.cpp`、`Yaml.cpp`）

- YAML：流式序列中的隐式键值对 `[a.pcf : {file_type : PCF}]`、`[ice40pll: {freq_out: 16}]`，各成为一个单项映射。
- 文件集中的文件名可以是条件表达式，按标志求值后再加入。
- 工具：`default_tool`，否则 `flow`，否则 `vivado`。
- 文件类型不区分大小写；`LPF` 也是约束（OpenCDC 读取其中的 `FREQUENCY`/`PERIOD`）。
- `datatype: file` 的参数和 `str` 一样作为字符串。
- 目标中的生成器项可以带参数，覆盖生成器定义中的同名参数。
- `fusesoc:utils:generators` 只提供生成器，找不到时不再报告为缺少的依赖；生成器本身在使用处报告。
- 报告代码 `PRJ-CORE-PRIMGEN` 改为 `PRJ-CORE-GENERATED`，列出 OpenCDC 写出的所有文件（primgen 与 icepll）。

### 1.2 `icepll` 生成器

fusesoc-generators 的 `icepll` 调用 IceStorm 的 `icepll` 程序。OpenCDC 按 `icepll.cc` 的算法自己计算：
- 简单反馈：DIVR 0..15（鉴相频率 10..133 MHz），DIVF 0..127（VCO 533..1066 MHz），DIVQ 1..6，取与目标频率最接近的第一组；
- FILTER_RANGE 按鉴相频率取 1..6；
- 默认写 `pll.vh`（参数列表，供 `#(`include "pll.vh")` 使用，所在目录加入包含目录）；`module: true` 时写模块 `pll(clock_in, clock_out, locked)`。
- 输入频率不在范围内时报告 `PRJ-CORE-GENERATOR`。

iCEBreaker（12 MHz → 16 MHz）：DIVR=0、DIVF=84、DIVQ=6，实际 15.9375 MHz。

### 1.3 输出线网端口的值（`FE-PORT-NET-DEFAULT`）

- `output wire x = expr`：SystemVerilog 只允许输出变量端口带默认值，slang 报告错误；Yosys 和 Vivado 把它当作连续赋值。
- 处理：值从端口声明中去掉（替换为空格，行号不变），在模块头的 `;` 之后加入 `assign x = expr;`。报告一条告警。
- 输出变量（`output reg q = 0`）合法，不处理；`inout` 端口带值仍是错误（反例测试）。

### 1.4 时钟原语模型

| 原语 | 模型 |
|---|---|
| `PLL_BASE`、`PLL_ADV`（Virtex-5、Spartan-6） | 与 PLLE2 相同的生成方式；输入 `CLKIN`（`PLL_ADV` 为 `CLKIN1`），`CLKOUTDCMn` 使用 `CLKOUTn_DIVIDE` |
| `DCM_SP`（Spartan-3/6） | `CLK0/90/180/270` 为输入周期，`CLK2X`/`CLK2X180` 为一半，`CLKDV` × `CLKDV_DIVIDE`，`CLKFX`/`CLKFX180` × `CLKFX_DIVIDE` / `CLKFX_MULTIPLY`；`CLKIN_DIVIDE_BY_2` 先把输入减半 |
| `DCM_CLKGEN`（Spartan-6） | `CLKFX`、`CLKFX180`、`CLKFXDV`；编程端口在 `PROGCLK` 时钟域 |
| `cycloneive_pll`、`cyclone10lp_pll` | Cyclone IV E / Cyclone 10 LP 的 PLL 原子，参数与 `altpll` 相同 |

- 输入时钟没有周期时，输出周期由实例上配置的输入周期推出。报告中的说明以前总写作 `CLKIN1_PERIOD`，对 `CLKIN_PERIOD`、`inclk0_input_frequency`、`FREQUENCY_PIN_CLKI` 都不准确，现在写作“the configured input period”。
- VHDL 组件包 `unisim.vcomponents` 加入 `PLL_BASE`、`PLL_ADV`、`DCM_SP`、`DCM_CLKGEN`。
- `docs/primitives.md` 已更新。

## 2. 真实工程验证

### SERV servant（`servant.core`，按目标分析）

| 目标 | 厂商 / 时钟生成 | 结果 |
|---|---|---|
| ac701、alchitry_au、arty_a7_35t、arty_s7_50t、nexys_a7、ebaz4205 | Xilinx 7 系列 `PLLE2_BASE` | 0 错误，2 个时钟 |
| cmod_a7_35t、te0802、zcu106 | `MMCME2_BASE`、`MMCME4_ADV` | 0 错误 |
| ax309、lx9_microboard、pipistrello | Spartan-6 `PLL_BASE`（新模型） | 0 错误 |
| nexys_2_500、nexys_2_1200 | Spartan-3E `DCM_SP`（新模型） | 0 错误 |
| alhambra、icebreaker、icestick、icesugar、icev_wireless、machdyne_kolibri、tinyfpga_bx、upduino2 | iCE40 PLL（icepll 生成） | 0 错误 |
| ecp5_evn、ulx3s_85 | ECP5 `EHXPLLL`，LPF 中的输入时钟 | 0 错误 |
| de0_nano、de10_nano、deca、max10_10m08evk、sockit | `altpll` | 0 错误 |
| cyc1000 | `cyclone10lp_pll`（新模型） | 0 错误 |
| chameleon96 | Cyclone V HPS | 0 错误；82 个寄存器在未使用的 HPS 接口中（`CLK-UNUSED-INTERFACE`） |
| go_board | iCE40，无 PLL | 0 错误，3 条 `FE-PORT-NET-DEFAULT` |
| tangnano9k | Gowin flow | 0 错误 |
| default、lint | 无板级顶层 | 0 错误 |
| polarfire_splashkit | Microchip `PLL`、`CLKINT` | 无模型（已知限制） |
| gmm7550 | Cologne Chip `CC_PLL`、`CC_USR_RSTN` | 无模型（已知限制） |
| sim、verilator_tb | 仿真目标（`$value$plusargs` 等仿真代码） | 不是综合目标 |

9 个代表性目标加入语料库（每个厂商的时钟生成方式各一个）。所有目标都没有跨时钟路径，与 SERV 的单时钟设计一致。

### 其他语料

- SV 语料 46 项全部匹配：原有 36 个项目与 ibex-demo-system 的结果与修改前逐行相同，新增 SERV 的 9 个目标。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- `src/engine/FuseSocProject.cpp`、`src/engine/Yaml.cpp`：见 1.1、1.2。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-PORT-NET-DEFAULT`。
- `src/frontend/Primitives.cpp`：`PLL_BASE`、`PLL_ADV`。
- `src/frontend/primitives/xilinx_unisim.v`：`DCM_SP`、`DCM_CLKGEN`；`intel.v`：`cycloneive_pll`、`cyclone10lp_pll`；`unisim_vcomponents.vhd`。
- `src/clock/ClockAnalyzer.cpp`、`src/clock/ClockModel.h`：配置的输入周期的说明。
- 测试（每个用例都含反例）：
  - `tests/engine/test_yaml.cpp`：1 个用例（隐式键值对、冒号前的空格）；
  - `tests/engine/test_fusesoc_project.cpp`：1 个用例（条件文件名、flow、LPF、PCF/CST、icepll 输出与分析、Quartus 条件、无法配置的输入频率），primgen 用例改用新报告代码；
  - `tests/frontend/test_quartus_compat.cpp`：1 个用例（输出线网端口的值；`inout` 仍是错误）；
  - `tests/frontend/test_primitives.cpp`：1 个用例（`PLL_BASE`、`DCM_SP`、`DCM_CLKGEN`、`cyclone10lp_pll` 的周期与关系），并更新输入周期说明的断言。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：`check_target`（带 `--target` 的检查），SERV 的 9 个目标；ibex 的报告代码。
- `CMakeLists.txt`：版本 0.83.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：671 个测试全部通过。
- 测试中我自己的两个错误，已改正：
  - 未使用的时钟输出（`CLKDV`、`CLKFXDV`）不会生成时钟，测试中没有寄存器读取它们的输出时查不到周期；后来又因为寄存器的值未被读取而被删除。改为输出端口上的寄存器。
  - 测试框架给 `clk_b` 约束了 7 ns，Cyclone PLL 的输出周期跟随输入时钟（2.625 ns），而不是 `inclk0_input_frequency`；同时报告 `CON-INPUT-PERIOD`。测试按此修改。
- SV 语料 46 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 82 的 Windows CI（提交 adf06b9，运行 42588935517）：Release 与 Debug 均 667/667 通过，生成 `OpenCDC-0.82.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- Microchip PolarFire（`PLL`、`CLKINT`）和 Cologne Chip GateMate（`CC_PLL` 等）没有模型。
- iCE40 板的 PCF 文件没有时钟频率，`icepll` 的 `freq_in` 也不作为输入时钟的约束，所以这些板上 PLL 输出的周期未知（比例已知）。
- `DCM_SP` 的相移、`DCM_CLKGEN` 的动态重配置不建模。

## 6. 下一步

阶段 84：继续用真实工程验证。候选：
- Microchip PolarFire / SmartFusion2 的时钟原语（`PLL`、`CCC`、`CLKINT`）；
- 一个多时钟的 FuseSoC 设计（例如带以太网或 SDRAM 控制器的 SoC）；
- 变量声明的非常量初始化。

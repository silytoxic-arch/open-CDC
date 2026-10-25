# 阶段 13 完成报告：约束建议、未知时钟关系报告、Intel/Lattice/Gowin PLL

日期：2026-10-25　版本：0.13.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T13.1 约束建议 | `opencdc analyze … --suggest-sdc suggested.sdc`、GUI"文件 → 导出建议约束…"，以及 HTML 报告中的一节。生成的内容：<br>- 未约束的时钟输入和 GT/黑盒时钟引脚：`create_clock`，周期标 TODO；<br>- 逻辑分频寄存器：`create_generated_clock`（翻转寄存器按 2 分频，计数器第 k 位按 2^(k+1) 分频），标 VERIFY；<br>- MMCM/PLL 等厂商派生时钟：注释说明"由厂商工具自动推导"及推导式；<br>- 关系未知的时钟族：`set_clock_groups -asynchronous`，标 VERIFY。<br>生成的文件可以直接读回 OpenCDC，也符合 Vivado 语法。 |
| T13.2 未知时钟关系报告 | HTML 报告新增"未知时钟关系与建议约束"一节：列出每个关系未知的时钟对及原因，附上建议约束全文。CLI 输出一行摘要（建议了几个时钟、几个时钟组、几个 TODO，可消除几个未知关系）。 |
| T13.3 PLL 模型 | **Intel**：`altpll`（倍频/分频参数）、`altera_pll`（频率字符串，如 `"125.000000 MHz"`）。<br>**Lattice**：ECP5 `EHXPLLL`；iCE40 `SB_PLL40_CORE/PAD`，以及 `SB_GB`、`SB_DFF*` 触发器。<br>**Gowin**：`rPLL`、`PLLVR`。<br>输出都是派生时钟，周期按参数计算；锁定输出没有时钟域。<br>时钟分析新增对频率字符串属性的解析。 |

## 2. 修改的文件

**新建**
- `src/engine/SdcSuggest.{h,cpp}`
- `src/frontend/primitives/lattice_gowin.v`
- `tests/engine/test_sdc_suggest.cpp`
- `docs/progress/stage-13.md`

**修改**
- `src/frontend/primitives/intel.v`：`altpll`、`altera_pll`
- `src/frontend/primitives/altera_mf_components.vhd`：`altpll` 组件
- `src/frontend/CMakeLists.txt`：嵌入 `lattice_gowin.v`
- `src/clock/ClockAnalyzer.cpp`：`OPENCDC_CLOCK_FREQ_IN/OUT` 属性
- `src/engine/CMakeLists.txt`、`src/engine/Report.cpp`：HTML 新增一节
- `src/cli/main.cpp`：`--suggest-sdc`
- `src/gui/MainWindow.{h,cpp}`、`src/gui/main.cpp`：导出动作；自测中检查导出
- `tests/CMakeLists.txt`（`cli.suggest_sdc`）、`tests/frontend/test_primitives.cpp`
- `docs/primitives.md`（Intel PLL、Lattice、Gowin 三部分）、`README.md`、`docs/architecture.md`（第 20 节）、`CMakeLists.txt`（版本 0.13.0）

已有接口没有删除，也没有改变语义。

## 3. 关键算法

### 3.1 时钟组的合并
每个时钟沿主时钟链找到根时钟。对每个关系为"未知"的时钟对，把两者的根用并查集合并。每个连通分量生成一条 `set_clock_groups -asynchronous`，每个根一个 `-group`，并用 `-include_generated_clocks` 带上它的派生时钟。所以一条约束同时覆盖 MMCM 输出、逻辑分频和 GT 时钟。

### 3.2 引脚命名
- 层次分隔符换成 `/`，并去掉顶层实例名；
- 寄存器引脚：`u/cnt_reg[2]/C`（Vivado 的 `_reg` 命名规则，OpenCDC 的约束匹配器同样识别）；
- 厂商模型引脚：`ch/RXOUTCLK`；
- 时钟名换成 Tcl 安全的标识符：`ch_RXOUTCLK`、`u_cnt_2`。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 344 个 CTest 用例全部通过（阶段 12 是 339 个）。新增：
  - 约束建议：输入时钟、逻辑分频（包括计数器位）和时钟组；建议读回后未知关系为 0，也不再有可建议的内容；
  - GT 引脚与厂商派生时钟的建议，读回后同样消除所有未知关系；
  - 完全约束的设计不需要任何建议（负例）；
  - Intel/Lattice/Gowin 五种 PLL 的周期，共 6 个时钟，误差小于 0.1%；
  - `cli.suggest_sdc`；
  - GUI 自测中的约束导出。
- **真实工程**：
  - PULP `axi_cdc_intf` 去掉约束后分析：50 条路径需复核。生成的建议是 2 个 `create_clock` 和 1 条时钟组；加上建议后需复核的路径降为 0，其他结果不变。
  - VHDL 语料（open-logic、neorv32、surf）和 SV 语料与阶段 12 相同。

## 5. 已知限制
- 建议的周期一律为 10 ns 加 TODO：设计中没有频率信息时不做猜测（MMCM 的 `CLKIN1_PERIOD` 只用于它自己的输出）。
- 逻辑分频比只是启发式猜测：单个翻转寄存器按 2，计数器第 k 位按 2^(k+1)，都标 VERIFY。
- 时钟组建议把未知关系一律视为异步。若设计实际依赖两个时钟同源（例如外部同一晶振），应改用 `create_generated_clock` 关联它们，注释中已说明。
- Lattice/Gowin 模型只能从 Verilog/SystemVerilog 使用，没有 VHDL 组件包。

## 6. 下一步建议
1. 支持读取 Vivado 自动生成的时钟报告（`report_clocks` 输出），用它校验 OpenCDC 推导的周期。
2. 约束检查：报告与设计不符的约束，例如时钟组引用了不存在的时钟，或生成时钟的分频比与模型推导不一致。
3. Lattice/Gowin 的 VHDL 组件包与更多原语（DDR、存储器）。

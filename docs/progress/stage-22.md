# 阶段 22：时钟 IP、存储器 IP 与板卡级工程

日期：2026-10-26　版本：0.22.0

本阶段用 Digilent 的两个板卡演示工程验证：Cmod A7 GPIO 和 Nexys A7 出厂演示（OOB）。两个工程都直接以 `.xci` 使用 Vivado IP。验证中新增了时钟 IP、存储器 IP 和数据通路 IP 的模型，并修正了这些工程暴露出的 VHDL 前端问题。

## 1. 完成的功能

### 1.1 Clocking Wizard（clk_wiz）

`.xci` 中有 Vivado 已经算好的乘除系数（`C_MMCM_CLKFBOUT_MULT_F`、`C_MMCM_CLKOUTn_DIVIDE`、`C_MMCM_DIVCLK_DIVIDE` 等）。模型就是一个用这些参数例化的 `MMCME2_ADV`（PLL 时为 `PLLE2_ADV`），时钟分析由此推导出各输出时钟，与 Vivado 一致。

支持：
- 单端或差分输入（IBUFDS）；
- 双输入时钟切换；
- 高或低有效复位；
- `locked`；
- 外部反馈。

**时钟别名**：Vivado 给 MMCM/PLL 输出时钟取的名字，是连到输出引脚上的线网名，clk_wiz 内部是 `clk_out1_clk_wiz_0`。用户约束常写 `get_clocks clk_out1_clk_wiz_0`。

现在时钟有 `aliases`：
- 对所有 MMCM/PLL 原语模型的输出时钟，都把父模块中连到输出引脚的线网名记为别名；用户 RTL 直接例化 MMCME2_ADV 也适用。
- clk_wiz 模型把内部线网命名为 `<端口>_<实例>`，与 Vivado 生成的代码一致。
- 约束中按名字匹配时钟的地方（`set_clock_groups`、时钟间 false path、路径例外中的时钟、`set_input_delay -clock`）都同时匹配别名。
- 命令行时钟列表显示 `alias=`，JSON 结果有 `aliases`，GUI 时钟视图在名字后显示别名。

### 1.2 Block Memory Generator（blk_mem_gen）

按 `C_MEM_TYPE` 生成端口：单端口 RAM、简单双端口、真双端口、单端口 ROM、双端口 ROM。

- 写口在自己的时钟下写入；
- 读口经"1 + 输出寄存器级数"在自己的时钟下输出；
- 真双端口时两个写口各写一个存储器，读出取两者的组合。

因此在 `clka` 写、在 `clkb` 读的数据会被报告为跨域，设计必须用协议交接地址和数据。存储器内容与 CDC 无关，模型只保留 256 个字，避免巨大的数组。

### 1.3 单时钟数据通路 IP

覆盖 cordic、floating_point、div_gen、mult_gen、c_addsub、c_accum、c_counter_binary、c_shift_ram、dds_compiler、fir_compiler、cic_compiler、cmpy、xbip_dsp48_macro。

每个输出都是 IP 时钟域内的寄存器，并依赖全部输入，这是保守的处理。端口取自 JSON `.xci` 的端口表；XML 格式下目前只能推导 cordic 的端口。

### 1.4 原语：XADC / SYSMONE1 / SYSMONE4

- DRP（`DO`/`DRDY`）和状态输出属于 `DCLK` 域；
- `JTAG*` 输出没有时钟域；
- VHDL 的 `unisim.vcomponents` 中也加入了这三个组件。

### 1.5 VHDL 前端

- **注释中的 "library" 干扰厂商库识别**：检测 VHDL 用了哪些厂商库时，正则没有去掉注释。注释 "-- Uncomment the following library declaration if instantiating ..." 让 `library UNISIM;` 没被识别，报 "UNISIM 未声明"。Digilent 的模板代码里普遍有这一行。现在两处检测都先去掉 `--` 注释。
- **`x <= v when rising_edge(clk);`**：VHDL-93 的并发寄存器写法，以前被当成锁存器并报 IR-UNSUPPORTED-EDGE。现在改写为等价的时钟进程，支持附带使能（`and en = '1'`）和 `else unaffected`。没有边沿的条件赋值不受影响。
- **有组件声明、没有实体的实例（厂商 IP 黑盒）**：
  - 输出端口作为黑盒输出驱动实参：时钟输出成为一个独立的黑盒时钟（例如 MIG 的 `ui_clk`），数据输出没有时钟域；
  - 输入端口读取实参，使 IP 前面的逻辑不被当成无用逻辑删除，例如 MIG 的复位寄存器 `rstn`。

  以前这些实例没有任何端口，Nexys OOB 中 69 个寄存器的时钟无法解析。

### 1.6 约束

引用黑盒实例内部的约束（例如 `Inst_DDR/u_ddr_mig/.../PRE`）降为提示，并说明它在哪个黑盒里。

## 2. 真实工程验证

| 工程 | IP | 结果 |
|---|---|---|
| Cmod A7 GPIO | clk_wiz（12 → 100 MHz） | 2 个时钟，37/37 个寄存器；时钟有别名 `clk_out1_clk_wiz_0`；0 个跨域 |
| Nexys A7 OOB（31 个 VHDL 文件） | clk_wiz、blk_mem_gen ROM、cordic、XADC；MIG 为黑盒 | 4 个时钟，362/362 个寄存器；5 条跨域路径：3 条已同步，2 条潜在风险，1 条需复核 |

Nexys A7 OOB 中报告的问题都是真实的：
- **CDC-006**：MMCM 的 `LOCKED` 直接作为 MouseCtl 等 17 个寄存器的异步复位，复位释放没有同步。
- **CDC-007/010**：PDM 脉冲从 MIG 的 `ui_clk` 域经多级同步器进入显示时钟域。`ui_clk` 来自黑盒、周期未知，无法确认脉冲能被采到。

两个工程已加入 VHDL 语料库，它们的 VHDL 文件也加入了"全部解析"一步。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：clk_wiz、blk_mem_gen、单时钟数据通路 IP。
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：XADC、SYSMONE1、SYSMONE4。
- `src/frontend/AutoFrontend.cpp`：库检测前去掉 VHDL 注释。
- `src/frontend/MixedSupport.h`：提示中的 IP 列表。
- `src/frontend/vhdl/VhdlStmt.cpp`：并发边沿赋值改写为时钟进程。
- `src/frontend/vhdl/VhdlElab.cpp`：黑盒组件的端口绑定。
- 时钟：
  - `src/clock/ClockModel.{h,cpp}`：`aliases`、`clockMatches`，`find` 也查别名；
  - `src/clock/ClockAnalyzer.cpp`：MMCM/PLL 输出的别名，名字匹配改用 `clockMatches`。
- `src/cdc/CdcAnalyzer.cpp`：路径例外中的时钟名匹配别名。
- `src/constraints/Sdc.{h,cpp}`：`blackBoxContaining`，黑盒内部引用降为提示。
- 输出：`src/cli/main.cpp`、`src/engine/ResultJson.cpp`、`src/gui/ClockView.cpp`（别名）。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - clk_wiz：派生频率；约束用 Vivado 名字，负例为不存在的名字；PLL、差分、低有效复位；
    - blk_mem_gen：端口按类型推导；双时钟是跨域，负例为 ROM；
    - cordic 和 XADC：同域读取不跨域（负例），异域读取跨域（正例）。
  - `tests/vhdl/test_vhdl_elab.cpp`：并发边沿赋值，负例为普通条件赋值仍是锁存器。
  - `tests/vhdl/test_vhdl_mixed.cpp`：注释中的 library 字样；黑盒组件的输入、输出绑定。
- 语料库：`tools/ci/vhdl-corpus.sh`。
- 文档：`docs/vivado-ip.md`、`docs/primitives.md`、`README.md`。

## 4. 已知限制

- MIG（DDR 控制器）、DMA、AXI 互连等复杂 IP 没有模型。有 VHDL 组件声明时，它们作为黑盒仍能让周围的逻辑得到分析；Verilog 的黑盒实例原本就是如此。
- 单时钟数据通路 IP 从 XML `.xci` 只能推导 cordic 的端口。
- clk_wiz 的动态重配置不建模，按配置的频率处理。
- 时钟别名只覆盖 MMCM/PLL 输出（Vivado 的自动派生时钟）。由寄存器分频得到的时钟，Vivado 不自动命名，也就没有别名。

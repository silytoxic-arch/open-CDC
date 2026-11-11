# 阶段 91：Arria V 收发器上的 DVI —— MiSTVGA

日期：2026-11-11　版本：0.91.0

阶段 90 的下一步是 Intel（Quartus）的视频或 SerDes 设计。这一阶段选择 **ianhan/MiSTVGA**（提交 6de2899）：
- ao486 的 VGA 核心移植到 PCI 总线；
- 运行在 Arria V GX 开发板上，DVI 输出经过收发器（Transceiver Native PHY，4 个通道，1250 Mbps）；
- SSRAM 帧缓冲由 `altpll` 驱动；
- Platform Designer 生成的 Avalon 互联；
- 时钟：板上 50 MHz、100 MHz、PCI 33 MHz、收发器参考时钟 125 MHz、SSRAM PLL、收发器 TX 时钟。

工程按 Quartus 工程（`.qpf`）分析，约束是作者的 SDC。

第一次分析：
- 41 个错误：
  - `dvi_tx_top.v` 在模块内写 `` `default_nettype none ``；
  - `altpll` 的 MegaWizard `defparam` 列表中有 OpenCDC 模型不声明的参数（`lpm_hint`、`port_*`）；
- 去掉这两个问题后，又有 25 个 `FE-UNKNOWN-MODULE`：收发器 IP 生成的核心实例化 Arria V 器件原子（`arriav_hssi_*`、`arriav_channel_pll`、`alt_cal_av`），它们只在 Quartus 的库中。
- 按黑盒分析（`--blackbox-unknown`）时：
  - TX 时钟来自黑盒，与其他时钟关系未知；
  - SDC 中有 1 条未匹配（PLL 引脚名）、多条 `CON-GROUP-OVERLAP`（`get_clocks -nowarn` 被误读）；
  - 14 个 `while` 循环未分析；
  - 388 个寄存器的时钟未解析。

修复后：0 个错误，7 条告警，0 个未分析的构造。

## 1. 完成的功能

### 1.1 收发器 IP 的内置模型（`intel.v`、`QuartusProject.cpp`、`ClockAnalyzer.cpp`）

- **方法**：Quartus 工程中，`.qip` 的 `IP_TOOL_NAME` 为下列之一时，`.qip` 列出的、位于该变体自己目录下的文件不读（`PRJ-IP-MODEL`）。这些是生成的核心及其器件原子。变体的封装（`tmds_tx_phy.v`）仍然读取，其中实例化的核心因此由内置模型代替。
  - `altera_xcvr_native_av`
  - `altera_xcvr_native_cv`
  - `alt_xcvr_reconfig`
- **`altera_xcvr_native_av`/`_cv`**：
  - 每个通道的 `tx_std_clkout` 由选中的参考时钟 `tx_pll_refclk[pll_refclk_select]` 派生，频率 = `data_rate` / `std_pcs_pma_width`（字节串行器再 / 2）；
  - 参考时钟频率取自 `pll_refclk_freq` 列表中对应的一项；
  - Quartus 名 `<实例路径>|txpmalocalclk`（作者的 SDC 用 `*txpmalocalclk*`）；
  - `tx_parallel_data`（每通道 44 位）在 `tx_std_coreclkin` 上寄存；
  - `pll_locked`、`tx_cal_busy` 没有时钟域。
- **`alt_xcvr_reconfig`**：Avalon-MM 从接口和输出都是 `mgmt_clk_clk` 上的寄存器。
- **时钟分析**新增三项：
  - `OPENCDC_CLOCK_FREQ_IN_INDEX`：从频率列表中按下标取一项；
  - 单位 `Mbps`/`Gbps`；
  - `OPENCDC_CLOCK_FREQ_OUT_DIVIDE`：数据率除以字宽。

### 1.2 Quartus 接受的写法（`SlangFrontend.cpp`）

| 写法 | 处理 |
|---|---|
| 模块内的编译指令（`` `default_nettype ``、`` `timescale ``、`` `resetall ``） | 移到模块的 `endmodule` 之后（之后的文件仍然受影响），`FE-DIRECTIVE-IN-MODULE` 告警 |
| `defparam` 一个厂商模型不声明的参数 | 与 `#(...)` 中的参数一样忽略，`FE-PRIMITIVE-PARAM`（提示） |

- `defparam` 的目标模块按源文件中 `inst` 的实例化语句确定。设计中的模块不在此列，仍然报错。

### 1.3 `while` 循环（`ProcessLowering.cpp`）

- Intel 的 `altera_merlin_width_adapter` 用设计中的值调用 `clogb2`，函数体是 `while (value > 0) value = value >> 1;`。
- 现在按综合工具的方式展开：
  1. 先做一次“每次迭代都执行”的试运行，找到条件必然变为常量假的那次迭代（64 位值右移 64 次后为 0）。任何真实执行若走到那一步，之前的迭代都已执行，所以循环在那之前一定结束。
  2. 在这个上界内，每次迭代在“之前的条件都成立”之下执行。
- 试运行不输出诊断。4096 次以内找不到结束的循环仍然报告 `IR-UNSUPPORTED-LOOP`。
- 以前 `while` 一律作为未支持语句（`IR-UNSUPPORTED-STMT`）。

### 1.4 常量移位是连线（`ConstFold.cpp`）

- 试运行的条件要成为常量，`value >> 1` 必须是位的重新连接，而不是一个移位单元。
- 现在：移位量为常量的 `<<`、`>>`、`>>>` 在 IR 中化为连线（空出的位补 0 或符号位）。
- 这也让 CDC 路径更准确。例如 corundum PTP 叶时钟的 `src_ns_sync_reg <= src_ns_reg >> 8`：
  - 以前算作“经过组合逻辑”，宽度 21；
  - 现在不经过逻辑，宽度 13（只有到达的位）。
- 与修改前比较 ao486、SNES、corundum、zybo_sobel、chiplab 的 JSON 结果：
  - 路径数、各状态计数和规则统计都不变；
  - 变化只有路径说明中不再出现移位单元、单元编号（如 `$mux$878` → `$mux$876`），以及 corundum 的 5 条路径的宽度和“经过逻辑”标记。
- `tests/ir_extract/golden/afifo.ir` 中格雷码的 `bin >> 1` 现在是 `{1'b0, bin[4:1]}`；我逐行核对了差异，其余不变。

### 1.5 Quartus SDC（`Sdc.cpp`）

- PLL 模型的时钟输出在 `get_pins` 中也有 Quartus 名（`u_ssram_pll|altpll_component|auto_generated|generic_pll1~PLL_OUTPUT_COUNTER|divclk`），可以作为 `create_generated_clock -source`。以前只有时钟名。
- `get_clocks -nowarn`（Quartus 的 `-quiet`）：以前被当作带值的选项，吞掉了模式，结果是“所有时钟”，造成多条 `CON-GROUP-OVERLAP`。
- `create_generated_clock` 的目标查询没有匹配时，不定义时钟（查询本身报告 `CON-UNMATCHED`），不再是错误 `CON-PARSE`。
  - SNES 的 SDC 原来因 `-source` 不匹配而跳过这两条命令；现在 `-source` 能匹配，目标（`altsyncram` 内部的 `clk0`）不匹配。结果与以前相同。

## 2. 真实工程验证

### MiSTVGA（`a5gx_mistvga.qpf`，加入 SV 语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 41（另有 25 个未知原子） | 0 |
| 告警 | — | 7 |
| 未分析的构造 | 39（按黑盒时） | 0 |
| 时钟 | 5 | 7（含 SSRAM 输出时钟和收发器 TX 时钟 125 MHz） |
| 跨时钟路径 | — | 316（18 条已同步） |

发现：
- **收发器参考时钟连接**（`CLK-UNRESOLVED`、`CLK-CONSTANT`）：
  - 变体配置为使用第 1 个参考时钟（`pll_refclk_select = "1"`，`pll_refclk_freq = "unused,125.0 MHz"`），生成的 RTL 中 PLL 读 `refclk[1]`；
  - `dvi_tx_top` 把唯一的时钟 `pll_refclk`（1 位）连接到 2 位的 `tx_pll_refclk`，按 Verilog 规则零扩展后 `refclk[1]` 是常量 0；
  - OpenCDC 因此报告 TX 时钟的来源是常量。这需要作者确认：要么连接应为 `{pll_refclk, 1'b0}`，要么 Quartus 对收发器参考时钟端口另有处理。
  - 在此之前，TX 时钟域的 385 个寄存器的跨时钟路径仍然按该时钟检查。
- **PCI 时钟 → 100 MHz**：194 条路径没有同步器。VGA 寄存器在 PCI 时钟域写入，在 VGA 核心的 100 MHz 时钟域使用（准静态配置）。
- **TX 时钟域**：进入 TX 时钟域的路径中，来自 100 MHz 的 33 条有 14 条已同步，来自 PCI 时钟的 6 条有 4 条已同步，其余报告。

### 其他语料

- SV 语料 55 项（新增 1 项）、VHDL 语料 49 个全部匹配。

## 3. 修改的文件

- `src/frontend/primitives/intel.v`：`altera_xcvr_native_av`、`altera_xcvr_native_cv`、`alt_xcvr_reconfig`。
- `src/engine/QuartusProject.cpp`：`PRJ-IP-MODEL`。
- `src/clock/ClockAnalyzer.cpp`：频率列表、数据率、字宽。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-DIRECTIVE-IN-MODULE`、`FE-PRIMITIVE-PARAM`（`defparam`）。
- `src/frontend/slang_adapter/ProcessLowering.cpp`、`IrBuilder.{h,cpp}`：`while` 循环；试运行不输出诊断。
- `src/ir/ConstFold.cpp`：常量移位。
- `src/constraints/Sdc.cpp`：PLL 引脚的 Quartus 名、`-nowarn`、无目标的生成时钟。
- `docs/primitives.md`、`README.md`。
- 测试（每个用例都含反例）：
  - `tests/engine/test_quartus_project.cpp`：收发器变体的生成文件不读、TX 时钟 8 ns 并带 `txpmalocalclk` 名、经 `*txpmalocalclk*` 的时钟组；反例：`.qip` 没有 `IP_TOOL_NAME` 时生成文件照常读取。
  - `tests/frontend/test_quartus_compat.cpp`：
    - 模块内的 `` `default_nettype ``；反例：模块外的指令没有告警；
    - `altpll` 的 `defparam`，模型声明的参数仍然生效（5 ns 的输出）；反例：设计模块不存在的参数仍是错误；
    - `clogb2` 的 `while` 循环；反例：条件永远不成为常量的循环；
    - PLL 引脚作为 `-source`、`-nowarn`、无目标的生成时钟；反例：拼错的引脚不匹配；
    - 常量移位不经过逻辑、宽度为到达的位数；反例：变量移位仍是逻辑。
  - `tests/ir_extract/test_ir_extract.cpp`：有符号算术移位与 `bin2gray` 的预期改为连线（并加变量移位的反例）；`while` 用例改为“找不到结束”的情形。
  - `tests/ir_extract/golden/afifo.ir`：见 1.4。
- `tools/ci/sv-corpus.sh`：MiSTVGA。
- `CMakeLists.txt`：版本 0.91.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：707 个测试全部通过。
- 常量移位改变了 4 个已有测试的预期（见第 3 节），都按新的 IR 核对后修改，没有放宽检查。
- SV 语料 55 项、VHDL 语料 49 个全部匹配。
- Clang 构建无告警。
- 阶段 90 的 Windows CI（提交 8cdd443，运行 42791864161）：Release 与 Debug 均 701/701 通过。

## 5. 已知限制

- 收发器模型只有 TX 的并行时钟和数据；RX 路径（恢复时钟）、串行侧、重配置接口不建模。Arria 10 及更新器件的收发器（`altera_xcvr_native_a10` 等）、10GBASE-R 等 PHY IP 没有模型。
- `while` 循环的上界只在“每次都执行”的试运行中条件变为常量假时可得；需要数值推理的循环（如 `while (y > 8) y = y - 1`）仍不分析。
- 模块内的编译指令移到 `endmodule` 之后，因此对该模块剩余部分不生效（`` `default_nettype none `` 的检查较宽松）。

## 6. 下一步

阶段 92：继续真实设计验证，例如 Gowin 或 Lattice 的视频设计、带 RX 收发器的设计。

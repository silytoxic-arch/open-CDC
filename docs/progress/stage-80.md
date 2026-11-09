# 阶段 80：内部振荡器、yosys/nextpnr 流程的 ULX3S 工程

日期：2026-11-09　版本：0.80.0

阶段 79 列出的两个候选，这一阶段都做了：
- **内部振荡器**：许多小型 Lattice/Gowin 设计用片内振荡器作为唯一时钟，以前这些原语是未知模块。现在为 MachXO2 `OSCH`、ECP5 `OSCG`、iCE40 UltraPlus `SB_HFOSC`/`SB_LFOSC` 和 Gowin `OSC` 建模。
- **yosys/nextpnr 流程的工程**：选择 lawrie/ulx3s_zx_spectrum（提交 19f242c）。这是 ULX3S（ECP5 85F）上的 ZX Spectrum：TV80 Z80 CPU、HDMI、USB 主机、SPI OSD，没有厂商工程文件，用 Makefile 构建。

MachXO2 的 `EFB`/`BB` 没有做：没有它们端口和行为的可靠资料，按猜测建模可能给出错误结果。

## 1. 完成的功能

### 1.1 内部振荡器模型

| 原语 | 频率 |
|---|---|
| `OSCH`（MachXO2/XO3） | `NOM_FREQ`（MHz，字符串，如 "2.08"） |
| `OSCG`（ECP5） | 310 MHz / `DIV` |
| `SB_HFOSC`（iCE40 UP） | 48 MHz / 2^`CLKHF_DIV`（"0b00".."0b11"） |
| `SB_LFOSC`（iCE40 UP） | 10 kHz |
| `OSC`（Gowin，只在 Gowin 工程中） | 250 MHz / `FREQ_DIV`（GW1N-1/GW1NZ-1 为 240 MHz） |

- 每个振荡器的输出是一个独立的时钟，与其他时钟的关系未知，跨越都会检查。
- 实现方式：模型的输出线带属性 `OPENCDC_CLOCK_FREQ`（单位 MHz）。时钟分析读取它作为周期，用于没有其他周期来源的黑盒时钟。
- Gowin 的 `OSC` 名称太通用，按阶段 77 的规则只在 Gowin 工程中使用（反例测试：普通工程中仍是未知模块）。
- `docs/primitives.md` 已更新；VHDL 组件包中加入了 `OSCH`、`OSCG`、`SB_HFOSC`、`SB_LFOSC`。

### 1.2 yosys/nextpnr 流程的工程

- 这类工程没有工程文件。OpenCDC 工程取 Makefile 的 `VERILOG` 文件列表和 `.lpf`：语料脚本从 Makefile 生成工程文件，与 Corundum 相同。
- 除了这一点，不需要修改分析器：
  - `ecppll` 生成的 `pll.v`（EHXPLLL）有模型；
  - `.lpf` 的 `FREQUENCY PORT` 已在阶段 78 支持；
  - 过程块中给 wire 赋值的写法已有 Quartus 兼容处理。

## 2. 真实工程验证

### ulx3s_zx_spectrum（加入语料库）

| | 结果 |
|---|---|
| 错误 | 0 |
| 实例 | 39 |
| 寄存器 | 270（全部有时钟），0 个锁存器，0 个未分析的构造 |
| 时钟 | 6 |
| 跨时钟路径 | 1 |

时钟结构：
- 输入时钟 `clk25_mhz` 由 LPF 定义（25 MHz）。
- 两个 EHXPLLL 都从它派生：125 MHz、28.41 MHz、6 MHz 等。
- CPU 时钟由计数器分频得到（`cpuClockCount[2]`）。
- 设计者没有声明任何异步时钟对，所以这些时钟都按同步处理。

唯一的跨时钟路径是一个异步输入到 USB 时钟域的复位。

### fpga-disk-controller `yellowstone_blink.ldf`（加入语料库）

- 一个只用 `OSCH #(.NOM_FREQ("2.08"))` 作为时钟的 MachXO2 设计。
- 以前 `OSCH` 是未知模块，现在 1/1 个寄存器有时钟，周期 480.769 ns（2.08 MHz）。

### 其他语料

- SV 语料共 35 个项目，全部匹配。新增两个，其余与修改前逐行相同。
- VHDL 语料 48 个全部匹配。

## 3. 修改的文件

- `src/frontend/primitives/lattice_gowin.v`：`OSCH`、`OSCG`、`SB_HFOSC`、`SB_LFOSC`。
- `src/frontend/primitives/gowin.v`：`OSC`。
- `src/frontend/primitives/lattice_components.vhd`：组件。
- `src/clock/ClockAnalyzer.cpp`：`OPENCDC_CLOCK_FREQ`。
- 测试（每个用例都含反例）：
  - `tests/frontend/test_primitives.cpp`：1 个用例；
  - `tests/engine/test_gowin_project.cpp`：1 个用例。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：ulx3s_zx_spectrum（工程由 Makefile 生成）、yellowstone_blink；fpga-disk-controller 改为非 cone 模式的稀疏检出，以便检出根目录下的单个文件。
- `CMakeLists.txt`：版本 0.80.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：653 个测试全部通过。
- 新测试中我自己的一个预期错误，已改正：普通工程中未知的 `OSC` 报告的是 `FE-SLANG-UnknownModule`，不是 `FE-UNKNOWN-MODULE`。
- 语料脚本的一处错误，已改正：稀疏检出的 cone 模式不接受单个文件，第一次运行时整个 SV 语料脚本失败。
- SV 语料 35 个、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 79 的 Windows CI（提交 576d886，运行 42577411641）：Release 与 Debug 均 651/651 通过，生成 `OpenCDC-0.79.0-windows-x64.zip` 与 `.exe`。
- 本阶段的 Windows CI：见提交后的补充。

## 5. 已知限制

- 振荡器频率取标称值，不考虑器件的频率偏差。
- MachXO2 的 `EFB`、`BB` 仍没有模型。
- yosys/nextpnr 流程没有工程文件，需要手写或生成 OpenCDC 工程。nextpnr 的 `--freq` 选项和 Python 时钟约束不读。

## 6. 下一步

阶段 81：继续用真实工程验证。候选：
- 一个 iCE40 UltraPlus 设计（`.pcf`，`SB_HFOSC`、`SB_SPRAM256KA`、`SB_IO`）；
- 一个较大的、以 SystemVerilog 为主的开源 SoC，例如 OpenTitan 的 FPGA 顶层，或 CVA6 的 Genesys2 工程。

# 阶段 81：iCE40 UltraPlus —— ice40-playground riscv_usb

日期：2026-11-09　版本：0.81.0

阶段 80 的下一步候选之一是 iCE40 UltraPlus 设计。这一阶段选择 **smunaut/ice40-playground** 的 `riscv_usb`（提交 d2fa005）：iCEBreaker 上的一个 PicoRV32 SoC，带 USB 全速设备，使用 yosys/nextpnr 流程。它的核心库 no2usb、no2misc、no2ice40 是 git 子模块，按工程中固定的提交获取。

这个设计用到了很多 iCE40 原语，以前它们都没有模型：
- `SB_PLL40_2F_PAD`（48 MHz 和它的一半 24 MHz）；
- `SB_IO`（USB 差分对的寄存器输入/输出、SPI 引脚）；
- `SB_SPRAM256KA`；
- 硬核 `SB_SPI`、`SB_LEDDA_IP`；
- `SB_RGBA_DRV`、`SB_WARMBOOT`。

第一次分析有 11 个错误，另有 3 个锁存器、1 个未分析的构造，整个设计无法分析。修复后：
- 0 个错误，0 个锁存器，0 个未分析的构造；
- 358/359 个寄存器的时钟已解析（剩下的 1 个在模型中，时钟接常量，属于未使用的接口）。

工程已加入语料库。

## 1. 完成的功能

### 1.1 iCE40 原语模型（`lattice_gowin.v`）

| 原语 | 模型 |
|---|---|
| `SB_PLL40_2F_PAD`、`SB_PLL40_2F_CORE`、`SB_PLL40_2_PAD` | 两个输出。`GENCLK` = f(ref) × (DIVF+1) / ((DIVR+1) × 2^DIVQ)，`GENCLK_HALF` 为其一半。`SB_PLL40_2_PAD` 的端口 A 就是输入时钟本身 |
| `SB_IO` | 按 `PIN_TYPE` 生成：输入可以直通，或在 `INPUT_CLK` 上寄存（`D_IN_1` 在另一个沿）；输出可以直通、在 `OUTPUT_CLK` 上寄存、反相寄存或 DDR；输出使能可以是从不、总是、`OUTPUT_ENABLE` 或寄存的使能。`NEG_TRIGGER` 交换时钟沿。输出从不使能时不生成输出寄存器 |
| `SB_SPRAM256KA` | 16k × 16 单口 RAM，在 `CLOCK` 上读写 |
| `SB_SPI`、`SB_I2C`、`SB_LEDDA_IP` | 硬核：系统总线、串行输入和所有输出都在总线时钟域（`SBCLKI`、`LEDDCLK`） |
| `SB_RGBA_DRV`、`SB_WARMBOOT` | 组合逻辑的电流驱动；只有输入的热启动 |

`docs/primitives.md` 已更新。

### 1.2 yosys 接受的写法

- **`(* full_case *)`**（`picorv32.v`：`case (mem_wordsize)` 只列出 0、1、2）：
  - 未列出的值作为无关项，最后一项取其余情况，不推断锁存器（以前有 3 个锁存器）。
  - 反例测试：没有这个属性时仍然推断锁存器。
- **边沿敏感块中包在复位 if 外面的常量条件**（`usb.v`：`always @(posedge clk or posedge rst) if (IRQ) begin if (rst) ...`）：
  - 用 slang 对常量求值，选择对应的分支。为 0 时整个块不赋值，也不报告。
  - 求值不经过 IR 降级，所以不会为非常量条件产生多余的单元。最初的实现会产生这样的单元，两个黄金转储测试因此失败，见第 4 节。
  - 反例测试：条件不是常量时仍然报告 `IR-AMBIGUOUS-CLOCK`。
- **模块内部驱动的输入端口**（`ice40_spi_wb.v`：`SB_IO` 的 `D_IN_0` 驱动输入端口 `sio_mosi_i`，实例化时不连接这个端口）：
  - 所有位都在内部驱动时，读作输出端口，报告告警 `IR-INPUT-DRIVEN`：外部连接的值不分析。
  - 反例测试：只有部分位在内部驱动时，仍然是多驱动错误。

### 1.3 工程

- 工程文件由语料脚本生成：源文件取各个 `no2core.mk` 和项目 Makefile 的列表，include 路径取 `no2usb/rtl`（`usb_defs.vh`）和项目的 `rtl`（`boards.vh`），宏 `BOARD_ICEBREAKER`（no2build 由 `BOARD` 生成）。
- nextpnr 的时钟约束在 `data/clocks.py` 中（`ctx.addClock("clk_24m", 24)`），是一个 Python 脚本，OpenCDC 不读取。
  - 不读的原因：如果把它译成 `create_clock`，两个 PLL 输出会变成彼此无关的约束时钟。但 24 MHz 是同一 PLL 的 `GENCLK_HALF`，两者实际上是同步的。
  - 时钟由 PLL 模型从输入时钟派生；没有时钟约束，所以输入 `clk_in` 没有周期。

## 2. 真实工程验证

### ice40-playground riscv_usb（加入语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 11（未知原语 10 个；加模型后又出现 3 个输入端口多驱动） | 0 |
| 锁存器 | 3（`full_case`） | 0 |
| 未分析的构造 | `IR-AMBIGUOUS-CLOCK` 1 处 | 0 |
| 寄存器 | — | 359（358 个有时钟） |
| 时钟 | — | 3：`clk_in`、PLL 端口 A（48 MHz，USB）、端口 B（24 MHz，系统） |
| 跨时钟路径 | — | 1 |

- 唯一的跨时钟路径是 PLL 的 `LOCK` 作为异步复位，进入 24 MHz 域的复位计数器（CDC-006，潜在风险）。
- USB 48 MHz 域与系统 24 MHz 域之间是同步关系：同一 PLL 的 `GENCLK` 与 `GENCLK_HALF`。设计中的 `xclk_*` 同步器在这种关系下不需要。

### 其他语料

- SV 语料共 36 个项目，全部匹配，新增 riscv_usb；其余结果与修改前逐行相同。
- VHDL 语料 48 个，结果与修改前逐行相同。

## 3. 修改的文件

- `src/frontend/primitives/lattice_gowin.v`：iCE40 模型。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：`full_case`，边沿敏感块中的常量条件。
- `src/ir/BodyEditor.cpp`：`IR-INPUT-DRIVEN`。
- 测试（每个用例都含反例）：
  - `tests/ir_extract/test_ir_extract.cpp`：3 个用例；
  - `tests/frontend/test_primitives.cpp`：1 个用例，并更新原语清单。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：riscv_usb（含 3 个子模块）。
- `CMakeLists.txt`：版本 0.81.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：657 个测试全部通过。
- 第一次运行时有 2 个黄金转储测试失败：
  - 原因：常量条件最初通过 IR 降级判断，非常量的复位条件也会创建单元，单元编号因此改变。
  - 修正：改用 slang 对常量求值，转储与以前完全相同。
- SV 语料 36 个、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 80 的 Windows CI（提交 aa27e89，运行 42579276544）：Release 与 Debug 均 653/653 通过，生成 `OpenCDC-0.80.0-windows-x64.zip` 与 `.exe`。
- 本阶段的 Windows CI：见提交后的补充。

## 5. 已知限制

- **nextpnr 约束**：Python 写的时钟约束（`clocks.py`）不读，`.pcf` 只有引脚，没有时钟信息。
- **硬核模型**：`SB_SPI`、`SB_I2C`、`SB_LEDDA_IP` 只建模了时钟域，不建模具体行为；它们的串行输入按“在总线时钟中采样”处理。
- **未建模的 iCE40 原语**：`SB_MAC16`、`SB_GB_IO`、`SB_IO_OD`、`SB_IO_I3C` 等。
- **`IR-INPUT-DRIVEN`**：如果实例从外部驱动了这个输入，外部的值不分析。

## 6. 下一步

阶段 82：一个较大的、以 SystemVerilog 为主的开源 SoC，例如 CVA6 的 Genesys2 工程或 OpenTitan 的 FPGA 顶层。

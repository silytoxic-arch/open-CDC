# 阶段 72：ao486_MiSTer —— translate_off、被其他驱动覆盖的高阻赋值、同一模块内对生成块的层次引用

日期：2026-11-08　版本：0.72.0

阶段 71 的下一步是用不同的真实工程继续验证。这一阶段选择 **ao486_MiSTer**（x86 处理器核心）。与前面的游戏机核心不同：
- 主要是 Verilog；
- 有 L1/L2 缓存、DMA、VGA、声卡（GUS、OPL）等外设；
- 规模更大：661 个实例。

第一次分析有 11 个错误，整个设计无法分析（0 个实例）。原因有两类，都是 OpenCDC 与综合工具的语义不一致。修复后还处理了 1 个未分析的构造。最终是 0 个错误、0 个锁存器、0 个未分析的构造，并加入了语料库。

## 1. 完成的功能

### 1.1 Verilog 的 `translate_off` / `translate_on`

```verilog
// synthesis translate_off
cpu_export cpu_export_inst(.clk(clk), ...);   // pipeline.v：只用于仿真的模块，工程中没有它的源文件
// synthesis translate_on
```

- 综合工具跳过这两个注释之间的代码。以前 OpenCDC 只在 VHDL 中这样处理，Verilog 中这个实例被当作未知模块（错误）。
- 现在 Verilog 也跳过这些区域，前缀与 VHDL 词法分析器相同：`synthesis`、`synopsys`、`pragma`、`xilinx`、`altera`、`intel`、`exemplar`、`cadence`，以及 `rtl_synthesis off/on`。
- 实现：使用 slang 词法分析器的注释处理（`LexerOptions::commentHandlers`，`CommentHandler::TranslateOff`）。
- 反例测试：
  - `translate_on` 之后的代码照常分析；
  - 没有工具前缀的 `// translate_off` 只是普通注释。

### 1.2 被其他驱动覆盖的高阻赋值

```systemverilog
assign {SDRAM_A, SDRAM_BA, SDRAM_DQ, SDRAM_CLK, ...} = 'Z;   // ao486.sv
sdram sdram(..., .SDRAM_CLK(SDRAM_CLK), ...);                // 同一个模块中，实例的输出端口驱动同一些位
```

- 按线网的决议规则，常量 `Z` 驱动遇到另一个驱动时，结果就是另一个驱动的值，Quartus 也这样综合。
- 以前 OpenCDC 把 `'Z` 当作一个普通驱动，报告 10 个多驱动错误（`IR-MULTIDRIVER`）。
- 现在，连续赋值中右边是常量 `Z` 的位，如果有其他驱动（输入端口、其他赋值、寄存器、锁存器、子实例的输出），就从这条赋值中去掉，IR 中只留下真正的驱动。
- 新的 `BodyEditor::dropOverriddenZ`：
  - 在多驱动检查之前运行；
  - 只有右边含 `Z` 常量的赋值才逐位检查，其他赋值不受影响，也不增加分析时间。
- 反例测试：
  - 没有其他驱动时，`'Z` 赋值保留；
  - 两个真正的驱动仍然报告多驱动错误。

### 1.3 同一模块内对生成块的层次引用

```systemverilog
for (i = 0; i < 2; i++) begin : clk_rate  reg [4:0] cnt; ... end
always @(posedge CLK_AUDIO) crossed <= (clk_rate[0].cnt <= clk_rate[1].cnt);   // sys/mt32pi.sv
```

- 引用的是同一个模块中、生成块里的变量，可以综合。
- 以前作为层次引用报告为不支持，结果按保守连接建模。由于 `cnt` 没有被读到，计数寄存器也被当作无用存储删除了。
- 现在，层次引用的目标如果在当前模块体内（沿生成块的作用域向上就是当前的实例体），并且是线网或变量，就按普通读取处理。
- 反例测试：对子实例内部信号的引用（`u.q`）仍然报告为不支持。

## 2. 真实工程验证

### ao486_MiSTer（直接分析 `ao486.qpf`，加入语料库）

| | 修复前 | 修复后 |
|---|---|---|
| 错误 | 11（未知模块 1，多驱动 10），整个设计无法分析 | 0 |
| 实例 | 0 | 661 |
| 寄存器 | — | 4487（4437 个已解析时钟） |
| 锁存器 | — | 0 |
| 未分析的构造 | — | 0（只修复 1.1、1.2 时为 2） |
| 时钟 | — | 13 |
| 跨时钟路径 | — | 605（80 已同步，525 未同步） |
| 被时序例外覆盖的未同步路径 | — | 122 |

- 修复 1.3 后，寄存器多了 10 个，跨时钟路径从 601 增加到 605。新增的 4 条都是真实的：
  - 路径：核心时钟域的 `hps_io.status` 经 `USER_IN` → 音频时钟域的 `mt32pi.clk_rate[i].clkr.clk_sr`、`clk`；
  - 以前这些寄存器被当作无用存储删除，所以这些路径没有出现；
  - 它们是一个去抖滤波器，不是可识别的同步器，所以报告为未同步。
- 剩下的 50 个未解析时钟的寄存器都在未使用的 HPS 接口中（`CLK-UNUSED-INTERFACE`，见阶段 71）。
- 整个分析约 10 秒（Release 版本）。

### 其他语料

- 修改前后，其余 26 个 SV 项目的 CDC 结果完全相同（逐行比较）。
- VHDL 语料 48 个全部匹配。

## 3. 修改的文件

- `src/frontend/slang_adapter/SlangFrontend.cpp`：`translate_off` 的注释处理。
- `src/ir/BodyEditor.{h,cpp}`：`dropOverriddenZ`。
- `src/frontend/slang_adapter/IrBuilder.cpp`：同一模块内的层次引用。
- 测试：`tests/ir_extract/test_ir_extract.cpp`，3 个用例（各含正反例）。
- `tools/ci/sv-corpus.sh`：ao486_MiSTer。
- `CMakeLists.txt`：版本 0.72.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：614 个测试全部通过。
- 新测试第一次运行时，3 个用例中有 5 处检查失败，都是我对 IR 文本格式的猜测不对：
  - `has()` 要求整行相同；
  - 多位 `Z` 赋值输出为 `assign {o[2], o[0]} = 2'bzz`。
  - 被测试的行为本身是正确的。按实际输出改正期望值后通过。
- SV 语料 27 个项目全部匹配（新增 ao486_MiSTer），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions，提交 8292ec3，运行 42485539613）：
  - Release：614/614 通过，生成 `OpenCDC-0.72.0-windows-x64.zip` 与 `.exe`；
  - Debug：614/614 通过。

## 5. 已知限制

- 层次引用只支持读取，并且只支持同一模块内的目标。对子实例内部信号的引用、对层次引用的赋值仍然不支持。
- 去掉 `Z` 位只针对常量 `Z`。`assign y = oe ? d : 'z` 这类三态缓冲不变（按阶段 64 的三态焊盘处理）。

## 6. 下一步

阶段 73：继续用真实工程验证，换一种类型。候选：一个使用 UltraScale+ 原语的较大 Vivado 工程，或一个以 VHDL 为主的 Quartus 工程。

# 阶段 70：SNES 的约束与单端口存储器 —— 花括号内的反斜杠换行、索引后的星号、altsyncram 的单端口和 ROM 模式

日期：2026-11-08　版本：0.70.0

阶段 69 分析 SNES_MiSTer 时发现三个问题。它们不改变跨时钟路径，但让设计者写的约束部分失效，并留下错误的告警：
- `SNES.sdc` 中 22 个 `CON-UNMATCHED: nothing matches '\'`；
- 3 个寄存器名称模式没有匹配；
- 10 个单端口存储器寄存器的时钟未解析。

这一阶段修正了这三个问题。

## 1. 完成的功能

### 1.1 花括号内的反斜杠换行

```tcl
set_max_delay 20 -from [get_registers { emu|hps_io|* \
                                        emu|main|* \
                                        emu|bsram|* }] \
                 -to   [get_registers { emu|sdram|addr[*][*] ... }]
```

- Tcl 规则（Tcl(n) 第 9 条）：反斜杠换行连同其后的空格和制表符替换为一个空格。这个替换在命令解析之前进行，所以在花括号内也成立，这是花括号内唯一的替换。
- 以前花括号、方括号和引号内保留了原样的 `\` 和换行，`\` 被当作一个名称模式查找，每行一个 `CON-UNMATCHED`。
- 现在三种情况都替换为一个空格，并继续计算行号。
- 花括号内其他的反斜杠转义保持原样（反例测试：`{data\[0\]}` 仍按字面查找 `data\[0\]`）。

### 1.2 索引后的星号

```tcl
get_registers { emu|sdram|st_num[*]*  emu|sdram|din[*][*]*  emu|sdram|SDRAM_DQ[*]* }
```

- Quartus 的寄存器名称是逐位的（`st_num[3]`），有时带后缀（`~DUPLICATE`），所以设计者在索引后加 `*`。
- 以前按位匹配只处理以 `]` 结尾的模式，这三个模式都没有匹配。
- 现在先去掉索引后的 `*`，再按位匹配。OpenCDC 的位名称没有后缀，所以这是等价的。
- 反例测试：不存在的位（`a_reg[2]*`）仍报告 `CON-UNMATCHED`。

### 1.3 altsyncram 的单端口和 ROM 模式

```vhdl
altsyncram_component : altsyncram generic map (operation_mode => "SINGLE_PORT", ...)
  port map (clock0 => clock, address_a => address, data_a => data, wren_a => wren, q_a => q);   -- SNES spram_sz
```

- `operation_mode` 为 `SINGLE_PORT` 或 `ROM` 时，存储器没有端口 B，`clock1` 不连接。
- 以前的模型仍在 `clock1` 上建模端口 B 的写数组和输出寄存器。每个实例因此多出 2 个寄存器，时钟未解析（`CLK-UNRESOLVED`）。
- 现在这两种模式只有端口 A，`q_b` 为常量 0。`altsyncram` 和 `altera_syncram` 都已修改。
- 双端口模式不变（反例测试：`clock1` 不连接的双端口存储器仍报告时钟未解析）。
- 双端口时端口 B 的寄存器名称多了一级生成块，例如 `dual.qb` 变为 `dual.g_dual.qb`。两个旧测试的路径名称已相应更新。
- `docs/primitives.md` 已更新。

## 2. 真实工程验证

| 工程 | 寄存器（修复前 → 后） | 未解析时钟的寄存器 | 跨时钟路径 | 告警 |
|---|---|---|---|---|
| SNES_MiSTer | 3506 → 3496 | 60 → 50 | 617，不变 | 122 → 92 |
| NES_MiSTer | 3886 → 3880 | 56 → 50 | 556，不变 | |
| Genesis_MiSTer | 3373 → 3371 | 52 → 50 | 591，不变 | 77 → 76 |

- 三个核心剩下的 50 个时钟未解析的寄存器都在 HPS 原语内部（`f2sdram.cmd_port_clk_*`、`fpga2hps.clk`、`hps2fpga.clk`），与 Template_MiSTer 相同。
- SNES.sdc 第 15 行的 `set_max_delay` 现在完整匹配。它约束的是同一时钟域内的路径，所以被时序例外覆盖的未同步路径数不变（113）。
- `SNES.sdc` 剩下的未匹配项：在 Quartus 内部引脚上定义的两个生成时钟（`GSU_CACHE_CLK`、`CX4_MEM_CLK`），见第 5 节。

## 3. 修改的文件

- `src/constraints/Sdc.cpp`：
  - `Tokenizer::continuation`，用于花括号、方括号和引号内；
  - `splitIndex`：去掉索引后的 `*`。
- `src/frontend/primitives/intel.v`、`docs/primitives.md`：`altsyncram`/`altera_syncram` 的单端口和 ROM 模式。
- 测试：
  - `tests/constraints/test_sdc.cpp`：1 个新用例（反斜杠换行，正反例）；在 Quartus 集合用例中加入索引后的星号（正反例）；
  - `tests/frontend/test_primitives.cpp`：1 个新用例（单端口、ROM，以及双端口反例），并更新 2 个路径名称。
- `tools/ci/sv-corpus.sh`：NES、Genesis、SNES 的期望值。
- `CMakeLists.txt`：版本 0.70.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：610 个测试全部通过。
- 我写的测试中有两处错误，已改正：
  - 引号内的 `\[` 在 Tcl 中替换为 `[`，期望值写错了；
  - 用 sed 修改一个行号期望值时，误改了另一个测试中相同的一行，已恢复。
- SV 语料 26 个项目全部匹配（期望值按第 2 节更新），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 69 的 Windows CI（提交 d3ed126，运行 42476044428）：Release 与 Debug 均 608/608 通过。
- 本阶段的 Windows CI：见提交后的记录。

## 5. 已知限制

- 在 Quartus 内部引脚上定义的生成时钟（`emu|main|GSUMap|GSU|CACHE|altsyncram_component|auto_generated|*|clk0`）不能匹配：OpenCDC 的 altsyncram 模型没有 `auto_generated` 这一层。这两个时钟只用于存储器内部的反相时钟，不影响跨时钟路径的判断。
- HPS 原语内部的时钟仍未连接（每个 MiSTer 核心 50 个寄存器）。

## 6. 下一步

阶段 71：
- 为 HPS 原语内部的接口时钟建模（`f2sdram`、`fpga2hps`、`hps2fpga` 的时钟来自 FPGA 侧的端口），使 MiSTer 核心的所有寄存器都有时钟；
- 继续用真实工程验证。

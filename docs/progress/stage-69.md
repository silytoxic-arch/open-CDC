# 阶段 69：SNES_MiSTer —— if/else if 链的条件覆盖、多余的 when others、端口映射形参中的类型转换、LPM 运算与 scfifo

日期：2026-11-08　版本：0.69.0

阶段 68 的下一步有两项：
- 把条件覆盖判断用于 Verilog 的 `if ... else if` 链和 `unique if`；
- 用另一个 MiSTer 核心验证。

这一阶段选择 **SNES_MiSTer**（超级任天堂核心，GPL-3.0）。它包括：
- 65C816 处理器、PPU、DSP，用 VHDL 编写；
- 卡带上的协处理器：CX4、SA1、GSU、SPC7110、DSPn、SDD1；
- Intel 的 LPM 乘法器、除法器和 `scfifo`。

第一次分析有 6 个错误，整个设计无法分析。修复后是 0 个错误、0 个锁存器、0 个未分析的构造，并加入了语料库。

## 1. 完成的功能

### 1.1 Verilog：if / else if 链的条件覆盖，unique if

```systemverilog
always_comb if (s == 2'd0) y = a; else if (s == 2'd1) y = b; else if (s[1]) y = c;   // 没有最后的 else
```

- **条件覆盖**：链中各个条件的或对所有两态取值都为真时，最后一个 `if` 的条件总会成立，所以执行它的分支时不再加条件。综合时不产生锁存器。
  - 判断使用阶段 68 的 `BodyEditor::alwaysTrue`，与 VHDL 的条件赋值相同。
- **`unique if` / `priority if`**：没有 `else` 时，链中总有一个条件成立（不成立是运行时的违规）。综合工具按完整的链处理，现在也这样处理。
- **实现**：降级时记录同一链中前面的条件（`elseChain_`）：
  - 进入 `else if` 时加入当前条件；
  - 进入分支体或普通 `else` 时清空，所以嵌套的 `if` 不会误用外层的条件。
- **反例测试**（仍推断锁存器）：
  - 缺少一种取值的链；
  - `unique0 if`；
  - 链中间嵌套的不完整 `if`。

### 1.2 VHDL：覆盖所有两态取值后的 when others

```vhdl
case OBJ_X_OFS(2 downto 0) is
  when "100" => obj_color := ...;  ...  when "011" => obj_color := ...;   -- 全部 8 个编码
  when others => null;
end case;
```

- 前面的选择已经覆盖选择器的全部两态取值时，`others` 只代表 `'X'`、`'Z'` 等非二值的取值，硬件中不存在。
- 现在最后一个显式选择承担其余情况，`others` 分支不会执行。
- 效果：
  - 组合进程中不再推断锁存器（SNES 65C816 ALU 的 `case CTRL.secOp`）；
  - 时钟进程中，每次都先赋值再读的变量不再是寄存器。Genesis_MiSTer 的 `vdp.vhd` 中 4 个这样的变量（`V_BGA_BASE`、`V_BGB_BASE`、`obj_color`、`cold`）以前被错误地当作寄存器：
    - 寄存器数从 3377 降为 3373；
    - 跨时钟路径的结果不变（这些变量的值总是在同一个周期里写入并使用）。
- 反例测试：缺少一个编码时，`others => null` 仍然保持原值，推断锁存器。

### 1.3 端口映射形参中的类型转换

```vhdl
port map (..., signed(q_a) => BRR_BUF_DO);   -- SNES DSP.vhd
```

- IEEE 1076 允许在形参一侧使用类型转换，把输出端口的值转换为实参的类型。
- 以前报告为不支持的端口关联，BRR 缓冲区的输出因此没有连接。
- 现在：形参是“数组类型名（端口名）”时，按该端口直接连接。
- 真正的转换函数（例如 `inv(q) => s`）仍然报告 `IR-UNSUPPORTED-PORT`（反例测试）。

### 1.4 长度不同的逻辑运算：结果按较长的操作数

```vhdl
CX4_A <= ("0" & PRG_ADDR) and ROM_MASK(22 downto 0);   -- CX4Map.vhd：22 位与 23 位，赋给 23 位端口
```

- 阶段 68 按 IEEE 包体把结果的长度取为左操作数的长度。
- SNES 中左操作数较短，赋值因长度不符而报错；Quartus 编译这个核心，结果的长度是较长的操作数。
- 现在结果取较长操作数的长度和类型，值仍然未知，仍报告警告 `VHDL-LENGTH-LOGIC`。
- T80 的情况（左操作数较长）结果不变。

### 1.5 Intel LPM 运算和 scfifo

以前是黑盒，现在加入 Intel 原语库：

| 原语 | 模型 |
|---|---|
| `lpm_mult`、`lpm_divide` | 组合的乘法器 / 除法器；`lpm_pipeline` > 0 时结果经过相应级数的寄存器，在 `clock` 上，`clken` 为 `0` 时保持 |
| `scfifo` | 单时钟 FIFO：在 `clock` 写入；`lpm_showahead` 为 `"ON"` 时 `q` 组合地从存储器读出，否则在 `clock` 寄存 |

- VHDL 的元件声明：
  - 新的 `lpm.lpm_components` 包：`lpm_mult`、`lpm_divide`；
  - `altera_mf_components` 加入 `scfifo`。
- `docs/primitives.md` 已更新。

## 2. 真实工程验证

### SNES_MiSTer（直接分析 `SNES.qpf`，加入语料库）

| | 修复前 | 修复后 |
|---|---|---|
| 错误 | 6（lpm 库不存在、CX4Map 长度不符等，整个设计无法分析） | 0 |
| 实例 | 0 | 367 |
| 寄存器 | — | 3506（3446 个已解析时钟） |
| 锁存器 | — | 0 |
| 未分析的构造 | — | 0 |
| 时钟 | — | 11 |
| 跨时钟路径 | — | 617（65 已同步，552 未同步） |
| 被时序例外覆盖的未同步路径 | — | 113 |

- 违例分布与 NES、Genesis 类似：主要是核心 → HDMI、两个 50 MHz 振荡器之间、核心 → 音频。
- 语料库只稀疏检出 `rtl/` 和 `sys/`，`build_id.v` 由脚本写入。

### Genesis_MiSTer

见 1.2：4 个错误的寄存器被去掉，其余结果不变。语料库的期望值已更新。

## 3. 修改的文件

- `src/frontend/slang_adapter/ProcessLowering.cpp`、`IrBuilder.h`：if / else if 链的条件覆盖，`unique if`/`priority if`。
- `src/frontend/vhdl/VhdlStmt.cpp`：多余的 `when others`。
- `src/frontend/vhdl/VhdlElab.cpp`：形参中的类型转换。
- `src/frontend/vhdl/VhdlBuiltins.cpp`：`VHDL-LENGTH-LOGIC` 的结果长度。
- `src/frontend/primitives/intel.v`：`scfifo`、`lpm_mult`、`lpm_divide`。
- `src/frontend/primitives/lpm_components.vhd`（新）、`altera_mf_components.vhd`、`src/frontend/CMakeLists.txt`。
- `docs/primitives.md`。
- 测试：
  - `tests/ir_extract/test_ir_extract.cpp`：1 个用例（if / else if 链与 unique if，正反例）；
  - `tests/vhdl/test_vhdl_elab.cpp`：2 个用例（多余的 `when others`；形参中的类型转换与转换函数），并扩展 `VHDL-LENGTH-LOGIC` 用例（较长的右操作数）；
  - `tests/frontend/test_primitives.cpp`：1 个用例（LPM 和 `scfifo` 的跨时钟路径），嵌入模型列表。
- `tools/ci/sv-corpus.sh`：SNES_MiSTer；Genesis_MiSTer 的期望值。
- `CMakeLists.txt`：版本 0.69.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：608 个测试全部通过。
- SV 语料 26 个项目全部匹配（新增 SNES_MiSTer）。
  - Genesis_MiSTer 第一次不匹配：寄存器 3377 → 3373。
  - 没有直接改期望值，先查原因：临时用环境变量分别关闭本阶段的两项新判断，再比较 IR 中的寄存器列表。
  - 结论：差异完全来自 1.2，4 个寄存器都是每次先赋值再读的变量，去掉是正确的。临时代码已删除。
- VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）：见下方 CI 记录。

## 5. 已知限制

- **SDC 中花括号内的反斜杠换行**：SNES.sdc 的 `set_max_delay` 把一长串名称写在 `{ ... \` 换行中。
  - Tcl 规定即使在花括号内，反斜杠换行也替换为一个空格；
  - OpenCDC 把 `\` 当作一个名称，出现很多 `CON-UNMATCHED: nothing matches '\'`，这些 `set_max_delay` 的部分名称没有生效。
- **生成的时钟**：SNES.sdc 在 Quartus 内部的引脚名（`...|auto_generated|*|clk0`）上定义 `GSU_CACHE_CLK`、`CX4_MEM_CLK`，OpenCDC 的模型中没有这些引脚，所以没有匹配。
- **单端口 altsyncram**：`spram_sz` 等单端口用法中，端口 B 的 `clock1` 没有连接，每个实例有 2 个寄存器的时钟未解析（共 10 个）。
- 条件覆盖的叶子信号位仍限于 12 位。

## 6. 下一步

阶段 70：
- 修正 SDC 解析中花括号内的反斜杠换行（SNES.sdc）；
- 检查单端口 altsyncram 的端口 B 是否应该被建模；
- 继续用真实工程验证。

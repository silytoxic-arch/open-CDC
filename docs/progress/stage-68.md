# 阶段 68：Genesis_MiSTer —— 长度不同的逻辑运算、并发过程调用、任务、unique case、条件覆盖、altdpram

日期：2026-11-08　版本：0.68.0

阶段 67 的下一步是用更多真实 Quartus 工程验证。这一阶段选择 **Genesis_MiSTer**（世嘉 MD 核心，GPL-3.0）。它比 NES 大，包括：
- 68000 处理器：FX68K，SystemVerilog；
- Z80：T80，VHDL；
- VDP：VHDL；
- 声音芯片：jt12、jt89；
- Intel 的 MLAB 存储器。

第一次分析就失败了：一个 VHDL 错误使整个设计无法分析（0 个实例）。修复后又处理了另外 5 类问题。最终 Genesis_MiSTer 是 0 个错误、0 个锁存器、0 个未分析的构造，并加入了语料库。

## 1. 完成的功能

### 1.1 长度不同的逻辑运算（使整个设计无法分析）

```vhdl
ioq := (ioq and x"7") xor ('0' & BusA);   -- T80.vhd：ioq 9 位，x"7" 4 位
```

- IEEE `std_logic_1164` 包体对长度不同的 `and`/`or`/`xor`… 报告断言失败（仿真时的错误），并返回左操作数长度、未初始化的结果。
- 综合工具忽略断言，所以 Quartus 能编译这个核心。以前 OpenCDC 把它当作错误（`VHDL-LENGTH`），整个设计无法分析。
- 现在报告警告 `VHDL-LENGTH-LOGIC`，结果按包体的语义处理：长度为左操作数，值未知。
- 以前出错时还返回了较长操作数的宽度，也已更正。
- 赋值长度不符仍然是错误（反例测试）。

### 1.2 并发过程调用

```vhdl
AddSub(BusA(3 downto 0), BusB(3 downto 0), ALU_Op(1), ..., Q_v(3 downto 0), HalfCarry_v);   -- T80_ALU：Z80 的加法器
```

- 以前报告为不支持，Z80 的 ALU 加法器因此缺失。
- 按 IEEE 1076 第 11.4 节，并发过程调用等价于一个对它读取的信号敏感的进程（`process (all)`），进程体是同一个顺序过程调用。现在按此处理。
- 同时支持了过程的信号类 `out` 参数（`signal Res : out std_logic_vector`）。以前在任何过程中对它用 `<=` 赋值都会报错。
  - 现在过程内部按局部副本处理；
  - 过程结束时以信号赋值写回实参。

### 1.3 不含时序控制的任务

```systemverilog
always_comb mySubber(aOperand, dOperand, addCin, ..., subResult, subCout, subOv);   // FX68K 的 ALU
```

- 不含时序控制的任务是可综合的，现在内联到调用者的进程中：
  - 输入参数在调用前按调用者的状态求值；
  - 任务体在调用者状态的副本上执行，所以读到调用者当前的值，对任务外变量（模块变量）的赋值像在调用者中一样生效；
  - 结束后丢弃任务自己的局部变量和形参，把输出参数写回实参。
- 仍然不支持（反例测试）：
  - 含延迟、事件控制或 `wait` 的任务（测试平台代码）；
  - `ref` 参数。

### 1.4 unique case 和 priority case

```systemverilog
always_comb unique case (col) 1: ccrMask = ccrMask1; 2,3: ... endcase   // FX68K ccrTable
```

- `unique`/`priority` 声明总有一项匹配：不匹配是运行时的违规，综合工具按完整 case 处理，不推断锁存器。
- 现在最后一项承担其余情况。
- `unique0` 和普通 case 没有这个承诺，仍然推断锁存器（反例测试）。

### 1.5 覆盖所有两态取值的条件

```vhdl
OBJ_MAX_LINE <= ... when H40 = '1' and E = '0' else ... when H40 = '1' and E = '1' else
                ... when H40 = '0' and E = '0' else ... when H40 = '0' and E = '1';   -- vdp.vhd，没有最后的 else
```

- 四个条件覆盖了 `H40` 和 `E` 的全部两态取值，综合时“都不成立”的情况化简为假，不产生锁存器。以前推断出锁存器。
- 新的 `BodyEditor::alwaysTrue`：
  - 对条件逻辑的叶子信号位（最多 12 位）逐一取 0/1；
  - 用常量折叠求值整个条件；
  - 每种组合都为真时，条件恒成立。
- VHDL 的条件信号赋值（并发和顺序）在所有条件的或恒为真时，最后一个条件承担其余情况。
- 缺少一种组合时仍然推断锁存器（反例测试）。

### 1.6 Intel altdpram

MLAB/LUT 双端口存储器 `altdpram` 以前是黑盒，现在加入 Intel 原语库：
- 在 `inclock` 写入；
- 读地址按 `rdaddress_reg` 在 `inclock` 或 `outclock` 寄存，或不寄存；
- 输出按 `outdata_reg` 在 `outclock` 或 `inclock` 寄存，或异步读出。

`docs/primitives.md` 已更新。

## 2. 真实工程验证

### Genesis_MiSTer（直接分析 `Genesis.qpf`，加入语料库）

| | 修复前 | 修复后 |
|---|---|---|
| 错误 | 1（整个设计无法分析） | 0 |
| 实例 | 0 | 381 |
| 寄存器 | — | 3377（3325 个已解析时钟） |
| 锁存器 | — | 0 |
| 未分析的构造 | — | 0 |
| 时钟 | — | 10 |
| 跨时钟路径 | — | 591（52 已同步，539 未同步） |
| 被时序例外覆盖的未同步路径 | — | 109 |

- 前端时间约 1.1 秒。
- 违例分布与 NES 类似：主要是核心 → HDMI、两个 50 MHz 振荡器之间、核心 → 音频。
- 语料库只稀疏检出 `rtl/` 和 `sys/`，`build_id.v` 由脚本写入。

### 其他语料

结果见第 4 节。

## 3. 修改的文件

- `src/frontend/vhdl/VhdlBuiltins.cpp`：`VHDL-LENGTH-LOGIC`。
- `src/frontend/vhdl/VhdlElab.cpp`：并发过程调用。
- `src/frontend/vhdl/VhdlElab.h`、`VhdlNames.cpp`、`VhdlStmt.cpp`：信号类 `out` 形参；条件覆盖。
- `src/ir/BodyEditor.{h,cpp}`：`alwaysTrue`。
- `src/frontend/slang_adapter/ProcessLowering.cpp`、`IrBuilder.h`：任务内联（`execTaskCall`）；`unique`/`priority case`；`OrderedSymMap::eraseIf`。
- `src/frontend/primitives/intel.v`、`docs/primitives.md`：`altdpram`。
- 测试：
  - `tests/vhdl/test_vhdl_elab.cpp`：3 个用例；
  - `tests/ir_extract/test_ir_extract.cpp`：2 个用例，并更新 1 个旧测试；
  - `tests/frontend/test_primitives.cpp`：1 个用例，嵌入模型列表。
- `tools/ci/sv-corpus.sh`：Genesis_MiSTer。
- `CMakeLists.txt`：版本 0.68.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：604 个测试全部通过。
  - 阶段早期的测试“Task calls inside always blocks are reported”假设任务调用都不支持；
  - 它的设计（任务对模块变量 `q` 赋值）现在被正确建模为 `reg q <= d`，所以改为检查这一点，并保留含延迟的任务作为反例。
- SV 语料 25 个项目全部匹配（新增 Genesis_MiSTer），其他项目结果不变；VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）：第一次运行（提交 5244cc9，运行 42466164822）的 Release 任务在 Configure 步骤失败：从 GitHub 下载 nlohmann/json 的发布包失败（运行器的网络问题，与代码无关）。没有可用的 GitHub 令牌重新运行这个任务，于是让 CI 的 Configure 步骤在失败时重试（最多 3 次，间隔 20、40 秒），再推送触发新的运行（提交 9eac7aa，运行 42466342418）：
  - Release：604/604 通过，生成 `OpenCDC-0.68.0-windows-x64.zip` 与 `.exe`；
  - Debug：604/604 通过。这次 Debug 任务的第一次 Configure 又遇到同样的下载失败，第二次尝试成功：重试起了作用。

## 5. 已知限制

- 条件覆盖只用于 VHDL 的条件信号赋值，且叶子信号位不超过 12 位；Verilog 的 `if ... else if` 链没有这样判断。
- `unique if`/`priority if` 没有 `else` 时，仍按不完整处理。
- 任务的 `ref` 参数不支持。
- HPS 原语内部的时钟（`f2sdram.cmd_port_clk_*`、`fpga2hps.clk` 等）没有连接，52 个寄存器的时钟未解析，与 Template_MiSTer 相同。

## 6. 下一步

阶段 69：Verilog 的 `if ... else if` 链（以及 `unique if`）使用同样的条件覆盖判断；继续用另一个 MiSTer 核心（例如 SNES_MiSTer）验证。

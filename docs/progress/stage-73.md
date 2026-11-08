# 阶段 73：C64_MiSTer —— 推断的真双端口 RAM、wire 函数端口、mem[i][j] 写入、受约束数组类型的 &、case 的覆盖判断

日期：2026-11-08　版本：0.73.0

阶段 72 的下一步是换一种类型的真实工程。这一阶段选择 **C64_MiSTer**（Commodore 64 核心）：
- VHDL 部分：fpga64 主体、VIC-II 视频芯片；
- SystemVerilog 部分：SID 声音芯片、OPL3、软驱（1541/1581）。

第一次分析有 14 个语法错误，整个设计无法分析（0 个实例）。逐层修复后又暴露出 4 类问题：多驱动、未分析的赋值目标、VHDL 长度错误，以及 1 个错误推断的锁存器。最终是 0 个错误、0 个锁存器、0 个未分析的构造，并加入了语料库。

## 1. 完成的功能

### 1.1 声明为 `wire` 的函数端口（14 个语法错误）

```systemverilog
function signed [15:0] tanh_y_mirror(wire x_neg, wire signed [15:0] y);   // sid_tables.sv
```

- SystemVerilog 不允许函数端口是线网，Quartus 接受，读作该类型的输入。
- 在已有的 Quartus 兼容修正中加入一种：slang 在端口列表位置的 `wire` 关键字上报告 `ExpectedFunctionPort`（第一个端口）或 `WireDataType`（后面的端口）。
- 修正方式：把关键字替换为等长的空格，端口变为隐式类型的输入。报告告警 `FE-WIRE-FUNCTION-PORT`。
- 只在 `wire` 紧跟 `(` 或 `,` 时修正。反例测试：其他位置把 `wire` 当作类型（`logic [wire] x`）仍然是错误。

### 1.2 两个 always 块写同一个数组：推断的真双端口 RAM

```systemverilog
always @(posedge clock_a) if (wren_a_d) ram[address_a_d] <= data_a; ...   // iecdrv_mem
always @(posedge clock_b) if (wren_b_d) ram[address_b_d] <= data_b; ...
```

- 这是 Intel 和 Xilinx 模板推断真双端口 RAM 的写法，以前报告多驱动错误（`IR-MULTIDRIVER`）。
- 现在使用与内置厂商存储器模型（altsyncram、XPM）相同的建模：
  - 每个写端口写自己的数组副本（`ram`、`ram#2` ……），寄存器在各自的时钟上；
  - 每次读取都读到所有副本（异或组合），所以读端口依赖于每个写端口，跨时钟路径（`ram#2` → `qa`、`ram` → `qb`）会被报告。
- 判断：模块中被多个时钟 always 块赋值的非压缩数组变量（`findSharedMemories`）。
- 反例测试：只在一个块中写、在另一个块中读的数组仍是一个数组。

### 1.3 两级下标都可变的写入 `mem[i][j]`

```systemverilog
ram[addr1 / RATIO][addr1 % RATIO] = data_in1;   // opl3fm.sv
```

- 以前只支持元素内的常量部分，可变的第二级下标报告为不支持的赋值目标。
- 现在按读-改-写处理：读出元素 `i`，在可变位置 `j` 动态插入新值，再写回元素 `i`。
- 反例测试：元素内可变的部分选择（`mem[i][j*4 +: 4]`）仍报告为不支持。

### 1.4 受约束数组类型声明的基类型

```vhdl
type charStoreDef is array(38 downto 0) of unsigned(11 downto 0);
charStore <= charStore(37 downto 0) & nextChar;   -- video_vicII_656x.vhd
```

- IEEE 1076（5.3.2.1）规定：受约束数组类型声明定义一个匿名的**非受约束**基类型，声明的名字是它的子类型。预定义的 `&` 作用于基类型。
- 以前 OpenCDC 把 `charStoreDef` 本身当作受约束的基类型，`&` 的参数是固定长度 39，报告 2 个长度错误。
- 现在按标准声明为非受约束基类型加受约束子类型。改动影响所有受约束数组类型，所以 VHDL 测试和 48 个 VHDL 语料都已重新检查，全部匹配。
- 反例测试：结果长度不符的赋值（`s <= s & d`）仍是错误。

### 1.5 case：计算出的选择器的覆盖，常量选择器

```systemverilog
case (OP[3] ? EXOP[2:0] : OP[2:0])  0,5: ...; 1: ...; 2,3: ...; 4,7: ...; 6: ...;  endcase   // NextZ80ALU：全部 8 个值
```

- **覆盖判断**：
  - 以前推断出锁存器（`csin`）。原因：选择器与 32 位整数项比较，三目运算在 32 位上下文中成为 32 位多路选择器，高位不是常量 0，已有的完整性检查认不出 3 位的有效宽度。
  - 现在在已有检查之后加一个后备：所有项条件的或在全部叶子输入位上恒为真时（阶段 68 的 `alwaysTrue`，最多 12 位），case 是完整的。
  - 反例测试：缺少一个值时仍推断锁存器。
- **常量选择器**：
  - `case (BITS)`（sid_dac.sv，`BITS` 是参数）中，不会执行的分支以前也被降级，产生 3 个错误的“常量下标越界”告警（`IR-OUT-OF-RANGE`）。
  - 有两个原因，都已修正：
    - 常量与常量的比较没有折叠，现在直接折叠；
    - 已有的完整性检查会把最后一项的条件改为 1，即使它已经是常量 0；现在常量条件保持不变。
  - 以前最终的值是正确的（排在前面、条件恒真的项优先），只是多降级了一个分支。
  - slang 自己对这些不会执行的代码报告的 `FE-SLANG-IndexOOB` 告警仍然保留。

## 2. 真实工程验证

### C64_MiSTer（直接分析 `C64.qpf`，加入语料库）

| | 修复前 | 修复后 |
|---|---|---|
| 错误 | 14（语法），整个设计无法分析 | 0 |
| 实例 | 0 | 317 |
| 寄存器 | — | 4328（4278 个已解析时钟） |
| 锁存器 | — | 0（只修复 1.1–1.4 时为 1） |
| 未分析的构造 | — | 0 |
| 时钟 | — | 11 |
| 跨时钟路径 | — | 515（65 已同步，450 未同步） |
| 被时序例外覆盖的未同步路径 | — | 113 |

- 修复的顺序：语法错误（1.1）→ 多驱动（1.2）→ VHDL 长度和赋值目标（1.3、1.4）→ 锁存器和越界告警（1.5）。每一步都在前一步之后才暴露出来。
- 软驱的 ROM（`iecdrv_mem`）现在有两个写端口副本；在这个设计中两个端口的时钟（`clk_sys`、`clk`）之间没有报告跨时钟路径。
- 整个分析约 4 秒（Release 版本）。

### 其他语料

- 修改前后，其余 27 个 SV 项目的 CDC 结果完全相同（逐行比较）。
- VHDL 语料 48 个全部匹配。

## 3. 修改的文件

- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-WIRE-FUNCTION-PORT`（`afterListSeparator`）。
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`、`ProcessLowering.cpp`：
  - 共享存储器（`findSharedMemories`、`ownCopy`、`sharedRead`）；
  - `mem[i][j]` 写入（`LTarget::subDynamic`）；
  - case 的覆盖后备和常量比较折叠。
- `src/frontend/vhdl/VhdlScope.cpp`：受约束数组类型声明。
- 测试：
  - `tests/frontend/test_quartus_compat.cpp`：2 个用例（wire 函数端口、真双端口 RAM）；
  - `tests/ir_extract/test_ir_extract.cpp`：2 个用例（`mem[i][j]`、case 覆盖和常量选择器）；
  - `tests/vhdl/test_vhdl_elab.cpp`：1 个用例（受约束数组类型的 `&`）。
  - 每个用例都含反例。
- `tools/ci/sv-corpus.sh`：C64_MiSTer。
- `CMakeLists.txt`：版本 0.73.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：619 个测试全部通过。
- 新测试中我自己的两处错误，已改正：
  - 真双端口 RAM 用例的反例中，读出的寄存器没有连到输出，被当作无用存储删除，所以没有路径；改为输出端口。
  - IR 文本中组合赋值的格式是 `comb  w = ...`，不是 `assign`。
- 诊断过程中加过临时的调试输出和环境变量开关，确认原因后都已删除：
  - 确认锁存器的原因是 32 位多路选择器形式的选择器；
  - 确认常量 case 中多余分支的来源。
- SV 语料 28 个项目全部匹配（新增 C64_MiSTer），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 72 的 Windows CI（提交 8292ec3，运行 42485539613）：Release 与 Debug 均 614/614 通过。
- 本阶段的 Windows CI：见提交后的记录。

## 5. 已知限制

- **共享存储器**：
  - 只识别被时钟 always 块写入的非压缩数组；被组合逻辑写入的数组不按 RAM 处理。
  - 读取把所有副本都作为依赖，不区分哪个端口的写入实际可见（与内置厂商模型相同）。
- **`mem[i][j]` 写入**：只支持第二级是单个元素的可变下标；可变的部分选择仍不支持。
- **case 覆盖后备**：叶子输入位不超过 12 位。

## 6. 下一步

阶段 74：继续用真实工程验证。候选：一个使用 UltraScale+ 原语的较大 Vivado 工程，或另一个以 VHDL 为主的 Quartus 工程（例如 Minimig 或 Amiga 类核心）。

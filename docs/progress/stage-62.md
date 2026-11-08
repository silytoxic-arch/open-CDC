# 阶段 62：没有事件控制的 always；赋值模式；非压缩结构体；常量表；完整 case 的判断

日期：2026-11-08　版本：0.62.0

阶段 61 之后，NES_MiSTer 还有 124 处没有分析的构造（`IR-UNSUPPORTED-*`）。这一阶段按出现次数处理最常见的三类，并修复了过程中发现的一个错误：完整的 `case` 被当成不完整，推断出假锁存器。

| 构造 | NES_MiSTer 中的数量 | 处理 |
|---|---|---|
| 赋值模式 `'{...}` | 43 | 支持 |
| 没有事件控制的 `always begin ... end` | 33 | 按组合逻辑读取 |
| 非压缩结构体成员作为赋值目标 | 29 | 支持 |
| 非压缩数组常量（`localparam` 表） | 3（以前被前两类掩盖） | 支持 |

## 1. 完成的功能

### 1.1 没有事件控制的 always

```verilog
always begin
  casez (mode[1:0]) ... vram_a10 = ...; endcase
end
```

仿真中这是一个死循环；Quartus 和 Vivado 综合时都把它当作组合逻辑（等同 `always @*`）。MiSTer 的 mapper 代码大量使用这种写法。

- 块中没有任何等待时，按组合逻辑处理，报告 `IR-ALWAYS-NO-EVENT` 警告，建议改为 `always @*` 或 `always_comb`。
- 块中有等待时仍然不分析（`IR-UNSUPPORTED-PROCESS`），因为这是测试平台代码：延迟 `#`、`wait`、事件触发、`forever`、`disable`、任务调用（任务可能等待）、赋值内延迟 `a = #2 b`。

### 1.2 赋值模式

`'{a, b, c}`、`'{default: x}`、`'{index: x}`、`'{n{a, b}}`：

- 非压缩数组以展开的向量存储，左边的元素在高位。模式的第一个元素对应左边的下标，所以模式就是这些元素的拼接，升序（`[0:3]`）和降序（`[3:0]`）都一样；
- 压缩数组和压缩结构体也是同样的顺序；
- 结构化模式和复制模式由 slang 展开为元素列表；
- 典型用法：常量表（`wire [15:0] init_data[12] = '{...}`）、单元素数组端口（`.sd_lba('{sd_lba})`）。

### 1.3 非压缩结构体

```systemverilog
typedef struct { logic signed [20:0] y; ... logic burst; } phase_t;
phase_t phase[5];
phase[0].y <= yr + yg + yb;
```

- 字段都是整数类型（或这类类型的数组和结构体）的非压缩结构体，以及这种结构体的数组，现在可以声明为信号；
- 存储布局：第一个字段在第 0 位，与 slang 的字段偏移一致，所以成员读写直接使用该偏移；
- 结构体的赋值模式按字段顺序从低位开始放置；
- 成员可以读，也可以作为赋值目标（包括数组元素的成员 `phase[0].y`）。

### 1.4 非压缩数组和结构体的常量

`localparam logic [7:0] chroma_SIN_LUT[64] = '{...}`：引用这种参数时，把常量值转换为上面的存储布局（数组左边的元素在高位，结构体第一个字段在低位），按下标查表时作为常量处理。

### 1.5 完整 case 的判断（错误修复）

```verilog
always @* case (s) 0: y = a; 1: y = b; 2: y = c; 3: y = d; endcase   // s 是 2 位
```

这个 case 列出了 `s` 的全部 4 个值，是完整的，不应推断锁存器。以前却报告了锁存器。

- **原因**：标签是 32 位整数，SystemVerilog 把选择表达式扩展到 32 位后再比较，所以检查按 32 位处理，而 4 个值远远覆盖不了 2^32 个值。用 `casez` 通配写法时没有这个问题。
- **修复**：零扩展的高位是常量 0，只有低位会变化，所以按低位判断是否覆盖了全部值；某个标签在这些高位上有 1，就永远不会匹配，不计入覆盖。
- **有符号选择**：`case ($signed(s)) -2: ... 1:` 的高位是符号位的复制，同样只按低位判断；高位与符号位不一致的标签永远不会匹配。
- **结果**：NES_MiSTer 的锁存器从 23 个减少到 7 个，消除的 16 个都是这种假锁存器（mapper 的 bank 选择等）。

## 2. 真实工程验证

### NES_MiSTer

| | 阶段 61 | 阶段 62 |
|---|---|---|
| 未分析的构造 | 124 | 10 |
| 寄存器 | 3720 | 3754 |
| 锁存器 | 18 | 7 |
| 警告 | 334 | 245 |
| 跨时钟路径 | 461 | 523 |
| 已同步 / 未同步 | 34 / 427 | 39 / 484 |
| 同步器结构 | 77 | 144 |

- 新增的跨时钟路径来自以前没有分析的逻辑：核心时钟到 HDMI 时钟（视频输出）、核心时钟与音频时钟之间（音频），以及两个 50 MHz 振荡器之间；
- 剩下的未分析构造（统计 10 处，9 条警告）：6 个条件在展开后仍不是常量的 `for` 循环（Intel PLL 重配置核心 `altera_pll_reconfig_core.v`、`pll_cfg_hdmi.v`），3 个在时钟进程条件之外使用的 `rising_edge`（`ascal.vhd`，也是那 3 个 VHDL 锁存器）。

### Template_MiSTer

- 寄存器从 1513 增加到 1534（`yc_out.sv` 的非压缩结构体数组），已解析 1461 个；
- 跨时钟路径不变（414 条）。

## 3. 修改的文件

- `src/frontend/slang_adapter/ProcessLowering.cpp`：没有事件控制的 always（含等待检测）；完整 case 按选择表达式实际变化的位判断。
- `src/frontend/slang_adapter/IrBuilder.cpp`：`flatUnpacked`/`flatConstant`；赋值模式；非压缩结构体的声明、成员读写；常量表。
- `tests/ir_extract/test_ir_extract.cpp`：5 个测试用例（正例和反例）。
- `tools/ci/sv-corpus.sh`：Template_MiSTer 和 NES_MiSTer 的新结果。
- `README.md`：Quartus 一行。
- `CMakeLists.txt`：版本 0.62.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：587 个测试全部通过（新增 5 个测试用例）。
- SV 语料 24 个项目全部匹配（Template_MiSTer、NES_MiSTer 的期望值已更新为新结果），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- GitHub Actions Windows x64 / VS2022（提交 40326ac，运行 42454176282）：
  - Release：587/587 通过，生成 `OpenCDC-0.62.0-windows-x64.zip` 与 `.exe`；
  - Debug：587/587 通过。

## 5. 已知限制

- 字段中有实数、字符串、动态数组的非压缩结构体仍然不支持（`IR-UNSUPPORTED-TYPE`）。
- 赋值模式作为赋值目标（`'{a, b} = x`）不支持。
- 完整 case 的判断只适用于不超过 12 位的选择表达式，以及常量标签。
- `ascal.vhd` 在时钟进程条件之外使用 `rising_edge`（3 处），被读成锁存器。

## 6. 下一步

阶段 63：`ascal.vhd` 的 `rising_edge` 用法和 `for` 循环条件（NES_MiSTer 和 Template_MiSTer 都有），以及分析 NES_MiSTer 的结果，检查新增的跨时钟路径是否都正确。

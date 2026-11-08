# 阶段 63：带异步复位的条件赋值寄存器；带标志的 for 循环；过程块中的静态变量

日期：2026-11-08　版本：0.63.0

阶段 62 之后，Template_MiSTer 和 NES_MiSTer 各剩下一些未分析的构造。这一阶段处理完后，两个工程都是 0 个未分析的构造。处理其中一处时发现一个更重要的错误：过程块中带初始值的静态变量每次执行都被重新赋初值，计数器被当成常量，寄存器丢失，而且没有任何提示。

## 1. 完成的功能

### 1.1 带异步复位的条件赋值（VHDL）

```vhdl
i_reset_na <= '0' WHEN reset_na = '0' ELSE '1' WHEN rising_edge(i_clk);   -- MiSTer ascal.vhd
```

等价于：

```vhdl
process (i_clk, reset_na) begin
  if reset_na = '0' then i_reset_na <= '0';
  elsif rising_edge(i_clk) then i_reset_na <= '1'; end if;
end process;
```

以前只支持 `x <= v when rising_edge(clk)` 这一种形式，这种写法被报告为 `rising_edge` 不在时钟进程条件中，并推断出锁存器。

- 现在条件赋值的**最后一个条件**是时钟沿时，按等价的 `if ... elsif rising_edge(clk)` 进程处理，前面的条件可以有多个（复位、置位）；
- 可以带 `else unaffected`；
- 时钟沿不是最后一个条件时仍然不支持（反例测试）。

ascal 的这三个复位信号是单级触发器：复位释放没有同步，现在报告为异步复位路径。

### 1.2 带标志的 for 循环

```verilog
for (i = 0; (i < number_of_counters) & !break_loop; i = i + 1'b1)   // Intel altera_pll_reconfig_core.v
  if (c_cnt_changed[i] & !c_cnt_done_q[i]) begin ... break_loop = 1'b1; end
```

这是用变量写的 `break`。条件在展开后不是常量，以前报告为不支持。

- 把条件拆开：常量部分（来自循环变量，`i < N`）决定展开多少次，其余部分作为每次迭代的条件；
- 条件逐次累积：一次迭代的条件为假后，之后的迭代都不执行，与 `break` 一致；
- 循环变量在每次迭代中照常递增，保持常量；
- 没有常量上界的循环（`for (i = 0; !found; ...)`）仍然不支持（反例测试）。

结果是正确的优先级逻辑：例如在测试中，编号最小的请求优先。

### 1.3 过程块中的静态变量（错误修复）

```systemverilog
always @(posedge clk, posedge reset) begin
  reg [1:0] dly1 = 0;      // MiSTer audio_out.sv
  if (reset) dly1 <= 0;
  else if (~&dly1) dly1 <= dly1 + 1'd1;
  ...
```

在 Verilog 中，这样声明的变量是静态的，初始值只在时刻 0 赋一次，是上电值；Quartus 和 Vivado 也是这样综合的。

- **以前的错误**：每次执行块时都赋一次初始值。计数器因此总是从 0 开始，看起来是常量，相关寄存器和跨时钟路径丢失，没有任何提示。在这个例子中，块开头的声明还使异步复位无法识别（`IR-AMBIGUOUS-CLOCK`）。
- **修复**：
  - 静态变量的初始值作为上电值（与模块级变量相同），不在每次执行时赋值；
  - 识别异步复位时跳过这些声明；
  - `automatic` 变量照旧在每次执行时初始化（反例测试）。
- **诊断**：SystemVerilog 要求这种声明写明 `static`。以前 `.sv` 文件中每处都报告警告（Template_MiSTer 中 39 个），现在改为信息 `FE-IMPLICIT-STATIC`，说明按综合工具的方式读取；`.v` 文件照旧不报告。
- **影响**：MiSTer 框架中多个模块使用这种写法（`audio_out.sv`、`alsa.sv`、`gamma_corr.sv`、`hps_io.sv`、`mt32pi.sv`）。Template_MiSTer 增加了 111 个寄存器。

## 2. 真实工程验证

| | Template_MiSTer 阶段 62 | 阶段 63 | NES_MiSTer 阶段 62 | 阶段 63 |
|---|---|---|---|---|
| 未分析的构造 | 5 | **0** | 10 | **0** |
| 寄存器 | 1534 | 1648 | 3754 | 3886 |
| 锁存器 | 3 | 0 | 7 | 4 |
| 警告 | 112 | 66 | 245 | 182 |
| 跨时钟路径 | 414 | 485 | 523 | 603 |
| 已同步 / 未同步 | 34 / 380 | 53 / 432 | 39 / 484 | 58 / 545 |

抽查新增的跨时钟路径：
- 核心时钟的配置寄存器（`cfg`、`aflt_rate`）被音频时钟域的逻辑读取（`audio_out` 的计数器和分频器，以前因 1.3 的错误丢失）。结构上确实没有同步器；MiSTer 用 `set_false_path` 把它们标为准静态。
- 核心时钟域的复位 `areset` 直接驱动 `audio_out` 的异步复位，没有复位同步器。
- 这些路径都是真实的结构，以前因为逻辑被丢弃而看不到。

新出现的 `GR-COMB-LOOP` 警告来自 `rtl/lfsr.v`：用 `lcell` 原语做成的环形振荡器（随机数源），是真实的组合环路，读取它的逻辑现在被建模了。

NES_MiSTer 剩下的 4 个锁存器：
- `T65.vhd` 的 3 个（6502 处理器核心的组合进程）；
- mapper 162 的 `prg_bank`。

## 3. 修改的文件

- `src/frontend/vhdl/VhdlStmt.cpp`：最后一个条件是时钟沿的条件赋值。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：带标志的 for 循环；静态变量的初始值；识别异步复位时跳过静态声明。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-IMPLICIT-STATIC`。
- 测试：
  - `tests/vhdl/test_vhdl_elab.cpp`：2 个用例；
  - `tests/ir_extract/test_ir_extract.cpp`：3 个用例；
  - `tests/frontend/test_quartus_compat.cpp`、`tests/ir_extract/test_ir_extract.cpp` 中原有的一个异步复位测试：更新。
- `tools/ci/sv-corpus.sh`：两个 MiSTer 工程的新结果。
- `CMakeLists.txt`：版本 0.63.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：592 个测试全部通过（新增 5 个测试用例）。
  - 阶段 33 的一个反例测试原来假设“带初始值的声明每次执行”，与 1.3 的修复矛盾；
  - 现在该测试改为检查静态声明下的异步复位能被识别，反例改用 `automatic` 变量（每次执行，仍然无法识别）。
- SV 语料 24 个项目全部匹配（两个 MiSTer 工程的期望值已更新），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）结果见交付说明。

## 5. 已知限制

- 带标志的 for 循环提前结束时，循环变量在循环之后的值不准确（按完整展开计算）。在循环之后读取循环变量的设计很少见。
- 只有 `&`/`&&` 连接的条件可以拆开；用 `||` 等其他形式组合常量上界的循环仍然不支持。
- 4 个锁存器未逐个确认是否是设计本意。

## 6. 下一步

阶段 64：检查 NES_MiSTer 剩下的 4 个锁存器，并按时钟对和规则统计 NES_MiSTer 的违例，找出数量最多的几类，逐类判断是真实问题还是误报。

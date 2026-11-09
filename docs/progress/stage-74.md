# 阶段 74：Corundum（UltraScale+）—— 不会执行的常量分支、Verilog 黑盒的输出、大型设计的时间和内存

日期：2026-11-09　版本：0.74.0

阶段 73 的下一步是验证一个较大的、使用 UltraScale+ 器件的 Vivado 工程。这一阶段选择 **Corundum**（开源 100G 网卡，BSD-2-Clause-Views），目标板卡是 ZCU106（`xczu7ev`）。它和前面的 MiSTer 核心有几点不同：
- PCIe 和 10G 以太网；
- 大量异步 FIFO 和跨时钟同步；
- 约束是按实例展开的 Tcl 脚本（`foreach ... get_cells -hier -regexp`、`set_max_delay -datapath_only`）；
- Vivado IP（PCIe、GT 收发器）由 Tcl 脚本生成，源码中没有它们的 HDL。

工程用 Makefile 构建，没有 `.xpr`。OpenCDC 工程文件按 Makefile 中的 `SYN_FILES`（122 个源文件）和 `XDC_FILES`（1 个 XDC 和 9 个 Tcl 脚本）生成，使用 RTL 的默认参数。

第一次分析有 15 个错误，整个设计无法分析。依次处理后，又遇到了四个问题，其中三个是分析器本身的性能问题：
- 前端 59 秒；
- 内存超过 7 GB，进程被终止；
- 约束读取 10 分钟以上；
- 只有 428/5936 个寄存器有时钟。

最终：0 个错误，所有寄存器都有时钟，整个分析约 80 秒，峰值内存约 1 GB，并加入了语料库。

## 1. 完成的功能

### 1.1 不会执行的常量分支中的错误（15 个错误）

```verilog
if (OUT_TLP_SEG_COUNT > 1) begin
    seg_mem_wr_sel[cur_seg*SEG_SEL_WIDTH +: SEG_SEL_WIDTH] = seg;   // pcie_tlp_fifo_raw.v：计数为 1 时 SEG_SEL_WIDTH = $clog2(1) = 0
```

- 综合工具只展开常量条件成立的分支，Vivado 因此能编译。slang 只在不求值的表达式（`?:`）中跳过检查，过程 `if` 的不执行分支仍报告“宽度必须为正”。
- 现在收集设计中所有过程 `if (常量)` 的分支：
  - 记录在每个实例中都不执行的分支，也记录执行的分支；
  - 同一条语句在另一组参数下执行时，按执行处理。
- 只在不执行分支中的语义错误降为告警，并附说明。语法错误不受影响。
- 实现上的两个细节：
  - slang 把含错误的语句包在 `InvalidStatement` 中，范围可能为空；分支范围改由条件表达式和整条语句的范围推出。
  - 过程降级时展开 `InvalidStatement`，否则整个 `always` 块会被当作没有事件控制。
- 反例测试：同样的错误位于执行的分支中时，仍然是错误。

### 1.2 Verilog 黑盒的输出

- 使用 `unknownModules: blackbox` 后，生成的 IP 是黑盒，但它的时钟输出（PCIe `user_clk`、GT `rxusrclk2`/`txusrclk2`）没有驱动，4735 个寄存器的时钟未解析。
- 原因：Verilog 中未知模块的端口方向未知，以前黑盒实例的连接完全没有记录。
- 现在：
  - 记录黑盒实例的连接，方向为未知；
  - 连接到黑盒、而设计中没有其他驱动的信号，视为黑盒的输出：作为时钟时是黑盒自己的时钟，作为数据时没有时钟域。
  - 已有驱动的信号（黑盒的输入）不受影响。
- 结果：5936/5936 个寄存器有时钟。
- `docs/vivado-ip.md` 已更新。
- 反例测试：连接到黑盒的寄存器输出保留原来的驱动，没有多驱动。

### 1.3 时钟关系未知时也记录时序例外

- 黑盒时钟之间没有已知的关系。以前，覆盖这类路径的 `set_false_path`/`set_max_delay` 只用来把关系改为“异步”，没有记录为路径的时序例外，所以：
  - 不显示；
  - 不计入摘要；
  - 不能用于按时序例外豁免。
- 现在两种情况都记录。
- Corundum 的 10 条未同步路径（GT 的 PRBS 选择、极性等配置寄存器，设计者用 `set_max_delay -datapath_only` 声明）因此显示为被时序例外覆盖。

### 1.4 性能和内存

| 问题 | 原因 | 修正 | 结果 |
|---|---|---|---|
| 前端 59 秒 | `initial` 块逐个元素清零 2^13 项的 RAM，每次赋值复制整个初值，二次复杂度 | `initial` 块从上电值出发：声明的初值，或未知值 X。常量写入常量时原地修改（`SigSpec::overwriteConst`） | 25 秒 |
| 内存超过 7 GB，进程被终止 | 2^14 × 128 位 RAM 的读写逻辑是 2M 位宽的单元。等待依赖时，每个单元保存全部逐位输入（最多 4M 项） | 整体求值的宽单元：同一个宽驱动单元只保留一项，其他来源只保留最低位和最高位（锥体本来就按来源合并为位区间） | 峰值约 1 GB |
| 约束读取 10 分钟以上 | `get_cells -regexp` 每次对每个寄存器、每一层级、每一位做 `std::regex_match` | 从正则表达式中取出每个匹配都必须包含的字面串，先做子串检查 | 见下一行 |
| （同上） | 匹配 `*` 时每个寄存器都按位展开成对象，RAM 也包括在内；`perBit` 逐位查找，二次复杂度 | 经过多路选择器写入的数组也识别为存储器（Vivado 中是 RAM，不是逐位触发器）；`perBit` 一次遍历 | 3.4 秒 |

- **字面串预检查的规则**：
  - 遇到分组、字符类、方括号（包括转义的）、带量词的字符时，字面串中断；
  - 顶层有 `|` 时不使用预检查；
  - 只含数字的字面串不使用（可能落在位下标中）。
- **`initial` 块的语义变化**：只部分赋值的变量，以前整个初值被忽略（`IR-INITIAL-IGNORED`）；现在未赋值的位是 X，已赋值的位是常量。

## 2. 真实工程验证

### Corundum ZCU106（加入语料库）

| | 修复前 | 修复后 |
|---|---|---|
| 错误 | 15（零宽度选择），整个设计无法分析 | 0（15 个降为告警） |
| 实例 | 0 | 532 |
| 寄存器 | — | 5936（全部有时钟） |
| 锁存器 | — | 0 |
| 未分析的构造 | — | 3（3 个黑盒 IP 实例） |
| 时钟 | — | 9（3 个约束时钟、MMCM 输出、5 个黑盒时钟） |
| 跨时钟路径 | — | 219（209 已同步，10 未同步） |
| 被时序例外覆盖的未同步路径 | — | 10（全部） |
| 识别的同步结构 | — | 149：异步 FIFO 6、握手 8、限定数据 20、复位同步 8、单级 10、翻转 6、两级 91 |
| 时间 / 峰值内存 | 未完成（内存不足） | 约 80 秒 / 约 1 GB |

规则结果：
- **CDC-003（4 个错误）、CDC-002（6 个警告）**：都是收发器 DRP 配置寄存器的单级或无同步器路径，正是设计者用 `set_max_delay -datapath_only` 声明的那 10 条（准静态配置）。
- **CDC-008**：PFC 使能和请求等多位值逐位同步，需要设计者确认。
- **CDC-005**：同步后的信号重新汇合。

修复前后，其他 28 个 SV 项目的 CDC 结果逐行相同。

## 3. 修改的文件

- `src/frontend/slang_adapter/SlangFrontend.cpp`：`ConstantBranches`/`ConstantBranchCollector`，不执行分支中的错误降级。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：
  - `unwrapInvalid`；
  - `initial` 块的上电值起点和原地写入。
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：`inInitial_`；黑盒实例的连接。
- `src/graph/DesignView.cpp`：黑盒端口方向未知时推断输出。
- `src/cdc/SeqGraph.cpp`：宽单元输入去重。
- `src/cdc/CdcAnalyzer.cpp`：未知时钟关系时也记录时序例外。
- `src/constraints/Sdc.cpp`：
  - 正则表达式字面串预检查（`requiredLiteral`）；
  - 经过多路选择器的存储器识别；
  - `perBit` 一次遍历。
- `src/ir/SigSpec.{h,cpp}`：`overwriteConst`。
- `docs/vivado-ip.md`。
- 测试：
  - `tests/constraints/test_sdc.cpp`：2 个用例；
  - `tests/frontend/test_quartus_compat.cpp`：2 个用例；
  - `tests/ir_extract/test_ir_extract.cpp`：1 个用例；
  - `tests/ir/test_ir.cpp`：1 个用例；
  - 每个用例都含反例。
- `tools/ci/sv-corpus.sh`：Corundum，工程文件由脚本从 Makefile 生成。
- `CMakeLists.txt`：版本 0.74.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：625 个测试全部通过。
- SV 语料 29 个项目全部匹配（新增 Corundum），其余 28 个项目的 CDC 结果与修改前逐行相同；VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 新测试中我自己的错误，已改正：
  - 我以为 `get_cells "x_reg[*]"` 返回逐位对象，实际返回一个包含所有位的对象，改为统计位数；
  - 测试名中的逗号被 Catch 当作分隔符，改用通配符运行。
- 诊断过程中加过临时的调试输出，都已删除。
- 阶段 73 的 Windows CI（提交 0121e06，运行 42487281494）：Release 与 Debug 均 619/619 通过。
- 本阶段的 Windows CI：见提交后的记录。

## 5. 已知限制

- **CDC 路径分析仍需约 44 秒**：宽 RAM 单元的每一位都要查一次驱动。需要按块解析驱动才能进一步加快。
- **前端约 25 秒**：主要是设计自身的常量函数求值（LFSR 掩码等），与 Vivado 相同。
- **Vivado IP**：由 Tcl 生成的 IP 没有模型，只作为黑盒；黑盒时钟之间的关系未知，靠设计者的约束判断。
- **黑盒输出的推断**：被设计中其他逻辑驱动的黑盒输出（实际上会形成多驱动）无法区分。

## 6. 下一步

阶段 75：
- 按块解析宽单元的驱动，缩短大型设计的 CDC 路径分析时间；
- 继续用真实工程验证。

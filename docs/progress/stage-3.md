# 阶段 3 完成报告：时钟域分析

日期：2026-10-20　版本：0.3.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T3.1 设计视图 | `graph::DesignView`：对 IR 做展平视图。任意信号位都能追溯到真正的驱动源（寄存器、锁存器、组合单元、顶层输入、常量、未驱动、黑盒或别名环），追溯会穿过 `assign` 别名，并在端口两侧上下穿越层次；同时提供层次命名和全局寄存器编号。 |
| T3.2 时钟回溯 | 从每个寄存器的时钟引脚向源头回溯，可以穿过反相器（同时翻转边沿）、门控（AND/OR/XOR，包括基于锁存器的 ICG）、时钟 MUX（寄存器同时属于多个时钟）、分频寄存器（生成派生时钟，并记录主时钟）。遇到黑盒、常量和未驱动的情况分别处理；无法判断时报告原因，不做猜测。 |
| T3.3 约束解析 | 实现了一个 SDC/XDC 子集的 Tcl 解释器，支持：花括号、引号、`[]` 命令替换、`$var`/`${var}`、`set`、续行、注释、`list`。<br>执行的命令：`create_clock`、`create_generated_clock`（`-source`、`-master_clock`、`-divide_by`、`-multiply_by`、`-invert`）、`set_clock_groups`（异步、逻辑互斥、物理互斥）、`get_ports`/`get_pins`/`get_nets`（`-hierarchical`、`-nocase`、`-quiet`，通配符，位选）、`get_clocks`、`all_clocks`。<br>引脚名兼容 Vivado 综合后的命名，`<reg>_reg[i]/Q` 和 `/C` 能映射回 RTL 寄存器。<br>时序类命令只记录、不执行；不支持的 Tcl 控制流和未知命令都会给出提示。 |
| T3.4 集成 | 工程文件新增 `options.unknownClockRelation`（`report`、`assume_async` 或 `assume_sync`）。引擎正式读取约束，"约束读取"和"时钟域分析"两个阶段都已实现。<br>CLI 打印时钟和关系表。JSON 新增 `clockAnalysis` 段（时钟、关系、理由、策略、约束统计），寄存器列表中加入时钟域信息。<br>GUI 的 Clock Domains 面板正式实现：时钟树（双击跳转源码）、未解析的寄存器、彩色关系矩阵（悬停显示理由）。 |
| T3.5 测试与示例 | 新增 34 个测试用例：时钟 21 个、约束 8 个、设计视图 5 个；两个示例补上约束文件（`smoke/constraints/clocks.sdc`、`async_fifo/constraints/afifo.xdc`）。 |

## 2. 修改的文件

**新建**
- `src/graph/DesignView.{h,cpp}`、`src/graph/CMakeLists.txt`
- `src/constraints/Sdc.{h,cpp}`、`src/constraints/CMakeLists.txt`
- `src/clock/ClockModel.{h,cpp}`、`src/clock/ClockAnalyzer.{h,cpp}`、`src/clock/CMakeLists.txt`
- `src/gui/ClockView.{h,cpp}`
- `tests/DesignUtil.h`、`tests/graph/test_design_view.cpp`、`tests/constraints/test_sdc.cpp`、`tests/clock/test_clock.cpp`
- `examples/smoke/constraints/clocks.sdc`、`examples/async_fifo/constraints/afifo.xdc`
- `docs/progress/stage-3.md`

**修改**
- `CMakeLists.txt`：版本号改为 0.3.0，新增 3 个库，并纳入"禁止依赖 Qt"检查
- `src/engine/{ProjectConfig,AnalysisSession,ResultJson}.*`、`src/engine/CMakeLists.txt`
- `src/cli/main.cpp`
- `src/gui/{MainWindow,main}.cpp`、`MainWindow.h`、`src/gui/CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/engine/*`：constraints 不再报"未实现"，新增 options 和时钟相关断言
- 示例工程文件：加入 `constraints`

没有修改阶段 1 和阶段 2 的公共接口，只做了新增（`AnalysisResult` 新增 `view`、`constraints`、`clocks` 字段；`ProjectConfig` 新增 `options`）。

## 3. 关键算法

### 3.1 驱动追溯（DesignView）
为每个模块体建立"线网 → 驱动区间"索引，驱动类型包括连接、寄存器、锁存器、单元、输入端口和子实例输出。查询某一位时：
- 遇到连接：取 rhs 对应位继续追。
- 遇到输入端口：跳到父实例端口绑定的实际信号。
- 遇到子实例输出：跳进子实例的端口线网。

直到到达真正的源头。查询还带一个"停止回调"：时钟分析用它在受约束的线网处停下，所以对 `assign` 别名链上任意一个名字做的约束都能生效。

### 3.2 时钟回溯
1. 先把约束时钟挂到它们的源位上。
2. **第一遍**：只穿过缓冲和反相器做回溯，收集"确定是时钟"的根（被寄存器直接用作时钟的输入）。
3. **第二遍**：完整回溯，结果按位记忆化。
   - 反相器：翻转边沿。
   - 门控：用"时钟可能性评分"判断哪个输入是时钟，评分从高到低依次为：约束时钟或已知根 > 名字像时钟的输入 > 其他输入 > 数据/寄存器。最高分唯一时，它就是时钟，其余输入记为门控信号；两个输入都是时钟时按组合时钟处理；无法区分时报告 `cannot tell which input`。
   - 时钟 MUX：两路都可能是时钟时，寄存器同时属于两个时钟。
   - 寄存器输出：生成分频时钟，主时钟等于该寄存器的时钟。先登记再追溯主时钟，避免寄存器自己驱动自己时陷入死循环。
4. 用 `create_generated_clock` 的 `-source` 或 `-master_clock` 确定主时钟，并推导周期（主时钟周期 × divide ÷ multiply）。

### 3.3 时钟关系（不以名字判定异步）
对每一对时钟，按以下顺序判定：
1. `set_clock_groups`：组成员匹配时也考虑主时钟链，所以派生时钟会继承主时钟的分组；只有一个 `-group` 时，它与所有其他时钟异步（SDC 语义）。
2. 主时钟链有包含关系，或者根时钟相同：同步，并说明派生自谁。
3. 其他情况：未知，理由中列出两个独立的源。

每个判定都带理由文字，包括约束所在的文件和行号。

## 4. 编译与测试结果

| 环境 | 构建 | 测试 |
|---|---|---|
| Linux x64，GCC 13.3，Release，`-Werror` | ✅ 本项目代码零警告 | ✅ 138/138 |
| Linux x64，GCC 13.3，Debug，`-Werror` | ✅ 零警告 | ✅ 138/138 |
| Windows x64 + VS2022 | ⚠️ 仍未验证 | — |

### 4.1 本地结果
138 个 CTest 测试：132 个 Catch2 用例（本阶段新增 34 个，另有 3 个引擎/工程用例更新）、5 个 CLI 端到端测试、1 个 GUI 无头冒烟测试（新增对时钟关系矩阵的检查）。

测试过程中发现并修复：
- 未知选项的值被当成对象名去匹配，导致多报"未匹配"（`CON-UNMATCHED`）。
- 约束对象解析器的内存问题（见第 4.3 节）。

### 4.2 开源工程验证（verilog-axis，31 个模块）
- 28 个有寄存器的模块，寄存器时钟全部解析，没有任何时钟警告。
- `axis_async_fifo` 正确识别为 `s_clk`（21 个寄存器）和 `m_clk`（24 个寄存器）两个时钟。没有约束时关系判为"未知"，不会仅凭名字不同就判为异步。

### 4.3 性能

| 设计 | Debug 耗时 | 峰值内存 |
|---|---|---|
| 2 万个实例、6 万个寄存器，无约束 | 2.9 s | 226 MB |
| 同上，加一个对 6 万个寄存器做层次通配的约束文件 | 3.8 s | 226 MB |

开发过程中发现一个性能问题：约束对象解析器最初会预先展开每个寄存器每一位的引脚名，导致 7 s、631 MB。已改为按需构建名字表、寄存器引脚按寄存器而不是按位建条目，并用 `(实例, 线网)` 索引代替线性扫描。

## 5. 已知限制

1. Windows/MSVC 仍未实际编译。
2. **原语模型尚未建立**：PLL/MMCM 等厂商原语如果作为黑盒出现，它的输出会成为独立的"黑盒时钟"，与输入时钟的关系是"未知"，需要用 `create_generated_clock` 或 `set_clock_groups` 补充。
   - 目前 HDL 中模块未定义时 slang 会报错，IR 不会生成，所以黑盒时钟只是路径上的预留，尚未用真实案例验证。
3. 锁存器的时钟域没有分析，只统计数量（`CLK-LATCHES`）。
4. `inout` 端口按输入处理。
5. 约束的以下部分不支持，会给出提示：
   - Tcl 控制流（if/for/foreach/proc/source）
   - `-filter`、`-of_objects`、`-regexp`
   - `get_cells`、`all_registers` 等
   - `-edges` / `-edge_shift`
6. 门控时钟的判定依赖启发式评分：两个都不是已知时钟、名字也不像时钟的普通输入相与时，会报告"无法判断"，而不是猜测。
7. 时序类约束（`set_false_path`、`set_max_delay` 等）只记录不执行；后续阶段可以用来过滤 CDC 路径，目前尚未实现。

## 6. 下一阶段建议（阶段 4：基础 CDC 检查）

1. **时序依赖图（SDG）**：对每个寄存器的 D/EN/SRST/ARST 引脚，经组合锥反向搜索到上游的寄存器、端口和黑盒，按位精度、结果记忆化，并记录锥内性质（是否只经过缓冲/反相、是否有多源汇合）。
2. **跨域边识别**：按时钟关系矩阵和 `unknownClockRelation` 策略判断哪些边跨域。
3. **交叉分组**：单比特/多比特，控制/数据。
4. **2/3 级同步器识别**。
5. **规则**：CDC-001、002、003、010，每条规则都要有正例和反例。
6. **界面与报告**：GUI 的 CDC Paths 和 Rule Violations 面板，JSON 的 CDC 段。
7. **验证**：用 async_fifo、smoke 和 verilog-axis 做端到端验证。

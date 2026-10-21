# 阶段 2 完成报告：Verilog 前端 + Design IR

日期：2026-10-20　版本：0.2.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T2.1 IR 数据结构 | `src/ir/`：四态常量 `ConstBits`；位区间信号 `SigSpec`（切片、拼接、扩展、替换，自动规范化）；`Net`、`Cell`（32 种字级运算）、`Register`、`Latch`、`Connection`、`ModuleBody`、`InstanceNode`、`Design`；常量折叠 `simplifyCell`；确定性的文本输出 `dumpDesign` |
| T2.2 结构降级 | 端口、线网和变量（包括 generate 作用域内的信号）、连续赋值、线网声明赋值、变量初值、模块实例（含表达式端口和拼接输出端口）、实例数组、门级原语（and/or/xor/nand/nor/xnor/buf/not）；表达式：全部算术、逻辑、比较、移位、归约、条件、拼接、复制运算，常量/动态位选，常量/动态索引部分选择，packed struct 成员访问，`$signed`/`$unsigned`，参数和常量函数的常量求值 |
| T2.3 过程块符号执行 | `always @*`、`@(a or b)`、`always_comb`、`always_latch`、`always_ff` 和边沿触发的 `always`；if/case/casez/casex 合并为 Mux；常量边界的 for/repeat 循环展开；用户函数内联；阻塞与非阻塞赋值语义；时钟和多个异步控制的识别（含优先级和极性检查）；同步复位和时钟使能的提取；按控制集合拆分寄存器；锁存器推断；`initial` 块中的常量初值 |
| T2.4 集成 | `IFrontend::elaborate` 返回 `FrontendOutput`（层次摘要和 Design IR）；引擎新增 "RTL extraction" 阶段报告；CLI 新增 `--dump-ir` 并输出 RTL 统计；JSON 结果新增 `rtl` 段（统计、不支持项列表、展平的寄存器列表，含时钟、复位、使能）；GUI 层次视图新增寄存器数列，报告视图列出 RTL 统计和"未分析/近似处理"表 |
| T2.5 回归测试 | 54 个提取测试，两份经过逐行人工审核的 golden 文件（smoke、async_fifo）；新增示例工程 `examples/async_fifo`（Cummings 异步 FIFO） |

## 2. 修改的文件

**新建**
- `src/ir/{Const,SigSpec,Design,ConstFold,Dump}.{h,cpp}`、`src/ir/CMakeLists.txt`
- `src/frontend/slang_adapter/{IrBuilder.h,IrBuilder.cpp,ProcessLowering.cpp}`
- `tests/ir/test_ir.cpp`、`tests/ir_extract/test_ir_extract.cpp`、`tests/ir_extract/golden/{smoke,afifo}.ir`
- `examples/async_fifo/`（6 个 RTL 文件、工程文件、README）
- `docs/progress/stage-2.md`

**修改**
- `CMakeLists.txt`：版本号改为 0.2.0；新增 `src/ir`；Qt 依赖检查加入 `opencdc_ir`
- `src/frontend/Frontend.h`：新增 `FrontendOutput`，`elaborate` 的返回类型随之改变（本阶段唯一的接口不兼容变更，已事先说明）
- `src/frontend/slang_adapter/SlangFrontend.cpp`：调用 `IrBuilder`；HDL 有错误时跳过 IR 生成（`IR-SKIPPED`）；位置转换改用共享的 `convertLocation`
- `src/frontend/CMakeLists.txt`、`src/engine/CMakeLists.txt`：链接 `opencdc_ir`
- `src/engine/AnalysisSession.{h,cpp}`：`AnalysisResult::design`，RTL 阶段报告
- `src/engine/ResultJson.cpp`：新增 `rtl` 段
- `src/cli/main.cpp`：新增 `--dump-ir`，输出 RTL 统计
- `src/gui/MainWindow.cpp`、`src/gui/main.cpp`：寄存器数列、报告内容、About 文字；GUI 冒烟测试增加对寄存器数列的检查
- `tests/CMakeLists.txt`、`tests/engine/test_analysis_session.cpp`、`tests/frontend/test_slang_frontend.cpp`：适配新接口，新增 RTL 相关断言

## 3. 关键算法

### 3.1 字级 IR 与位精度
信号统一表示为 `SigSpec`，即若干"线网位区间"或常量的拼接，内部按 LSB 在前存储，并自动合并相邻区间。位选、部分选择、拼接和扩展都在 `SigSpec` 层面完成，不产生单元，也不做逐位展开。每种运算单元带有 `BitDep`（逐位相关或全相关），供阶段 4 建立位精度的依赖图。

### 3.2 过程块符号执行
每个变量映射到它当前的 `SigSpec` 值。执行时维护 `cur`（阻塞赋值的可见值）、`nba`（非阻塞赋值的待更新值）、`any`（至少一条路径写过的位）和 `all`（所有路径都写过的位）。
- **if/case**：两条分支分别从同一状态的副本执行，在合并点按变量生成 `Mux(S=条件, A=否, B=是)`。条件为常量时直接只执行对应分支，参数化代码和展开后的循环因此不会产生多余逻辑。
- **for 循环**：每轮求值循环条件，条件必须折叠为常量，最多 65536 轮。
- **函数调用**：以独立状态执行函数体，返回值变量即为结果。
- **结果生成**：
  - 组合块中，`all` 覆盖的位生成 `Process` 连接，只在部分路径赋值的位生成锁存器。
  - 时序块中，所有被写过的位生成寄存器，D 为最终值。没有被写到的位，D 中保留 Q 本身，也就是"保持"。

### 3.3 时钟与异步控制识别
事件列表中只有一个边沿时，它就是时钟。有多个边沿时，要求块体形如 `if (c1) … else if (c2) … else <时钟逻辑>`：把每个条件归一化为"信号 + 极性"（支持 `!x`、`~x`、`x==0`、`x!=1` 等写法），再与事件信号按 `SigSpec` 匹配，逐层剥离为异步控制，剩下的那个事件就是时钟。如果无法匹配，报告 `IR-AMBIGUOUS-CLOCK`，并且不做猜测。极性和边沿不一致时报告 `IR-RESET-POLARITY`。

### 3.4 使能与同步复位提取
在 D 的驱动 Mux 链上识别：
- **使能**：一路输入就是 Q 本身（保持），选择信号即为使能，取反时自动加 Not。
- **同步复位**：一路是常量，**并且选择信号是具名信号（可以取反）**。由比较器译码出的常量选择属于数据，不算复位。这条规则来自 smoke 示例中 `case` 被误判为同步复位的修复。

### 3.5 寄存器拆分
同一变量的位，按"连续被写"以及"受哪些异步控制影响"拆分成多个寄存器。这样每个 `Register` 的所有位共享完全相同的时钟、复位和使能，阶段 3/4 可以直接按寄存器做域分配。

### 3.6 IR 清理
- **结构哈希**：相同的单元只生成一次。
- **无用存储清理**：从可观测点（端口、连续赋值、子实例输入）沿组合单元向后做活性传播，迭代到不动点；输出到达不了任何可观测点的寄存器和锁存器会被删除，并给出 Info 诊断 `IR-DEAD-STORAGE`。锁存器警告在清理之后才发出，避免"只在分支内使用的临时变量"被误报为锁存器。
- **无用单元清理**：删除没有被引用的单元，并重新编号线网。

### 3.7 模块体共享
以 slang 的 canonical body 为键，相同参数的实例共享同一个 `ModuleBody`。`Design::addTop` 用显式栈（不递归）展开实例树，并分配展平偏移。

## 4. 编译与测试结果

| 环境 | 构建 | 测试 |
|---|---|---|
| Linux x64，GCC 13.3，Release，`-Werror` | ✅ 本项目代码零警告 | ✅ 102/102 |
| Linux x64，GCC 13.3，Debug，`-Werror` | ✅ 零警告 | ✅ 102/102 |
| Windows x64 + VS2022 | ⚠️ 仍未验证（原因同阶段 1） | — |

### 4.1 本地结果
102 个 CTest 测试：96 个 Catch2 单元测试（其中 54 个 RTL 提取测试和 6 个 IR 数据结构测试），5 个 CLI 端到端测试，1 个 GUI 无头冒烟测试（新增寄存器列检查）。修复过程中发现的问题（属性值显示、同步复位误判、低有效同步复位、复位位拆分、动态部分选择左值、临时变量误报锁存器）都有对应的回归测试。

### 4.2 开源工程压力测试
在 [alexforencich/verilog-axis](https://github.com/alexforencich/verilog-axis)（MIT，Verilog-2001）上，把 31 个模块逐个作为顶层运行：
- 无崩溃，无错误，0 个不支持的语法项，0 个锁存器。
- 这次测试暴露并修复了 2 个问题：
  - 变量基址的部分选择（`x[i*W +: W] <= …`）不能作为赋值左侧，现已支持。
  - 只在分支内使用的模块级临时变量被误报为锁存器，现已通过活性清理解决。
- `axis_async_fifo` 的 IR 中，复位同步器、格雷码指针的两级同步和 toggle 脉冲同步器都与源码一致（逐项对照了源码行）。

### 4.3 性能

| 设计 | 实例 | 寄存器 | Debug | Release | 峰值内存（Release） |
|---|---|---|---|---|---|
| 2000 个参数各不相同的实例 | 2001 | 6000（19.2 万位） | 1.3 s | — | — |
| 20000 个参数相同的实例 | 20001 | 60000（192 万位） | 2.8 s | 0.33 s | 185 MB |

## 5. 已知限制

1. **Windows/MSVC 仍未实际编译**。
2. **Yosys 差分测试未做**：开发环境的 apt 和 PyPI 都不可用，没有安装 Yosys；作为替代，用开源工程做了压力测试，并对 golden 文件做了人工审核。
3. 以下语法会报告 `IR-UNSUPPORTED-*` 并做保守处理：
   - 不建模：任务调用、while/forever/foreach 循环、fork/join、interface 和 program、非 packed 的 struct、多维 unpacked 数组。
   - 用 Opaque 单元近似、保留连接关系：跨层次引用。
   - 只支持函数体末尾的 return，条件分支中的 return 不支持。
4. 函数体内读取模块级变量时，读到的是线网值，而不是调用方过程块中阻塞赋值后的当前值（少见写法）。
5. 锁存器没有单独提取使能信号（D 中包含保持 Mux）。阶段 3 处理时钟和锁存器域时再细化。
6. 同步复位的识别规则是启发式的：外层 Mux、一路为常量、选择信号是具名信号。会出现"由具名信号选择常量的数据通路"被识别为同步复位的情况（功能上等价，只影响引脚分类）。
7. `initial` 块只提取常量初值，`$readmemh` 等系统任务会被忽略。
8. 端口别名合并（并查集）按架构设计放到阶段 3/4 的依赖图构建中做，IR 中保留的是原始端口绑定。
9. 无用存储清理会让寄存器总数小于源码中声明的数量，同时用 `IR-DEAD-STORAGE` 说明删除了多少个。

## 6. 下一阶段建议（阶段 3：时钟域分析）

1. **依赖图基础**：用并查集合并端口和 `assign` 别名，建立展平后的位段视图；做组合依赖图（CDG）的反向查询，并检测组合环。
2. **时钟网络回溯**：处理反相、门控、时钟 MUX、分频（寄存器作为时钟）、黑盒或原语输出等情况，得到根时钟和派生关系。
3. **SDC/XDC 子集解析器**：支持 `create_clock`、`create_generated_clock`、`set_clock_groups` 以及 `get_ports`、`get_pins`、`get_nets` 的通配匹配。
4. **时钟关系矩阵**：同步、异步、互斥、未知，未知关系默认使用 `report` 策略。
5. **寄存器域分配**：给每个寄存器分配时钟域；GUI 的 Clock Domain Viewer 正式实现。
6. **测试**：不少于 15 个时钟测试用例，覆盖 async_fifo 和 verilog-axis 中真实的多时钟设计。

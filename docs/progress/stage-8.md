# 阶段 8 完成报告：VHDL 与混合语言

日期：2026-10-21　版本：0.8.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T8.1 解析 | 自研 VHDL-93/2008 词法和递归下降语法分析器（`opencdc_vhdl`，不依赖 slang 和 Qt）。名字保持通用形式（下标、切片、函数调用、类型转换在语义阶段区分）。出错后在语句和设计单元边界恢复。`synthesis translate_off` 区域跳过。 |
| T8.2 语义与展开 | **库与包**：多个逻辑库、`library`/`use`/`context`、包体、延迟常量、包实例化（VHDL-2008）。<br>**内置包**：`std.standard`、`textio`、`env`，`ieee.std_logic_1164`、`numeric_std`、`numeric_bit`、`numeric_std_unsigned`、`std_logic_arith`、`std_logic_unsigned`、`std_logic_signed`、`std_logic_misc`、`math_real`、`std_logic_textio`，全部原生实现。<br>**类型**：枚举、整数（宽度由范围决定）、物理类型、实数（只用于常量计算）、多维数组、记录、子类型、别名、属性（`'range`、`'length`、`'high`、`'image` 等）。<br>**重载决议**：按参数类型和期望结果类型筛选；显式声明隐藏预定义运算符；通用整数优先（LRM 9.3.6）。<br>**层次**：实体/结构体/generic、组件声明默认绑定、`entity work.x`、配置（使用默认绑定并给出警告）、for/if/case generate、带 generic 的 block、部分端口关联（`a(3 downto 2) => p`、记录字段）、输入端口上的表达式、无约束端口取实参的约束。 |
| T8.3 降级到 IR | 进程按符号执行：变量立即生效，信号收集为下一个值，if/case 以多路选择器合并，循环展开，`exit`/`next`/`return` 用执行条件表示，子程序内联（含 out 参数）。<br>**时钟进程**：所有常见写法（`rising_edge`、`'event`、`wait until`、`and en` 使能、异步/同步复位）。<br>**组合进程**：组合连接或锁存器（`IR-LATCH-INFERRED`）。<br>**存储器**：动态下标读写（`DynSelect`/`DynInsert`），包括元素的一部分（`mem(i)(15 downto 8)`、`mem(i).f`）。<br>**属性**：`ASYNC_REG` 等信号属性成为线网属性。 |
| T8.4 混合语言 | Verilog 实例化 VHDL、VHDL 实例化 Verilog，可任意嵌套（测试覆盖 VHDL→Verilog→VHDL→Verilog）。参数/generic 跨语言转换（整数、向量、布尔、字符串、实数），端口按名字（不区分大小写）或位置绑定，宽度差异按 Verilog 规则。不收敛、未知模块、端口名错误、两种语言重名都有诊断。 |
| T8.5 集成 | **工程文件**：`sources` 条目可写 `{"file", "library"}`；新增 `parameters`（顶层参数/generic 覆盖，对 Verilog 也生效）。<br>**CLI/JSON/GUI**：显示语言（`vhdl`、`verilog-2001 + vhdl`）；GUI 工程树标出 VHDL 文件的库和顶层参数。<br>**示例**：`examples/vhdl/`（四个示例的 VHDL 版本）、`examples/mixed/`（两个混合版本）。<br>**CI**：Linux 作业新增 `tools/ci/vhdl-corpus.sh`，对固定版本的五个开源项目做解析和 CDC 回归。<br>**文档**：README、`docs/architecture.md`（2.3 节、第 14 节新增 3 条、新增第 15 节）。 |

## 2. 修改的文件

**新建**
- `src/frontend/vhdl/`：`VhdlAst.h`、`VhdlLexer.{h,cpp}`、`VhdlParser.{h,cpp}`、`VhdlTypes.{h,cpp}`、`VhdlElab.{h,cpp}`、`VhdlScope.cpp`、`VhdlBuiltins.cpp`、`VhdlExpr.cpp`、`VhdlNames.cpp`、`VhdlStmt.cpp`、`VhdlMixed.cpp`、`VhdlFrontend.cpp`、`CMakeLists.txt`（共约 1.3 万行）
- `src/ir/BodyEditor.{h,cpp}`（从 slang 适配层提取，两种语言共用）
- `src/frontend/AutoFrontend.cpp`、`src/frontend/MixedSupport.h`
- `tools/vhdl/vhdl_parse.cpp`、`tools/vhdl/vhdl_elab.cpp`、`tools/ci/vhdl-corpus.sh`
- `tests/vhdl/test_vhdl_parser.cpp`、`test_vhdl_elab.cpp`、`test_vhdl_mixed.cpp`
- `examples/vhdl/{smoke,cdc_rules,cdc_advanced,async_fifo}/`、`examples/mixed/{smoke,cdc_advanced}/`
- `docs/progress/stage-8.md`

**修改**
- `src/frontend/Frontend.h`：
  - `FrontendInput::sourceLibraries`、`topParameters`；
  - `makeVhdlFrontend()`、`makeDefaultFrontend()`、`isVhdlFile()`。
- `src/frontend/slang_adapter/`：
  - 混合模式（`IgnoreUnknownModules`，额外顶层）；
  - 未解析实例保留参数值和端口实参；
  - 收集模块的参数名；
  - `topParameters` 映射到 slang 的 `paramOverrides`；
  - 改用 `ir::BodyEditor`。
- `src/ir/Design.h`：`InstanceParam`、`ChildInstance::unresolvedParams`。
- `src/ir/ConstFold.cpp`：按位运算的恒等化简（`x & 1…1`、`x | 0`、`x ^ 0` 等）。
- `src/cdc/SeqGraph.{h,cpp}`：
  - 宽单元整体建锥，大锥合并用索引；
  - 组合环诊断给出例子。
- `src/cdc/AdvancedAnalysis.cpp`：Gray 识别穿过寄存器复制（多一级源端寄存器）。
- `src/cdc/CdcAnalyzer.cpp`：`ASYNC_REG` 取值判断不区分大小写和引号。
- `src/engine/`：
  - `ProjectConfig`：`sourceLibraries`、`parameters`、`languageSummary()`；
  - `AnalysisSession`：默认前端；
  - `ResultJson`：`project.hdl`。
- `src/cli/main.cpp`、`src/gui/MainWindow.cpp`：语言显示、库与参数显示。
- `tests/`：
  - `CMakeLists.txt`：新测试文件、`cli.analyze_vhdl`、`cli.analyze_mixed`、`gui.vhdl_mixed`；
  - `cdc/test_cdc_advanced.cpp`、`ir/test_ir.cpp`。
- `.github/workflows/ci.yml`、`CMakeLists.txt`（版本 0.8.0）、`README.md`、`docs/architecture.md`。

已有接口没有删除，也没有改变语义。`ChildInstance` 末尾新增一个字段，聚合初始化需要多写一个 `{}`（只有一个测试受影响）。

## 3. 关键算法

### 3.1 常量与硬件统一求值
每个表达式的结果是 `SVal {类型, 可选编译期值, IR 位}`。输入全部已知时直接计算值，例如 generic、常量、循环变量、`log2ceil(Depth_g)` 这类用户函数。按位运算在常量上也可以通过 IR 折叠得到值。否则生成单元，单元经过 `BodyEditor::emit` 折叠并做结构哈希。所以宽度计算、generate 范围、常量表和进程逻辑用的是同一份代码。

### 3.2 进程的符号执行
- **进程状态 `ProcState`** 包含：
  - 变量的当前值、信号的下一个值；
  - "某条路径写过"和"每条路径都写过"两种位掩码；
  - 执行条件 `alive`、`returned`；
  - 每层循环的 `exit`/`skip` 条件。
- **分支合并**：分支各自执行，再按条件合并成多路选择器。缺省的一侧取存储值；只有真正读到存储值时才把变量记为"需要存储"。
- **时钟进程**：
  - 在 if 链中找到边沿分支，它前面的分支是异步复位（条件为常量假的分支去掉）。
  - 寄存器按"存储线网片段 × 异步分支签名"切分，同步复位和使能由 `extractControls` 提取。
- **组合进程**：每条路径都写到的位成为组合连接，其余位成为锁存器。

### 3.3 混合语言链接（迭代到不动点）
1. 编译 Verilog 源文件和生成的包装模块 `opencdc_mixed_wrapper`。包装模块为 VHDL 请求的每个 `模块 #(参数)` 放一个实例。
   - VHDL 实体名作为"外部定义"传给 slang；
   - 这些实例的参数从语法重新绑定、求值，端口实参降级为 IR 位。
2. VHDL 展开器把模块体加入同一个 Design，然后把 Verilog 中的未解析实例绑定到 VHDL 模块体（按名字或位置，宽度补零/截断）。
3. VHDL 中的 Verilog 实例从包装模块取模块体。若出现新的请求，就重新开始一轮（最多 16 轮），只报告最后一轮的诊断。
4. 只保留从顶层可达的模块体，再建立实例树。

### 3.4 宽单元整体建锥（性能）
宽度超过 4096 位的逐位单元，例如整块存储器的写入多路选择器和 `DynInsert`，不再逐位记忆化，而是作为一个整体计算一个依赖锥。合并超过 16 个源节点的锥时用哈希索引去重。neorv32（64 KB 存储器，20.8 万位寄存器）的 CDC 路径分析从 7.6 s 降到 0.34 s（Release），结果相同。

## 4. 构建与测试结果

- **Linux GCC 13（Debug，`-Werror`）**：291/291 通过。其中 C++ 单元测试 271 个用例、4587 条断言；VHDL 相关 53 个用例，混合语言 9 个用例。
- **Linux Clang（Debug，`-Werror`）**：291/291 通过。
- **等价性**：4 个 VHDL 示例和 2 个混合示例的 CDC 结果与 Verilog 原版逐条相同（路径、状态、同步器类型和级数、规则命中），已写成测试 `VHDL examples give the same CDC results as the Verilog examples`。
- **真实代码，解析**：五个项目共 1935 个文件（open-logic、neorv32、PoC、OSVVM、surf），除 3 个本应拒绝的文件外，其余 1932 个文件、3865 个设计单元全部通过。这 3 个是文档片段、模板和 VHDL-2019 语法，CI 也检查它们确实被拒绝。
- **真实代码，open-logic 跨时钟组件**（9 个）：全部完成分析。
  - fifo_async 的两个指针识别为 Gray，读数据为受限定数据，0 个锁存器；
  - cc_status、cc_handshake 识别为握手；
  - cc_n2xn、cc_xn2n 用于有倍数关系的同步时钟，在异步约束下报告违规，这是正确的。
- **真实代码，neorv32 SoC**（打开 42 个功能 generic）：110 个实例、77 个模块体、770 个寄存器（20.8 万位），展开 0.22 s，CDC 分析 0.38 s（Release）。报告的 24 个组合环全部来自 TRNG 的环形振荡器，是设计本意。
- **真实代码，surf 同步器**：`Synchronizer`、`SynchronizerVector`、`SynchronizerFifo`、`SyncStatusVector`（262 个实例）、`RstSync`、`FifoAsync` 全部可以展开。

## 5. 已知限制

- 不支持的构造按 `IR-UNSUPPORTED-*` 报告，不会静默忽略：
  - 受保护类型、访问类型、文件操作（可以解析，没有硬件）；
  - VHDL-2008 的子程序 generic 和包 generic、外部名（`<< signal ... >>`）；
  - 并发过程调用；带端口的 block；
  - 两个变量下标的赋值目标（`m(i)(j) <= …`）；多维数组的变量下标。
- 配置（configuration）只用来找到实体，其中的绑定被忽略（`VHDL-CONFIGURATION` 警告）。
- 一个进程只支持一个 `wait until`；其他 `wait` 视为测试平台代码。
- 混合语言：
  - VHDL 记录类型和多维数组端口不能连接到 Verilog；
  - 部分关联只支持 VHDL 一侧；
  - Verilog 侧把 VHDL 输出连到比端口窄的实参时，只连接低位。
- 厂商原语（`unisim`、`altera_mf` 等）没有模型，作为黑盒报告。
- 没有 VHDL 语法高亮（源码查看器对两种语言都只显示纯文本）。

## 6. 下一步建议

- 为常用厂商原语提供行为模型（`FDRE`、`FDCE`、`XPM_CDC_*`），使使用原语的同步器也能被识别。
- 混合语言中支持记录端口：按字段展开成 Verilog 的多个端口。
- GUI 中加入简单的工程编辑（添加源文件、选择库），目前需要手写工程文件。

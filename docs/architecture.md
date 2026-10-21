# OpenCDC Analyzer — 架构设计文档（v0.1）

> 状态：**已确认（v0.1）**，阶段 1–8 已全部实现，见 `docs/progress/stage-{1,…,8}.md`（阶段 8：VHDL 与混合语言，第 15 节）；性能见 `docs/benchmarks.md`，豁免见 `docs/waivers.md`。实现与本文的差异见第 14 节。本文对应《总提示词》第 8 节"第一次任务"的 8 项要求。
> 所有标注【决策】的条目需要你确认；标注【风险】的条目是已知的实现风险。

---

## 0. 结论摘要

| 问题 | 建议 |
|---|---|
| HDL 前端 | **集成 slang（MIT，C++20，原生 CMake/MSVC）**，不自研解析器；仅在 `frontend/slang_adapter` 内引用 slang 头文件 |
| 我们自己的"AST" | 直接使用 slang 的语法树与已展开（elaborated）语义 AST，**不重复造 AST**；我们的第一个自有数据结构是 **RTL IR（字级网表）** |
| IR 粒度 | **字级（word-level）+ 位区间精确依赖**，不做全量 bit-blast |
| 层次处理 | IR 按"唯一模块体"存储，展开视图用"实例偏移"映射成全局 ID，跨层次路径天然连通 |
| 分析核心 | 寄存器级**时序依赖图（SDG）**：每个寄存器的 D/EN/CLK/RST 引脚 → 组合锥 → 上游时序源 |
| 时钟关系 | 三态关系矩阵：Synchronous / Asynchronous / **Unknown**；不同名时钟默认 Unknown 而非异步 |
| 同步器 | 结果分三级：`StructureRecognized` → `NecessaryConditionsVerified` → `ProtocolUnverified`（列出未验证条件） |
| MVP | CLI + 最小 GUI；Verilog-2001 子集；CDC-001/002/003/010；JSON/文本报告；真实 Verilog 用例端到端 |

---

## 1. CDC 静态检查工具的核心技术架构

### 1.1 处理流水线

```
 工程文件(.ocdc.json / .f)      约束(.sdc/.xdc 子集)      规则配置 / waiver(后续)
          │                            │                         │
          ▼                            │                         │
 ┌──────────────────┐                  │                         │
 │ ① 前端 Frontend   │ slang: 预处理(include/define/ifdef) → 语法树 → 语义展开
 │   (slang adapter)│ 参数求值 / generate 展开 / 位宽 / 实例连接
 └────────┬─────────┘
          │ 未支持语法 → Diagnostic（禁止静默忽略）
          ▼
 ┌──────────────────┐
 │ ② RTL 抽取        │ always 块符号执行 → 寄存器/锁存器/组合单元
 │   (IR Builder)   │ assign/端口连接 → 网络别名；输出 Design IR
 └────────┬─────────┘
          ▼
 ┌──────────────────┐
 │ ③ 结构图          │ 组合依赖图(位区间精确) → 寄存器级时序依赖图 SDG
 │   (graph)        │ 多驱动 / 组合环 / 黑盒 诊断
 └────────┬─────────┘
          ▼
 ┌──────────────────┐        ┌───────────────────┐
 │ ④ 时钟分析        │◄───────│ 约束解析 constraints│
 │   (clock)        │ 时钟树回溯、根时钟、派生/分频/门控、时钟关系矩阵
 └────────┬─────────┘
          ▼
 ┌──────────────────┐
 │ ⑤ CDC 分析        │ 跨域边识别 → 交叉分组(单/多比特、控制/数据)
 │   (cdc/path,     │ → 同步器识别 → 重汇聚 → 复位域
 │    synchronizer, │
 │    reconvergence)│
 └────────┬─────────┘
          ▼
 ┌──────────────────┐
 │ ⑥ 规则引擎 rules  │ 每条规则只读 AnalysisContext，输出 Violation
 └────────┬─────────┘
          ▼
 ┌──────────────────┐     ┌──────────┐   ┌──────────┐
 │ ⑦ AnalysisResult │────►│ CLI      │   │ Qt GUI   │  （共用同一引擎）
 │ (不可变快照)       │────►│ report   │   │ 查看器    │
 └──────────────────┘     └──────────┘   └──────────┘
```

### 1.2 分层与依赖方向（严格单向）

```
gui ──► engine ◄── cli
          │
          ├─► report ─┐
          ├─► rules ──┤
          ├─► cdc ────┤
          ├─► clock ──┼─► graph ─► ir ─► core
          ├─► constraints ────────────► core
          ├─► project ────────────────► core
          └─► frontend ─► ir, core, (slang 仅在此处)
```

- `core`：ID 类型、源码位置、诊断、日志接口、取消令牌、进度接口。**不依赖 Qt、不依赖 slang**。
- `engine`：门面（Facade），`AnalysisSession` 按阶段调度，CLI/GUI 只见到它和 `AnalysisResult`。
- 所有分析库在 CMake 层面**不链接 Qt**（通过 CMake 检查保证：`gui` 以外的 target 若出现 `Qt6::` 依赖则配置失败）。

### 1.3 设计原则

1. **结果可追溯**：每个违规都能回溯到源码位置 + 层次路径 + 依据（"为什么判为未同步"）。
2. **保守与诚实**：不能静态证明的写 `PotentialRisk` / `NeedsReview`；不支持的结构进入报告"未分析部分"。
3. **不可变结果**：分析结果是 `std::shared_ptr<const AnalysisResult>`，GUI 只读，避免线程间共享可变状态。
4. **规则与分析解耦**：重计算（时钟、交叉、同步器识别）在分析层做一次，规则只做判定，便于新增规则与写单元测试。

---

## 2. HDL 前端：自研还是集成？

### 2.1 结论【决策】

**集成 slang 作为前端**。理由：

- 需求里的前端能力（预处理、条件编译、参数、位宽、generate、层次展开、符号表）正是一个**完整 SystemVerilog 编译器前端**的工作量。自研仅 Verilog-2001 的可靠前端（含常量求值、参数覆盖、generate、位宽推导、层次名解析）保守估计 4–6 人月，且后续扩展 SV 时几乎要重写。
- slang 是 C++20、MIT 许可、CMake 构建、官方支持 MSVC，与本项目技术栈完全一致；能处理 IEEE 1800-2017/2023（Verilog-2001/2005 是其子集），提供精确的 `SourceLocation`、诊断系统、常量求值与完整展开。
- 我们的核心价值在 **RTL 抽取、时钟/CDC 分析、规则和 GUI**，前端外包给成熟开源组件是正确的投入分配。

自研仍然存在的部分（不可外包）：**always 块 → 寄存器/组合逻辑的语义抽取**（slang 给的是语义 AST，不是网表），这是本项目的第一个硬核模块。

### 2.2 开源方案对比

| 方案 | 语言/形态 | 许可证 | 能力 | Windows/MSVC 集成 | 结论 |
|---|---|---|---|---|---|
| **slang** (MikePopoloski/slang) | C++20 库 | MIT | 完整预处理+解析+语义展开，SV-2017/2023，诊断精确，近年加入数据流分析模块 | 原生 CMake，支持 MSVC；`FetchContent` 直接拉取 | **首选** |
| slang-netlist (jameshanlon) | C++ 库，基于 slang | MIT | 从 slang AST 构建网表图、路径查询 | 依赖 slang，实验性质 | 作为**参考实现**阅读，不直接依赖 |
| yosys-slang (povik) | Yosys 插件 | ISC | slang → Yosys RTLIL | 依赖 Yosys | 仅用于交叉验证 |
| Yosys | C++ 程序/库 | ISC | 综合到 RTLIL，`proc` pass 可靠抽取触发器（$dff/$adff/$dffe…）；自带 Verilog 前端 SV 支持有限 | MSVC 构建困难（通常 MinGW/WSL），体量大 | **路线 B**；作为 Linux CI 上的**差分测试基准**（对比我们抽取的寄存器集合） |
| Surelog + UHDM | C++ 库 | Apache-2.0 | 完整 SV 解析+展开，输出 UHDM 数据模型 | ANTLR 依赖，构建慢、体量大，Windows 可用但较重 | 备选 |
| Verible | C++ 工具集 | Apache-2.0 | 解析/格式化/lint，**无语义展开** | Bazel 为主，Windows 支持弱 | 不适合 |
| Verilator | C++ 程序 | LGPL-3.0 / Artistic-2.0 | 完整展开，面向仿真 | 非库形态，难以嵌入 | 不适合 |
| sv-parser | Rust 库 | MIT/Apache-2.0 | 仅语法树 | 需 Rust FFI | 不适合 |
| tree-sitter-verilog | C 库 | MIT | 容错 CST，适合编辑器高亮 | 易集成 | 可用于 **GUI 源码高亮**（可选） |
| Icarus Verilog | C++ 程序 | GPL-2.0 | 仿真器 | — | 许可证不合适 |
| hdlConvertor | C++/Python | MIT | SV+VHDL 解析（ANTLR），无完整展开 | 中等 | VHDL 备选之一 |
| GHDL / nvc | Ada / C | GPL-2.0 / GPL-3.0 | 完整 VHDL | 进程外调用可避开链接传染 | VHDL 阶段再评估 |

> slang 版本：阶段 1 实施时锁定 **v12.0**（GitHub 上的最新 tag），见 `cmake/Dependencies.cmake`。

### 2.3 VHDL

前端通过接口 `IFrontend` 抽象，输出同一种 Design IR。候选路线曾有三条：自研前端、进程外调用 GHDL、hdlConvertor 解析加自研展开。阶段 8 选择了**自研 VHDL 前端**（许可证干净，保留完整源码位置），设计见第 15 节。

### 2.4 slang 相关风险

- 【风险】**API 随大版本变化**。对策：只有 `frontend/slang_adapter/*.cpp` 包含 slang 头文件；锁定版本；升级时只改适配层。
- 【风险→已解决】**Verilog-2001 关键字冲突**（如信号名叫 `logic`、`bit`）。阶段 1 已通过 slang 的 `PreprocessorOptions::keywordMapping` 为每个源文件指定 1364-2001/2005 关键字集，并有单元测试覆盖。
- 【风险】slang 作为 SV 编译器会接受比 Verilog-2001 更多的语法。我们的 RTL 抽取层有自己的"支持清单"，遇到清单外构造（如 `interface`、`class`、`always_latch` 之外的复杂结构）一律报 `FE-UNSUPPORTED` 诊断，**不因为 slang 能解析就假装能分析**。

---

## 3. 数据结构设计

### 3.1 基础类型（`core`）

```cpp
// 强类型 32 位 ID，杜绝不同 ID 混用；所有大对象存放在 arena(vector) 中，用 ID 引用
template <class Tag> struct Id {
    uint32_t value = UINT32_MAX;
    constexpr bool valid() const { return value != UINT32_MAX; }
    auto operator<=>(const Id&) const = default;
};
using FileId     = Id<struct FileTag>;
using ModuleId   = Id<struct ModuleTag>;     // 唯一的已展开模块体（参数化后）
using InstanceId = Id<struct InstanceTag>;   // 实例树节点
using NetId      = Id<struct NetTag>;        // 模块体内局部 net
using CellId     = Id<struct CellTag>;       // 模块体内局部单元
using ClockId    = Id<struct ClockTag>;
using DomainId   = Id<struct DomainTag>;

struct SourceLocation { FileId file; uint32_t line = 0, column = 0; };
struct SourceRange    { SourceLocation begin, end; };

enum class Severity { Info, Warning, Error, Fatal };
struct Diagnostic {
    std::string    code;       // "FE-UNSUPPORTED", "IR-MULTIDRIVER", "CLK-UNKNOWN-REL" ...
    Severity       severity;
    std::string    message;
    SourceRange    range;
    std::vector<std::pair<std::string, SourceRange>> notes;
};
class DiagnosticSink { public: virtual void report(Diagnostic) = 0; virtual ~DiagnosticSink() = default; };
```

### 3.2 AST 层

**不自定义 AST。** slang 提供两层：
- `SyntaxTree`：具体语法树（含预处理结果、精确位置）；
- `Compilation` / `RootSymbol`：语义层（`InstanceSymbol`、`InstanceBodySymbol`、`ProceduralBlockSymbol`、`ContinuousAssignSymbol`、`Expression`、`Statement`、类型与常量值）。

适配层用 slang 的 `ASTVisitor` 遍历语义层生成 IR。好处：参数、generate、位宽全部已由 slang 求值完毕。

### 3.3 Design IR（`ir`）——字级网表

#### 信号表示：位区间 + 拼接

```cpp
struct BitRange { uint32_t lo = 0, width = 0; };     // 规范化到 [0, net.width)，与声明的 [msb:lsb] 解耦

struct SigChunk {                                      // 一段连续位，或常量
    NetId    net;          // 无效表示常量
    BitRange bits;
    std::optional<LogicVector> constant;   // 4 态常量 (0/1/x/z)
};
struct SigSpec { std::vector<SigChunk> chunks; uint32_t width() const; };  // 拼接 {a[3:0], b, 2'b01}
```
位选择、部分位选择、拼接、切片统一归约成 `SigSpec`。**非常量下标**（`a[i]`，i 为信号）建模为 `DynSelect` 组合单元，依赖关系按"整个向量 → 结果"保守处理并记录精度损失。

#### Net

```cpp
enum class NetKind { Wire, Variable, PortIn, PortOut, PortInOut, Supply };
struct Net {
    std::string    name;         // 局部名
    uint32_t       width;
    int32_t        declMsb, declLsb;   // 用于把内部位号映射回源码写法
    NetKind        kind;
    SourceRange    decl;
    std::vector<Attribute> attrs;      // (* ASYNC_REG = "TRUE" *) 等
};
```

#### Cell（单元）

```cpp
enum class CellKind {
    // 组合
    Buf, Not, And, Or, Xor, Xnor, ReduceAnd, ReduceOr, ReduceXor,
    LogicAnd, LogicOr, LogicNot, Eq, Ne, Lt, Le, Gt, Ge,
    Add, Sub, Mul, Shl, Shr, Sshr, Mux, PMux /*case*/, DynSelect, Concat,
    // 时序
    Register, Latch,
    // 存储/层次/未知
    Memory, MemRead, MemWrite, Instance /*子实例占位*/, BlackBox
};

struct Pin { PinRole role; SigSpec sig; };  // role: A,B,Y,S(select),D,Q,CLK,EN,ARST,SRST,...

struct Cell {
    CellKind          kind;
    std::vector<Pin>  pins;
    SourceRange       origin;          // 产生该单元的源码表达式/语句
    BitDepKind        depKind;         // Bitwise(i→i) / Full(全→全) / Shift(const k) / Custom
};
```

`depKind` 是**避免 bit-blast 又保持位精度**的关键：按位运算 `i→i`，算术/比较 `全部→全部`（对 CDC 是保守且正确的），常量移位按偏移映射。

#### 寄存器（时序单元的专用视图）

```cpp
enum class Edge { Pos, Neg };
struct ClockSpec { SigSpec sig /*1 bit*/; Edge edge; };
struct ResetSpec { SigSpec sig; bool activeHigh; SigSpec value; };

struct RegisterInfo {        // 对 kind==Register 的 Cell 的结构化解释
    SigSpec                  q;          // 被赋值的变量位
    SigSpec                  d;          // 下一状态（复位/使能已剥离后的数据输入）
    ClockSpec                clock;
    std::optional<ResetSpec> asyncReset;
    std::optional<ResetSpec> syncReset;
    std::optional<SigSpec>   enable;     // 识别出 "if (en) q <= d" 结构时
    ProcessRef               process;    // 来源 always 块
};
```

**为什么把使能、同步复位从 D 里剥离**：CDC 中"跨域信号进入 EN 引脚"与"进入 D 引脚"含义不同（前者常见于 MUX/使能型同步方案），需要按引脚分类边。

#### 模块体与实例树

```cpp
struct ModuleBody {                 // 一个参数化后唯一的模块体
    std::string                 defName;      // "fifo_async"
    std::string                 paramSignature; // "#(DW=8,AW=4)"
    std::vector<Net>            nets;
    std::vector<Cell>           cells;
    std::vector<RegisterInfo>   registers;    // 与 Register cell 一一对应
    std::vector<PortBinding>    ports;        // 端口 net 与方向
    std::vector<ChildInstance>  children;     // 子实例: ModuleId + 端口连接 SigSpec
};

struct InstanceNode {
    std::string  name;              // "u_sync"
    ModuleId     body;
    InstanceId   parent;
    std::vector<InstanceId> children;
    uint32_t     netBase, cellBase; // 扁平化偏移 → 全局 ID
};

class Design {                      // 不可变；由 IrBuilder 一次构建
public:
    std::span<const ModuleBody>   bodies() const;
    std::span<const InstanceNode> instances() const;
    InstanceId                    top() const;
    std::string hierName(GlobalNetId) const;   // "top.u_rx.u_sync.meta[0]"
    // ...
};
```

**扁平化策略**：同一模块体被实例化 N 次时只存一份；全局 ID = `instance.netBase + localNetId`。分析层看到的是扁平的全局 ID 空间，内存只与"唯一模块体 × 实例数的偏移表"相关，而不是复制 N 份网表。

#### 端口连接 → 位级别名（Alias）

子实例 `.din(bus[7:4])` 使子模块端口 `din[3:0]` 与父模块 `bus[7:4]` 成为**同一组物理位**。实现：
- 在全局空间上做**区间并查集**（Segment Union-Find）：节点是 `(GlobalNetId, 连续位段)`，必要时分裂段；
- 合并后每个"规范位段"保存所有别名（用于 GUI 显示每一层的名字）；
- `assign a = b;`（纯连线）同样折叠为别名；带运算的 `assign` 生成组合单元。

这样跨层次追踪**不需要显式穿过端口**，同时保留各层名字用于报告的 Hierarchical Path。

### 3.4 always 块 → 寄存器的抽取算法（自研核心）

对每个 `always` / `always_ff` / `always_comb` / `always_latch`：

1. **分类**：根据敏感列表：
   - 含 `posedge/negedge` → 时序过程；
   - `@*` 或完整电平敏感列表 → 组合过程；
   - 不完整电平敏感列表 → 诊断 `IR-INCOMPLETE-SENS`（按组合处理并标注）。
2. **符号执行**：遍历语句树，维护 `变量位 → 表达式DAG` 的映射：
   - 非阻塞赋值写入"下一状态表"，阻塞赋值写入"当前值表"；
   - `if/else`、`case` 在合并点生成 `Mux/PMux` 单元（选择信号作为 `S` 引脚）；
   - `for` 循环（常量边界）由展开处理；非常量边界 → `FE-UNSUPPORTED`。
3. **时钟/复位识别**（时序过程）：
   - 敏感列表中的边沿信号若出现在最外层 `if` 的条件里（如 `if (!rst_n)`），且该分支只赋常量 → 异步复位，记录极性与复位值；
   - 剩余唯一边沿信号 → 时钟；多于一个且无法匹配 → `IR-AMBIGUOUS-CLOCK` 诊断，该块标记为"未分析"。
4. **使能/同步复位剥离**：下一状态表达式若形如 `Mux(cond, d, q_self)` → 使能 `cond`；形如 `Mux(srst, const, x)` → 同步复位。
5. **锁存器推断**：组合过程中存在未在所有路径上赋值的变量 → 生成 `Latch` 单元并报 `IR-LATCH-INFERRED`。
6. **多驱动检查**：同一变量位被多个过程或 `assign` 驱动 → `IR-MULTIDRIVER`。

### 3.5 依赖图（`graph`）

#### 组合依赖图（CDG）
节点 = 全局单元 + 规范位段；边 = 单元输入引脚位 → 输出位（按 `depKind` 生成位映射）。只在需要时按位段查询，不预先展开全部位。组合环 → `GR-COMB-LOOP` 诊断。

#### 时序依赖图（SDG）——CDC 分析的主要数据结构

```cpp
enum class SeqNodeKind { Register, Latch, PrimaryInput, PrimaryOutput, BlackBoxOut, BlackBoxIn, MemoryWrite, MemoryRead, Constant };
enum class SinkPin    { Data, Enable, SyncReset, AsyncReset, Clock, MemAddr, MemData, MemWe };

struct SeqNode {
    SeqNodeKind kind;
    GlobalCellId cell;          // 寄存器/黑盒
    uint32_t     bitLo, width;  // 节点覆盖的位（一个寄存器向量按时钟/复位一致性分组）
    DomainId     domain;        // 由 clock 阶段填入
};

struct SeqEdge {
    SeqNodeId   src;  BitRange srcBits;
    SeqNodeId   dst;  BitRange dstBits;
    SinkPin     pin;            // 进入目标的哪个功能引脚
    ConeId      cone;           // 经过的组合锥（按需重建具体路径，不预存全部路径）
    uint16_t    combDepth;      // 0 = 直连（只经过 Buf/别名）
    CombFlags   flags;          // HasInverterOnly / HasMux / HasMultiSourceLogic ...
};
```

构建：对每个时序节点的每个输入引脚，在 CDG 上反向 DFS 直到遇到时序节点/端口/黑盒/常量为止；组合锥结果**记忆化**（同一组合节点的上游时序源集合缓存），整体近似线性于网表规模。

`CombFlags` 记录锥内性质（是否只有反相器、是否有多个不同源汇合、是否含 MUX 且选择端来自哪个域），供 CDC-004/009 使用。

---

## 4. 时钟域识别

### 4.1 时钟网络回溯

对每个寄存器的 `ClockSpec.sig`，在 CDG 上**反向**追踪，规则：

| 遇到 | 处理 |
|---|---|
| Buf / 别名 | 继续 |
| Not | 继续，极性翻转（posedge 变为 negedge 同源） |
| And/Or 且另一输入非时钟 | **门控时钟**：继承上游时钟，标记 `gated=true`，记录门控信号 |
| Mux（两路均为时钟） | **时钟 MUX**：生成多候选时钟集合，标记 `muxed`，关系待约束 |
| 寄存器 Q（作为时钟使用） | **分频/派生时钟**：根 = 该寄存器的时钟；关系 = 与父时钟同步（相位已知待验证） |
| 顶层输入端口 | **根时钟** |
| 黑盒输出（PLL/MMCM 等） | 查"原语库描述"（JSON）：若声明输出与某输入时钟相关 → 派生时钟；否则视为新根，关系 Unknown |
| 常量 / 无法解析 | `CLK-UNRESOLVED` 诊断 |

### 4.2 数据结构

```cpp
enum class ClockOrigin { PrimaryPort, Constraint, Derived_Divider, Derived_Gated, Derived_Mux, Derived_Primitive, Unresolved };

struct Clock {
    std::string  name;               // 约束名或自动名 "clk_a"
    ClockOrigin  origin;
    ClockId      master;             // 派生时钟的父时钟
    GlobalNetBit source;             // 定义点
    std::optional<double> periodNs;  // 来自约束
    bool         gated = false;
};

struct ClockDomain { ClockId clock; Edge edge; };   // posedge/negedge 同一时钟 = 同一关系族，半周期路径单独标注，不算 CDC

enum class ClockRelation { Same, Synchronous, Asynchronous, Exclusive, Unknown };
class ClockRelationMatrix { ClockRelation get(ClockId, ClockId) const; std::string reason(ClockId, ClockId) const; };
```

### 4.3 时钟关系判定（按优先级）

1. `set_clock_groups -asynchronous` → Asynchronous；`-logically/-physically_exclusive` → Exclusive（不报 CDC）。
2. 共同根时钟（派生链可追溯到同一根）→ Synchronous。
3. `create_generated_clock -source` 指定的关系 → Synchronous。
4. 其余所有组合 → **Unknown**，原因字段写明"两个时钟均为独立根，且无约束说明其关系"。

【决策】对 Unknown 的处理策略（可配置 `clockPolicy.unknownRelation`）：
- `report`（默认建议）：Unknown 对按异步做结构分析（否则会漏掉真正问题），但所有结果额外打上 CDC-010，状态为 `NeedsReview`；
- `assume_async`：与 SpyGlass 等工具的常见默认一致，不额外报 CDC-010；
- `assume_sync`：不分析（不推荐）。

### 4.4 约束子集解析（`constraints`）

不实现完整 Tcl。实现一个"Tcl 词法 + 命令调用"小解释器：支持 `[]` 命令替换、`{}`/`""` 引用、变量 `set/$var`（仅字面量）、以下命令：

- `create_clock -name -period -waveform [get_ports|get_pins|get_nets ...]`
- `create_generated_clock -name -source -master_clock -divide_by -multiply_by -invert ...`
- `set_clock_groups -asynchronous|-logically_exclusive|-physically_exclusive -group {...} -group {...}`
- 选择器：`get_ports/get_pins/get_nets/get_clocks`，支持 `*`/`?` 通配和 `-hierarchical` 的基础形式

其余命令（`set_false_path`、`set_max_delay` 等）**解析并记录为"已忽略"**，在报告中列出，不静默丢弃。

【风险】XDC 中内部 pin 名通常是综合后网表名，与 RTL 名不一定一致。顶层端口名一致性高；内部名采用"精确 → 去除综合后缀（`_reg`、`[\d]`）→ 模糊匹配并告警"的匹配策略，匹配失败报 `CON-UNMATCHED`。

---

## 5. 跨层次 CDC 路径追踪算法

由于第 3 节已把层次结构通过"实例偏移 + 位段别名"映射到扁平全局空间，跨层次追踪退化为在 SDG 上的图问题。

### 5.1 交叉（Crossing）识别

```text
for each SeqNode dst in SDG (寄存器 / 锁存器 / 黑盒输入 / 存储器写端口):
    Dd = domain(dst)
    for each incoming SeqEdge e (src → dst, pin != Clock):
        Ds = domain(src)                     // 输入端口: 由约束 set_input_delay -clock 或配置指定, 否则 Unknown
        rel = relation(Ds.clock, Dd.clock)
        if rel in {Same, Synchronous, Exclusive}: continue
        emit RawCrossing{src, srcBits, dst, dstBits, pin, e.cone, rel}
```
复杂度：O(|SDG 边|)；SDG 构建是主要开销（记忆化后近似 O(网表规模)）。

### 5.2 分组

原始交叉是位段级的，需要聚合成用户能理解的"一条 CDC 路径"：

1. **按目标寄存器向量 + 源寄存器向量**分组 → 一条 Crossing；位宽 >1 即为多比特。
2. **控制/数据分类**（启发式，需在报告中注明依据）：
   - 交叉信号进入目标的 `Enable` / `Mux.S` / `SyncReset` / `AsyncReset` 引脚 → Control；
   - 单比特且只进入同步链 → Control；
   - 多比特进入 `Data` 引脚且目标寄存器有使能/MUX 由另一同步后信号控制 → Data（受控）；
   - 其余多比特 → Data（未受控）。
3. **路径重建**：为 GUI/报告按需从 `ConeId` 在 CDG 上重建一条（或全部）具体路径：`源寄存器 → 组合单元序列 → 目标寄存器`，每一步带全局层次名与源码位置。

### 5.3 CDC 路径记录

```cpp
enum class SyncType { None, TwoFlop, ThreeFlop, MultiFlopN, SingleFlop, Toggle, Pulse, Handshake, Gray, AsyncFifo, MuxEnable, ResetSync, Unknown };
enum class SyncStatus { NotSynchronized, StructureRecognized, NecessaryConditionsVerified, ProtocolUnverified };
enum class AnalysisStatus { Violation, PotentialRisk, NeedsReview, Safe, Waived /*预留*/ };

struct CdcPath {
    CdcPathId        id;
    ClockDomain      srcDomain, dstDomain;
    ClockRelation    relation;
    GlobalSeqNode    srcRegister, dstRegister;
    std::string      crossingSignal;          // 进入目标域的第一个信号的层次名
    uint32_t         width;
    CrossingClass    cls;                     // SingleBitControl / MultiBitData / ...
    SyncType         syncType;
    SyncStatus       syncStatus;
    std::vector<std::string> unverifiedConditions;  // 例如 "Gray 编码每次只变化 1 位（需形式验证/仿真确认）"
    std::vector<PathStep>    steps;           // 层次路径：每步含层次名、单元种类、源码位置
    std::vector<RuleHit>     hits;            // 规则命中: RuleId, Severity, AnalysisStatus, 消息
    std::string      fingerprint;             // 稳定哈希: 规则无关, 基于层次名, 为 waiver 预留
};
```

---

## 6. 同步器识别（概要，阶段 4/5 详细实现）

| 类型 | 结构识别条件（StructureRecognized） | 必要条件验证（NecessaryConditionsVerified） | 无法静态证明的条件（ProtocolUnverified） |
|---|---|---|---|
| 2/3 级 FF | `S1.D` 直连交叉信号（combDepth=0，可配置允许反相器）；`S1.Q` → `S2.D` 直连；S1..Sn 同时钟同边沿 | 源为源域寄存器（非组合）；S1 扇出仅到 S2；中间无逻辑；（可选）有 `ASYNC_REG` 属性 | 时钟频率比是否保证脉冲被采到 |
| 单级 | 同上但只有 S1 后即进入逻辑 | — | 直接报 CDC-002 |
| Toggle | 源域 `t <= t ^ p`；目标域 2FF + `q ^ q_d` 边沿检测 | 源 toggle 寄存器无其他赋值源 | 相邻脉冲间隔 ≥ 目标域若干周期 |
| Pulse | Toggle 的变体/展宽脉冲 + 2FF + 边沿检测 | 同上 | 同上 |
| Req/Ack | req: A→B 2FF；ack: B→A 2FF；数据寄存器在 A 域、在 B 域由同步后 req 使能采样 | 数据寄存器使能来自同步后的 req | 发送端在 ack 返回前保持数据稳定 |
| Gray | 源域存在 `g = b ^ (b >> 1)` 结构，各位独立 2FF；目标域存在 gray→bin | 各位同步链长度一致 | 计数器每次只 ±1 |
| 异步 FIFO | Memory 写端口在 A 域、读端口在 B 域；写/读指针 Gray 化后相互 2FF 同步；满/空比较存在 | 指针同步链合规 | 指针单调递增 1、深度为 2 的幂 |
| MUX/使能型 | 多比特数据进入目标寄存器 `Enable`/`Mux.S`，选择信号来自已识别的单比特同步器 | 数据在源域寄存 | 使能有效时数据稳定 |
| 复位同步器 | 若干 FF 同时钟、异步复位接外部复位、首级 D 为常量 | 输出用作该时钟域寄存器的复位 | — |

识别是**模式匹配 + 条件检查**，每个条件的成功/失败都记录进 `CdcPath`，GUI 可展示"为什么判为/不判为某种同步器"。

---

## 7. 规则引擎

```cpp
struct RuleMeta {
    std::string id;              // "CDC-001"
    std::string title;
    std::string purpose;
    std::string applicability;
    Severity    defaultSeverity;
    std::string falsePositiveNotes;
    std::string fixSuggestion;
};

class AnalysisContext;           // 只读: Design, SDG, ClockModel, Crossings, Synchronizers, Config

class IRule {
public:
    virtual ~IRule() = default;
    virtual const RuleMeta& meta() const = 0;
    virtual void check(const AnalysisContext&, ViolationSink&) const = 0;
};

class RuleRegistry {             // 显式注册（非静态全局自注册，避免全局状态与链接期丢失）
public:
    void add(std::unique_ptr<IRule>);
    std::span<const std::unique_ptr<IRule>> rules() const;
};
```

### 7.1 第一批规则的检测算法概要

| 规则 | 检测 | 默认严重度 | 结果状态 | 误报限制 |
|---|---|---|---|---|
| CDC-001 单比特直接跨异步域 | 单比特 Crossing 且 `syncType==None` | Error | Violation | Unknown 关系降为 NeedsReview；静态信号（约束/配置声明为 quasi-static）豁免 |
| CDC-002 仅一级同步 | `syncType==SingleFlop` | Warning | Violation | 用户配置的最小级数（默认 2） |
| CDC-003 多比特直接跨域 | 宽度>1，无受控/Gray/FIFO 识别 | Error | Violation | 若其使能来自已同步控制信号则归入 MUX/使能型，不报 |
| CDC-004 同步链中有组合逻辑 | 同步链 S1 前或 Si→Si+1 之间 `combDepth>0` | Error | Violation | 可配置允许单个反相器 |
| CDC-005 重汇聚 | 见 7.2 | Warning | PotentialRisk | 已识别 Gray/FIFO 的同组位不互相计入 |
| CDC-006 异步复位释放未同步 | 寄存器异步复位源的根所在域 ≠ 该寄存器域，且路径上无复位同步器 | Error | Violation | 复位源为顶层端口且声明为已外部同步 → NeedsReview |
| CDC-007 脉冲丢失 | 源为"可能为脉冲"的寄存器（下一状态在若干分支被清零）且目标域通过电平同步器采样 | Warning | PotentialRisk | 无时钟周期信息时只报 NeedsReview |
| CDC-008 多比特同步不一致 | 同一源向量的多位分别经过独立 FF 同步器，且未识别为 Gray | Error | PotentialRisk | 若下游只单独使用各位（无汇合）→ 降级 |
| CDC-009 组合毛刺跨域 | 交叉边 `cone` 内含组合逻辑，或交叉信号源为组合输出/多域信号混合 | Warning | PotentialRisk | 纯反相器/纯 Buf 不报 |
| CDC-010 时钟关系未知 | Crossing 的 relation==Unknown | Info/Warning | NeedsReview | 用户补充约束后自动消失 |

每条规则在实现阶段会补齐完整规格文档（`docs/rules/CDC-xxx.md`：目的、适用条件、算法、严重度、误报限制、测试用例、修复建议），并在 `tests/cdc/rules/CDC-xxx/` 下至少有一个正例和一个反例 Verilog 工程。

### 7.2 重汇聚算法（CDC-005）

```text
S = 所有已识别同步器的输出节点（同一目标域）
for each s in S: 从 s 前向 BFS（SDG 上，限制时序深度 K，默认 K=2，可配置）, 给沿途节点打标签 bitset{s}
for each node n: 若 labels(n) 含有来自"不同源信号"的 ≥2 个同步器 → 报告 (n, 汇聚的同步器集合, 汇聚深度)
```
标签用位集；同步器数量大时按目标域分批，避免 O(S×N) 内存。

---

## 8. 引擎接口（CLI 与 GUI 共用）

```cpp
struct ProjectConfig {
    std::string                  name;
    std::vector<std::filesystem::path> sources, includeDirs, constraintFiles;
    std::vector<std::pair<std::string,std::string>> defines;
    std::string                  top;
    LanguageStd                  lang = LanguageStd::Verilog2001;
    AnalysisOptions              options;   // 时钟策略、同步器最小级数、重汇聚深度…
    RuleConfig                   rules;     // 启用/禁用/严重度覆盖
};

class ProgressSink { public: virtual void stage(std::string_view name, double fraction) = 0; virtual ~ProgressSink() = default; };
class CancellationToken { public: bool cancelled() const noexcept; void cancel() noexcept; };

class AnalysisSession {
public:
    static std::expected<ProjectConfig, Diagnostic> loadProject(const std::filesystem::path&); // .ocdc.json
    std::shared_ptr<const AnalysisResult> run(const ProjectConfig&, ProgressSink&, const CancellationToken&);
};

struct AnalysisResult {
    ProjectInfo                           project;    // 名称、时间、范围
    std::shared_ptr<const Design>         design;
    ClockModel                            clocks;
    std::vector<CdcPath>                  paths;
    std::vector<Violation>                violations;
    std::vector<Diagnostic>               diagnostics;
    std::vector<UnanalyzedRegion>         unanalyzed; // 未分析/不支持部分（报告必列）
    Statistics                            stats;
};
```

> `std::expected` 需要 C++23 库支持；MSVC 2022 在 `/std:c++latest` 下可用。若坚持纯 C++20，则用项目内的 `Result<T>`（tl::expected 风格的小实现）替代。【决策：默认用自带 `Result<T>`，保持 C++20】

GUI 在 `QtConcurrent::run` 中调用 `run()`，`ProgressSink` 由 GUI 层适配为 Qt 信号；完成后把 `shared_ptr<const AnalysisResult>` 交给各视图的 `QAbstractItemModel`。

---

## 9. 首个可运行 MVP

**目标**：在真实 Verilog 小工程上，端到端地发现单比特/多比特未同步跨域和一级同步器问题，并在 GUI 中双击跳到源码。

| 范围 | MVP 包含 | MVP 不包含（明确标注） |
|---|---|---|
| 语言 | Verilog-2001：module/端口/wire/reg/assign/always(posedge/negedge/@*)/if/case/实例化/参数/generate-for/`include/`define/`ifdef | function/task、memory 数组的完整建模、SV 结构、VHDL |
| 时钟 | 顶层端口时钟、`create_clock`、`set_clock_groups -asynchronous`；反相器、简单门控 | 分频/MUX/PLL 原语、`create_generated_clock` |
| CDC | SDG、交叉识别、2/3 级同步器识别 | 重汇聚、复位、Gray/FIFO/握手 |
| 规则 | CDC-001、002、003、010 | 其余规则 |
| 输出 | CLI：文本摘要 + JSON 报告；退出码反映违规数 | HTML/CSV |
| GUI | 打开工程 → 运行 → 违规表格（过滤）+ 源码查看器跳转 + 时钟域列表 | 原理图、报告视图 |
| 测试 | 每条已实现规则正/反例；`examples/` 下 3 个小工程（单比特、两级同步、多比特总线） | 大工程性能 |

---

## 10. 开发路线图

估算为单人全职粗估，仅供排期参考。

| 阶段 | 内容 | 关键交付 / 验收 | 估算 |
|---|---|---|---|
| 1 工程基础 | CMake、目录、core/engine 空壳、CLI、GUI 骨架、测试框架、**slang 构建冒烟** | 见第 11 节 | 1–1.5 周 |
| 2 Verilog 前端 + IR | slang 适配、always 抽取、IR、别名并查集、诊断 | 解析回归测试 ≥30 个片段；IR dump 与期望文件比对；与 Yosys 寄存器集合差分（Linux CI） | 4–6 周 |
| 3 时钟域 | 时钟树回溯、约束子集解析、关系矩阵 | 时钟测试 ≥15 个（门控、反相、分频、约束分组） | 2–3 周 |
| 4 基础 CDC（=MVP 分析能力） | CDG/SDG、交叉识别、2FF 识别、CDC-001/002/003/010 | 每规则正/反例；examples 端到端 | 3–4 周 |
| 6a 最小 GUI | 违规表、过滤、源码跳转、时钟视图 | **MVP 达成** | 2 周 |
| 5 高级 CDC | 其余同步器、重汇聚、复位域、CDC-004~009 | 每规则正/反例；同步器三级状态在报告可见 | 6–8 周 |
| 6b 完整 GUI | 层次视图、路径图（自研分层布局，QGraphicsView）、报告视图、高亮 | — | 3–4 周 |
| 7 工程化 | HTML/CSV 报告、性能（10⁵–10⁶ 寄存器级工程）、waiver、打包（windeployqt + 安装包） | 性能基准、回归套件 | 4–6 周 |

> 顺序上建议 MVP 先于阶段 5：先把最小 GUI 做出来，让整条链路尽早可用。

---

## 11. 第一阶段：目录、接口与验收标准

### 11.1 目录（阶段 1 结束时实际存在的文件）

```
OpenCDC/
├── CMakeLists.txt                 # 顶层: C++20, 选项 OPENCDC_BUILD_GUI / OPENCDC_BUILD_TESTS
├── CMakePresets.json              # windows-msvc-debug/release, linux-gcc/clang (CI 用)
├── cmake/
│   ├── CompilerWarnings.cmake     # /W4 /permissive- ; -Wall -Wextra
│   ├── Dependencies.cmake         # FetchContent: slang(锁定 tag), Catch2 v3, nlohmann_json, CLI11
│   └── NoQtInCore.cmake           # 检查非 gui target 不链接 Qt
├── docs/
│   └── architecture.md            # 本文
├── src/
│   ├── core/
│   │   ├── CMakeLists.txt
│   │   ├── Id.h  SourceLocation.h  Diagnostic.h/.cpp  Result.h
│   │   ├── Progress.h  Cancellation.h  Version.h.in
│   ├── frontend/
│   │   ├── CMakeLists.txt
│   │   ├── Frontend.h              # IFrontend 接口
│   │   └── slang_adapter/SlangSmoke.cpp   # 阶段 1 仅: 解析文件并列出模块名
│   ├── engine/
│   │   ├── CMakeLists.txt
│   │   ├── AnalysisSession.h/.cpp  # 阶段 1: 加载工程 + 调用前端冒烟; 其余阶段返回 "NOT_IMPLEMENTED" 诊断
│   │   └── ProjectConfig.h/.cpp    # .ocdc.json 读取
│   ├── cli/main.cpp                # opencdc --version | opencdc analyze <proj.ocdc.json> [--json out]
│   └── gui/
│       ├── CMakeLists.txt
│       ├── main.cpp  MainWindow.h/.cpp   # 菜单/状态栏/8 个 Dock 占位（标注"未实现"）
├── tests/
│   ├── CMakeLists.txt
│   ├── core/test_diagnostic.cpp  test_id.cpp
│   ├── engine/test_project_config.cpp
│   └── frontend/test_slang_smoke.cpp
├── examples/
│   └── smoke/{top.v, sub.v, smoke.ocdc.json}
└── third_party/README.md          # 依赖版本、许可证清单
```
其余目录（ir/graph/clock/cdc/rules/constraints/report）在各自阶段创建，**不预先建空壳**，避免出现"看似已支持"的假象。

### 11.2 阶段 1 接口

```cpp
// core/Result.h —— C++20 下的 expected 替代
template <class T> class Result { /* value 或 Diagnostic */ };

// frontend/Frontend.h
struct FrontendInput { std::vector<std::filesystem::path> files, includeDirs; std::vector<std::pair<std::string,std::string>> defines; std::string top; };
struct FrontendSmokeInfo { std::vector<std::string> moduleNames; std::string topName; };
class IFrontend {
public:
    virtual ~IFrontend() = default;
    virtual Result<FrontendSmokeInfo> smoke(const FrontendInput&, DiagnosticSink&) = 0;   // 阶段 2 替换为 elaborate() -> Design
};
std::unique_ptr<IFrontend> makeSlangFrontend();

// engine/AnalysisSession.h (阶段 1 子集)
class AnalysisSession {
public:
    static Result<ProjectConfig> loadProject(const std::filesystem::path&);
    SmokeReport runSmoke(const ProjectConfig&, ProgressSink&, const CancellationToken&);
};
```

### 11.3 验收标准

1. Windows 10/11 x64 + VS2022：`cmake --preset windows-msvc-release && cmake --build --preset windows-msvc-release` 零错误；`/W4` 下本项目代码零警告（第三方除外）。
2. Debug 与 Release 均可构建。
3. `ctest --preset ...` 全部通过，至少包含：诊断格式化、ID 类型、工程文件读取（含错误路径）、slang 冒烟（解析 `examples/smoke`，断言模块名列表**由解析得出**而非硬编码比较以外的逻辑）。
4. `opencdc --version` 输出版本；`opencdc analyze examples/smoke/smoke.ocdc.json` 输出解析到的模块与顶层名，并明确打印"CDC 分析：未实现（阶段 4）"。
5. GUI 可启动，能打开 `.ocdc.json`，在后台线程运行冒烟分析，在 Project Explorer 中显示模块列表；未实现的 Dock 显示"未实现"字样。
6. `gui` 以外的 target 不链接 Qt（CMake 检查强制）。
7. `third_party/README.md` 列出全部依赖的版本与许可证。

### 11.4 关于构建验证（需要你知悉）

- 我的执行环境是 Linux 云容器，**无法直接运行 MSVC/VS2022**。我能做到的验证：在 Linux 上用 GCC/Clang 构建并跑 CTest（真实运行，不虚构结果）。
- Windows x64 + MSVC 的构建验证需要：(a) 你在本地 VS2022 构建并把输出反馈给我；或 (b) 配置 GitHub Actions `windows-latest` 自动构建（推荐，阶段 1 一并提供 workflow）。
- 你当前连接的电脑看起来是 macOS，它也可以做 Clang 构建验证，但不能替代 MSVC 验证。

---

## 12. 主要技术难点与风险清单

| # | 难点/风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | always 块语义抽取（阻塞/非阻塞混用、复杂 case、for 展开、task/function） | 寄存器识别错误会导致后续全部错误 | 符号执行 + 明确支持清单 + Yosys 差分测试 |
| R2 | 时钟识别的歧义（多边沿敏感、时钟 MUX、PLL 原语） | 误报/漏报 | 歧义即诊断；原语库描述文件；约束优先 |
| R3 | 约束中的名字与 RTL 名不一致 | 约束失效 | 分级匹配 + `CON-UNMATCHED` 诊断 |
| R4 | 位精度与性能的平衡 | 大工程内存/时间 | 字级 IR + depKind 位映射 + 记忆化锥 + 实例偏移扁平化 |
| R5 | 同步协议不可静态证明 | 用户误以为"安全" | 三级状态 + 列出未验证条件 |
| R6 | slang API 版本变化 | 升级成本 | 适配层隔离 + 锁定版本 |
| R7 | Windows 构建验证不在我环境内 | 可能出现仅 MSVC 的编译错误 | GitHub Actions windows-latest CI |
| R8 | Qt 许可证（LGPLv3）与发布方式 | 商业发布合规 | 动态链接 Qt；`gui` 独立 target |

---

## 13. 需要你确认的事项

1. **前端**：是否同意集成 slang（MIT）而非自研解析器？
2. **未知时钟关系的默认策略**：`report`（按异步分析 + 标注 CDC-010 待确认，建议）还是 `assume_async`？
3. **Windows 构建验证方式**：GitHub Actions（需要一个 GitHub 仓库）还是你本地 VS2022 构建后回传日志？
4. **测试框架**：Catch2 v3（建议，轻量）还是 GoogleTest？
5. **项目许可/发布意图**（开源、内部、商业）：影响 Qt 的链接方式与 VHDL 阶段能否使用 GPL 组件。
6. **依赖获取方式**：CMake `FetchContent`（建议，零额外工具）还是 vcpkg？Qt 6 默认使用官方安装器安装，通过 `CMAKE_PREFIX_PATH` 指定。

---

## 14. 实现与设计的差异（随阶段更新）

| 设计（本文） | 实现 | 原因 |
|---|---|---|
| 寄存器是 `CellKind::Register` 的一种 Cell | `Register`、`Latch` 是 `ModuleBody` 中独立的对象，组合单元才是 `Cell` | 时序元件的字段（时钟、复位、使能）与组合单元差异大，独立类型更清晰 |
| 端口连接在 IR 构建时就用区间并查集合并别名 | IR 保留原始的端口绑定（`PortBinding`）和 `Connection`；别名合并放到阶段 3/4 的图构建中做 | IR 保持与源码一一对应，便于输出和定位；合并属于分析视图 |
| `SourceLocation` 使用 `FileId` | 暂时直接保存 UTF-8 路径字符串 | 尚无文件表需求；以后改为 `FileId` 时接口位置不变 |
| 未规划 | 结构哈希、无用单元清理、无用存储清理（活性分析） | 真实工程的压力测试中发现，不清理会导致锁存器误报和寄存器数量虚高 |
| 同步复位：选择信号固定为常量的 Mux | 额外要求选择信号是具名信号（可以取反） | 避免把 `case` 译码出的常量误判为复位 |
| 时钟和依赖分析基于区间并查集的全局别名合并 | `graph::DesignView` 按需追溯驱动：沿连接和端口绑定逐跳走，结果由使用方记忆化 | 内存与实例数无关（模块体共享）；只为实际查询的位付出代价。阶段 4 的 SDG 在它上面构建 |
| 时钟 MUX 的输出作为一个新时钟 | 寄存器同时属于 MUX 的每一个候选时钟（`RegisterClocking::clocks` 是列表） | CDC 检查需要对每个可选时钟分别判断；与商业工具"传播所有时钟"的做法一致 |
| 约束中的内部 pin 名采用"精确、去后缀、模糊"三级匹配 | 名字使用 Vivado 风格的 `/` 层次分隔；寄存器引脚按 `<net>_reg[i]/Q`、`/C` 规则映射；匹配不到时报告 `CON-UNMATCHED` | 规则明确、可预期，不做模糊猜测 |
| SDG 节点为"寄存器向量位段"，边带组合锥 ID | SDG 以"寄存器输入引脚位"为单位记录上游源，按源节点聚合成位区间（hull），并带 `kLogic`/`kSelect` 标志；只经过导线或反相器的单比特连接单独精确记录（`BitInput::direct*`） | 让内存与设计规模成线性关系（6 万寄存器的设计增加不到 10 MB）。同步器识别所需的位精度由 direct 字段保证 |
| 组合锥在 CDG 上按需重建路径 | 路径重建沿 `DesignView::driver` 的 hops 和单元输入回溯，选第一个能到达源节点的输入 | 只为报告出的路径付出代价 |
| 交叉分组"按目标寄存器向量 + 源寄存器向量" | 按（源节点、目标寄存器、目标引脚）分组；宽度 = max(源位数, 目标位数)，存储器源按元素宽度计 | 引脚区分数据/控制；存储器若按整个数组计宽度会产生误导 |
| CDC-002 仅检查链长 | 还检查"第一级是否有额外扇出"（第一级被其他逻辑使用时，那些读者只经过一级同步） | 这是常见的真实错误 |
| 受控（MUX/使能型）数据：选择信号直接来自同步器 | 在目标寄存器的使能锥和数据锥的选择输入中查找，可沿目标域寄存的控制逻辑追溯最多 2 级；排除同步复位输入，并排除源触发器 D 端为常量的同步器（复位桥） | 真实设计（verilog-axis 异步 FIFO）中，限定信号经过比较逻辑和寄存器；复位同步器若被误当作限定信号会造成漏报 |
| 输入端口的时钟域"由约束或配置指定，否则 Unknown" | 没有 `set_input_delay -clock` 的输入不参与 CDC 分析，以 `CDC-NO-DOMAIN` 列出 | 若按 Unknown 处理，单时钟设计的每个数据输入都会被报告 |
| 异步复位跨域在交叉识别中一起处理 | 阶段 4 只统计；阶段 5 起生成路径，按（复位源、目标时钟、复位同步器）分组（`groupSize`），由 CDC-006 判断。没有时钟域的输入或黑盒作为异步复位源时也生成路径（`srcUnclocked`，判为 PotentialRisk）；用 `set_input_delay -clock` 声明后视为同步 | 一个外部复位常驱动成千上万个寄存器，不分组会淹没其他结果；外部复位天然与所有时钟异步 |
| `SyncType` 含 Pulse、MuxEnable、Unknown | 实现为 Toggle（包含脉冲同步器）、QualifiedData（即 MuxEnable）；不设 Pulse、Unknown。协议类类型（Toggle/Gray/Handshake/AsyncFifo）沿用 `syncStages` 描述底层触发器链 | 脉冲同步器的结构就是 toggle 加边沿检测；边沿检测是否存在作为说明给出，不影响判定 |
| Gray 识别：源端存在 `g = b ^ (b >> 1)`，目标端存在 gray→bin | 只检查源端：沿 XOR/XNOR、NOT、常量移位、带常量分支/保持分支/常量选择的 MUX 追溯，要求 `D[k] = X[k] ^ X[k+1]`、`D[N-1] = X[N-1]`；另外证明二进制值每次至多 ±1（加/减一位增量，含 Verilog 位宽扩展后的常量 0；或"增量/保持"MUX），证明成功则为"必要条件已验证" | 目标端是否转回二进制不影响跨域安全性；"每次只变一位"是 Gray 同步成立的真正条件，能静态证明时就不留作未验证项 |
| 同步链：中间级只驱动下一级 | 当链长已达到 `minSyncStages` 后，某级若还被其他逻辑读取，链在该级结束（该级就是同步器输出） | `s1 → s2 → s3; pulse = s2 ^ s3` 中 s3 是边沿检测的延迟寄存器，不是第三级 |
| CDC-005 标签按 SDG 节点 | 标签按"寄存器输入位"记录（使能/同步复位作用于整个寄存器），标签集合驻留去重、并集记忆化；同一源寄存器的不同位分别同步不算重汇聚（由 CDC-008 报告），同一位同步两次（发散）算重汇聚 | 按寄存器记录会把打包的状态寄存器误报为重汇聚；驻留集合让宽数据通路（4000 位加法器）的内存保持线性 |
| CDC-008 只针对"同一源向量经逐位同步链" | 还包括"同一多比特源寄存器的各位经各自独立的单比特同步器"（如 generate 例化的 `sync_2ff`）；目标寄存器同时读取两位以上为 PotentialRisk，否则 NeedsReview | 后一种写法在真实设计中很常见（smoke 示例的 `status[3:0]`） |
| CDC-006：顶层端口复位"声明为已外部同步"时为 NeedsReview | 没有声明机制时，无时钟域的输入作为复位源判为 PotentialRisk；用 `set_input_delay -clock` 声明 | 复用已有约束命令，不引入私有语法 |
| CDC-007：无周期信息只报 NeedsReview | 同上；"可能为脉冲"的判据：无使能、无自反馈、由逻辑计算；只复制同域寄存器时沿用其结论；只复制输入端口时不判断 | 只复制输入的寄存器波形未知，若一律报告会在几乎每个同步器上产生噪声 |
| 未知时钟关系只把 Violation 降为 NeedsReview | 阶段 5 起 PotentialRisk 也降为 NeedsReview | 关系未知时，任何跨域结论都需要先确认时钟关系 |
| CDC-004：同步链首级前 `combDepth>0` 即报 | 只有首级某一位依赖两个以上"异步位"（非目标时钟、非同步时钟或无时钟的源位）时才报；只与目标域信号或常量组合（使能/清零门控）视为无毛刺，记为说明 | verilog-ethernet 的帧 FIFO 用目标域复位同步信号清零首级；单一异步输入的门控不会产生毛刺，报告它会误导 |
| Toggle 只针对单比特 | 每一位都自翻转的向量（`t <= t ^ {e1, e0}`）识别为 toggle，各位是独立事件，不报 CDC-008 | verilog-ethernet 的错误状态同步就是这种写法 |
| 握手：ack 由 B 域经 2FF 回到 A 域 | ack 必须是单比特链路径；优先选择源寄存器就是 req 同步器某一级的路径，其次是 2 级内依赖它的路径 | 避免把同方向的多比特 Gray 指针误当作 ack |
| 路径图在 GUI 层用 QGraphicsView 自研分层布局 | 原理图模型、分层布局和 SVG 输出放在独立的无 Qt 库 `opencdc_schematic`；GUI 只负责绘制，CLI 用同一份结果输出 SVG（`--schematic-dir`） | 布局可以单元测试；CLI 和以后的 HTML 报告不依赖 Qt |
| 路径图只画一条路径 | 画出该路径、进入同一目标引脚的全部路径（汇合逻辑完整）、同步器各级、限定信号、重汇聚点，以及所属同步结构的全部成员路径（最多 24 条） | 只画一条路径时，OR 门的另一个输入、FIFO 的反向指针都看不到 |
| `SeqGraph::consumersOf` 每个读者位一条记录 | 连续且读取方式相同（非直连、源位区间和标志相同）的读者位合并为一条（`Consumer::bitEnd`） | 4000 位全相关加法器读 4000 个同步器输出时，逐位记录需要 1600 万条 |
| 同步链：只检查连接关系 | 链的下一级带使能（Enable 引脚）时不算同步级；首级带使能时不算两级同步器，而是作为受限定数据去找限定信号 | verilog-ethernet 中被使能控制的捕获寄存器曾被误认作同步级 |
| 豁免是报告层的过滤 | 豁免（`rules/Waiver`）在规则检查之后作为独立阶段应用：命中保留，标记 `waiver`；路径状态由未被豁免的命中重新计算；过期和未命中的豁免给出警告。文件格式和校验在 `engine/WaiverFile` | 所有输出（CLI、JSON、HTML、CSV、GUI）看到同一份结果，被豁免的问题不会消失，只是不再计为违规 |
| 报告模块输出 HTML/CSV/JSON | `engine/Report` 生成自包含 HTML（内联 CSS 和 SVG，不需要脚本）与 CSV（RFC 4180，带 BOM）；JSON 改为流式写出（`writeJson`），输出与原 DOM 方式逐字节相同 | 10⁶ 寄存器时 JSON 达 760 MB，用 DOM 会使峰值内存翻三倍 |
| HTML 报告列出全部路径 | 超过 `maxPaths`（默认 5000）时只列有规则命中或状态不是 safe 的路径；超过 `maxSchematics`（400）时不嵌入原理图 | 17 万条路径的 HTML 有 390 MB，浏览器打不开；完整数据在 CSV 和 JSON 中 |
| 每个阶段只报告状态 | `StageReport::seconds` 记录每个阶段的耗时，CLI、JSON、HTML 都输出 | 性能回归和用户排查都需要 |
| GUI 表格使用 QTableWidget | 路径表和违规表改为 `QAbstractTableModel` + `QSortFilterProxyModel`，不复制数据；报告页超过 300 个同步结构时按类型汇总 | 17 万条路径时，逐单元格的 item 占用 2 GB 内存、填充需 15 s |
| 打包：windeployqt + 安装包 | CMake `install()` 只安装 `OpenCDC` 组件（FetchContent 依赖自带的安装规则被排除）；Windows 用 `qt_generate_deploy_app_script` 在安装时运行 windeployqt；CPack 生成 ZIP/NSIS（Windows）、TGZ/DEB（Linux）；`package.install_smoke` 测试安装后的树 | 安装后的程序必须能单独运行；只有安装测试能证明这一点 |
| slang 使用默认编译选项 | `maxGenerateSteps` 提高到 2²⁴ | 默认值 131072 会把 34 万次迭代的合法 generate 当作死循环 |

| 计划：VHDL 前端在阶段 8 再决策 | 自研 VHDL-93/2008 前端 `opencdc_vhdl`（解析、语义、展开、降级到同一种 Design IR），不依赖 slang 和 Qt；与 Verilog 共用 `ir::BodyEditor`（折叠、结构哈希、控制信号提取、清理） | 许可证干净；源码位置完整；两种语言的 IR 质量一致 |
| 混合语言：生成桩模块 | Verilog 中未知的 VHDL 实体保留为"未解析实例"（带参数值和端口实参），由链接器绑定到 VHDL 模块体；VHDL 中的 Verilog 模块通过生成的包装模块请求，迭代到不再出现新的参数组合 | 桩模块需要按参数计算端口宽度，等于在 Verilog 中重写 VHDL 的类型系统；未解析实例加迭代只依赖两侧各自的展开结果 |
| 宽单元逐位建立依赖锥 | 宽度超过 4096 位的逐位单元（整块存储器的写入逻辑）作为一个整体建锥；大锥的合并用索引 | neorv32 的 64 KB 存储器使 CDC 路径分析从 7.6 s 降到 0.34 s，结果不变 |
| 厂商原语：要求用户提供仿真库 | 内置可综合的行为模型（Verilog）和 VHDL 组件包，只在设计实例化了原语且源文件没有定义时加入；MMCM/PLL 的模型按实例使用的参数名生成 | 仿真库含大量不可综合代码和加密模型；只需描述 CDC 相关的结构（触发器、同步链、时钟关系） |
| 寄存器控制信号：原样保留 | 经层次连到无效常量的使能、同步复位去掉；异步复位只有全部为常量无效时才去掉 | `FDRE` 的 `CE=1'b1`、`R=1'b0` 在模型内部是端口，不去掉会把同步链的每一级都当作带使能的捕获寄存器 |
| MUX 原样保留 | `BodyEditor` 把"两个数据输入在某位上相同"的 MUX 输出位替换为该输入位（生成单元时和结束时各做一次） | 两进程写法中记录的未修改字段要经过每一级 if 的整记录 MUX；不替换会使常量位看起来依赖选择信号（surf FIFO 的 CDC-007 误报） |
| 时钟进程：只认 if/elsif 链 | 时钟 if 之后的 `if <cond> then ... end if` 是最高优先级的异步复位（VHDL 中后一条赋值生效） | surf 的 `Synchronizer` 用这种写法 |
| 接口：不支持 | 接口实例展平到声明它的模块；接口端口按使用的成员拆成普通端口（第 17.3 节） | 保持 IR 只有模块和普通端口，后续分析无需了解接口 |
| 派生时钟没有周期 | 时钟生成器模型通过属性给出周期比例，时钟分析计算周期 | CDC-007 需要周期；Vivado 也从 MMCM 参数自动推导这些时钟 |
| Gray 识别：一个多比特源寄存器 | 还识别"按位拆开的寄存器组"（`xpm_cdc_gray` 每一位一条 generate 链）：同一 generate 的兄弟寄存器合起来证明 Gray 编码 | XPM 宏和很多手写代码都按位生成同步链 |

## 15. VHDL 前端（阶段 8）

### 15.1 结构

```
src/frontend/vhdl/
  VhdlLexer / VhdlParser   VHDL-93/2008 词法和递归下降语法分析 → AST（名字保持通用形式）
  VhdlTypes                类型、编译期值、值与 IR 位的映射
  VhdlScope                库、包、可见性（use/library/context）、声明
  VhdlBuiltins             std / ieee 包的原生实现（std_logic_1164、numeric_std、numeric_bit、
                           numeric_std_unsigned、std_logic_arith/unsigned/signed/misc、math_real、textio）
  VhdlExpr / VhdlNames     重载决议、表达式求值（常量与硬件统一）、名字（下标、切片、字段、属性、调用）
  VhdlStmt                 顺序语句的符号执行，进程降级为寄存器 / 组合连接 / 锁存器
  VhdlElab                 层次展开：实体 + 结构体 + generic 值 → 唯一模块体；generate、block、端口映射
  VhdlMixed                与 Verilog 之间的值和端口转换
  VhdlFrontend             IFrontend 实现
src/frontend/AutoFrontend  按扩展名选择前端；混合工程的链接器
```

### 15.2 统一求值

每个表达式求值为 `SVal {类型, 可选的编译期值, IR 位}`。输入全部已知（generic、常量、循环变量、字面量）时得到编译期值，否则生成 IR 单元。所以 generic 相关的宽度、generate 范围、常量表、`log2ceil` 之类的用户函数和进程逻辑由同一份代码处理。子程序在调用处内联；循环展开；`return`、`exit`、`next` 用执行条件（alive）表示。

### 15.3 位布局

- 数组：最左元素在最高位；记录：第一个字段在最高位，信号和端口按字段拆成独立的线网（`r.Cnt`）。
- 枚举按位置编码；`std_ulogic` 为 1 位（'0'/'L'→0，'1'/'H'→1，'Z'→Z，其余→X）。
- 整数的宽度由范围决定，范围含负数时为有符号。
- 元素不是 1 位标量的一维数组标记为存储器（`arrayDepth`/`elemWidth`）。

### 15.4 进程

- 时钟进程：`rising_edge`/`falling_edge`、`clk'event and clk = '1'`、`wait until`；if 链中边沿分支之前的分支是异步复位（条件为常量假时去掉，例如 `RST_ASYNC_G and rst = '1'`）；`edge and en` 中的 `en` 成为使能；同步复位和使能由 `BodyEditor::extractControls` 提取。
- 变量：存储值被读到时才成为寄存器；整个覆盖的写不算读取。
- 组合进程：每条路径都写到的位成为组合连接，其余成为锁存器（报告 `IR-LATCH-INFERRED`）。
- 含其他 `wait` 的进程视为测试平台代码，不分析（`VHDL-TESTBENCH`）。

### 15.5 混合语言

1. slang 以 `IgnoreUnknownModules` 编译 Verilog 源文件和一个生成的包装模块；对 VHDL 实体的实例记录参数值（从语法重新绑定并求值）和端口实参。
2. VHDL 展开器把模块体加入同一个 Design。Verilog 中的 VHDL 实例按名字（不区分大小写）或位置绑定端口；输入宽度不一致时按 Verilog 规则补零或截断。
3. VHDL 中的 Verilog 实例向包装模块请求 `模块 #(参数)`；本轮没有的请求触发下一轮（最多 16 轮），最后一轮的诊断才报告。
4. 结果只保留从顶层可达的模块体；两种语言都定义的名字给出 `FE-MIXED-DUPLICATE`。
5. 记录与结构体：VHDL 记录和 SystemVerilog packed struct 都是第一个字段在最高位，整端口直接按位相连。slang 适配层把 packed struct 的字段布局（含嵌套字段，`hdr.len`）记在 `ir::Net::fields` 中；VHDL 对 Verilog 端口的部分关联（`i.hdr.len => x`、`d(3) => y`、`d(0 to 3) => z`）按字段布局和端口的声明范围换算成位偏移，未关联的位为常量 X（输出端口上不产生驱动）。

## 16. 厂商原语与工程编辑（阶段 9）

### 16.1 原语库

```
src/frontend/primitives/   xilinx_unisim.v、xilinx_xpm.v、intel.v（可综合行为模型）
                           unisim_vcomponents.vhd、xpm_vcomponents.vhd、altera_mf_components.vhd（VHDL 组件包）
cmake/EmbedFiles.cmake     构建时把这些文件嵌入为 C++ 字符串（PrimitiveSources.cpp）
src/frontend/Primitives    PrimitiveLibrary：查找（不区分大小写）、按依赖拼出模型、时钟生成器模型
```

1. Verilog 工程：slang 报告的未知模块中有原语时，改走混合链接器；链接器把用到的模型写入 `temp/opencdc-primitives-<版本>/`，作为额外的源文件。源文件中同名的模块优先。
2. VHDL 工程：用到 `unisim`/`xpm`/`altera_mf` 库时，加入对应的组件包；组件的实例按外语言实例处理，由 Verilog 模型提供模块体。
3. MMCM/PLL：模型按实例实际使用的参数名生成（参数多、取值各异，固定模型无法覆盖），每个 `CLKOUT*` 是由 `CLKIN1` 上升沿驱动的翻转寄存器，于是时钟分析把它识别为 `CLKIN1` 时钟的分频派生时钟：同一 MMCM 的各输出互为同步，与输入时钟的约束关系（`set_clock_groups`）一致。`LOCKED` 是寄存器。模型签名在迭代中收敛（新参数组合触发下一轮）。
4. `ASYNC_REG` 可以写在实例上（Verilog 属性，VHDL 的 `attribute ... of label : label is`），记在 `ChildInstance::attrs` 中。

### 16.2 工程编辑

`engine::projectToJson` / `saveProjectFile` 写出工程文件（路径相对于工程文件，`/` 分隔；先写临时文件再替换）；与 `parseProjectJson` 往返不变。GUI 的 `ProjectEditor` 对话框只编辑 `ProjectConfig`，保存和重新分析由主窗口负责。`opencdc-gui --edit-test` 是无界面的自测：编辑、保存、重新打开、分析。

## 17. 时钟频率、存储器原语与 SystemVerilog 接口（阶段 10）

### 17.1 时钟生成器的输出周期

MMCM/PLL 模型的每个输出寄存器带三个属性：`OPENCDC_CLOCK_RATIO`（`DIVCLK_DIVIDE × CLKOUTn_DIVIDE / CLKFBOUT_MULT`，按实例参数由 slang 求值）、`OPENCDC_CLOCK_INPUT_PERIOD`（`CLKIN1_PERIOD`）、`OPENCDC_CLOCK_PIN`（引脚名）。时钟分析建立分频时钟时读取这些属性：时钟名改为 `实例.引脚`；在生成时钟的主时钟解析之后，周期 = 主时钟周期 × 比例，主时钟没有周期时用 `CLKIN1_PERIOD` × 比例。链式生成器（MMCM 驱动 MMCM）按固定点迭代。

### 17.2 存储器与 DDR 原语

- 块 RAM 和 XPM/Intel 存储器：每个写端口写自己的数组，每个读端口读取所有数组（按位异或合并）。这样"端口 A 写、端口 B 读"在两个时钟之间形成跨域，同一端口的读写不跨域；共用时钟的模式（`common_clock`、`address_reg_b = "CLOCK0"`）不跨域。
- DDR 输入：下降沿采样寄存器 + 上升沿输出寄存器；DDR 输出：两个上升沿寄存器经或门到引脚。
- `IDELAYCTRL.RDY`、MMCM `LOCKED` 由内置黑盒 `opencdc_async_source` 驱动：没有时钟域。
- 宽容处理：模型不需要的参数（`INIT_xx` 等）忽略；模型没有的引脚（级联、ECC）的连接作为 `FE-PRIMITIVE-PORT` 信息报告，Verilog（slang `PortDoesNotExist` 降级）和 VHDL（`ElabOptions::lenientModules`）一致。VHDL 中没有类型上下文的聚合 generic（`INIT_00 => (others => '0')`）跳过。

### 17.3 SystemVerilog 接口

| 构造 | 处理 |
|---|---|
| 接口实例 `bus_if ia(clk)` | 展平到声明它的模块体：成员为 `ia.data` 线网，接口内的 always/assign 在该模块体中降级，接口端口连接成为连续赋值 |
| 接口端口 `bus_if.src b` | 模块体中被引用的每个成员成为一个端口 `b.data`（`ifaceTarget` 把 modport 端口解析到接口成员，按成员相对接口实例的路径命名） |
| 端口方向 | modport 给出 input/output 时用之；否则（通用 `interface` 端口、modport inout）按使用：本模块或子模块驱动它则为输出 |
| 父模块绑定 | 按端口名前缀找到连接的接口实例（或接口数组元素 `b[1]`），在实例中按路径查找成员，实参为父模块中该成员的线网（本地展平的或父模块自己的接口端口） |
| 接口端口继续向下传递 | 同一机制递归；子模块驱动的成员使父模块的对应端口成为输出 |
| 只含断言的 always 块 | 跳过（不是硬件） |
| 不支持 | 接口中的函数/任务、modport 表达式、clocking block、虚接口、跨模块的普通层次引用（仍为 `IR-UNSUPPORTED-EXPR`） |

slang 对相同参数、相同接口连接定义的实例共享规范模块体；端口按"端口名.成员路径"命名而不是按具体接口实例命名，所以共享的模块体对每个实例都正确。

# 阶段 82：FuseSoC 核心 —— lowRISC ibex 演示系统

日期：2026-11-09　版本：0.82.0

前几个阶段验证的工程都有厂商工程文件（`.xpr`、`.qpf`、`.gprj`、`.ldf`）或 Makefile。很多开源 RISC-V 设计用 **FuseSoC** 组织：每个 IP 是一个 CAPI2 核心文件（`.core`，YAML），写明文件集、依赖、目标、参数和生成器。这一阶段选择 **lowRISC/ibex-demo-system**（提交 d37beb7，Apache-2.0）：Ibex 处理器加 PULP 调试模块，运行在 Arty A7 上，顶层核心是 `ibex_demo_system.core`，依赖 60 个核心。

这个工程有几点不同：
- 原语库 `prim` 的抽象原语（`prim_flop`、`prim_ram_1p` 等 9 个）和 `prim_pkg` 不在仓库中，由 FuseSoC 生成器 `primgen`（Python 程序）在构建时生成；
- 文件集按条件选择，例如 `tool_vivado ? (files_xilinx)`；目标通过 YAML 锚点与合并键（`<<: *default_target`）共享设置；
- 调试模块有两个 JTAG TAP（`BSCANE2`，链 3 和链 4）；
- 大量使用 SystemVerilog `inside` 与 `case ... inside`。

本环境不允许运行仓库中的外部代码（安装和运行 fusesoc 被拒绝），所以导入和 `primgen` 都由 OpenCDC 自己实现，不运行任何生成程序。

第一次分析（导入完成后）：
- 0 个错误，23 个告警；
- 两个 `BSCANE2` 的 TCK 被当作两个无关时钟，它们之间有 4 条“未知关系”的路径；
- 16 条跨时钟路径。

修复后：
- 0 个错误，1 个告警（`ibex_top.sv` 中一个只用于 lint 的变量的非常量初始化）；
- 3 个时钟，271/271 个寄存器的时钟已解析；
- 26 条跨时钟路径：24 条已同步（调试模块 `dmi_cdc` 在 TCK 与系统时钟之间），2 条未同步的是异步输入（复位、按键）。

工程已加入语料库。

## 1. 完成的功能

### 1.1 YAML 子集（`src/engine/Yaml.{h,cpp}`）

- 块映射与块序列，包括与键同缩进的序列、`- key: v` 形式的序列项；
- 流式 `{}`/`[]`（可跨多行）；单引号、双引号与普通标量；注释；
- 锚点与别名（`&name`、`*name`），合并键 `<<`（映射自己的键优先）。
- 不支持：多行块标量（`|`、`>`）、标签、多文档；未闭合的引号会报错。
- 在 ibex-demo-system 的全部 220 个核心文件上解析，0 个失败。

### 1.2 FuseSoC 核心导入（`src/engine/FuseSocProject.{h,cpp}`）

`.core` 文件可以直接分析，也可以用 `opencdc import` 写成 OpenCDC 工程；`--target` 选择目标（默认：有 `synth` 用 `synth`，否则 `default`）。GUI 的打开对话框也接受 `.core`。

| 内容 | 处理 |
|---|---|
| 核心 | 扫描所给核心所在目录下的所有 `.core`（跳过 `.git`、`build`、`node_modules`）；同一核心有多个版本时取最高版本 |
| 目标 | 目标的 `filesets` 与 `filesets_append`；依赖使用它们的 `default` 目标 |
| 条件 | `[!]flag ? (...)`（可嵌套），标志为 `tool_<工具>`（目标的 `default_tool`，否则 `vivado`）、`target_<目标>`、`is_toplevel` |
| 顺序 | 后序遍历：依赖在前，然后是目标的生成器，最后是核心自己的文件 |
| 文件类型 | `systemVerilogSource*`、`verilogSource*`、`vhdlSource*`（库名取 `logical_name`）为源文件；`is_include_file` 给出包含目录（`include_path`、`copyto`）；`xdc`、`sdc` 为约束；其他类型计数后报告（`PRJ-CORE-SKIPPED`） |
| 参数 | `vlogdefine` 为宏；`vlogparam`、`generic` 为顶层参数（`str` 加引号，`bool` 为 1/0） |
| `SYNTHESIS` 宏 | 工具是综合工具（Vivado、Yosys 及基于它的流程）时定义，与工具本身的行为一致。这样只用于仿真的代码（lowRISC 的 `INC_ASSERT`、读取子模块内部信号的断言辅助逻辑）被排除，和真实综合一致 |
| 生成器 | lowRISC `primgen` 由 OpenCDC 生成（见 1.3）；其他生成器是程序，不运行，报告 `PRJ-CORE-GENERATOR` |

报告：找不到的依赖（`PRJ-CORE-MISSING-DEP`）、无法读取的核心（`PRJ-CORE-UNREADABLE`）、目标不存在（`PRJ-CORE-TARGET`，错误）、导入摘要（`PRJ-CORE-IMPORT`）。

### 1.3 lowRISC primgen

按 `primgen.py` 的规则生成：
- `generate_prim_pkg`：`prim_pkg.sv`，枚举 `impl_e` 先是 `ImplGeneric`，然后是找到的各技术库（`lowrisc:prim_<库>:*`）按名字排序；
- `prim_name: <名字>`：抽象原语 `prim_<名字>.sv`。模块头（导入、参数、端口）从通用实现 `prim_generic_<名字>.sv` 中读取（先去掉注释），然后按 `PRIM_DEFAULT_IMPL`（默认 `ImplGeneric`）用 if/else 链实例化各库的实现，参数逐个传递，端口用 `.*` 连接。
- 先导入 `prim_pkg` 与各技术库的核心，再写抽象原语。
- 没有通用实现时报告 `PRJ-CORE-GENERATOR`。
- 生成的文件写在系统临时目录 `opencdc-fusesoc-<哈希>` 下；内容不变时不重写。报告 `PRJ-CORE-PRIMGEN`，列出生成的文件。

### 1.4 SystemVerilog `inside` 与 `case ... inside`

以前两者都是未分析的构造（ibex 中 8 处）。现在：
- 值：通配相等（项中的 x/z 位匹配任意值）；完全确定的常量用普通相等；两边都是常量时直接求值；
- 范围 `[a:b]`：`>= a` 且 `<= b`；`$` 表示无下界或无上界；两边都是有符号数时按有符号比较；
- `inside` 是所有项的或；`case ... inside` 每个分支的条件同样构造；
- `case ... inside` 的完整性检查（没有 default 时是否推断锁存器）也计入常量范围与通配值；
- 不支持的项（容差范围 `+/-`、`+%-`，非打包数组）仍然报告为未分析。

### 1.5 所有 `BSCANE2` 共用一个 JTAG TCK

- 器件只有一个 JTAG 端口，每个 `BSCANE2`（不同的 `JTAG_CHAIN`）输出的都是同一个 TCK。
- 模型的 TCK 源加上属性 `OPENCDC_CLOCK_SHARED = "xilinx_jtag_tck"`。时钟分析中，带同一键的黑盒时钟只建一个：以第一个实例命名，其他实例的名字作为别名，约束对任一实例的引脚都有效。
- 修复前 ibex 的两个 TAP（`dtmcs` 与 `dmi`）之间有 4 条“未知关系”的路径，现在它们在同一时钟域。

## 2. 真实工程验证

### ibex-demo-system（直接分析 `ibex_demo_system.core`，加入语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 导入 | 160 个源文件、1 个约束文件、60 个核心、9 个 primgen 文件 | 同左，另定义 `SYNTHESIS` |
| 错误 / 告警 | 0 / 23 | 0 / 1 |
| 未分析的构造 | `inside` ×4、`case inside` ×4、子模块内部信号的层次引用（多处）、非常量初始化 ×1 | 非常量初始化 ×1 |
| 时钟 | 4（两个 TCK） | 3：`sys_clk_pin`、PLL 的 `CLKOUT0`（50 MHz）、JTAG TCK |
| 跨时钟路径 | 16（10 条已同步） | 26（24 条已同步，2 条未同步） |

- 层次引用全部在 `` `ifdef INC_ASSERT `` 中，只用于断言和功能覆盖。定义 `SYNTHESIS` 后 `prim_assert.sv` 不再定义 `INC_ASSERT`，这些代码不参与分析，和 Vivado 的综合一致。
- `inside` 分析后，调试模块的握手逻辑连通，路径从 16 条增加到 26 条，新增的都是 `dmi_cdc` 中已同步的路径。
- TCK 与系统时钟没有约束，22 条路径标为“需要复核”，`--suggest-sdc` 可以给出 `set_clock_groups`。
- 2 条未同步路径是 `IO_RST_N` 等异步输入进入 `CLKOUT0` 时钟域。

### 其他语料

- SV 语料 37 个项目全部匹配（新增 ibex-demo-system），其余 36 个与修改前逐行相同。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- 新增 `src/engine/Yaml.{h,cpp}`、`src/engine/FuseSocProject.{h,cpp}`；`src/engine/CMakeLists.txt`。
- `src/engine/ProjectConfig.{h,cpp}`：`.core` 为厂商工程文件；`loadProjectFile` 增加目标参数。
- `src/cli/main.cpp`：`analyze`、`import` 的 `--target`，帮助文字；`src/gui/MainWindow.cpp`：打开对话框的过滤器。
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：`inside`，`insideItem`。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：`case ... inside` 及其完整性检查。
- `src/frontend/primitives/xilinx_unisim.v`：`BSCANE2` 的 `OPENCDC_CLOCK_SHARED`。
- `src/clock/ClockAnalyzer.cpp`：共用的黑盒时钟。
- 测试（每个用例都含反例）：
  - `tests/engine/test_yaml.cpp`：3 个用例（映射/序列/标量/注释、锚点与合并键、不支持或格式错误的文档）；
  - `tests/engine/test_fusesoc_project.cpp`：4 个用例（synth 目标、依赖顺序与版本、条件、参数、`SYNTHESIS`；另一个目标；错误与生成器；primgen 输出并完成分析）；
  - `tests/ir_extract/test_ir_extract.cpp`：2 个用例（`inside`、`case ... inside` 与锁存器）；
  - `tests/frontend/test_primitives.cpp`：1 个用例（两个 `BSCANE2` 一个时钟，设计寄存器仍然跨域）。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：ibex-demo-system（稀疏获取，不含软件、文档和 Ibex 中的测试套件）。
- `CMakeLists.txt`：版本 0.82.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：667 个测试全部通过。
- Release 构建一次失败：`case inside` 范围边界的 `std::optional` 被 GCC `-O3` 报告“可能未初始化”（`-Werror`）。改为普通变量加有效标志后通过。
- 我的两处测试预期写错，已按实际行为改正：`case inside` 的常量项是 32 位（与 `case` 相同，选择器被扩展到 32 位）；完整的 `case` 最后一项条件为常量真，所以测试中加了第三个分支才能看到中间分支的比较。
- SV 语料 37 个、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 81 的 Windows CI（提交 0729d19，运行 42584253226）：Release 与 Debug 均 657/657 通过。

## 5. 已知限制

- 除 `primgen` 外的 FuseSoC 生成器（如 OpenTitan 的 `reggen`、`tlgen`）不运行，需要先用 `fusesoc run --setup` 生成后写入 OpenCDC 工程。
- 依赖的版本约束（`>=`、`~` 等）不检查，总是用找到的最高版本；只在所给核心的目录下查找核心，不读 FuseSoC 的库配置（`fusesoc.conf`）。
- `opencdc import` 写出的工程引用临时目录中的 primgen 文件，临时目录被清理后需要重新导入。
- 带非常量初始化的变量声明（`logic x = expr;`）仍然是未分析的构造。
- 容差范围（`[a +/- b]`）的 `inside` 不分析。

## 6. 下一步

阶段 83：继续用真实工程验证。候选：
- 一个使用 FuseSoC 的 VHDL 或混合语言设计，或者 OpenTitan 的一个较小的顶层（需要处理 `reggen` 生成的文件）；
- 变量声明的非常量初始化（按综合工具的处理方式）；
- Intel 的 `sld_virtual_jtag` 与 Lattice 的 `JTAGG`，与 `BSCANE2` 一样共用 JTAG TCK。

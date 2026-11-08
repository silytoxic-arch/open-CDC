# 阶段 60：Quartus 工程（.qpf/.qsf）；Quartus 的宽松写法；Cyclone V 的 HPS 原语

日期：2026-11-08　版本：0.60.0

阶段 59 支持了 Vivado 工程。这一阶段支持 Intel Quartus 工程。真实工程是 MiSTer：DE10-Nano 开发板（Cyclone V，带 ARM 处理器 HPS）上的复古游戏平台，有数百个核心，都使用同一套框架 `sys/`。

- **Template_MiSTer**（GPL-2.0）：框架和模板核心，加入语料库；
- **NES_MiSTer**（GPL-3.0）：只用于对照，未加入语料库（见已知限制）。

## 1. 完成的功能

### 1.1 读取 Quartus 工程

Quartus 工程的组成：

- `.qpf` 列出修订版本（`PROJECT_REVISION`）；
- 每个修订版本的设置是一个 Tcl 脚本 `.qsf`，由赋值语句组成（`set_global_assignment -name VERILOG_FILE ...`）；
- `.qsf` 可以 `source` 其他脚本，可以包含 IP 文件列表 `.qip`；`.qip` 用 `$::quartus(qip_path)` 指向自己的目录。

导入时按 Quartus 的方式求值这些脚本。实现了一个小型 Tcl 求值器，支持：

- 花括号、引号、方括号命令替换、`$var`、`$::var`、`$arr(key)`、反斜杠续行；
- 命令：`set`、`source`、`file join/dirname/tail/rootname/extension/normalize/exists`、`join`、`list`、`concat`、`lindex`、`llength`、`regexp`、`string`；
- 变量：
  - `$::quartus(qip_path)` 是正在读取的 `.qip` 所在目录，与 Quartus 一样以 `/` 结尾。MiSTer 依赖这一点：`[join [list $::quartus(qip_path) pll_q [regexp -inline {[0-9]+} $quartus(version)] .qip] {}]`；
  - `$quartus(version)` 取自 `LAST_QUARTUS_VERSION` 或 `.qpf`；
- `-to SDRAM_A[0]` 中的方括号：内容不是已知命令时按字面保留（总线下标），与 Quartus 读取 `.qsf` 的方式一致。

读取的内容：

- **设计文件**：`VERILOG_FILE`、`SYSTEMVERILOG_FILE`、`VHDL_FILE`（及其 `-library`），按顺序，包括 `QIP_FILE` 列表中的文件。
  - Platform Designer 系统（`QSYS_FILE`）和 IP 变体（`IP_FILE`）读取 Quartus 为它们生成的 `.qip`（`<name>/synthesis/<name>.qip` 等）；
  - 没有生成的报 `PRJ-QSF-NOT-GENERATED` 警告，并给出生成方法。
- **路径**：相对于工程目录；在 `.qip` 中找不到时再相对于 `.qip` 所在目录。
- **其他设置**：
  - 包含目录（`SEARCH_PATH`、`VERILOG_INCLUDE_FILE` 所在目录）；
  - 宏（`VERILOG_MACRO`）；
  - 顶层参数（不带 `-to` 的 `set_parameter`）；
  - 顶层（`TOP_LEVEL_ENTITY`）；
  - SystemVerilog 模式：有文件作为 SystemVerilog 读取（`SYSTEMVERILOG_FILE`，包括 `.v` 文件），或 `VERILOG_INPUT_VERSION` 指定时。
- **约束**：`SDC_FILE`。
- **报告未读取的内容**：
  - `PRJ-QSF-MISSING`：缺失的文件；
  - `PRJ-QSF-UNSUPPORTED-FILE`：AHDL、原理图、网表；
  - `PRJ-QSF-SKIPPED`：存储器初始化文件等；
  - `PRJ-QSF-TCL`：没有求值的 Tcl 命令；
  - `PRJ-QSF-PRE-FLOW`：编译前运行、可能生成设计文件的脚本（MiSTer 的 `build_id.v`）；
  - `PRJ-QSF-REVISION`：其他修订版本；
  - 脚本错误（括号不平衡、未定义变量）时报 `PRJ-QSF-PARSE`。
- **使用方式**：与 `.xpr` 相同，即 `opencdc analyze`、`opencdc import`、GUI 打开工程。导入的工程不会写回 `.qsf`。

### 1.2 Quartus 的宽松写法

Quartus 接受一些在 (System)Verilog 中是错误的写法，MiSTer 的框架和核心大量依赖它们。以前整个设计因此无法分析。现在按 Quartus 的方式读取，每处给一个警告，然后正常分析。

- **在过程块中赋值的线网**（`FE-NET-PROCEDURAL-ASSIGN`）：
  - `wire x; always_comb x = ...`：把 `wire` 改为 `reg `（长度相同，位置不变）；
  - 没有类型的输出端口（`output [7:0] q`，按 IEEE 1800 是线网）在 `always` 中赋值：加上 `reg`；
  - `inout reg` 端口在过程块中赋值：读作输出端口，外部输入的值不分析；
  - 声明可以在使用之后（Quartus 和 Vivado 都允许），也可以在被包含的头文件中（MiSTer 的 `emu_ports.vh`）。
- **在模块中使用生成块里声明的名字**（`FE-GENERATE-SCOPE`）：
  - 例如 MiSTer `hps_io.sv` 的 `kbd_rd`；
  - 把声明移到生成结构之前（同一行，行号不变）。
- **实现方式**：
  - 第一次编译后，根据 slang 的诊断（`AssignToNet`、`UndeclaredIdentifier`）和语法树计算源文本修改，然后重新编译，最多四次；
  - slang 的错误上限从 64 提高，使所有位置在一次编译中都能报告出来。
- **不修改的情况**：带声明赋值的线网、真正未声明的名字，仍然报错。
- **`.v` 文件中带初值的过程声明**：在 SystemVerilog 模式下，Verilog-2001 语义（静态变量）不再报 `StaticInitializerMustBeExplicit`；`.sv` 文件照常报告。

### 1.3 SDC：Quartus 的 `set_clock_groups -exclusive`

- Quartus 文档说 `-exclusive` 是 `-logically_exclusive` 的旧别名。实际上它被用来切断无关时钟之间的路径，Quartus 时序分析器对所有类型的时钟组都同样处理。
- 读作 `-asynchronous`，并报 `CON-EXCLUSIVE-AS-ASYNC` 提示。这样组之间的跨时钟路径仍然被检查，而不是作为互斥时钟被忽略。
- 显式写出的 `-logically_exclusive` 或 `-physically_exclusive` 不变。

### 1.4 Quartus 的 PLL 时钟名

- `derive_pll_clocks` 按 Quartus 的方式命名 PLL 时钟，例如 `emu|pll|pll_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk`。
- 现在 `altera_pll` 的每个输出时钟都有这个别名（`altpll` 为 `.../auto_generated|pll1|clk[n]`）：实例路径去掉顶层，用 `|` 分隔，加上模型的后缀（新属性 `OPENCDC_CLOCK_QUARTUS_SUFFIX`）。
- 所以 MiSTer `sys_top.sdc` 中的 `get_clocks { *|pll|pll_inst|altera_pll_i|*[*].*|divclk}` 能匹配到时钟。

### 1.5 Cyclone V 原语和 Quartus 原语

- **`cyclonev_clkselect`**：时钟多路选择器，输出携带所有输入时钟（写成嵌套条件表达式，时钟追踪可以穿过）。
- **组合与缓冲**：`cyclonev_lcell_comb`、`lcell`、`soft`、`carry`、`carry_sum`、`cascade`、`opndrn`、`alt_inbuf/outbuf/outbuf_tri/iobuf`。
- **触发器**：`dff`、`dffe`、`dffea`，带低有效的异步清零和置位。
- **HPS 原语**：
  - `clocks_resets`：`h2f_user0..2_clk` 是独立时钟，复位没有时钟域；
  - `hps2fpga`、`hps2fpga_light_weight`、`fpga2hps`：AXI 端口都在 `clk` 中；
  - `fpga2sdram`：各端口在各自的时钟中；
  - SPI 的 `sclk_out` 和 I2C 的 `out_clk` 是时钟；
  - 通用寄存器、中断、UART、调试、跟踪、启动：没有时钟域。

### 1.6 从子模块内部驱动的 inout 端口

- **问题**：MiSTer 的 `HPS_BUS` 是一个 inout 总线。核心在 `hps_io` 中把自己的时钟驱动到其中一位（`assign HPS_BUS[36] = clk_sys`），`sys_top` 通过拼接 `{..., clk_sys, ...}` 连接。以前驱动查找不进入 inout 端口，`clk_sys` 被认为没有驱动，104 个寄存器没有时钟。
- **现在**（`graph/DesignView`）：
  - 子模块（包括更深层的子模块，经过它们自己的 inout 端口）在内部驱动的位，在父模块中由该子模块驱动；
  - 其他位来自外部；
  - 逐层向上传播，直到不再变化。

### 1.7 其他修正

- **VHDL**：numeric_std 的 `shift_left/right`、`rotate_left/right` 和 std_logic_arith 的 `SHL/SHR` 的结果范围是 `(ARG'LENGTH-1 downto 0)`，与参数的范围无关。以前沿用参数的范围，所以 `SHL("00000000" & x, n)(19 downto 2)` 报切片方向错误（NES_MiSTer 的 VM2413）。
- **混合语言**：
  - 传给 VHDL generic 的参数由 defparam 设置时，取该参数的值；
  - VHDL 实体只绑定设计实际使用的模块体中的实例。

## 2. 真实工程验证

### Template_MiSTer（直接分析 `Template.qpf`）

- **工程**：46 个源文件，10 个 IP 文件列表，2 个 SDC。
- **宽松写法**：7 处线网在过程块中赋值（`ddr_svc.sv`、`f2sdram_safe_terminator.sv`），8 个生成块中的名字（`hps_io.sv`）。
- **时钟**：9 个时钟，1440/1513 个寄存器已解析。
  - 三个 50 MHz 振荡器；
  - HPS 的 `h2f_user0_clk`（100 MHz，来自 SDC）；
  - SPI 和 I2C 时钟；
  - 三个 PLL 的输出：核心 20 MHz、音频 24.58 MHz、HDMI 148.5 MHz。
- **约束**：`sys_top.sdc` 的时钟组匹配到 Quartus 名字的 PLL 时钟。
- **结果**：0 个错误，414 条跨时钟路径，34 条已同步，380 条未同步，358 个错误级违例。
  - 主要是两个独立 50 MHz 振荡器之间的复位请求（`reset_req` 进入 HDMI PLL 重配置逻辑），以及核心时钟到 HDMI 时钟的配置信号（OSD 和缩放器参数）；
  - 框架用 `set_false_path` 把这些标为准静态，但这些路径确实没有同步器。
- **未解析的 73 个寄存器**：在 HPS 原语内部，MiSTer 没有连接这些接口的时钟（`hps2fpga.clk` 等）。

### NES_MiSTer（对照，未加入语料库）

- 导入成功（93 个源文件，5 个 IP 文件列表），34 处宽松写法都已处理，VHDL 部分（VM2413）也通过。
- 仍然无法分析：`apu.sv` 中 VHDL 实体的参数来自两层 defparam（`nes.v` 设置 `apu.X`，`apu.sv` 再设置 `Dmc.X = X`）。slang 没有为这种链式 defparam 赋值（纯 Verilog 设计中也是如此，见已知限制）。
- **更正（阶段 61）**：原因不是链式 defparam，而是没有默认值的隐式类型参数（`parameter [9:0] X`）由 defparam 赋值时得到错误类型。已在阶段 61 修复，NES_MiSTer 已加入语料库。

### 本地示例

`examples/quartus_project`：

- 两个 PLL 输出时钟和一个独立振荡器；
- 一个 VHDL 库中的同步器；
- 一个 `.qip`、一个 `.mif`；
- 结果：一条已同步路径，一条 8 位未同步路径。

## 3. 修改的文件

- `src/engine/QuartusProject.{h,cpp}`（新）：Tcl 子集求值器、赋值处理、`.qpf` 修订版本。
- `src/engine/ProjectConfig.{h,cpp}`：`isVendorProjectFile`，`loadProjectFile` 接受 `.qpf/.qsf`。
- `src/engine/VivadoProject.cpp`：`.sv` 源文件或 `.svh` 头文件选择 SystemVerilog（以前直接分析 `.xpr` 时没有设置）。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`quartusCompatEdits`（源文本修改与重新编译）、错误上限、`.v` 文件的静态初值。
- `src/frontend/slang_adapter/IrBuilder.cpp`：defparam 设置的参数值。
- `src/frontend/AutoFrontend.cpp`：只绑定使用中的模块体。
- `src/frontend/primitives/intel.v`：Cyclone V 原语、HPS 原语、Quartus 原语、PLL 时钟名后缀。
- `src/frontend/vhdl/VhdlBuiltins.cpp`：移位函数的结果范围。
- `src/graph/DesignView.cpp`：inout 端口的驱动。
- `src/clock/ClockAnalyzer.cpp`：`OPENCDC_CLOCK_QUARTUS_SUFFIX` 别名。
- `src/constraints/Sdc.cpp`：`-exclusive`。
- `src/cli/main.cpp`、`src/gui/MainWindow.cpp`：Quartus 工程。
- `examples/quartus_project/`（新）。
- 测试：
  - `tests/engine/test_quartus_project.cpp`（新）：
    - 修订版本、`.qip` 与 `qip_path`（含版本选择）、`source`、库、SystemVerilog 模式、宏、搜索路径、参数、生成的系统、SDC；
    - 负例：缺失、未生成、AHDL、MIF、未知命令、总线下标、另一版本的列表不读、`-remove`；
    - 另一修订版本；直接分析；
    - 负例：不是工程、括号不平衡、未定义变量、没有修订版本、文件不存在。
  - `tests/frontend/test_quartus_compat.cpp`（新）：
    - 线网、端口、`inout reg`；生成块名字；
    - 负例：真正的错误不修改；
    - `.v`/`.sv` 静态初值；
    - inout 拼接的时钟；负例：子模块不驱动的位；
    - Cyclone V 原语与 PLL 名字；
    - VHDL SHL 切片；defparam 到 VHDL generic；负例：没有值的 generic。
  - `tests/constraints/test_sdc.cpp`：`-exclusive`；负例：显式类型、没有类型。
  - `tests/cli/RunImport.cmake`：导入 Quartus 工程；负例：`waive`、输出为 `.qsf`。
  - `cli.analyze_quartus_project`、`gui.quartus_project`。
- `tools/ci/sv-corpus.sh`：Template_MiSTer。
- 文档：`README.md`（Quartus 工程一节）、`docs/primitives.md`、`docs/architecture.md`。
- `CMakeLists.txt`：0.60.0。

## 4. 构建与测试结果

- 本地（Linux GCC Debug/Release、Clang）：579 个测试全部通过；SV 语料 23 个、VHDL 语料 48 个均通过；Clang 检查无告警。
- GitHub Actions Windows x64 / VS2022（提交 4d42954，运行 42440183559）：
  - Release：579/579 通过，生成 `OpenCDC-0.60.0-windows-x64.zip` 与 `.exe`；
  - Debug：579/579 通过。

## 5. 已知限制

- **链式 defparam**：一个 defparam 的值是另一个被 defparam 设置的参数时，slang 不赋值，相关寄存器丢失，而且没有报告。这阻碍了 NES_MiSTer 的分析。（阶段 61 更正：实际原因见阶段 61 第 1.1 节，已修复。）
- **Tcl 子集**：Quartus 设置脚本中的 `if`、`foreach`、`proc` 等不求值，并报告。
- **Platform Designer 系统**：必须先生成（`qsys-generate`）。
- **`inout reg` 端口**：读作输出后，外部输入的值不分析。
- **HPS 原语的端口**：只建模了 MiSTer 使用的端口和 AXI 的标准信号，宽度取最大值，其他端口的连接被忽略（`FE-PRIMITIVE-PORT`）。

## 6. 下一步

阶段 61：链式 defparam。一个参数被 defparam 设置为另一个被 defparam 设置的参数时，求出正确的值；求不出时报告，而不是静默丢失寄存器。用 NES_MiSTer 验证。

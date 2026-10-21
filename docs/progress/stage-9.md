# 阶段 9 完成报告：厂商原语、跨语言记录端口、GUI 工程编辑

日期：2026-10-21　版本：0.9.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T9.1 厂商原语模型 | **Xilinx UNISIM**：`FDRE/FDSE/FDCE/FDPE` 及旧式 `FD*`、`LDCE/LDPE`、`BUFG*`/`BUFH*`/`BUFR`/`BUFIO`、`IBUF*`/`OBUF*`/`IOBUF`、`SRL16E`/`SRLC32E`、`LUT1`–`LUT6_2`、`MUXF7/8`、`VCC/GND`。<br>**XPM**：`xpm_cdc_single/array_single/gray/handshake/pulse/async_rst/sync_rst`、`xpm_fifo_async`，分别识别为对应的同步结构。<br>**时钟生成器**：`MMCME2/3/4`、`PLLE2/3/4`（BASE/ADV），每个 `CLKOUT*` 成为 `CLKIN1` 的派生时钟。<br>**Intel**：`altera_std_synchronizer(_bundle)`、`altera_reset_synchronizer`、`dcfifo`。<br>**VHDL**：`unisim.vcomponents`、`xpm.vcomponents`、`altera_mf.altera_mf_components` 组件包。<br>只在设计实例化了原语且源文件没有定义时加入模型（`FE-PRIMITIVES` 列出用到的原语）；实例上的 `ASYNC_REG`（Verilog 属性、VHDL label 属性）标记同步级。 |
| 配套分析改进 | 经层次连到无效常量的使能/同步复位/异步复位被去掉（`FDRE` 的 `CE=1`、`R=0`）；按位拆开的寄存器组（`xpm_cdc_gray` 的逐位 generate 链）识别为 Gray；Gray 识别穿过寄存器复制；按位常量恒等化简。 |
| T9.2 跨语言记录端口 | VHDL 记录 ↔ SystemVerilog packed struct 双向整端口连接（字段顺序一致）；VHDL 对 Verilog 端口的部分关联：字段 `i.valid`、嵌套字段 `i.hdr.len`、元素 `d(3)`、切片 `d(0 to 3)`（按端口声明范围换算，支持 `[0:7]` 升序）。未指定 `language` 时，含 `.sv/.svh` 源文件的工程自动使用 SystemVerilog-2017。 |
| T9.3 GUI 工程编辑 | 文件菜单新增"新建工程…"、"编辑工程…"（Ctrl+E）、"工程另存为…"。对话框编辑名称、顶层、Verilog 标准、源文件（顺序、VHDL 库）、约束、豁免文件、顶层参数/generic、宏定义、include 目录和分析选项；保存为 `*.ocdc.json`（路径相对工程文件），然后重新分析。引擎新增 `projectToJson`/`saveProjectFile`。 |

## 2. 修改的文件

**新建**
- `src/frontend/primitives/`：`xilinx_unisim.v`、`xilinx_xpm.v`、`intel.v`、`unisim_vcomponents.vhd`、`xpm_vcomponents.vhd`、`altera_mf_components.vhd`
- `cmake/EmbedFiles.cmake`、`src/frontend/Primitives.{h,cpp}`
- `src/gui/ProjectEditor.{h,cpp}`
- `tests/frontend/test_primitives.cpp`、`tests/vhdl/test_vhdl_records.cpp`
- `docs/progress/stage-9.md`

**修改**
- `src/frontend/AutoFrontend.cpp`：原语改道到混合链接器，模型文件写入、时钟生成器参数收集与签名收敛。
- `src/frontend/CMakeLists.txt`：嵌入原语文件。
- `src/frontend/slang_adapter/IrBuilder.cpp`：实例属性、packed struct 字段布局。
- `src/frontend/vhdl/`：`VhdlScope.cpp`（label 属性、提前绑定子程序体）、`VhdlNames.cpp`、`VhdlStmt.cpp`（时钟 if 之后的复位 if）、`VhdlElab.{h,cpp}`、`VhdlMixed.cpp`（实例属性、部分关联到 Verilog 端口）。
- `src/ir/Design.h`：`ChildInstance::attrs`、`NetField`/`Net::fields`；`src/ir/ConstFold.cpp`；`src/ir/BodyEditor.{h,cpp}`（`passThroughMuxBits`）。
- `tools/ci/vhdl-corpus.sh`：surf 的三个工程。
- `src/cdc/SeqGraph.cpp`、`AdvancedAnalysis.cpp`、`CdcAnalyzer.cpp`。
- `src/engine/ProjectConfig.{h,cpp}`：保存、SystemVerilog 自动选择。
- `src/gui/MainWindow.{h,cpp}`、`main.cpp`（`--edit-test`）、`CMakeLists.txt`。
- `tests/CMakeLists.txt`（`gui.project_edit`）、`tests/engine/test_project_config.cpp`、`tests/cdc/test_cdc_advanced.cpp`、`tests/ir/test_ir.cpp`、`tests/vhdl/test_vhdl_elab.cpp`、`tests/ir_extract/test_ir_extract.cpp` 与 `golden/smoke.ir`。
- `CMakeLists.txt`（版本 0.9.0）、`README.md`、`docs/architecture.md`（第 14 节新增 3 条，15.5 第 5 点，新增第 16 节）。

`ChildInstance` 和 `Net` 只在末尾新增字段，已有接口不变。

## 3. 关键算法

### 3.1 原语模型的按需加入
前端先正常编译。Verilog 工程中，slang 报告的未知模块若有原语，或 VHDL 工程使用了 `unisim`/`xpm`/`altera_mf` 库，就改用混合链接器：每一轮收集被实例化而未定义的原语名，把模型（连同它依赖的模型）写入 `temp/opencdc-primitives-<版本>/` 并加入源文件，直到所需模型集合（签名）不再变化。用户定义的同名模块总是优先。

### 3.2 MMCM/PLL 模型
原语参数多、取值各异，固定的模型会因为未知参数名报错，所以模型按实例实际使用的参数名生成（全部默认为 0，只用于通过参数检查）。每个 `CLKOUT*` 由 `posedge CLKIN1`（PLLE3/4 为 `CLKIN`）驱动的翻转寄存器产生，时钟分析把它识别为输入时钟的分频派生时钟：同一 MMCM 的各输出互为同步；输入时钟的 `set_clock_groups` 经主时钟传到所有输出。

### 3.3 无效常量控制信号的去除
`FDRE` 的 `CE`、`R` 在模型内部是端口。SeqGraph 沿层次追溯控制信号的驱动，若是使能的有效常量或复位的无效常量就去掉；异步复位按位置索引，只有全部为无效常量时才去掉。这样 `FDRE` 链与手写的 `always @(posedge clk)` 链得到同样的结论。

### 3.4 按位拆开的 Gray 组
`xpm_cdc_gray` 在 generate 中为每一位建立独立的同步链，源寄存器也是按位的。识别时把同一 generate 的兄弟寄存器合起来，按 `D[k] = X[k] ^ X[k+1]` 证明 Gray 编码；同步结构包含全部位，各位不再算作重汇聚或 CDC-008。

### 3.5 部分关联到 Verilog 端口
slang 适配层记录 packed struct 每个字段（含嵌套字段）的位偏移和宽度。VHDL 形参 `p.f.g`、`p(i)`、`p(h downto l)` 逐层换算：字段查表；下标按端口的声明范围（`[7:0]` 或 `[0:7]`）换算成规范化偏移，字段内按 `[w-1:0]`。同一端口的多个部分关联合成一个 SigSpec，未关联的位为常量 X，输出端口上的常量位不产生驱动。

### 3.6 真实工程中发现并修复的问题（surf）
| 现象 | 原因 | 修复 |
|---|---|---|
| `AxiLitePkg` 报 `VHDL-NO-BODY` | 包声明中的常量调用函数，函数体在包体中；为此提前展开整个包体时，包体里的函数找不到尚未声明的规格，规格随后也失去函数体 | 包声明尚在展开时只绑定被调用的那个子程序体（`elaborateEarlyBodies`），其余包体等声明完成后再展开 |
| surf `RstSync` 报 CDC-006（误报） | `Synchronizer` 用 `if rising_edge(clk) ... end if; if rst = .. then .. end if;` 写异步复位（后一条语句优先），时钟 if 之后的语句被丢弃，复位丢失 | 时钟 if 之后的 `if <cond> then ... end if` 作为最高优先级的异步复位；其他语句给出 `VHDL-CLOCKED-TAIL` 警告 |
| MMCM `LOCKED` 进入复位同步器却没有检查 | 模型中 `LOCKED` 是 `CLKIN1` 域的寄存器，与输出时钟同步 | `LOCKED` 由内置黑盒 `opencdc_async_source` 驱动：没有时钟域，作为异步复位源时由 CDC-006 检查 |
| surf `FifoAsync` 的 `rdRdy` 报 CDC-007/CDC-005（误报） | 两进程写法中 `v.rdRdy := '1'` 之后整条记录经过多级 MUX，该位在两个分支相同，却被当作由选择信号计算的逻辑 | `BodyEditor` 在生成单元和结束时把"两个数据输入相同的 MUX 位"直接替换为该输入位；SeqGraph 对这种位不再记选择依赖 |

## 4. 构建与测试结果

### 4.1 构建
- Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 和其他 CI 作业的结果见推送后的 CI。

### 4.2 测试
- 全部 317 个 CTest 用例通过（阶段 8 结束时 299 个）。新增：
  - `test_primitives.cpp` 12 个用例：原语库内容、FDRE 链（带/不带 `ASYNC_REG`）、带 CE 的 FDRE 不是同步级（负例）、XPM 全部宏、VHDL unisim/xpm、Intel、用户同名模块优先（负例）、其他未知模块仍报错（负例）、MMCM 输出互为同步、独立输入的 MMCM 输出互为异步（负例）、VHDL 中的 MMCM、`LOCKED` 异步（正例 + 未同步的负例）。
  - `test_vhdl_records.cpp` 4 个用例：SV 结构体 → VHDL 记录、VHDL 记录 → SV 结构体、元素/切片/嵌套字段关联、未知字段和越界下标（负例）。
  - `test_vhdl_elab.cpp` 4 个用例：时钟 if 之后的复位 if、其他尾随语句（负例）、包声明常量调用包体函数、记录中未修改的字段绕过 MUX。
  - `test_project_config.cpp` 3 个用例：SystemVerilog 自动选择、JSON 往返、保存和写入失败（负例）。
  - `gui.project_edit`：无界面编辑自测（编辑、去重、保存、重新打开、写入失败、重新分析）。
  - `test_cdc_advanced`：按位拆开的 Gray（正例、负例）、大存储器 FIFO。
- 黄金文件 `smoke.ir` 有 2 行变化（MUX 中两分支都为 0 的位变成常量），逐位核对等价。

### 4.3 真实工程
| 工程 | 结果 |
|---|---|
| surf `ClockManager7`（MMCM，3 个输出） | 14 个实例；5 个时钟：3 个 MMCM 输出识别为 `clkIn` 的派生时钟，经 `-include_generated_clocks` 与 `axilClk` 异步；`LOCKED` → 3 个 `RstSync`，全部识别为复位同步器；0 个违规 |
| surf `ClockManager7`（PLL） | 同上，`PLLE2_ADV` |
| surf `FifoAsync` | 7 条路径：2 个复位同步器、2 个三级同步、存储器读为受限定数据、2 个 Gray 指针（结构识别，需复核）；0 个违规（修复前 4 条潜在风险为误报） |
| open-logic（6 个 CC 组件）、neorv32 SoC | 与阶段 8 结果完全相同 |
| 语料解析（5 个项目约 1900 个文件） | 与阶段 8 相同 |

surf 的三个工程已加入 `tools/ci/vhdl-corpus.sh`，CI 每次运行都会检查。

## 5. 已知限制

- MMCM/PLL 输出按"派生时钟"处理，不计算实际频率和相位（`CLKOUT*_DIVIDE`、`CLKFBOUT_MULT` 不参与 CDC-007 的周期计算）；需要周期时用 `create_generated_clock` 约束。
- 动态重配置（DRP）、相移等端口在模型中为常量 0；`LOCKED` 只是 `CLKIN1` 域的寄存器。
- 模型是 CDC 结构的抽象，不是功能仿真模型：`SRL`、`LUT` 等按组合/移位行为建模，GT 收发器、`IDELAY`、`ISERDES`、URAM/BRAM 原语没有模型（`FE-UNKNOWN-MODULE`）。
- Verilog 端口是 unpacked 结构体、接口（interface）或 VHDL 端只用到记录的一部分且 Verilog 端口是普通向量时，字段名无法使用（需整端口连接或用下标/切片）。
- GUI 编辑器不检查文件是否存在（分析时报告）；没有撤销。

## 6. 下一步建议

1. 根据 MMCM/PLL 参数计算输出周期，自动生成 `create_generated_clock`，让 CDC-007 等依赖周期的检查可用。
2. 更多原语：BRAM（`RAMB36E1/RAMB18E2`，真双口跨时钟）、`IDDR/ODDR`、`IDELAYCTRL`、Intel `altsyncram`、Lattice/Gowin 常用原语。
3. SystemVerilog 接口（interface/modport）端口和 unpacked 结构体。

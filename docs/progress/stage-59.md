# 阶段 59：直接分析 Vivado 工程（.xpr），或导入为 OpenCDC 工程

日期：2026-11-07　版本：0.59.0

以前每个设计都要手写 `.ocdc.json`：列出源文件、VHDL 库、块设计、IP、约束和顶层。对 Vivado 用户来说，这些信息都已经在 Vivado 工程文件 `.xpr` 中。现在可以直接用 `.xpr`。

## 1. 完成的功能

### 1.1 读取 Vivado 工程

`.xpr` 是 XML，列出工程的文件集（源文件、约束、仿真）、运行（综合、实现）以及每个文件的属性。导入时取 Vivado 综合的内容：

- **文件集**：当前综合运行（`State="current"` 的 `Ft3:Synth` 运行）的源文件集和约束集；没有运行信息时用 `sources_1` 和 `constrs_1`。
- **源文件**：
  - 用于综合（`UsedIn` 含 `synthesis`，或没有 `UsedIn`）且未被禁用（`AutoDisabled`、`IsEnabled="0"`）的 HDL、`.xci`/`.xcix`、`.bd`，按工程中的顺序；
  - VHDL 文件取其 `Library` 属性，没有时用工程的默认库（`DefaultLib`，通常是 `xil_defaultlib`）；
  - 在单独文件集（`BlockSrcs`）中的 IP 也加入，但块设计或核心容器（`.xcix`）中的 IP 不重复加入；
  - IP 文件集中的其他文件（输出产品的约束等）不读。
- **Verilog 头文件**（`.vh`、`.svh`、`.h`，以及标为全局包含的文件）不作为源文件，其目录加入包含目录，因为 Vivado 按文件名找到它们。
- **源文件集的选项**：顶层（`TopModule`）、Verilog 宏（`VerilogDefine`）、包含目录（`VerilogDir`）、generic（`Generic`）。
- **约束**：
  - 约束集中用于综合或实现的 XDC、SDC、Tcl 文件；
  - `ScopedToRef` 的文件作为限定到模块的约束（与 `read_xdc -ref` 相同）；
  - 只限定到单元（`ScopedToCell`）的文件不读，报 `PRJ-XPR-SCOPED` 警告。
- **IP 仓库**：工程的 `IPRepoPath`。
- **路径变量**：`$PPRDIR`、`$PSRCDIR`、`$PGENDIR`、`$PCACHEDIR`、`$PRUNDIR`、`$PIPUSERFILESDIR`（可由 `IPUserFilesDir` 重定向），相对于 `.xpr` 所在目录解析。
- **报告未读取的内容**：
  - 缺失的文件（`PRJ-XPR-MISSING` 警告）；
  - 网表（`.edf`、`.dcp` 等，`PRJ-XPR-NETLIST` 警告）；
  - 禁用的源文件（`PRJ-XPR-DISABLED`）；
  - 存储器初始化、仅仿真和其他文件（`PRJ-XPR-SKIPPED`，逐个列出）；
  - 导入摘要（`PRJ-XPR-IMPORT`）。
- **XML 读取**：自己写的小型读取器（元素与属性、实体、注释），不用正则表达式，避免以前遇到的栈溢出问题。

### 1.2 使用方式

- **直接分析**：`opencdc analyze project.xpr`、`opencdc waivers project.xpr` 和 GUI 的"打开工程"都接受 `.xpr`。
  - 每次分析都重新导入，所以 Vivado 中的修改会自动生效；
  - 导入时的发现与其他诊断一起报告（CLI、JSON、HTML、GUI）。
- **导入**：`opencdc import project.xpr [-o project.ocdc.json] [--force]` 写出 OpenCDC 工程，之后可以添加豁免、选项，或构建脚本才加入的文件。
  - 默认写在 `.xpr` 旁边；已存在时不覆盖，除非加 `--force`；
  - 输出不能是 `.xpr`。
- **不改动 Vivado 工程**：
  - 导入的工程没有 `projectFile`（来源记录在 `importedFrom`），所以 GUI 的"保存"会转为"另存为"，写成新的 `.ocdc.json`；
  - 在 Vivado 工程上 `opencdc waive` 会被拒绝，并提示先导入；
  - 另存后，它就是独立的 OpenCDC 工程，不再报告导入诊断。
- **示例**：`examples/vivado_project/demo.xpr`，Vivado 2023.2 格式。
  - 内容：两个时钟，一个 VHDL 库中的同步器，一个头文件，一个禁用的文件（内容不是合法 Verilog，证明它没有被读取），一个仅仿真的测试平台，一个 `.coe`，一个限定到模块的约束。

## 2. 真实工程验证

对语料库中已有手写工程的 8 个 Vivado 工程，比较导入结果与手写工程的分析结果：

| 工程 | 结果 |
|---|---|
| ArtyS7-RPU-SoC | 相同：6 个时钟（369/369），10 条跨时钟路径 |
| cteqeu 的 Zynq 工程：NeoPixel、BRAM、IRQ_F2P、XADC、Timers_GIC | 相同；NeoPixel 多一个警告：工程的 IP 仓库不在仓库中（`PRJ-XPR-MISSING`），这是真实情况 |
| Fuxi SoC | 工程中的 `Fuxi.v` 是 Chisel 生成的构建产物，不在仓库中，报告为缺失；加 `--blackbox-unknown` 后与手写工程相同（9 个时钟，1 条已同步路径） |
| Loongson chiplab（2023.2） | CPU 由构建脚本加入，不在 `.xpr` 中，所以 `core_top` 未知；补上 CPU 源文件后与手写工程完全相同（6 个时钟，132 条路径）。工程中 4 个被 Vivado 禁用的 SRAM IP 确实不需要，包含目录从头文件自动得到，也与手写的一致 |

另外导入了本机全部 65 个 `.xpr`（2017–2023 各版本，含 IP 打包工程 `edit_*.xpr`），全部成功。

- nontrivial-mips 的 `.xpr` 早于仓库中的代码：缺少 FPU 和 ASIC 的源文件，所以有未知模块。这是工程文件过时，不是导入的问题。

SystemVerilog 语料库中 ArtyS7-RPU-SoC 和 5 个 cteqeu 工程改为直接分析 `.xpr`，删除了它们的手写工程配置。

## 3. 修改的文件

- `src/engine/VivadoProject.{h,cpp}`（新）：XML 读取器、`projectFromXprText`/`projectFromXprFile`、`isVivadoProjectFile`。
- `src/engine/ProjectConfig.{h,cpp}`：
  - `importedFrom`、`importNotes`；
  - `loadProjectFile` 接受 `.xpr`。
- `src/engine/AnalysisSession.cpp`：报告导入诊断。
- `src/engine/WaiverFile.cpp`：导入工程的默认豁免文件位置在 `.xpr` 旁边。
- `src/engine/ResultJson.cpp`、`Report.cpp`：导入工程的来源。
- `src/cli/main.cpp`：`import` 命令；`waive` 拒绝 Vivado 工程。
- `src/gui/MainWindow.cpp`：
  - 打开对话框接受 `.xpr`；
  - 导入状态；
  - 另存后成为 OpenCDC 工程。
- `examples/vivado_project/`（新）。
- 测试：
  - `tests/engine/test_vivado_project.cpp`（新）：
    - 运行的文件集、变量、库、头文件、禁用和仅仿真文件、`.coe`、网表、缺失文件、块设计内的 IP、核心容器、宏、generic、限定约束、只限定单元的约束、`out_of_context`、IP 仓库；
    - 当前运行的选择和没有运行时的默认值、`IPUserFilesDir`；
    - 直接分析、导入诊断、另存后重新加载；
    - 负例：不是工程、没有源文件集、XML 损坏、文件不存在。
  - `tests/cli/RunImport.cmake`（新，`cli.import_vivado`）：
    - 写出位置和内容；不覆盖已有文件；`--force`；写出的工程与 `.xpr` 分析结果相同；
    - 负例：`waive` 拒绝 Vivado 工程；输出为 `.xpr`；输入不是 `.xpr`。
  - `cli.analyze_vivado_project`、`gui.vivado_project`。
- `tools/ci/sv-corpus.sh`：6 个工程直接用 `.xpr`。
- 文档：`README.md`（Vivado 工程一节、用法与示例输出，示例输出由测试运行）、`docs/architecture.md`。
- `CMakeLists.txt`：0.59.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 只读取当前综合运行；其他运行（例如 `synth_2`）要在 Vivado 中设为当前运行。
- `VerilogDefine`、`VerilogDir`、`Generic` 的值按空白分隔，路径中不能有空格。
- 由 Tcl 脚本在打开工程后加入的文件不在 `.xpr` 中。
- 网表（`.edf`、`.dcp`）不分析；只由网表定义的模块仍是未知模块。
- Quartus 工程（`.qsf`/`.qpf`）尚不支持。

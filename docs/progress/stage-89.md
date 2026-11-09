# 阶段 89：接口环回的端口方向；ICAPE2/ICAPE3

日期：2026-11-09　版本：0.89.0

阶段 88 留下一个已知限制：同一个接口实例连接到一个模块的两个接口端口时，`IR-INPUT-DRIVEN` 报告输入端口在模块内被驱动。语料库中的 cndm ADM-PCIE-9V3 因此有 24 条告警。

这一阶段修正它。同时为 taxi、corundum 中使用的配置访问端口 `ICAPE2`/`ICAPE3` 建模：以前它们在 38 个 taxi 工程中是黑盒。

## 1. 完成的功能

### 1.1 一个接口实例连接两个端口（`IrBuilder.{h,cpp}`）

- 写法（cndm `cndm_micro_port`）：

  ```systemverilog
  cndm_micro_cpl_wr cpl_wr_inst (..., .s_axis_event(axis_event), .m_axis_event(axis_event));
  ```

  `s_axis_event` 用 `snk` modport（`tdata` 为输入），`m_axis_event` 用 `src` modport（`tdata` 为输出）。模块把 `m_axis_event` 交给子模块驱动，自己从 `s_axis_event` 读取：这是模块内部的事件环回。
- 接口的每个成员在模块中只有一个端口，以第一个接口端口命名（`s_axis_event.tdata`）。以前方向按 `snk` modport 取为输入；子模块驱动它，于是报告 `IR-INPUT-DRIVEN`，并按输出处理。
- 现在：一个接口实例连接到模块的多个接口端口时，它的成员端口按模块的使用方式确定方向（模块内驱动则为输出），不再取某一个 modport 的方向。
- 结果：cndm 的 24 条告警消失；除告警数外，分析结果与修改前逐行相同（信号以前已按输出处理）。

### 1.2 `ICAPE2`、`ICAPE3`（`xilinx_unisim.v`）

- 配置访问端口：命令和数据（`I`、`CSIB`、`RDWRB`）在 `CLK` 上寄存；回读数据 `O` 以及 `ICAPE3` 的 `AVAIL`、`PRDONE`、`PRERROR` 来自 `CLK` 上的寄存器（UG470、UG570）。
- 配置逻辑本身不建模。
- 以前它们是未知模块（黑盒，`FE-BLACKBOX-MODULE`），在另一时钟域中读取 `O` 或写入 `I` 的跨时钟路径不检查。
- `unisim.vcomponents` 增加两个组件声明（VHDL 中实例化时绑定到 Verilog 模型），`docs/primitives.md` 从“未建模”移到表中。

## 2. 真实工程验证

- cndm ADM-PCIE-9V3（语料库）：
  - 告警从 43 条减为 18 条（24 条 `IR-INPUT-DRIVEN`，1 条 `ICAPE3` 黑盒）；
  - 实例 859、寄存器 11891、未分析的构造 3（`ICAPE3` 的 5 个寄存器与实例计入）；
  - 603 条跨时钟路径全部已同步，与修改前相同。`ICAPE3` 在 PCIe 用户时钟域中读写，没有新的跨时钟路径。
- SV 语料 53 项、VHDL 语料 48 个全部匹配，其余结果不变。

## 3. 修改的文件

- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：连接到多个端口的接口实例。
- `src/frontend/primitives/xilinx_unisim.v`：`ICAPE2`、`ICAPE3`。
- `src/frontend/primitives/unisim_vcomponents.vhd`：组件声明。
- `docs/primitives.md`。
- 测试（每个用例都含反例）：
  - `tests/frontend/test_sv_interfaces.cpp`：同一实例连接 `snk`、`src` 两个端口，子模块经 `src` 驱动，模块经 `snk` 读取：没有 `IR-INPUT-DRIVEN`，计数器经环回进入另一时钟域。反例：两个不同的实例保持各自 modport 的方向，没有这条路径。我确认过：修改前这个设计报告 2 条 `IR-INPUT-DRIVEN`。
  - `tests/frontend/test_primitives.cpp`：`ICAPE3` 的 `O`、`AVAIL` 在另一时钟域读取，`ICAPE2` 的 `I` 来自另一时钟域：都是跨时钟路径。反例：同一时钟域的读写没有路径。
- `tools/ci/sv-corpus.sh`：cndm 的预期值。
- `CMakeLists.txt`：版本 0.89.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：694 个测试全部通过。
- 测试中发现的一处遗漏：新的 Verilog 原语模型需要 `unisim.vcomponents` 中的组件声明（已有测试检查每个 Xilinx 模型都有声明），已补上。
- 测试预期的一处修改：`IR-INPUT-DRIVEN` 的反例最初在模块内给输入 modport 的成员赋值，slang 直接报错（`InputPortAssign`），改为两个不同实例的对照。
- SV 语料 53 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 88 的 Windows CI（提交 23b06ab，运行 42657339185）：Release 与 Debug 均 692/692 通过，生成 `OpenCDC-0.88.0-windows-x64.zip` 与 `.exe`。
- 本阶段的 Windows CI（提交 826317b，运行 42663578786）：Release 第一次在配置时下载 nlohmann_json 得到 HTTP 500（GitHub 的临时错误，与代码无关），重新运行失败的作业后通过。Release 与 Debug 均 694/694 通过，生成 `OpenCDC-0.89.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- 连接到多个端口的接口实例，成员端口以第一个接口端口命名（`s_axis_event.tdata`），约束或报告中不出现另一个端口的名字。
- `ICAPE2`/`ICAPE3` 不建模配置逻辑：回读数据被视为由写入的数据决定。

## 6. 下一步

阶段 90：继续真实设计验证，优先选择带视频采集或 SerDes、以及 Intel/Lattice 器件的多时钟域设计。

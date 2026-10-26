# 阶段 21：Analog Devices HDL 库验证

日期：2026-10-26

本阶段用两个真实的开源 IP 库验证工具：

- **Analog Devices 的 HDL 库**（`analogdevicesinc/hdl`，提交 `c9a501d`）：FPGA 上使用最广的开源 IP 库之一。验证对象是 AD9361 收发器核 `axi_ad9361`，以及它在 fmcomms2 ZedBoard 工程中的系统连接。
- **Digilent 的 vivado-library**（提交 `0362ce72`）：其中的 IP 直接以 `.xci` 的形式使用 Vivado IP。为此新增了从 `.xci` 建模 Vivado IP 的功能。

## 1. 完成的功能

### 1.1 比较写法的同步复位

ADI 的代码统一把同步复位写成 `if (up_rstn == 1'b0)`。以前只识别 `if (!rstn)` 和 `if (rst)`，比较写法的复位留在 D 输入里，成为一个多路选择器。后果是 `up_xfer_status`、`up_xfer_cntrl`、`up_clock_mon` 中的 toggle 同步器都被判为未同步，因为第一级之前有组合逻辑。

现在下列写法都识别为同步复位：`x == 1'b0`、`x == 1`、`x != 0`、`x != 1'b1`。其中 `x` 是 1 位具名信号，常量可以被扩展到 32 位。多位比较（`state == 2'd0`）和两个信号的比较（`a == b`）仍是数据。

### 1.2 `get_pins` 按名字匹配所有触发器引脚

ADI 的 `ad_rst_constr.xdc` 写 `set_false_path -to [get_pins -hier *rst_sync_reg/PRE]`。以前按名字只能匹配 `Q` 和 `C`。现在还能匹配 `D`、`CE`、`R`、`S`、`CLR`、`PRE`，置位还是清零取决于复位值，与 Vivado 的 FDPE/FDCE 一致。`set_false_path` 按引脚区分端点角色：`PRE`/`CLR` 只覆盖异步复位路径，`D` 不覆盖异步复位。

### 1.3 系统级验证工程

单独分析 IP 时，`clk` 输入与 `rx_clk_in` 被当作两个独立时钟，`axi_ad9361_lvds_if` 里出现一批"单级跨域"。在 fmcomms2 的 block design 中（`fmcomms2_bd.tcl`：`ad_connect axi_ad9361/l_clk axi_ad9361/clk`），`clk` 就是 `rx_clk_in` 经 IBUFGDS/BUFG 后回送的时钟，这些都不是跨域。

语料库新增 `fmcomms2.ocdc.json`，配置如下：
- 一个按 block design 连接 `l_clk → clk` 的包装顶层；
- ADI 自带的 IP 约束：`up_xfer_cntrl/status`、`up_clock_mon`、`ad_rst`、`axi_ad9361_constr.xdc`；
- ZedBoard 板级约束：`create_clock rx_clk`；
- PS7 FCLK 的时钟定义。

结果为 3 个时钟，1667 个寄存器全部有时钟；78 条跨域路径中 77 条已同步，0 个错误级违规，21 条潜在风险。唯一的未同步路径是 `s_axi_aresetn`：这个输入没有时钟域，却驱动异步复位（CDC-006）。

### 1.4 重汇聚（CDC-005）：互斥读取不再误报

ADI 的寄存器读出写法：
- 每个子模块用 `case (up_raddr)` 从几十个状态字中选一个，其中很多是不同同步器的输出；
- 子模块不被寻址时读数据为 0；
- 上层把所有子模块的读数据按位"或"在一起。

以前一个位只要依赖两个同步器就算汇聚，所以这种写法全部被报告。

现在先按位求"一次求值同时读取的标签组"（`SeqGraph::dataAlternatives`）：
- 多路选择器的数据输入互为备选，选择端总被读取；
- 每个备选带有其选择条件推出的寄存器取值，即"某寄存器某位在 d 拍前等于 0/1"；
- 条件矛盾的组合不可能出现，被丢弃。

这样既能处理一个 `case` 内部的互斥，也能处理不同子模块地址范围的互斥：common 要求 `up_raddr[8]=0`，channel 要求 `up_raddr[8]=1`。

安全性：
- 结果只会从原来的标签并集中去掉标签，不会增加；
- 带使能的寄存器可能保持旧值，经过它时丢弃条件；
- 组超过上限时保守合并；
- 逻辑过深或超出预算时回退到原来的并集判断。

详见 [docs/rules/CDC-005.md](../rules/CDC-005.md)。

fmcomms2 剩余的 21 条潜在风险经人工逐条确认，都是真实的组合：
- 四个通道的 PN 错误标志按位"或"；
- `tdd_tx_valid & dac_valid`；
- I/Q 校正乘法器同时使用两个通道各自同步过来的系数；
- `enable_int` 的选择端 `tdd_mode` 与数据 `enable_up` 来自不同同步器；
- TDD 计数器。


### 1.5 从 `.xci` 建模 Vivado IP

用户试用时，`fifo_8X4K`（Vivado FIFO Generator）因为没有 HDL 而无法分析。现在可以直接把 IP 的 `.xci` 文件加入源文件。工具读取配置（XML 和 JSON 两种格式），生成同名、同端口的 CDC 模型，详见 [docs/vivado-ip.md](../vivado-ip.md)。

| IP | 模型 |
|---|---|
| FIFO Generator 12.x/13.x（Native 与 AXI-Stream） | 独立时钟时，Gray 指针经 `C_SYNCHRONIZER_STAGE` 级同步，被识别为异步 FIFO；`rst` 在两个时钟域各自做复位同步。公共时钟时为单时钟 FIFO。可选端口按配置生成，各自属于自己的时钟域。支持标准读和首字直通（FWFT），支持读写宽度不同。 |
| AXI4-Stream Data FIFO | 同上；端口由 `C_AXIS_SIGNAL_SET` 决定。 |
| ILA | 只接收信号；约束中引用 ILA 内部（`*ila_core_inst/*`）的命令降为提示。 |
| VIO | `probe_out` 是 `clk` 域的寄存器（VIO 内部把 JTAG 写入的值同步到 `clk`）。 |

其他 IP（如 clk_wiz、blk_mem_gen）给出 `FE-XCI-UNSUPPORTED` 警告，仍按未知模块处理。

验证：
- Digilent vivado-library 的 13 个 FIFO `.xci`（12 个 FIFO Generator，1 个 axis_data_fifo；Native 和 AXI-Stream；公共时钟和独立时钟）全部建模成功；独立时钟的都被识别为异步 FIFO。
- 两个完整的 Digilent IP 端到端通过：
  - ZmodScopeController：VHDL，含 FIFO Generator、MMCM、OSERDES，以及 Digilent 自己的 SyncAsync/HandshakeData/ResetBridge；
  - MIPI CSI-2 接收器：VHDL，含 AXI-Stream FIFO Generator、axis_data_fifo 和 3 个 ILA。
- 在 ZmodScopeController 中发现一处真实问题：`sInitDoneADC and sInitDoneRelay` 先做与运算再进入同步器（CDC-004）。

### 1.6 Digilent 代码暴露的 VHDL 前端问题

- `use work.DebugLib;`（没有 `.all`）没有让包名可见，`DebugLib.DebugLM_t` 这样的扩展名报"未声明"。现在 `use lib.pkg;` 会让包名直接可见。
- 偏移量可变、宽度固定的切片做赋值目标（`rbTdataInt((i+cnt+1)*8-1 downto (i+cnt)*8) <= ...`）报"范围必须是静态的"。以前只有读取一侧支持。现在赋值目标拆成逐元素的动态插入。两个边界必须是同一个变量的同一线性倍数（`k*x + c`）。
- 混合语言时顶层名写错，以前报 slang 的 "not a valid top-level module"，看不出原因。现在说明顶层既不是 VHDL 实体也不是 Verilog 模块，并列出最相近的名字。

## 2. 修改的文件

- `src/ir/BodyEditor.cpp`：`compareWithBit`，比较写法的复位条件。
- `src/constraints/Sdc.cpp`：`get_pins` 按名字匹配所有触发器引脚。
- `src/cdc/SeqGraph.{h,cpp}`：
  - `dataAlternatives`：按位计算带条件的标签族；
  - `implied`：由信号取值推出寄存器取值；
  - `syncResetFacts`；
  - `FamilyCache`：跨调用缓存与预算。
- `src/cdc/AdvancedAnalysis.cpp`：汇聚判定使用标签族，成员为（标签集合, 条件）的驻留对。
- `tools/ci/sv-corpus.sh`：新增 ADI fmcomms2 工程。
- `src/frontend/XciModels.{h,cpp}`（新）：解析 `.xci` 并生成 FIFO Generator、AXI4-Stream Data FIFO、ILA、VIO 的模型。
- `src/frontend/AutoFrontend.cpp`：入口处把 `.xci` 替换为生成的模型；顶层名不存在时给出相近名字。
- `src/frontend/MixedSupport.h`：未知模块提示中加入 `.xci` 的用法。
- `src/frontend/vhdl/VhdlScope.cpp`：`use lib.pkg;` 的包名可见性。
- `src/frontend/vhdl/VhdlNames.cpp`、`VhdlElab.h`：`sliceSpan`；可变偏移切片作为赋值目标。
- `src/constraints/Sdc.cpp`：调试核内部引用降为提示；带过滤器的未匹配查询统一报告。
- `src/gui/ProjectEditor.cpp`：文件对话框接受 `.xci`。
- `tools/ci/vhdl-corpus.sh`：
  - 新增 Digilent vivado-library（308 个 VHDL 文件全部解析）；
  - 新增 ZmodScopeController、MIPI CSI-2 两个工程；
  - 13 个 FIFO `.xci` 逐个检查。
- 测试：
  - `tests/ir_extract/test_ir_extract.cpp`：比较写法的复位，正例 4 种写法，负例为多位比较和信号比较；
  - `tests/engine/test_adi_patterns.cpp`（新）：toggle 同步器的端到端测试，正例 3 种复位写法，负例为复位不掩盖未同步路径；
  - `tests/cdc/test_cdc.cpp`：`get_pins` 的 PRE/CLR/D/CE，含正例和负例；
  - `tests/cdc/test_cdc_advanced.cpp`：读出互斥。负例覆盖 case、跨子模块"或"和下游复制寄存器；正例覆盖旁边真正的 `a2 & c2`，以及地址范围相同时的"或"。
  - `tests/frontend/test_xci.cpp`（新），6 个用例：
    - JSON 独立时钟 FIFO 识别为异步 FIFO，未建模输出有说明；
    - 负例：公共时钟 FIFO 跨域读取仍报告；
    - XML 端口推导，以及 VHDL 组件例化的端到端；
    - axis_data_fifo 的字段布局；
    - 负例：AXI4 FIFO、非 IP 文件、clk_wiz 仍是未知模块，黑盒模式下照常分析；
    - ILA/VIO，以及约束中 ILA 内部引用为提示。
  - `tests/vhdl/test_vhdl_elab.cpp`：可变偏移切片目标（负例为边界变量不同）；`use lib.pkg;` 加扩展名（负例为没有 use 子句）。
  - `tests/vhdl/test_vhdl_mixed.cpp`：顶层名建议（负例为毫不相近的名字）。
- 文档：`docs/rules/CDC-005.md`、`README.md`。

## 3. 测试有效性

- 撤回 `compareWithBit` 后，复位相关的新测试失败（8 处断言）。
- 关闭标签族细化（`dataAlternatives` 直接返回 nullopt）后，读出互斥的负例和正例都失败。

## 4. 性能

microwatt Arty（2445 个寄存器、173 条跨域路径，语料库中最大的设计）：
- 0.20.1：22 秒；
- 现在：27 秒。

增加的时间来自对粗判汇聚的寄存器做逐位锥遍历。每个时钟域每个深度步骤的预算是 100 万次单元计算。

## 5. 已知限制

- 互斥只从"同一拍内寄存器取值与常量比较"的条件推出。独热编码、不同信号的 `if/else if` 优先级链、依赖数据的互斥都证明不了，仍会报告。
- 带使能的读出寄存器会丢弃条件。
- `.xci` 模型是 CDC 模型，不是功能模型。clk_wiz（时钟 IP）、blk_mem_gen（双时钟 BRAM）、AXI4 接口的 FIFO 还没有模型。`.xcix` 压缩容器不能直接读取。
- VHDL 时钟进程中，在时钟分支之外用变量给信号赋值（Digilent LLP.vhd 的 `sAxisTready`），仿真语义相当于寄存器，Vivado 综合为导线。工具按寄存器处理，并给出 VHDL-ASYNC-LOAD 警告。

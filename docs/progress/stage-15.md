# 阶段 15 完成报告：真实多时钟 SoC（microwatt Arty A7）

日期：2026-10-25　版本：0.15.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T15.1 `get_clocks -of_objects` | `set_clock_groups -group [get_clocks -include_generated_clocks -of [get_nets system_clk]]` 这种 Vivado 写法现在生效：取出这些网线、引脚或端口上的时钟作为组成员（时钟追踪之后解析）。Tcl 选项可以写成唯一前缀（`-of`、`-per`、`-async`）。 |
| T15.2 原语 | Xilinx `DSP48E1`/`DSP48E2`（输出在 `CLK` 上寄存或为组合逻辑）、`BSCANE2`（JTAG 时钟 `TCK` 作为独立时钟源）、`IOBUFDS`、`OBUFTDS`；Verilog 模型和 unisim VHDL 组件都有。 |
| T15.3 LiteX 生成代码 | 完全相同的重复连续赋值视为一个驱动；`mem[adr][i*8 +: 8] <= …`（动态索引元素中的静态部分）作为赋值目标。 |
| T15.4 VHDL | 读取文件的初始化函数：在第一次读取处停止，结果为未知；带乘法的切片边界 `(w+1)*64-1 downto w*64`；空切片目标；无上下文类型的拼接选择表达式（`with a & b select`）不再报歧义。 |
| T15.5 真实工程 | microwatt 的 Arty A7-35 配置，包括 LiteDRAM、LiteEth、LiteSDCard、UART16550 和 JTAG 调试，加入 VHDL 语料回归。有原样和修正两个版本。 |

## 2. 修改的文件

**修改**
- `src/constraints/Sdc.{h,cpp}`：`ClockGroups::memberBits`、`get_clocks -of_objects`、选项前缀匹配
- `src/clock/ClockAnalyzer.cpp`：`resolveGroupObjects`，时钟组检查和关系计算中使用这些成员
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：DSP48E1/E2、BSCANE2、IOBUFDS、OBUFTDS
- `src/ir/BodyEditor.cpp`：完全相同的重复连续赋值不算多驱动
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`、`ProcessLowering.cpp`：动态元素中的静态部分作为目标（`LTarget::sub`）
- `src/frontend/vhdl/VhdlNames.cpp`、`VhdlElab.h`、`VhdlExpr.cpp`：文件读取、切片边界的线性分解、空切片目标、拼接的歧义
- `src/frontend/AutoFrontend.cpp`：设计有错误时，同样过滤原语未连接引脚的警告
- 测试：`tests/clock/test_clock.cpp`、`tests/constraints/test_sdc.cpp`、`tests/frontend/test_primitives.cpp`、`tests/vhdl/test_vhdl_elab.cpp`、`tests/ir_extract/test_ir_extract.cpp`
- `tools/ci/vhdl-corpus.sh`：microwatt Arty（原样 / 修正）
- `docs/primitives.md`、`README.md`、`docs/architecture.md`（第 22 节）、`CMakeLists.txt`（版本 0.15.0）

## 3. 关键算法

### 3.1 `get_clocks -of_objects`
解析约束时还没有时钟模型（MMCM/PLL 的输出时钟由时钟分析推导），所以 `get_clocks -of_objects` 先记下对象的网线位。时钟追踪结束后再解析：在追踪缓存中查找该位上的时钟（该网线在某个寄存器的时钟路径上），或者该位是约束时钟的源。不对任意网线重新追踪，因为追踪一根数据网线会把它当成时钟源而凭空产生时钟。

### 3.2 动态索引元素中的静态部分
目标 `mem[adr][k*8 +: 8]`：`LTarget` 记录动态索引和元素内的偏移（`sub`/`subOffset`）。赋值时先用 `DynSelect` 读出整个元素，替换这 8 位后用 `DynInsert` 写回，所以元素的其余位保持原值。

### 3.3 读取文件的子程序
`readline`/`read`/`hread` 等例程在子程序内执行时，把当前过程状态标记为已返回（存活条件为 0），并累计一个计数器。包含它的函数检查到计数变化后返回未知值。这样 litedram-initmem 中按负载大小展开、本应由 `endfile` 结束的循环不再执行，也就不会产生几万条越界错误。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 377 个 CTest 用例全部通过（阶段 14 是 370 个）。新增：
  - `-of_objects` 组：分频时钟与另一时钟被正确声明为异步，而主时钟仍是未知关系；数据网线上没有时钟（负例）；其他查询的 `-of_objects` 仍报不支持（负例）。
  - 选项前缀，及有歧义的前缀（负例）。
  - DSP48E1 寄存输出、DSP48E2 组合输出、BSCANE2 的 TCK 时钟域、IOBUFDS。
  - 读取文件的初始化函数；带乘法的切片和空切片目标；`with a & b select`。
  - 完全相同的重复赋值不报多驱动；源不同时仍报（负例）。
  - 动态元素的静态部分作为目标。
- **真实工程**：
  - microwatt Arty A7-35 原样分析：3 个 `IR-MULTIDRIVER` 错误，都是设计本身的问题（见第 5 节）。
  - 修正这三行后：0 个错误，2 个警告（RAM 初始化读取文件；LiteEth 中一处移位越界，slang 报出）。8 个时钟，2445/2445 个寄存器解析出时钟，173 条跨域路径，其中 145 条已同步，32 个同步器结构。XDC 的三条时钟组约束生效后，系统时钟与以太网时钟之间没有未知关系。
  - 4 条潜在风险：
    - `ext_rst_n` 直接异步复位 `soc_rst`，释放没有同步（源码注释："XXX why does this need to be an asynchronous reset?"）；
    - LiteEth CSR 读多路选择器上的 3 处再汇聚（几个独立同步的状态位在同一个读数据寄存器汇合）。
  - 其余 156 条需复核的路径都来自 JTAG TCK：XDC 没有约束这个时钟。
  - 其他语料（open-logic、neorv32、PoC、OSVVM、surf、microwatt ECP5）的结果不变。

## 5. 已知限制
- microwatt `top-arty.vhdl` 的多驱动是设计本身的问题：
  - `ddram_clk_p_vec <= (others => ddram_clk_p)` 方向写反了，使用 DRAM 时顶层输出 `ddram_clk_p` 没有驱动；
  - `uart1_txd <= '1'` 与 SoC 的 UART1 输出冲突。

  OpenCDC 报错后不做 CDC 分析，语料中用修正了这三行的副本完成分析（corpus 脚本中有记录）。Vivado 按网表合并的方式处理这类连接，所以原工程在 Vivado 中能综合。
- JTAG 请求数据（`request`，TCK 域）在系统时钟域中由同步后的 `dmi_req` 限定后使用。目前只有直接由限定信号使能的目的寄存器被识别为握手；经过多级组合逻辑才到达的目的寄存器仍算作未同步，每个目的寄存器单独成一条路径。
- DSP48 的内部流水级（AREG/BREG/MREG 等）没有分别建模。
- `get_clocks -of_objects` 只用在时钟组中；`set_input_delay -clock [get_clocks -of …]` 还不支持。

## 6. 下一步建议
1. 识别经过组合逻辑的限定数据：以同步后的使能或请求作为限定条件，沿组合逻辑判断每个目的寄存器是否受限定条件控制，并把同一源寄存器到多个目的寄存器的路径合并显示。
2. 对 JTAG TCK 等未约束的黑盒时钟，在约束建议中给出 `create_clock` 和时钟组，并把需复核路径按源时钟汇总。
3. `get_clocks -of_objects` 支持更多命令（`set_input_delay`、`set_false_path` 的 `-from/-to`）。

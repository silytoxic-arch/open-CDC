# 阶段 18 完成报告：厂商 Tcl 约束脚本与寄存器级例外

日期：2026-10-25　版本：0.18.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T18.1 寄存器级例外 | `set_false_path` 和 `set_max_delay -datapath_only` 写在寄存器、引脚、端口之间时也求值。可以只写一侧（`set_false_path -to <复位引脚>`），也可以时钟与寄存器混合（`-from <时钟> -to <寄存器>`）。被覆盖、且时钟关系未知的跨域路径按异步分析，说明中写明是哪条约束。 |
| T18.2 Tcl 子集 | `foreach`、`if/elseif/else`、`for`、`while`、`break`、`continue`、`expr`（数值、字符串、数学函数、`NaN`）、`llength`、`lindex`、`lappend`、`incr`、`puts`，以及 `get_property`（时钟 `PERIOD`、单元属性）。 |
| T18.3 `get_cells` | 返回触发器和层次实例，支持 `-hierarchical`、`-regexp`、`-filter`。过滤属性有 `NAME`、`PARENT`、`REF_NAME`、`ORIG_REF_NAME`、`IS_SEQUENTIAL`，以及设计中出现的 HDL 属性。`-regexp` 下 `=~` 按正则匹配。 |
| T18.4 沿网表追溯 | `get_pins`、`get_nets`、`get_cells` 的 `-of_objects`。触发器引脚有 `C D Q CE CLR PRE R S`，引脚过滤属性有 `REF_PIN_NAME`、`DIRECTION`、`IS_LEAF`、`IS_PRESET`、`IS_CLEAR` 等。`get_clocks -of_objects` 可以作用于单元。 |
| T18.5 命名 | 1 位寄存器也接受 `x_reg[0]`。只以常量下标写入的数组按元素命名为触发器（`pipe_reg[0][*]`）；以计算出的下标写入的数组是存储器，不产生触发器。 |
| T18.6 真实工程 | verilog-ethernet 的 Arty 示例连同它自带的 4 个 Vivado Tcl 约束脚本加入 Verilog 语料回归。 |

## 2. 修改的文件

**修改**
- `src/constraints/Sdc.h`
  - `Object` 增加 `item`、`instance`、`pin`；
  - 解析器接口增加单元查询、属性和网表追溯；
  - `PathException` 改为带引脚角色的端点和时钟侧。
- `src/constraints/Sdc.cpp`
  - 解析器：实例表、正则匹配、数组触发器、引脚、网络负载表；
  - 解释器：表达式求值器、控制流、列表命令、`get_property`、过滤表达式、`-of_objects`；
  - 例外的预检与求值，`-quiet`。
- `src/clock/ClockModel.h`、`src/clock/ClockAnalyzer.cpp`：路径例外中 `get_clocks -of_objects` 的解析。
- `src/cdc/CdcAnalyzer.cpp`：路径例外按源节点或源时钟、目的寄存器与引脚角色或目的时钟匹配。
- `src/engine/ResultJson.cpp`：约束摘要增加 `clockExceptions`、`pathExceptions` 两个计数。
- 测试：
  - `tests/constraints/test_sdc.cpp`：Tcl 子集、错误、设计查询、网表追溯、正则过滤；
  - `tests/cdc/test_cdc.cpp`：寄存器级例外、厂商脚本、复位源追溯、只写复位引脚、时钟到寄存器、`-quiet`；
  - `tests/cdc/CdcTestUtil.h`、`tests/engine/test_analysis_session.cpp`。
- `tools/ci/sv-corpus.sh`：Arty 示例工程。
- `README.md`、`docs/architecture.md`（第 25 节）、`CMakeLists.txt`（版本 0.18.0）。

**行为变化**
- 原来只记录不用的 `set_false_path -from [get_ports rst]`、`set_false_path -to [get_pins …/PRE]` 等写法，现在会求值。
- 原来遇到 `foreach` 和 `if` 时报"不支持"并跳过整个命令，现在会执行。
- `get_cells` 的过滤条件引用设计中没有的属性时，查询返回空并告警。原来的做法是忽略过滤条件、返回全部匹配。

## 3. 关键算法

### 3.1 先看写法，再决定是否求值
- 求值的条件：
  - `-from`、`-to` 的原始写法是 `get_clocks`/`all_clocks`、`get_cells`/`get_pins`/`get_ports` 查询，或保存查询结果的变量；
  - 不带 `-through`、`-hold` 和沿选项；
  - `set_max_delay` 带 `-datapath_only`；
  - 不对 `get_nets`/`get_ports` 使用过滤条件。
- 其余写法只记录，参数不求值，所以不会因为查询失败产生告警。

### 3.2 例外端点
- `-from`：触发器记为其 Q，端口记为输入。
- `-to`：
  - 单元是整个寄存器；
  - 触发器引脚带角色：`D/CE/R/S` 为数据，`CLR/PRE` 为异步复位；
  - 实例端口（如 LiteX 中 FDPE 原语实例的 PRE）记为实例内部由该端口驱动的触发器。
- CDC 分析中，异步复位路径只匹配复位角色，数据路径只匹配数据角色。例如 `-to` 复位引脚的例外，不会把同一寄存器的数据跨域也盖掉。

### 3.3 网表追溯
- 网络上的叶子引脚按驱动源归类：驱动该网络的寄存器给出 `Q`，所有触发器的输入引脚按其驱动源建立一张负载表（首次使用时构建）。
- 这样 `get_pins -of_objects [get_nets -of_objects $D] -filter {IS_LEAF && DIRECTION == OUT}` 能找到源寄存器，与别名和层次端口无关。

### 3.4 替换规则
`"$inst/s1_reg"` 这类由对象和字面文本拼成的词是字符串，不带对象。这一点修正了一个缺陷：原来 `foreach` 的循环变量带着实例对象，`get_cells "$inst/x_reg"` 实际查询的是实例本身。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release 与 Clang 18 Debug（均为 `-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 394 个 CTest 用例全部通过（阶段 17 为 386 个）。新增用例：

  | 类别 | 内容 |
  |---|---|
  | 正例 | 控制流与表达式；按 `REF_NAME` 遍历实例、按 `PARENT` 和 `ASYNC_REG` 过滤、按位正则；`get_clocks -of_objects [get_cells …]`；生成时钟的 `PERIOD`；网表追溯（引脚、网络、驱动者、负载、单元）；数组触发器命名；复位源追溯链；只写 `-to` 复位引脚；时钟到寄存器 |
  | 负例 | 表达式语法错误、未定义变量、非布尔条件、死循环上限、多列表 `foreach`；不存在的属性、坏的正则；只写复位引脚的例外不覆盖数据跨域；来自另一时钟的例外不生效；不带 `-datapath_only` 的最大延迟；`-quiet` 下告警降为提示 |

- **真实工程**：

  | 工程 | 结果 |
  |---|---|
  | verilog-ethernet Arty 示例（MMCM、MII、UDP/IP，608 个寄存器，4 个时钟） | 只用工程自带的 `fpga.xdc` 和 4 个 Tcl 脚本，27 条跨域路径全部已同步，需要复核的路径从 26 条降到 0 条 |
  | microwatt Arty 约束 | 第 637 行（LiteX 的 `ars_ff1` 复位同步器）求值后没有告警；第 634 行（`-through`）、第 639 行（不带 `-datapath_only`）按设计不求值 |
  | microwatt（ECP5、Arty、JTAG）、verilog-ethernet RGMII、PULP axi_cdc、open-logic、neorv32、PoC、OSVVM、surf | 结果不变 |

  Arty 示例中每条路径引用的约束行都已逐一核对：

  | 路径 | 约束 |
  |---|---|
  | FIFO 指针、提交握手、状态同步 | `axis_async_fifo.tcl` 的 `set_max_delay -datapath_only` |
  | FIFO 内部复位同步器 | 同一脚本中由 D 引脚追溯源寄存器的写法 |
  | FIFO 输出寄存器 | `set_false_path -from $write_clk -to $output_reg_ffs` |
  | PHY 复位 | `mii_phy_if.tcl` 的 `-to` PRE 引脚 |
  | MAC toggle 同步 | `eth_mac_fifo.tcl` |

## 5. 已知限制

- 数组触发器的元素按存储顺序从 0 编号，IR 不记录数组的声明范围；`[N-1:0]` 和 `[0:N-1]` 的编号可能与 Vivado 不同。
- 触发器没有 `REF_NAME`（具体原语由综合选定），也没有 `IS_PRIMITIVE`；实例端口没有 `DIRECTION`。
- 不支持的写法：
  - `get_nets`/`get_ports` 的过滤条件；
  - `proc`、`source`、`switch`、`eval`；
  - `-through` 例外。
- 由 `get_clocks -of_objects` 得到的时钟，周期在约束求值时还不知道（返回 `nan`）。这个值只用于延迟数值，而延迟数值不参与分析。
- 例外只作用于时钟关系未知的路径；已知为同步的路径不会因为例外改判。

## 6. 下一步建议

1. 支持 `proc` 定义与调用、`source`，以便直接读入更多的 Vivado 工程脚本。
2. Intel Quartus SDC 的常见写法：`get_registers`、`get_keepers`、`set_false_path -to [get_keepers *altera_reset_synchronizer*]`。
3. 更多真实工程：带 Xilinx MIG/PCIe 的设计、Intel 平台的设计。

# 阶段 19 完成报告：Tcl 过程与 source、Quartus SDC

日期：2026-10-25　版本：0.19.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T19.1 Tcl 过程 | `proc`（缺省参数、`args`）、`return`、`global`，每次调用一个局部变量帧。过程体在定义它的文件中运行，诊断指向过程体所在的行。 |
| T19.2 source | 相对路径先在执行 `source` 的文件所在目录查找，再在工程新字段 `tclSourceDirs` 的目录中查找（GUI 工程编辑器同步支持）。找不到时告警，并提示配置哪个字段。 |
| T19.3 共享解释器 | 新增 `SdcReader`：一次分析的所有约束文件共用一个解释器，与 Vivado、Quartus 一致，前面文件定义的过程后面可以调用。 |
| T19.4 Quartus 集合 | `get_registers`/`get_keepers`/`get_nodes`，以及带 `|` 名字的 `get_cells`/`get_pins`（寄存器按 HDL 信号名命名）；`get_collection_size`、`foreach_in_collection`、`get_node_info -name` 等集合命令。伸进 IP 内部的引脚路径解析为 IP 的端口。 |
| T19.5 其他 Quartus 写法 | 周期带单位（`"40.000 ns"`、`{125 MHz}`）；`derive_pll_clocks` 等；`-from *`；Quartus 风格的寄存器间 `set_max_delay`；`altera_reserved_*` 端口记为提示。 |
| T19.6 缺陷修复 | 单组 `set_clock_groups` 的诊断原来说"不起作用"，而关系计算一直按"与其他所有时钟异步"处理（这也是 SDC 的语义）；现在改为提示，说明实际处理。重复的告警去掉：查询已经报过的未匹配，不再由 `set_input_delay` 或例外再报一次。 |
| T19.7 真实工程 | verilog-ethernet 的 DE2-115 示例（Cyclone IV、altpll、altddio）连同其 Quartus SDC 加入语料回归；同时验证了 C10LP 示例。 |

## 2. 修改的文件

**修改**
- `src/constraints/Sdc.h`
  - 新增 `SdcReader`、`SdcOptions`、`QuartusKind`；
  - 解析器接口新增 `quartus()`。
- `src/constraints/Sdc.cpp`
  - 变量帧；
  - `proc`、`return`、`global`、`source`；
  - Quartus 集合与名字表，IP 内部路径的回退；
  - 带单位的周期；
  - 例外预检中的 Quartus 查询和 `*`；
  - 重复告警的处理。
- `src/clock/ClockAnalyzer.cpp`：`CON-GROUP-SINGLE` 改为提示，并更正措辞。
- `src/engine/ProjectConfig.{h,cpp}`、`src/engine/AnalysisSession.cpp`：`tclSourceDirs` 字段，所有约束文件共用一个 `SdcReader`。
- `src/gui/ProjectEditor.{h,cpp}`：工程编辑器中的 Tcl 源目录列表。
- 测试：
  - `tests/constraints/test_sdc.cpp`：过程、source 与共享解释器、周期单位、Quartus 集合；
  - `tests/cdc/test_cdc.cpp`：Quartus 端到端；
  - `tests/clock/test_clock.cpp`：单组时钟组；
  - `tests/engine/test_project_config.cpp`：新字段的往返。
- `tools/ci/sv-corpus.sh`：DE2-115 工程。
- `README.md`、`docs/architecture.md`（第 26 节，并更正 `CON-GROUP-SINGLE` 的说明）、`CMakeLists.txt`（版本 0.19.0）。

## 3. 关键算法

### 3.1 过程与作用域
- 变量按帧保存：全局帧，加上每次调用压入的局部帧；`global` 把名字链接到全局帧。
- `return` 通过控制标志逐层返回：循环遇到它不清除，而是继续向外传递，直到过程调用（或文件顶层）收下返回值。
- 过程记录定义它的文件。调用时临时切换当前文件，所以过程体中的诊断和嵌套的 `source` 都相对于定义它的文件。

### 3.2 Quartus 名字
- 由 Vivado 风格的寄存器表派生出 Quartus 名字表：去掉 `_reg` 后缀，`/` 换成 `|`。两张表的下标一一对应，所以寄存器的引脚、属性、时钟查询都可以直接复用。
- `get_pins` 的路径找不到时，沿 `|` 逐段在实例表中前进，找到最深的已知实例：
  - 如果路径在它之后还有未建模的层级，就取最后一段作为它的端口名；
  - 如果路径没有越过已知的层级，按普通的未匹配处理。

### 3.3 Quartus 的最大延迟
Quartus 没有 `-datapath_only`。在 Quartus SDC 里，寄存器间的 `set_max_delay` 就是跨域约束的写法。所以 `-from`/`-to` 用 Quartus 集合写成时，最大延迟也按路径例外处理；Vivado 写法仍然要求 `-datapath_only`。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release 与 Clang 18（均为 `-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 400 个 CTest 用例全部通过（阶段 18 为 394 个）。新增用例：

  | 类别 | 内容 |
  |---|---|
  | 正例 | 缺省参数与 `args`；`return`（含循环中）；`global`；source 的两级查找；后一个文件调用前一个文件的过程；周期单位；Quartus 寄存器、端口、集合命令；IP 内部路径；寄存器引脚；Quartus 端到端（过程中的最大延迟、`-from *`）；单组时钟组 |
  | 负例 | 参数个数错误、递归上限、局部变量不外泄；找不到的 source；另一个解释器不认识该过程；未知单位；没有越过已知层级的路径；`-from * -to *`；Vivado 写法的普通最大延迟 |

- **真实工程**：

  | 工程 | 结果 |
  |---|---|
  | verilog-ethernet DE2-115 示例（618 个寄存器） | 改前：1 个错误、41 条告警，`source` 与 5 个过程调用全部跳过。<br>改后：0 个错误、14 条告警。<br>• 过程定义的 RGMII 输入/输出时钟都已建立，其中 `enet0_tx_clk_125m` 由 `altpll_component|auto_generated|pll1|clk[0]` 正确解析为 PLL 的 `clk[0]` 派生。<br>• 剩下的告警：9 条是前端的未连接端口，5 条是工程 SDC 引用了 RTL 中没有的端口（`CLOCK2_50`、`enet0_mdio` 等，Quartus 同样会报）。<br>• 18 条跨域路径中 17 条已同步。2 条需要复核：输入延迟用的是虚拟时钟，它与采样时钟之间没有约束关系。2 条潜在风险：与阶段 17 在 RGMII MAC 中的发现相同，是速率选择同步器与 FIFO 读指针的再汇聚。 |
  | verilog-ethernet C10LP 示例 | 0 个错误；剩下的告警同样是工程 SDC 引用了不存在的端口 |
  | Arty、RGMII、axi_cdc、microwatt、open-logic、neorv32、PoC、OSVVM、surf | 结果不变 |

## 5. 已知限制

- 不支持 `switch`、`eval`、`upvar`、`uplevel`、`namespace`、数组变量。
- Quartus 名字中，generate 块的层级沿用 IR 的写法（`g[0].u`），与 Quartus 的命名可能不同。
- `get_nodes` 只返回寄存器和端口，不返回组合逻辑节点。
- 虚拟时钟（用于 `set_input_delay`）与实际时钟之间没有约束时，关系为未知，路径需要复核；SDC 的时序分析会把它们当作相关时钟处理。

## 6. 下一步建议

1. 虚拟时钟：对 `set_input_delay` 使用的虚拟时钟，如果它与采样时钟周期相同、并且有 `set_false_path -rise_from/-fall_to` 这类边沿约束，可以识别为源同步接口，作为说明附在路径上。
2. Intel 的复位同步器与 IP：`altera_reset_synchronizer`、`get_keepers *|altera_reset_synchronizer*` 的常见写法。
3. 更多真实工程：带 Xilinx MIG/PCIe 或 Intel EMIF 的设计。

# 阶段 20 完成报告：源同步输入、Intel 写法、原语模型修正

日期：2026-10-26　版本：0.20.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T20.1 源同步输入 | 输入端口以虚拟时钟为参考（`set_input_delay`）、被同周期时钟采样时，按 STA 的处理视为同步路径，不算跨域，并以 `CDC-SOURCE-SYNC` 提示说明。 |
| T20.2 Quartus 完整名字 | 支持 `实体名:实例名` 层级的写法（`*altera_std_synchronizer:*|din_s1`）。 |
| T20.3 Intel 同步器模型 | `altera_std_synchronizer`、`altera_reset_synchronizer` 的寄存器名和结构与 Intel 源码一致，Intel 自带的约束可以直接匹配。 |
| T20.4 缺陷修复：原语被挡住 | 用户模块与原语只差大小写时（verilog-ethernet 的 `iddr` 包装 `IDDR`），原语模型被排除，实例成了不报告的黑盒。改为区分大小写比较。另加收尾检查：没有模块体的实例一律报 `FE-UNRESOLVED-INSTANCE`。 |
| T20.5 Vivado 名字中的 generate 块 | 匹配时也用去掉 `genblkN.` 各段的名字，`oddr[0].oddr_inst` 这类约束可以匹配。 |
| T20.6 延迟单元模型 | IDELAYE2/ODELAYE2/IDELAYE3/ODELAYE3 在 FIXED 模式下没有抽头计数器（原来会误报"时钟恒为 0"）。 |
| T20.7 真实工程 | verilog-ethernet NexysVideo 示例（RGMII，Vivado 脚本）加入语料回归。 |

## 2. 修改的文件

**修改**
- `src/cdc/CdcAnalyzer.cpp`：源同步判定与 `CDC-SOURCE-SYNC`
- `src/constraints/Sdc.{h,cpp}`：Quartus 完整名字表；`withoutGenblk` 别名匹配
- `src/frontend/AutoFrontend.cpp`：原语名区分大小写；`FE-UNRESOLVED-INSTANCE`
- `src/frontend/primitives/intel.v`、`src/frontend/primitives/xilinx_unisim.v`
- 测试：
  - `tests/cdc/test_cdc.cpp`：源同步；
  - `tests/frontend/test_primitives.cpp`：Intel 同步器与约束、大小写、FIXED 延迟；
  - `tests/constraints/test_sdc.cpp`：genblk。
- `tools/ci/sv-corpus.sh`：NexysVideo；RGMII、DE2-115 的期望值
- `README.md`、`docs/architecture.md`（第 27 节）、`CMakeLists.txt`（版本 0.20.0）

## 3. 关键算法

### 3.1 源同步输入
在判断一条路径是否跨域时，对"源时钟是虚拟时钟、目的时钟有源、两者周期相同、关系未知"的组合跳过。这与 STA 一致：没有时钟组时，STA 会对两个时钟之间的路径计时，而同周期的虚拟时钟正是源同步接口描述外部发送时钟的写法。

### 3.2 原语与用户模块
Verilog 区分大小写，所以只有名字完全相同的模块才替代原语。VHDL 不区分大小写，仍按小写比较。链接结束后，任何没有模块体的子实例都要报告：内部黑盒（`opencdc_async_source`）除外，其他诊断已经提到的名字也不重复报告。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release 与 Clang 18（`-Werror`）通过；Windows MSVC 由 CI 确认。
- **测试**：本地 405 个 CTest 用例全部通过（阶段 19 为 400 个）。新增用例：

  | 类别 | 内容 |
  |---|---|
  | 正例 | 源同步输入不算跨域；Intel 约束写法匹配，三级同步器与复位同步器识别；`iddr` 包装 `IDDR` 时模型被使用；FIXED 延迟单元没有寄存器；genblk 别名匹配 |
  | 负例 | 周期不同的虚拟时钟仍是跨域；时钟组声明异步时仍是跨域；实体名不符时不匹配；用户定义同名 `IDDR` 时替代模型；可变延迟保留计数器；层级不对时不匹配 |

- **真实工程**：

  | 工程 | 结果 |
  |---|---|
  | verilog-ethernet NexysVideo（Vivado 脚本） | 645/645 个寄存器解析；IDDR/ODDR/IDELAYE2 模型生效；16 条跨域路径全部已同步，需要复核 0 条；剩下的 9 条告警都是前端的未连接端口 |
  | verilog-ethernet DE2-115 | 需要复核的路径从 2 条降到 0 条（源同步输入识别） |
  | verilog-ethernet RGMII | IDDR/ODDR 模型现在被使用，寄存器从 179 个增至 206 个；跨域结论不变 |
  | Arty、axi_cdc、microwatt、open-logic、neorv32、PoC、OSVVM、surf | 结果不变 |

**对先前报告的更正**：阶段 17 的报告写到 RGMII 工程使用了 Xilinx IDDR/ODDR 模型，实际上由于上述大小写缺陷，这两个模型当时没有被使用。本阶段已经修复。

## 5. 已知限制

- 源同步判定只看周期是否相同，不检查边沿约束（`-rise_from`/`-fall_to`）和输入延迟的数值。
- IDELAYE3/ODELAYE3 在 FIXED 模式下的 `CNTVALUEOUT` 取 0（TIME 格式的延迟值是皮秒，不是抽头数）；对 CDC 分析而言只需要它是常数。
- `FE-UNRESOLVED-INSTANCE` 是兜底检查，目前已知的触发路径都已修复，所以没有能直接触发它的测试用例。

## 6. 下一步建议

1. 源同步接口的输出方向：`set_output_delay` 到由 ODDR 转发的时钟（`create_generated_clock` 在输出端口上），检查发送数据与转发时钟的同源关系。
2. 更多真实工程：带 Xilinx MIG/PCIe 或 Intel EMIF 的设计。
3. Quartus 中 generate 块的命名规则（`gen:0:inst`）。

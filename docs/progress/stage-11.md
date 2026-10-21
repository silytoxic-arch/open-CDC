# 阶段 11 完成报告：SERDES/UltraRAM、分频缓冲器、各视图中的时钟频率

日期：2026-10-21　版本：0.11.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T11.1 原语 | **SERDES 与存储器**：`ISERDESE2/3`、`OSERDESE2/3`、`URAM288`、Intel `altera_syncram`。<br>**分频缓冲器**：`BUFR`（`BUFR_DIVIDE`）、`BUFGCE_DIV`（`BUFGCE_DIVIDE`）的输出成为周期按分频系数计算的派生时钟；不分频时与输入是同一个时钟。<br>**VHDL**：组件包新增对应组件。 |
| T11.2 接口 | modport 表达式端口（`input .g(gray)`）；接口中的函数通过 modport `import` 调用（Gray 编码函数写在接口里，同步器照样识别为 Gray 且条件已验证）。 |
| T11.3 频率与时钟树 | **频率显示**：CLI（`period=5ns (200 MHz)`）、HTML 报告（新增"派生自"和"频率"列）、JSON（`frequencyMHz`）都给出频率。<br>**GUI 时钟视图**：新增频率列；派生时钟放在主时钟下面。 |
| 发现并修复 | **未写入的存储器误报跨域**：只读 RAM 端口（`WEA = 0`）的写数组原来被当作跨域源。现在沿层次和简单逻辑求常量控制：使能恒为 0 的寄存器不再加载，CDC 分析不把它当作源。<br>**未连接引脚的警告**：原语未连接的输入引脚不再报警告（阶段 10 已处理输出引脚）。 |
| 示例 | `examples/primitives/`：<br>- MMCM 派生两个时钟（200/50 MHz）；<br>- 由 `LOCKED` 释放的复位同步器；<br>- XPM Gray 同步器；<br>- 块 RAM 跨域（真实违规）；<br>- 一个单级同步（真实违规）。<br>对应测试为 `cli.analyze_primitives` 和 `gui.primitives`。 |

## 2. 修改的文件

**新建**
- `examples/primitives/`：`top.v`、`clocks.sdc`、`primitives.ocdc.json`
- `docs/progress/stage-11.md`

**修改**
- `src/frontend/primitives/`：`xilinx_unisim.v`（BUFR 分频、BUFGCE_DIV、SERDES、URAM288）、`intel.v`（altera_syncram）、`unisim_vcomponents.vhd`、`altera_mf_components.vhd`
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：`modportExpression`
- `src/frontend/AutoFrontend.cpp`：未连接输入引脚的过滤
- `src/cdc/SeqGraph.{h,cpp}`：`constantBit`（沿层次和逻辑求常量）、`neverLoads`
- `src/cdc/CdcAnalyzer.cpp`：不加载的寄存器不作为源
- `src/cli/main.cpp`、`src/engine/Report.cpp`、`src/engine/ResultJson.cpp`：频率；HTML 主时钟列
- `src/gui/ClockView.cpp`：频率列、派生时钟树
- `src/gui/main.cpp`：自测检查时钟树中每个时钟恰好出现一次
- `tests/`：`CMakeLists.txt`、`frontend/test_primitives.cpp`、`frontend/test_sv_interfaces.cpp`
- `CMakeLists.txt`（版本 0.11.0）、`README.md`、`docs/architecture.md`（第 18 节）

JSON 只新增 `frequencyMHz` 字段；已有接口没有删除，也没有改变语义。

## 3. 关键算法

### 3.1 常量控制求值
`SeqGraph::constantBit(inst, sig, bit)` 先按位找驱动：
- 常量：直接返回该值；
- 组合单元：按单元类型递归求值，深度不超过 8。

规则（只对 0/1 求值，X/Z 视为未知）：
- 与：任一输入为 0 则为 0；
- 或：任一输入为 1 则为 1；
- 归约或：任一位为 1 则为 1，全为 0 则为 0；
- 选择器：选择信号是常量时取对应输入；两个数据输入相等时取该值。

结果用于三处：
- 去掉恒为有效的使能；
- 去掉恒为无效的复位；
- 标记使能恒为 0 的寄存器为 `neverLoads`。被标记的寄存器在跨域枚举中不作为源。

### 3.2 派生时钟树
GUI 先为每个时钟建一个节点，填入寄存器之后，再把有主时钟的节点移到主时钟节点下。若主时钟链回到自身（环），该节点留在顶层。自测会遍历整棵树，检查每个时钟恰好出现一次。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过。Windows MSVC 的结果以推送后的 CI 为准。
- **测试**：本地 335 个 CTest 用例全部通过（阶段 10 是 329 个）。新增：
  - BUFR/BUFGCE_DIV 周期、SERDES 不跨域、OSERDES 输入跨域、URAM 单时钟、未建模引脚；
  - `altera_syncram`；
  - 使能恒为 0 的寄存器不是源（正例），以及使能取决于输入时仍是源（负例）；
  - 块 RAM 只读端口不跨域；
  - 接口函数与 modport 表达式；
  - 示例的 CLI 和 GUI 测试。
- **语料回归**：VHDL 语料（open-logic、neorv32、surf）和 SV 语料（PULP AXI CDC）的结果与阶段 10 完全相同。

## 5. 已知限制
- `ISERDESE2` 的 `NETWORKING` 模式时序、`BITSLIP` 的细节、`OSERDESE2` 的三态字节控制只做结构近似。
- 常量求值不覆盖算术和比较单元。
- GT 收发器（`GTXE2`、`GTYE4` 等）没有模型。

## 6. 下一步建议
1. GT 收发器的时钟输出（`TXOUTCLK`/`RXOUTCLK`）作为独立时钟源，并提供 `create_clock` 提示。
2. 把阶段 9–11 的原语库整理成文档表格：每个原语建模了哪些行为、忽略了哪些引脚。
3. 时钟视图中显示每个派生时钟的计算过程（输入周期 × 比例）。

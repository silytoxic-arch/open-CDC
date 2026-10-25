# 阶段 12 完成报告：GT 收发器、原语库文档、周期推导显示

日期：2026-10-25　版本：0.12.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T12.1 GT 收发器 | 新增模型：<br>- 通道：`GTXE2/GTHE2/GTPE2/GTHE3/GTHE4/GTYE3/GTYE4_CHANNEL`；<br>- 公共模块：同系列的 `_COMMON`；<br>- 参考时钟缓冲：`IBUFDS_GTE3/4`。<br>行为：<br>- `RXOUTCLK`/`TXOUTCLK` 是独立的时钟源，按引脚命名（`top.ch.RXOUTCLK`）。未约束时，它们与其他时钟的关系为未知，跨域路径标为待复核（CDC-010）。<br>- Vivado 写法的 `create_clock -period 3.2 [get_pins ch/RXOUTCLK]` 可以直接使用。<br>- `TXDATA` 在 `TXUSRCLK2` 上采样，`RXDATA` 在 `RXUSRCLK2` 上输出。<br>- 复位完成、锁定类输出没有时钟域，用作异步复位时按 CDC-006 检查。<br>- QPLL 输出是参考时钟的派生时钟。<br>模型由 `tools/primitives/gen_gt.py` 生成。 |
| T12.2 原语库文档 | 新增 `docs/primitives.md`：每个原语（共 100 余个）建模了什么、忽略了哪些引脚、有哪些已知近似，以及未建模原语的处理方式。<br>新增单元测试，检查库中每个模型都出现在文档里。 |
| T12.3 周期推导 | 派生时钟记录周期的来源（比例与基准）。<br>- CLI：`period=5ns (200 MHz) = clk_in x 0.5`；<br>- HTML 报告与 GUI 时钟视图：`= clk_in × 0.5`；<br>- JSON：`periodRatio`、`periodBasis`。<br>适用范围：SDC 生成时钟、MMCM/PLL 输出、BUFR/BUFGCE_DIV 分频。 |
| 改进 | **未建模引脚的提示**：同一原语的提示合并成一条，列出前 8 个引脚名和总数。surf 的 GTY 通道原来会报 257 条，现在是 1 条。 |

## 2. 修改的文件

**新建**
- `tools/primitives/gen_gt.py`
- `docs/primitives.md`
- `docs/progress/stage-12.md`

**修改**
- `src/frontend/primitives/xilinx_unisim.v`：GT 模型，由生成器追加。
- `src/frontend/primitives/unisim_vcomponents.vhd`：GT 组件。
- `src/clock/ClockModel.h`：`periodRatio`、`periodBasis`。
- `src/clock/ClockAnalyzer.cpp`：黑盒时钟按引脚命名；记录周期推导。
- `src/frontend/AutoFrontend.cpp`：合并未建模引脚的提示。
- `src/cli/main.cpp`、`src/engine/Report.cpp`、`src/engine/ResultJson.cpp`、`src/gui/ClockView.cpp`：显示周期推导。
- `tests/frontend/test_primitives.cpp`、`tests/clock/test_clock.cpp`。
- `tools/ci/vhdl-corpus.sh`：新增 surf 的 GTY 四通道 PLL。
- `CMakeLists.txt`（版本 0.12.0）、`README.md`、`docs/architecture.md`（第 19 节）。

JSON 只新增字段；已有接口没有删除，也没有改变语义。

## 3. 关键算法

### 3.1 GT 时钟
- **恢复时钟和发送时钟**：各由一个 `opencdc_async_source` 驱动，驱动线网带 `OPENCDC_CLOCK_PIN` 属性。时钟分析建立黑盒时钟时，用这个属性把时钟名改成"实例名.引脚名"。
- **用户约束**：约束写在模型的端口线网上。时钟追溯遇到约束源就停止，所以用户的约束优先于黑盒时钟。

### 3.2 周期推导
- 生成时钟：比例为 `divide_by / multiply_by`，基准为主时钟。
- 时钟生成器和分频缓冲器：比例来自模型属性，基准为主时钟；主时钟没有周期时，基准为 `CLKIN1_PERIOD`。

各个输出只负责格式化，不重新计算。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过。Windows MSVC 的结果在推送后由 CI 确认。
- **测试**：本地 339 个 CTest 用例全部通过（阶段 11 是 335 个）。新增：
  - GT 时钟是独立时钟源，未约束时跨域路径带 CDC-010；`RXRESETDONE` 未同步时报 CDC-006；
  - Vivado 写法约束 GT 引脚后报 CDC-003；同一恢复时钟内部不跨域（负例）；
  - VHDL 中的 UltraScale+ GTY 通道和公共模块；
  - 原语文档完整性；
  - 生成时钟和生成器输出的周期推导。
- **真实工程**：
  - surf `GtyUltraScaleQuadPll`（GTYE4_COMMON + AXI-Lite DRP）：0 个错误、0 个警告，18 个寄存器全部找到时钟；
  - surf `Gtye4ChannelDummy`：能正常展开，257 个未建模引脚合并为一条提示；
  - VHDL 语料（open-logic、neorv32、surf）和 SV 语料（PULP AXI CDC）的结果与阶段 11 相同。

## 5. 已知限制
- GT 的 `TXOUTCLK` 与参考时钟的分频关系（由线速率和 `TXOUTCLKSEL` 决定）没有建模，作为独立时钟源处理。这样做偏保守：不会漏报跨域，代价是需要约束才能消除待复核项。
- GT 内部的 8b/10b、通道绑定、弹性缓冲不建模，只建模数据进出的时钟域。
- 未建模引脚的提示按"原语类型 + 文件"合并：同一文件里同类型的多个实例会合成一条。

## 6. 下一步建议
1. 约束辅助：根据设计中的生成器和 GT，导出建议的 SDC（`create_clock`、`set_clock_groups`），供用户审阅后使用。
2. 跨文件的时钟关系报告：列出所有关系为"未知"的时钟对，以及能消除它们的约束。
3. Intel 和 Lattice 常用 PLL（`altera_pll`、`EHXPLLL`）的频率模型。

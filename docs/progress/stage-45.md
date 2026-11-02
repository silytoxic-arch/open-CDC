# 阶段 45：BUFG_GT 的 DIV 分频、IBUFDS_GTE 的 ODIV2

日期：2026-11-02　版本：0.45.0

## 1. 完成的功能

### 1.1 `BUFG_GT`：按 `DIV` 端口 + 1 分频

`DIV` 是端口而不是参数，通常在实例上接常量（SURF：`DIV => "001"`）。原语模型在模块内部看不到这个常量，所以由时钟分析读取：

- **模型**：一个分频寄存器，属性 `OPENCDC_CLOCK_DIVIDE_PORT = "DIV"`。
- **时钟分析**：沿驱动关系向上，穿过层次追到实例上的常量，得到 `DIV` 的值：
  - `DIV` = 0：输出就是输入时钟本身，不产生新的时钟；
  - `DIV` = n：派生时钟，周期 = 输入周期 ×（n + 1）；
  - `DIV` 不是常量：派生时钟不带周期，报 `CLK-DIVIDE-UNKNOWN`。
- **`DIV` = 0 时的分频寄存器**：它不是设计的一部分，不属于任何时钟域，也就不会凭空引出一个时钟（例如 `rxoutclk_out`）和一串未知关系；但仍计入"已解析"。

### 1.2 `IBUFDS_GTE2/3/4` 的 `ODIV2`

- `IBUFDS_GTE2`：`ODIV2` = `O` / 2（UG476）。SURF 中的信号名 `refClockDiv2` 也是这个意思。
- `IBUFDS_GTE3/4`：由 `REFCLK_HROW_CK_SEL` 决定（UG576/UG578）：`00` 为 `O`，`01` 为 `O` / 2，`10` 为低电平。
- 没有使用 `ODIV2` 时，分频寄存器作为死逻辑被删除，不影响寄存器数。

## 2. 真实工程验证

- **`TenGigEthGthUltraScale`（SURF，UltraScale）**：
  - `phyClock` 现在是 `CLK156_BUFG_GT.O` 派生时钟，周期 6.206 ns（161.13 MHz）= `txoutclk` 322.27 MHz / 2，别名 `txClockGt`；
  - `CLK312_BUFG_GT`（`DIV "000"`）就是 `txoutclk` 本身；
  - 阶段 44 中把 `phyClock` 看成 322 MHz 的已知限制由此解决。时钟从 5 个变为 6 个，寄存器 767/767（两个 `BUFG_GT` 的分频寄存器）；路径与违例不变。
- **`Pgp2fcGtyCoreWrapper`（SURF）**：`RXOUTCLK_BUFG_GT`（`DIV "000"`）的分频寄存器不属于任何域，时钟仍为 5 个，寄存器 459/459，路径与违例不变。
- 两个工程的语料库期望值已按上面的结果更新。

## 3. 修改的文件

- `src/frontend/primitives/xilinx_unisim.v`：`BUFG_GT`、`IBUFDS_GTE2/3/4`。
- `src/clock/ClockAnalyzer.cpp`：
  - `constantPort`：实例端口上的常量；
  - 按端口分频的寄存器（`OPENCDC_CLOCK_DIVIDE_PORT`）；
  - `DIV` = 0 时的分频寄存器不属于任何域。
- 测试 `tests/frontend/test_primitives.cpp`：
  - `BUFG_GT`：`DIV` 为 0、1、3 时分别为直通、×2、×4；负例：`DIV` 接信号时没有周期并报 `CLK-DIVIDE-UNKNOWN`；`DIV` = 0 的分频寄存器不属于任何域；×2 时钟与原时钟之间不是跨域；
  - VHDL 中通过 `unisim` 使用 `BUFG_GT`（`DIV => "001"`）；
  - `ODIV2`：`GTE2` 为 /2，`GTE3 01` 为 /2；负例：`00` 是原时钟，`10` 是常量（`CLK-CONSTANT`）。
- `tools/ci/vhdl-corpus.sh`：两个工程的期望值。
- 文档：`docs/primitives.md`。
- `CMakeLists.txt`：0.45.0。

## 4. 构建与测试结果

- Linux（GCC Debug）：499 个测试全部通过；Release 和 Clang 构建通过。
- 语料库：SystemVerilog 9 项、VHDL 38 项全部通过。
- Mac：请开发者运行 `tools/macos/build-macos.sh` 确认。

## 5. 已知限制

- `BUFG_GT` 的 `CE`/`CLR`（时钟门控和清零）不影响时钟分析。
- `DIV` 由运行时逻辑驱动（动态分频）时只能给出没有周期的派生时钟。

# 阶段 86：同步链的并行副本；BrianHG DDR3 的 Cyclone V 版本

日期：2026-11-09　版本：0.86.0

阶段 85 留下一个已知限制：同步链的第一级被多个寄存器读取时，只沿其中一个读取者向后追踪。BrianHG DDR3 的两个相同的行缓冲因此得到不同结论：一个“已限定”，一个“违例”。

阶段 85 试过两种修改，都因为大量改变已验证的语料结果而撤回：
- 只让满足全部条件的链作为限定信号；
- 把第一级的所有直接读取者都当作第二级。

这一阶段换了一个更窄的规则，修复了这个限制，并用同一控制器的 Cyclone V 版本和 16 层图形版本继续验证。

## 1. 完成的功能

### 1.1 同步链的并行副本（`CdcAnalyzer.cpp`）

- 规则：第一级被多个寄存器读取时，另一个读取者如果自己也开始一条链，并且长度不短于被追踪链的剩余部分（至少 2 级），就是这条链的并行副本。副本的各级同样是“同步后的信号”。
  - 读取者的条件：数据引脚、直接连接、同一时钟、没有使能。
- 与阶段 85 两次尝试的区别：
  - 不要求链满足全部条件（原有的限定关系不变）；
  - 只接受本身构成同样长度链的读取者。chiplab 中 AXI 互联模型里的普通寄存器只有一级，不再被当作限定信号。
- 结果：
  - BrianHG 两个行缓冲的存储器读出现在得到相同的结论（都由视频时序的延迟管道限定）；
  - SV 语料中其余 48 项、VHDL 语料 48 项的结果逐项不变。只有 BrianHG 一项从 5/47 变为 6/46（已同步/未同步）。
- 关于这个“限定”本身：视频同步发生器的输出由调试器的复位命令同步复位，延迟管道把它带到行缓冲。这个结构本身是一个合格的同步器（第一级只有一个异步输入，不会产生毛刺），所以把它作为限定信号与已有规则一致。

### 1.2 `altddio_bidir`（`intel.v`）

- 用于 Cyclone III/IV/V 版本的 DQ/DQS 引脚。
- 输入侧与 `altddio_in` 相同（`inclock`，下降沿采样后在上升沿重新寄存），读取 `padio`。
- 输出侧在 `outclock` 上寄存，不驱动 `padio`。与 `IOBUF` 一样，双向引脚只读。

### 1.3 Quartus 中 PLL 时钟的其他名字

- 模型属性 `OPENCDC_CLOCK_QUARTUS_SUFFIX` 可以用 `;` 分隔多个名字。不同 Quartus 版本、不同器件给同一个 PLL 输出不同的名字：
  - `altpll` 在 Cyclone V 上：`<实例>|auto_generated|generic_pll<n+1>~PLL_OUTPUT_COUNTER|divclk`；
  - `altera_pll`：除 `general[n].gpll~PLL_OUTPUT_COUNTER|divclk` 外，还有 `cyclonev_pll|counter[n].output_counter|divclk`。
- BrianHG Cyclone V 版本的 SDC 用 `*DDR3_PLL5*counter[2]*` 和 `VGA_PLL|VGA_SW_216_297|auto_generated|generic_pll1~PLL_OUTPUT_COUNTER|divclk`。以前这些输入/输出延迟都报告“未知时钟”，端口没有时钟域。

## 2. 真实工程验证

### BrianHG DDR3 控制器的其他版本

| 工程 | 第一次分析 | 修复后 |
|---|---|---|
| Cyclone V（`BrianHG_DDR3_CV_top.qpf`，加入语料库） | 多个错误（`altddio_bidir` 未知） | 0 错误，11 个时钟，710/710 个寄存器，37 条跨时钟路径 |
| Cyclone IV / Cyclone III | 同上 | 0 错误，10 个时钟，37 条跨时钟路径 |
| DECA 16 层图形 + 硬件寄存器（加入语料库） | 0 错误（阶段 85 的修改已足够） | 12 个时钟，961/961 个寄存器，159 条跨时钟路径（77 条已同步） |

- Cyclone V 版本的约束中还有 6 条 `CDC-OUTPUT-CLOCK` 告警（SDC 第 77、78 行各 3 条），是工程本身的问题：`HDMI_I2C_SCL`/`HDMI_I2C_SDA`/`HDMI_TX_INT` 按视频时钟约束输出延迟，但它们由 I2C 的 1 MHz 时钟域或输入直接驱动。
- 同一个 SDC 对 `u_I2C_HDMI_Config|mI2C_CTRL_CLK|q` 设置的伪路径引用了不存在的时钟名（时钟按 `create_clock` 的目标命名为 `I2C_HDMI_Config:u_I2C_HDMI_Config|mI2C_CTRL_CLK`），在 Quartus 中同样不会生效。

### 其他语料

- SV 语料 51 项全部匹配：新增两个 BrianHG 版本；BrianHG DECA 的同步路径数按 1.1 更新；其余与修改前逐行相同。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- `src/cdc/CdcAnalyzer.cpp`：同步链的并行副本。
- `src/clock/ClockAnalyzer.cpp`：多个 Quartus 名字。
- `src/frontend/primitives/intel.v`：`altddio_bidir`；`altpll`、`altera_pll` 的 Cyclone V 名字。
- 测试（每个用例都含反例）：
  - `tests/cdc/test_cdc.cpp`：两条并行链都限定数据；只有一级的读取者不是副本。我确认过：去掉新规则时这个用例失败。
  - `tests/frontend/test_quartus_compat.cpp`：`altddio_bidir` 与 Cyclone V 的时钟名；另一个时钟域的输出仍然报告。
- `docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：BrianHG 的 Cyclone V 与 16 层版本；DECA 的预期值。
- `CMakeLists.txt`：版本 0.86.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：678 个测试全部通过。
- 测试预期的两处修改：
  - 反例最初用 `clk50` 时钟域的输出，但 PLL 输出由 `clk50` 派生，属于相关时钟，不会报告。改为另一个独立时钟域的输出。
  - `altddio_bidir` 输出侧的寄存器没有读取者，被删除，它的 `outclock` 因此不是时钟。约束改为引用输入侧的时钟。
- SV 语料 51 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 85 的 Windows CI（提交 1eb94a3，运行 42602634006）：Release 与 Debug 均 676/676 通过。
- 本阶段的 Windows CI（提交 85d3aed，运行 42622852774）：Release 与 Debug 均 678/678 通过，生成 `OpenCDC-0.86.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- Quartus 中 PLL 时钟的名字只覆盖了已见到的几种形式（`pll1|clk[n]`、`generic_pll<n>`、`general[n].gpll`、`cyclonev_pll|counter[n]`）；Arria 10 及更新器件的 `iopll` 名字没有加入。
- `altddio_bidir` 的输出侧不驱动引脚。

## 6. 下一步

阶段 87：继续验证多时钟域的真实设计，优先选择带以太网 MAC 或视频采集的 SoC；同时检查 CDC 规则在大型设计上的运行时间。

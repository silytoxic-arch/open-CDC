# 阶段 85：多时钟域设计 —— BrianHG DDR3 控制器（MAX 10）

日期：2026-11-09　版本：0.85.0

阶段 84 的下一步是一个多时钟域的真实设计。这一阶段选择 **BrianHGinc/BrianHG-DDR3-Controller**（提交 a2b1746）的 DECA 演示工程 `BrianHG_DDR3_DECA_GFX_DEMO_v16_2_LAYERS`。设计包括：
- 一个自带 PHY 的 DDR3 控制器，运行在 Intel MAX 10（Terasic DECA 板）上；
- 两层图形和 HDMI 输出；
- 一个 RS232 调试器；
- 时钟：DDR3 PLL 给出 5 个时钟（400 MHz ×3、200 MHz、100 MHz），视频 PLL 链给出 54 MHz、297 MHz、148.5 MHz，另有板上 50 MHz、10 MHz 输入和 I2C 的 1 MHz 分频时钟。

工程直接按 Quartus 工程（`.qpf`）分析，约束是作者自己的 SDC。

第一次分析：
- 多个 `FE-UNKNOWN-MODULE` 错误：MAX 10 的 I/O 原子（`fiftyfivenm_ddio_out/oe/in`、`io_obuf/ibuf`、`pseudo_diff_out`）没有模型，`altera_gpio_lite` 无法细化；
- 补上模型后还有 3 个多驱动错误；
- 另有 19 个“任务会等待”的未分析构造、1 个组合环；SDC 中 1 个错误（`CON-PARSE`）、1 条 `CON-UNMATCHED` 和 24 条“未知时钟”告警（`CDC-INPUT-CLOCK`、`CDC-OUTPUT-CLOCK`）。

修复后：
- 0 个错误，15 个告警，0 个未分析的构造；
- 12 个时钟，903/903 个寄存器的时钟已解析；
- 52 条跨时钟路径（5 条已同步）。

## 1. 完成的功能

### 1.1 MAX 10 I/O 原子（`intel.v`）

| 原语 | 模型 |
|---|---|
| `fiftyfivenm_ddio_out` | `datainhi`/`datainlo` 在 `clkhi`/`clklo` 上寄存，输出按 `muxsel` 选择 |
| `fiftyfivenm_ddio_oe` | 寄存的输出使能 |
| `fiftyfivenm_ddio_in` | 下降沿采样再在上升沿重新寄存。增强 DDR 模式的 `clkout` 不建模，连接被忽略（见 1.2） |
| `fiftyfivenm_ff` | 带异步清零的 I/O 寄存器 |
| `fiftyfivenm_io_clock_divider` | `clkout` = `clk` / 2，派生时钟 |
| `fiftyfivenm_io_obuf`、`fiftyfivenm_io_ibuf`、`fiftyfivenm_pseudo_diff_out` | 缓冲（连线，`obar` 取反） |

### 1.2 `fr_clock` 的多驱动

`altera_gpio_lite` 在非增强模式下把 `fr_clock` 接常量 0，同时 `fiftyfivenm_ddio_in` 的 `clkout` 也连到这个线网。Quartus 接受，因为该模式下原子不驱动 `clkout`。模型不声明 `clkout`，按已有的“原语端口未建模”机制忽略这个连接（`FE-PRIMITIVE-PORT`，提示）。

### 1.3 调用任务的任务（`ProcessLowering.cpp`）

- 以前：一个任务体里只要调用了另一个任务，就认为它“会等待”（延迟或事件控制），整个调用不分析。
- 现在：递归检查被调用的任务；只有它确实含有延迟、事件控制或 `wait` 时才拒绝（递归调用、嵌套超过 16 层也拒绝）。
- DDR3 的命令序列器 `send_cmd` 调用 `SET_cas`，PHY 的 `DDR3_TX_CMD` 调用 `DDR3_TX`：19 处现在都内联分析。

### 1.4 `x && 0`、`x || 1` 的常量折叠（`ConstFold.cpp`）

- 以前：逻辑与/或只在两个操作数都是常量时折叠。
- 现在：一个操作数是确定的假（`&&`）或真（`||`）时，结果是常量。
- 影响：
  - 命令端口的 `CMD_busy[x] = busy_e[x] ^ (CMD_ena[x] && PORT_TOGGLE_INPUT[x])` 在 `PORT_TOGGLE_INPUT[x] = 0` 时不再是从 `CMD_ena` 到 `CMD_busy` 的组合路径。以前图形模块的请求经过它形成一个假的组合环（`GR-COMB-LOOP`）。
  - 语料库中 6 个工程有更多寄存器被识别为从不读取（例如 picorv32 中受 `ENABLE_* && ...` 控制的寄存器，多出 5 个）。寄存器数相应减少，跨时钟路径的结果逐项不变，预期值已更新。

### 1.5 Quartus SDC

- 带实体的网络名：`get_nets {I2C_HDMI_Config:u_I2C_HDMI_Config|mI2C_CTRL_CLK}` 按层级查找（每层去掉 `实体:` 前缀）。在它上面创建的时钟按 Quartus 的方式以这个名字命名。
- 时钟模式：`set_input_delay`/`set_output_delay -clock [get_clocks {*DDR3_PLL5*clk[2]}]`。读约束时 PLL 的时钟还不存在（`derive_pll_clocks`），时钟以模式文本保存。现在查找时按模式匹配名字或别名（`[` `]` 按字面匹配），取第一个匹配的时钟。
- 以前这两处共有 1 个错误、25 条告警，输入/输出端口没有时钟域。

## 2. 真实工程验证

### BrianHG DDR3 DECA 演示（`BrianHG_DDR3_DECA_top.qpf`，加入语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 未知原语 → 补模型后 3 个多驱动 | 0 |
| 未分析的构造 | 19（任务调用任务） | 0 |
| 时钟 | — | 12，903/903 个寄存器 |
| 跨时钟路径 | — | 52（5 条已同步，47 条未同步） |

分析结果中值得注意的发现：
- **`CON-INPUT-PERIOD`**：DDR3 PLL 的 `inclk0_input_frequency` 为 26.67 ns，而板上输入是 50 MHz（20 ns）。这是作者有意为之：`DDR_TRICK_MTPS_CAP` 给 Quartus 报告一个假的输入频率，让时序分析认为 DDR3 总线在规定速率内（源码注释说明这会导致时序报告不准确）。OpenCDC 按实际输入时钟推出各输出的周期，并报告这个差异。
- RS232 调试器的复位命令 `cmd_rst`（100 MHz DDR3 命令时钟域）直接作为视频同步发生器和行缓冲的复位，没有同步器（CDC-001/002/003）。
- 开关 `SW` 和 `switches_l` 未经同步进入滚动控制（CDC-003）。
- 视频行使能 `H_ena`/`V_ena` 以单级寄存器进入 DDR3 命令时钟域（CDC-002）。`lb_stat_hrst` 的边沿检测直接使用第一级 `lb_stat_hrst_dl0`。
- 两个行缓冲的双时钟存储器读出：第一个被识别为由“同步过的”信号限定（qualified data），第二个没有（CDC-003）。原因见第 5 节，这是一个已知限制。

### 其他语料

- SV 语料 49 项全部匹配（新增 BrianHG DDR3）。6 个工程的寄存器数按 1.4 减少，跨时钟路径结果与修改前逐项相同。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- `src/frontend/primitives/intel.v`：MAX 10 I/O 原子。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：任务中的任务调用。
- `src/ir/ConstFold.cpp`：`&&`/`||` 的常量折叠。
- `src/constraints/Sdc.cpp`：Quartus 网络名。
- `src/clock/ClockModel.{h,cpp}`：按模式查找时钟。
- 测试（每个用例都含反例）：
  - `tests/ir_extract/test_ir_extract.cpp`：任务调用任务（被调用的任务等待时仍然报告）；
  - `tests/cdc/test_cdc.cpp`：`x && 0` 不形成组合环（参数为 1 时环是真实的）；
  - `tests/frontend/test_quartus_compat.cpp`：Quartus 网络名与时钟模式（不匹配的模式仍报告）；MAX 10 I/O 原子（DDR 输出寄存器跨时钟，DDR 输入寄存器不跨）。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：BrianHG DDR3；6 个工程的寄存器数。
- `CMakeLists.txt`：版本 0.85.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：676 个测试全部通过。
- SV 语料 49 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 84 的 Windows CI（提交 60c8f0c，运行 42593957365）：Release 与 Debug 均 672/672 通过，生成 `OpenCDC-0.84.0-windows-x64.zip` 与 `.exe`。
- 尝试过但撤回的修改（见第 5 节）：
  - 只让满足全部结构条件的同步链作为限定信号：MiSTer、chiplab、surf 等 30 个语料项目的结果大幅改变；
  - 第一级扇出到多个寄存器时把它们都作为第二级：chiplab 中 AXI 互联模型里的普通寄存器被当作限定信号，30 条路径错误地变为安全。
  - 两种做法都会让已验证的结果变差，没有采用。

## 5. 已知限制

- 同步链从第一级出发只沿一个读取者向后追踪。第一级被多个寄存器读取时，只有被追踪的那一路的后级被当作“同步后的信号”。BrianHG 中，复位命令经视频同步发生器的输出进入两个行缓冲的延迟管道，只有第一个行缓冲的存储器读出被视为“已限定”。这种情况下“已限定”的判断本身也可疑：限定它的是一个复位命令，而不是数据握手。需要更细的设计后再改，而不是用会改变大量已验证结果的规则。
- `fiftyfivenm_ddio_in` 的 `clkout`（增强 DDR 模式）不建模。
- 时钟模式只取第一个匹配的时钟。

## 6. 下一步

阶段 86：
- 改进同步链在第一级扇出时的处理（第 5 节），并用语料逐项核对；
- 继续多时钟域的真实设计验证，例如 BrianHG 的 Cyclone V 或 Cyclone IV 版本（`cyclonev_*`、`cycloneive_*` 的 I/O 原子）。

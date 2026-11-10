# 阶段 90：MIPI CSI-2 摄像头接收器 —— Genesys2（Vivado）与 iCEBreaker（Yosys/nextpnr）

日期：2026-11-10　版本：0.90.0

阶段 89 的下一步是带视频采集或 SerDes 的多时钟域设计。这一阶段选择 **daveshah1/CSI2Rx**（提交 582f870，MIT）。它的两个演示分别使用两种工具链：

| 工程 | 器件与工具链 | 时钟 |
|---|---|---|
| `vhdl_rx/examples/ov13850_demo`（`.xpr`） | Genesys2（Kintex-7），Vivado，VHDL | 板上 200 MHz 差分时钟、CSI 400 MHz DDR 字节时钟及其 4 分频、两个 Clocking Wizard（摄像头像素时钟与 I2C 时钟；DVI 像素时钟与位时钟）、MIG DDR3 的 `ui_clk`、I2C 分频时钟 |
| `verilog_cores/test/icebreaker` | iCEBreaker（iCE40 UP5K），Yosys + nextpnr-ice40，Verilog | 12 MHz 板上时钟、96 MHz D-PHY 时钟及其 4 分频（像素时钟） |

第一次分析：
- Genesys2：
  - I2C 控制器中的计数器被识别为锁存器，并有 1 个“时钟沿不在时钟进程中”的未分析构造；
  - HDMI 输出延迟的约束 `get_pins pll1/inst/plle2_adv_inst/CLKOUT1` 不匹配，相应端口不检查；
  - 共 14 条告警。
- iCEBreaker：
  - 3 个错误（Yosys 接受、(System)Verilog 不允许的两种写法）；
  - 时钟约束在 nextpnr 的 `--pre-pack` 脚本中，OpenCDC 不读；`.pcf` 的每一行都报告“未知命令”。

修复后两个工程都是 0 错误，并加入语料库。

## 1. 完成的功能

### 1.1 VHDL：`else` 分支中嵌套的时钟沿（`VhdlStmt.cpp`）

- 写法（`ov_i2c_control.vhd`）：

  ```vhdl
  if start_xfer = '1' then state_cntr <= "00000000";
  else
      if rising_edge(clock_in) then ... end if;
  end if;
  ```

- 综合工具把它读作带异步复位的寄存器，与 `elsif rising_edge(...)` 相同。以前时钟沿只在 `if`/`elsif` 的条件中查找。
- 现在：`else` 分支只有一条 `if`，且它的第一个条件是时钟沿时，前面的分支是异步复位。嵌套 `if` 中时钟沿之后的分支与 `elsif` 形式一样给出 `VHDL-CLOCKED-ELSE` 告警。

### 1.2 Clocking Wizard 的 Vivado 引脚名（`XciModels.cpp`、`Sdc.cpp`）

- Vivado 中 Clocking Wizard 的 MMCM/PLL 是 `<实例>/inst/mmcm_adv_inst`（PLL 为 `plle2_adv_inst`）；OpenCDC 的模型中它是 `oc_mmcm`/`oc_pll`。
- 模型为这个实例加上属性 `OPENCDC_VIVADO_NAME`。`get_pins`、`get_cells` 也按这个名字查找（模型自己的名字仍然可用）。
- 同时支持嵌套的 `get_clocks [get_clocks -of_objects ...]`：外层 `get_clocks` 收到时钟对象时原样返回。以前外层把它当作名字，得到一个不存在的时钟。
- `get_clocks -filter` 仍不支持（`CON-UNSUPPORTED-OPTION`）；在这个工程中 `-of_objects` 已确定唯一的时钟，忽略过滤条件不影响结果。

### 1.3 Yosys 接受的两种写法（`SlangFrontend.cpp`）

| 写法 | 问题 | 处理 |
|---|---|---|
| `!&(btn_debounce)` | 一元运算符的操作数必须是 primary，`&(x)` 需要括号 | 加括号 `!(&(x))`，`FE-UNARY-OPERAND` 告警 |
| `last_byte[i:0] == 0`（`for` 循环中，`i` 是循环变量） | 部分选择的宽度必须是常量 | 只在作为 `==`/`!=` 的操作数时改写为 `((x >> (lo)) & ~({$bits(x){1'b1}} << ((hi) - (lo) + 1)))`，`FE-VARIABLE-RANGE` 告警 |

- 第二种改写的宽度是整个向量，而不是 `hi-lo+1`。零扩展后与另一个操作数比较，结果不变，所以只在比较中改写。其他位置（如拼接中）宽度有影响，仍然是错误。

### 1.4 nextpnr 的约束文件（新文件 `src/constraints/Nextpnr.{h,cpp}`）

- `.pcf`（iCE40）：`set_frequency <net> <MHz>` 生成时钟；`set_io` 等布局命令跳过，不再报告“未知命令”。
- `.py`（`nextpnr --pre-pack` 脚本）：`ctx.addClock("<net>", <MHz>)` 生成时钟。
  - 其他语句计数，在一条 `CON-NEXTPNR-IGNORED` 提示中给出；
  - 参数不是字面量的 `addClock` 给出 `CON-NEXTPNR-PARSE`。
- 网名是 Yosys 的层次名（`uart_i.sys_clk_i`，`.` 分隔层次），并且指物理网络：
  - 只是转接端口的网络（`uart_i.sys_clk_i` 就是 `clk12`），时钟放在信号的起点（顶层端口）上。以前在层次内的网络上创建时钟，会把同一个时钟分成两个“独立”时钟，得到假的跨时钟路径；
  - 由设计中的逻辑驱动的网络（`video_clk` 是 `div[1]`），时钟及其与源时钟的关系来自设计（推断出的分频时钟）。nextpnr 的约束只用于检查频率，不使用，给出 `CON-NEXTPNR-DERIVED` 提示。以前这里得到一个与 D-PHY 时钟无关的时钟。
- 实现：读取器生成带内部选项 `-opencdc_physical_net` 的 `create_clock`，SDC 读取器经 `IObjectResolver::physicalNet`/`drivenByLogic` 查找驱动。每条 SDC 命令写在原约束的行上，诊断给出原文件的行号。

## 2. 真实工程验证

### Genesys2 演示（`ov13850_demo.xpr`，加入 VHDL 语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 0 | 0 |
| 告警 | 14 | 8 |
| 锁存器 / 未分析的构造 | 1 / 1 | 0 / 0 |
| 时钟 | 12，237/237 个寄存器 | 12，238/238 个寄存器 |
| 跨时钟路径 | 66（0 条已同步） | 66（0 条已同步） |

- 剩余告警：
  - MIG 约束中引用内部单元的 6 条 `CON-UNMATCHED`（MIG 按其配置建模，没有这些内部单元）；
  - 2 条 `get_clocks -filter`。
- 66 条路径全部未同步，大部分是真实结构：
  - 摄像头配置状态机的 18 位计数器 `statecntr`（I2C 分频时钟）经组合比较后，作为 CSI 接收器各级寄存器（字节时钟）的使能/复位：多位计数器的组合逻辑直接跨时钟域，可能产生毛刺；
  - 时钟检测器在 200 MHz 时钟域直接采样分频后的字节时钟；
  - 行缓冲（字节时钟写、像素时钟读）的读出与帧开始信号没有同步器。
- 工程的约束没有 `set_clock_groups`，CSI 时钟与板上时钟的关系未知，这些路径都是“需复查”。

### iCEBreaker（加入 SV 语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 3 | 0 |
| 告警 | — | 3（`FE-VARIABLE-RANGE`、2 条 `FE-UNARY-OPERAND`） |
| 时钟 | — | 3：D-PHY 96 MHz、板上 12 MHz（`uart_i.sys_clk_i`）、`div[1]`（由 D-PHY 时钟派生） |
| 跨时钟路径 | — | 3 |

- 物理网络的处理很重要：只按 SDC 的语义把时钟放在层次内的网络上时，同一工程得到 5 个时钟、31 条跨时钟路径，其中 28 条是假的（板上时钟与 UART 时钟、分频时钟与 `video_clk` 之间）。
- 剩余 3 条：
  - 按键复位在两个 D-PHY 相关时钟域中异步释放（CDC-006）；
  - 降采样帧缓冲 `ds_i.buffer`（像素时钟写）在 UART 时钟域读出（CDC-003）。按键触发发送时，读出过程中可能仍在写入。

### 其他语料

- SV 语料 54 项（新增 1 项）、VHDL 语料 49 个（新增 1 个）全部匹配，其余结果不变。

## 3. 修改的文件

- `src/frontend/vhdl/VhdlStmt.cpp`：`else` 分支中的时钟沿。
- `src/frontend/XciModels.cpp`：`OPENCDC_VIVADO_NAME`。
- `src/constraints/Sdc.{h,cpp}`：Vivado 名、嵌套 `get_clocks`、物理网络、nextpnr 文件。
- 新增 `src/constraints/Nextpnr.{h,cpp}`；`src/constraints/CMakeLists.txt`。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-UNARY-OPERAND`、`FE-VARIABLE-RANGE`。
- 测试（每个用例都含反例）：
  - `tests/vhdl/test_vhdl_elab.cpp`：`else if rising_edge` 是异步复位；反例：`else` 中不是时钟沿的 `if` 仍是组合逻辑（锁存器）。
  - `tests/frontend/test_xci.cpp`：`pll1/inst/mmcm_adv_inst/CLKOUT1` 与嵌套 `get_clocks`：同一时钟的输出不报告，另一时钟的输出报告。反例：MMCM 配置下的 `plle2_adv_inst` 不匹配。
  - `tests/frontend/test_quartus_compat.cpp`：
    - `!&(c)`、`!^ m`；反例：带括号的写法没有告警；
    - 比较中的 `b[i:0]`；反例：拼接中的 `b[i:0]` 仍是错误。
  - 新增 `tests/constraints/test_nextpnr.cpp`：
    - `.pcf`/`.py` 的转换与行号；反例：缺少频率、参数是变量；
    - 端到端：UART 的时钟就是板上时钟，分频输出保持派生，没有跨时钟路径；`set_io` 无告警；反例：不存在的网络。
- `README.md`；`tools/ci/sv-corpus.sh`、`tools/ci/vhdl-corpus.sh`：CSI2Rx 的两个工程。
- `CMakeLists.txt`：版本 0.90.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：701 个测试全部通过。
- 测试预期的一处修改：Clocking Wizard 的用例最初让同一 MMCM 的另一个输出作为“错误的时钟”，但它们是相关时钟，不报告。改为另一个独立输入时钟的寄存器。
- SV 语料 54 项、VHDL 语料 49 个全部匹配。
- Clang 构建无告警。
- 阶段 89 的 Windows CI（提交 826317b，运行 42663578786）：Release 与 Debug 均 694/694 通过（Release 第一次因 GitHub 下载 HTTP 500 失败，重新运行后通过）。
- 本阶段的 Windows CI（提交 8cdd443，运行 42791864161）：Release 与 Debug 均 701/701 通过，生成 `OpenCDC-0.90.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- `get_clocks -filter` 不支持；需要按属性（`IS_GENERATED`、`MASTER_CLOCK`）选择时钟的约束会忽略过滤条件。
- Clocking Wizard 的 Vivado 名只有 7 系列的形式（`mmcm_adv_inst`、`plle2_adv_inst`）；UltraScale 的 `mmcme3_adv_inst`/`mmcme4_adv_inst` 等没有加入。
- `FE-VARIABLE-RANGE` 只处理比较中的部分选择。
- nextpnr 脚本只读字面量的 `ctx.addClock`；ECP5 的 `.lpf` 已经支持，其他 nextpnr 约束（`set_frequency` 以外的 `.pcf` 时序命令）不读。

## 6. 下一步

阶段 91：继续真实设计验证，例如 Intel（Quartus）的视频或 SerDes 设计、Gowin 器件的设计。

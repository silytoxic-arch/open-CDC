# 阶段 28：输出端口的时钟检查；Intel 原语参数的大小写

日期：2026-10-27　版本：0.28.0

## 1. 完成的功能

### 1.1 输出端口与 set_output_delay

`set_output_delay -clock C` 说明外部接收方用时钟 C 采样这些输出端口。以前这条约束只被记录、不使用。现在检查驱动端口的寄存器和输入（有时钟域的）是否与 C 同步：

- 驱动的时钟与 C 异步，或两者没有任何约束相关：报 `CDC-OUTPUT-CLOCK` 警告。每条约束、每个驱动时钟各报一条，列出端口和驱动。接收方看到的数据相对它的时钟随时变化，这本身就是一次跨时钟域。
- 同步的情况不报：
  - 同一时钟、派生时钟；
  - 转发时钟，即输出端口上的 `create_generated_clock`，主时钟就是数据的时钟（RGMII 发送的写法）；
  - 与驱动时钟周期相同的虚拟时钟：由 STA 按同一时钟定时，作为 `CDC-SOURCE-SYNC` 提示，与输入方向一致。
- `set_false_path -to [get_ports …]` 免除该端口；带 `-from [get_clocks …]` 时只免除这些时钟的驱动。
- 没有时钟域的输入直通到输出：不检查，与输入方向相同。
- 未知关系按 `unknownClockRelation` 选项处理：`assume_sync` 时不报。
- 约束引用了不存在的时钟时给出警告。
- JSON 结果的统计增加 `outputCrossings`，约束统计增加 `outputDelays`。

### 1.2 修正：Intel 原语参数名的大小写

verilog-ethernet 的 `oddr.v` 写 `altddio_out #(.WIDTH(WIDTH))`，而 Intel 的参数名是小写 `width`，Quartus 两种写法都接受。以前模型只认 `width`，`.WIDTH` 被悄悄忽略，宽度保持 1。DE2-115 的 RGMII 发送数据因此只有 1 位被建模，`ENET0_TX_DATA[3:0]` 没有驱动。

现在，设计中与模型参数只有大小写不同的写法，会在模型里声明为别名：`parameter WIDTH = 1, parameter width = WIDTH`。两种写法都生效。与模型参数无关的名字仍然忽略。

DE2-115 因此多了一个模块体，寄存器位数从 233768 增加到 233788。

## 2. 真实工程验证：DE2-115（verilog-ethernet，Quartus）

- 工程自己的 13 条 `set_output_delay` 中，RGMII 发送的 8 条（转发时钟 `enet0_tx_clk_125m`/`enet1_tx_clk_125m`，主时钟为 PLL 的 `clk[0]`）能匹配端口并被检查：`CDC-OUTPUT-CLOCK` 为 0（负例）。其余 5 条引用的是设计中不存在的端口（MDIO/MDC、JTAG 保留端口）。
- 临时加一条错误的约束 `set_output_delay -clock enet0_rx_clk_125m [get_ports {ENET0_TX_D*}]`（接收时钟与 PLL 时钟异步）：报告 `ENET0_TX_DATA[0..3]` 由 `altddio_out` 的寄存器驱动（正例）。修正参数大小写之前，这几位没有驱动，查不出来。
- 警告从 14 个变为 18 个：新增的 4 个是 `set_output_delay` 引用了设计中不存在的端口 `enet0_mdc`、`enet0_mdio`、`enet1_mdc`、`enet1_mdio`（该示例没有 MDIO）。以前这条命令不求值，所以没有报。与 `set_input_delay` 对同样端口的报告一致。

## 3. 修改的文件

- `src/constraints/Sdc.{h,cpp}`：`set_output_delay` 的解析（与 `set_input_delay` 共用 `setPortDelay`），`ConstraintSet::outputDelays`。
- `src/cdc/CdcAnalyzer.cpp`：`checkOutputs`。查询时新建的源节点补上时钟域。
- `src/cdc/CdcModel.h`、`src/engine/ResultJson.cpp`：统计字段。
- `src/frontend/AutoFrontend.cpp`：
  - 原语模型的参数别名（`modelText`、`paramAliases_`）；
  - 混合链接收敛时要求包装文本不变。
- 测试：
  - `tests/cdc/test_cdc.cpp`：
    - 负例：同时钟、转发时钟（上升沿与下降沿）、同周期虚拟时钟、`set_false_path -to` 端口与 `-from` 时钟、没有时钟域的输入、`assume_sync`；
    - 正例：异步驱动（含两个时钟混合的组合输出）、输入直通、未知关系、不存在的时钟。
  - `tests/constraints/test_sdc.cpp`：`set_output_delay` 的解析；原有"被忽略命令"测试改用 `set_multicycle_path`。
  - `tests/frontend/test_primitives.cpp`：`altddio_out` 的 `.WIDTH(5)` 与 `.width(5)` 都得到 10 位寄存器；不存在的参数名不影响宽度。
- 语料库：`tools/ci/sv-corpus.sh`（DE2-115 的警告数）。
- 文档：`README.md`。

## 4. 已知限制

- `CDC-OUTPUT-CLOCK` 是诊断，不是规则命中，不能用豁免文件豁免；请用 `set_false_path -to`。
- 只看端口的驱动时钟。`-rise_from/-fall_to` 这类边沿约束不解析。
- 参数别名只用于内置原语模型。用户自己的 Verilog 模块按 Verilog 规则区分大小写。

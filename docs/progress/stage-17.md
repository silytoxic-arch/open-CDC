# 阶段 17 完成报告：帧 FIFO 与 FIFO 结构、时钟级例外、verilog-ethernet

日期：2026-10-25　版本：0.17.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T17.1 帧 FIFO | 写指针在帧结束时经握手提交到读侧（alexforencich `axis_async_fifo`，`FRAME_FIFO=1`），读侧比较二进制指针：这种结构现在识别为异步 FIFO。 |
| T17.2 FIFO 结构 | FIFO 子实例内、同一对时钟之间的跨域路径并入同一个结构，包括指针、更新与应答、复位同步器和状态脉冲；同一实例内的提交握手也并入。这些协议信号之间的再汇聚不再报 CDC-005；同一作用域中其他独立的同步标志仍然报告。 |
| T17.3 限定条件 | 限定信号可以经过多级与门和反相器后再门控数据。 |
| T17.4 时钟级例外 | `set_false_path` 和 `set_max_delay -datapath_only` 的 `-from` 与 `-to` 都是时钟（可以用 `-of_objects`）时，把这对时钟视为异步。寄存器级的例外不求值，仍然只记录。 |
| T17.5 前端 | 修正一个已有缺陷：源文件带 `timescale` 时，混合语言包装文件因缺少 `timescale` 而报错，这个错误又被过滤掉，结果是"0 个错误、没有设计"。现在缺省时间尺度为 1ns/1ps，包装文件中的错误一律报告。另外，未连接端口的告警按实例合并为一条。 |
| T17.6 真实工程 | verilog-ethernet 的 RGMII MAC 和帧 FIFO（Xilinx IDDR/ODDR/BUFR/BUFIO，4 个时钟）加入 Verilog 语料回归；另外分析了它的 Arty 示例工程（MMCM、MII、UDP/IP 协议栈）。 |

## 2. 修改的文件

**修改**
- `src/cdc/AdvancedAnalysis.cpp`：写指针的搜索、由比较指针算出的 Gray 返回指针、FIFO 实例路径并入结构、协议内再汇聚的过滤
- `src/cdc/CdcAnalyzer.cpp`：多级与门的限定；`buildStructures` 移到再汇聚分析之前
- `src/constraints/Sdc.{h,cpp}`：`ClockException`、`clockException`（先检查原始写法再求值）
- `src/clock/ClockAnalyzer.cpp`：`exceptionBetween`，以及例外中 `-of_objects` 的解析
- `src/frontend/slang_adapter/SlangFrontend.cpp`：缺省时间尺度
- `src/frontend/AutoFrontend.cpp`：包装文件中的错误不再丢弃；`mergeUnconnectedPorts`
- 测试：`tests/cdc/test_cdc.cpp`、`tests/cdc/test_cdc_advanced.cpp`、`tests/clock/test_clock.cpp`、`tests/frontend/test_primitives.cpp`
- `tools/ci/sv-corpus.sh`：verilog-ethernet RGMII；`tools/ci/vhdl-corpus.sh`：期望值
- `README.md`、`docs/architecture.md`（第 24 节）、`CMakeLists.txt`（版本 0.17.0）

行为变化：slang 的 `FE-SLANG-UnconnectedInputPort`/`OutputPort` 不再逐个报告，改为每个实例一条 `FE-UNCONNECTED-PORTS`。

## 3. 关键算法

### 3.1 帧 FIFO 的写指针
读数据的限定信号若是 1 位更新标志，就在同一对时钟之间寻找多位指针路径（Gray、握手或限定数据），要求其目的寄存器位于读数据寄存器的控制锥中（如 `wr_ptr_commit_sync`，参与 `empty` 比较）。返回写侧的 Gray 指针，其源寄存器可以在控制锥中，也可以从控制锥中的寄存器算出（`rd_ptr_gray <= bin2gray(rd_ptr…)`）。

### 3.2 协议内的再汇聚
"协议角色"指 Gray 指针、握手数据、作为某个 FIFO 或握手的请求或应答的路径。再汇聚点上的所有路径都属于同一个 FIFO 或握手结构、且都承担协议角色时，只在路径说明中记下，不报告 CDC-005。

### 3.3 时钟级例外
解析约束时只看原始写法：`-from`、`-to` 都是 `[get_clocks …]` 或 `[all_clocks]` 才求值并记录 `ClockException`，否则按原来的方式记为"已记录、未使用"，这样寄存器级例外中的 `get_cells` 等查询不会产生告警。时钟分析时，一对时钟若没有其他关系（时钟组、派生关系），而有例外覆盖它（任一方向），就判为异步，原因中写明命令和位置。

## 4. 构建与测试结果

- **构建**：Linux GCC Debug/Release（`-Werror`）通过；Windows MSVC 由推送后的 CI 确认。
- **测试**：本地 386 个 CTest 用例全部通过（阶段 16 是 381 个）。新增：
  - 帧 FIFO 的识别；FIFO 外部的独立同步器仍然报再汇聚（负例）。
  - 多级与门、反相的限定信号。
  - 时钟级 `set_false_path`、`-of_objects` 形式的 `set_max_delay -datapath_only`；不带 `-datapath_only` 的最大延迟、寄存器级例外仍为未知关系（负例）。
  - 带 `timescale` 的源文件与原语链接。
  - 按实例合并的未连接端口：有输入悬空为警告，只有输出悬空为提示。
- **真实工程**：
  - verilog-ethernet RGMII MAC（4 个时钟、两个帧 FIFO）：0 个错误；30 条跨域路径中 29 条已同步，11 个同步器结构（含 2 个异步 FIFO）。剩下 2 条需要用户确认：
    - `logic_rst` 按设计与 `logic_clk` 同步，需要 `set_input_delay -clock logic_clk` 说明；
    - 2 位的 `speed` 逐位同步（CDC-008，需复核）。
  - verilog-ethernet Arty 示例（MMCM、MII、UDP/IP，608 个寄存器）：27 条跨域路径全部已同步。仓库自带的约束是 Vivado Tcl 脚本（`foreach`、`get_cells -hier -filter`、寄存器级 `set_max_delay`），超出了支持的子集，所以 26 条路径为"需复核"（时钟关系未知）。按时钟对汇总一目了然，可以用 `--suggest-sdc` 补上约束。
  - microwatt（ECP5、Arty、Arty 加 JTAG 约束）、PULP axi_cdc，以及 open-logic、neorv32、PoC、OSVVM、surf：结果不变。

## 5. 已知限制
- FIFO 结构的合并按实例作用域和时钟对判断。在 LiteX 这类把所有逻辑放进一个模块的生成代码中，结构会偏大，不过再汇聚仍按协议角色区分。
- 时钟级例外按对称关系处理：只写了一个方向时，另一个方向的路径也视为异步。
- 不支持完整的 Tcl（`foreach`、`if`、网表属性过滤），所以 verilog-ethernet 自带的约束脚本不生效。

## 6. 下一步建议
1. 支持寄存器级的 `set_max_delay -datapath_only -from <寄存器> -to <同步器第一级>`：把被覆盖的路径记为"设计者已约束"，作为 CDC-010 的依据。
2. Tcl 子集扩展：`foreach` 遍历静态列表、`set` 变量、`if`，以及 `get_cells -hier -filter {REF_NAME == …}` 按模块名查找实例。
3. 更多真实工程：带 Xilinx MIG/PCIe 的设计、Intel 平台的设计。

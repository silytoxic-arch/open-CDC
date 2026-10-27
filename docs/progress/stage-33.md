# 阶段 33：GUI 中的局部约束模块与输出端口视图

日期：2026-10-27　版本：0.33.0

本阶段补上阶段 30 和阶段 31 留在 GUI 中的两个限制。

## 1. 完成的功能

### 1.1 在 GUI 中编辑局部约束的模块

以前 GUI 的工程设置只能保留约束条目的模块（`{"file", "ref"}`），不能新建或修改。

现在工程设置的"Constraints & Waivers"页中，约束是一个两列的表格：
- 第一列：约束文件；
- 第二列：模块（`read_xdc -ref`）。填写后这个文件成为局部约束，在该模块的每个实例中各执行一次；留空就是普通约束文件。表头的提示说明了这一点。

其他行为：
- 保存工程时写成 `{"file": ..., "ref": ...}`，重新打开时显示在表格中。
- 文件对话框也列出 `.tcl`（open-logic 的局部约束是 `.tcl`）。
- 校验：模块名中含有空格、`/`、`[]`、`{}`、`*`、`?` 时拒绝保存，并提示是哪个约束文件。

### 1.2 Output Ports 视图

阶段 30 的输出端口跨域记录（`CDC-OUTPUT-CLOCK`）以前在 GUI 中只能通过 Messages 窗口看到警告，被豁免的记录完全看不到。现在新增 **Output Ports** 停靠窗口（与 CDC Paths、Rule Violations 同组）：
- **表格**：每条记录一行，列为输出端口、驱动、驱动时钟、输出延迟时钟、关系、约束位置、指纹、豁免。未豁免的行有底色。
- **过滤器**：可选"全部 / 未豁免 / 已豁免"，并显示计数。
- **详情**：端口与驱动的完整列表。已豁免的显示豁免的编号、原因和作者；未豁免的给出修改建议，以及可用于豁免的指纹。
- **双击**：打开对应的 `set_output_delay` 命令。

### 1.3 示例 `examples/output_ports`

4 个输出：
| 输出 | 情况 | 结果 |
|---|---|---|
| `io_data` | 同时钟驱动 | 不报告 |
| `io_req` | 先同步到 `clk_io` 再输出 | 不报告 |
| `sys_flag` | 异步时钟驱动 | 报告 |
| `dbg_led` | 异步时钟驱动 | 被豁免 |

示例内部的跨域路径用了同步器，所以结果是 0 个规则违例、1 个警告、1 个豁免。

## 2. 修改的文件

- `src/gui/ProjectEditor.{h,cpp}`：
  - 约束表格 `constraintTable()`；
  - `setConstraintScope`、`removeConstraint`；
  - 模块名校验。
- `src/gui/CdcViews.{h,cpp}`：`OutputPortView`。
- `src/gui/MainWindow.{h,cpp}`：Output Ports 停靠窗口，`outputView()`。
- `src/gui/main.cpp`：
  - 冒烟测试新增 `checkOutputPorts`：
    - 行数与记录一致；
    - "未豁免 / 已豁免"过滤器的计数正确；
    - 豁免列、输出时钟、详情与记录一致；
    - 双击跳到约束的行。
  - 编辑测试新增局部约束模块检查：
    - 负例：含空格的模块名被拒绝；
    - 设置模块后出现在配置中；
    - 保存后重新打开，模块保留，编辑器也显示它；
    - 清除模块后与原工程相同。
  - Rule Violations 过滤器检查改为只统计路径上的豁免：`waivedHits()` 也包含输出记录，它们在 Output Ports 视图中。
- 示例：`examples/output_ports/`（RTL、约束、豁免、工程、README）。
- 测试：
  - `tests/engine/test_waivers.cpp`：示例端到端，2 条记录，其中 1 条被 `W-LED` 豁免；同时钟输出与经同步器的输出都不报告。
  - `tests/CMakeLists.txt`：新增 `gui.output_ports`。
- 文档：`README.md`、`docs/waivers.md`。

## 3. 真实工程验证

- **DE2-115，注入错误约束**（`de2_neg`）：GUI 冒烟测试通过。Output Ports 中有 1 条记录，双击跳到注入的 `set_output_delay` 行。
- **DE2-115 原工程、RGMII**：Output Ports 为空，与 CLI 一致。
- **open-logic `olo_base_cc_status`**（6 个约束文件，其中 5 个是局部约束）：GUI 编辑测试通过，打开编辑器再保存后各条目的模块不变，重新分析得到同样的 11 条跨域路径。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- Output Ports 是普通表格，没有按时钟过滤；记录数通常很少。
- GUI 中不能从视图直接生成豁免条目，需要编辑豁免文件。

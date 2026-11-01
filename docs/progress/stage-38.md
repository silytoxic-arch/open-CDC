# 阶段 38：报告中的豁免审计（HTML 与 CSV）

日期：2026-11-01　版本：0.38.0

## 1. 完成的功能

豁免的状态以前只能在 GUI 和 `opencdc waivers` 中看到。交给评审的 HTML 报告和导入表格软件的 CSV 里没有状态，输出端口跨域记录也不在 CSV 中。

### 1.1 HTML 报告

- **Waivers 一节**：
  - 开头有按状态的统计，例如"2 waiver(s): 2 active, 0 expiring, 1 unused, 1 expired"。unused 和 expired 不为 0 时标红，并说明 unused 表示什么也没匹配（过时或范围太窄），expired 表示已不再应用。
  - 表格新增 State 列，不是 active 的状态标红。
- **摘要**：有豁免时增加一张卡片"waivers to review (unused, expiring, expired)"，数目不为 0 时标红。

### 1.2 CSV

`--csv` 和 GUI 的 Export CSV 现在写出 4 个文件：
- `waivers.csv`（新）：每个豁免一行，列为 id、state、uses、rule、fingerprint、source、destination、reason、author、expires、expires_in_days、file、line。
- `outputs.csv`（新）：每条输出端口跨域记录一行，列为 ports、drivers（多个用分号分隔）、driver_clock、output_delay_clock、relation、waiver、fingerprint、constraint_file、constraint_line。被豁免的记录也在其中，并给出豁免名。
- `paths.csv`、`violations.csv`：不变。

格式与已有的 CSV 相同：RFC 4180，UTF-8 带 BOM。

## 2. 真实工程验证

- **Digilent 同步器示例（工程副本）**：
  - `waivers.csv` 中，W-1 的 state 为 expiring，uses 为 1，expires_in_days 为 10，line 为 6；
  - 含逗号的原因被正确加了引号；
  - HTML 中显示"1 waiver(s): 0 active, 1 expiring, 0 unused, 0 expired"。
- **DE2-115（注入错误约束）**：`outputs.csv` 中有一行，列出 4 个 `ENET0_TX_DATA` 端口、两个 altddio 驱动、`altpll_component.clk[0]` 对 `enet0_rx_clk_125m`、asynchronous、指纹和注入的约束文件第 1 行。

## 3. 修改的文件

- `src/engine/Report.{h,cpp}`：`waiversCsv`、`outputsCsv`；HTML 的状态统计、State 列、摘要卡片。
- `src/cli/main.cpp`：`--csv` 写出 4 个文件。
- `src/gui/MainWindow.cpp`：Export CSV 写出 4 个文件。
- 测试：
  - `tests/engine/test_report.cpp`：
    - 豁免 CSV 的行数、各列与记录一致；HTML 的统计和卡片；
    - 输出 CSV：没有输出约束时只有表头；`output_ports` 示例中 2 行，其中 1 行豁免为 W-LED，时钟和指纹正确；
    - 有未使用和已过期豁免的工程：HTML 中的标红统计与 State 单元格，CSV 中的状态。
  - `tests/CMakeLists.txt`：`cli.analyze_reports` 检查 4 个文件名。
  - `src/gui/main.cpp`：冒烟测试检查导出了 `waivers.csv` 和 `outputs.csv`。
- 文档：`README.md`、`docs/waivers.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 一个 CSV 文件只对应一种对象，豁免与命中之间的对应关系需要用 id 关联（`violations.csv` 的 waiver 列）。

# 阶段 30：输出端口跨域的豁免与报告

日期：2026-10-27　版本：0.30.0

## 1. 完成的功能

阶段 28 加入了输出端口的检查（`CDC-OUTPUT-CLOCK`），当时是分析时直接发出的诊断，不能豁免，也不出现在结果文件里。现在：

- **结果记录**：每条约束、每个驱动时钟一条记录（`CdcResult::outputs`），包括端口、驱动、输出时钟、驱动时钟、关系、约束位置、指纹（16 位十六进制，由第一个驱动、第一个端口和两个时钟算出）和覆盖它的豁免。
- **豁免**：豁免文件中 `"rule": "CDC-OUTPUT-CLOCK"`（或匹配它的通配符）可以覆盖记录：
  - `source` 与驱动寄存器匹配，`destination` 与输出端口匹配，各有一个匹配即可；
  - `fingerprint` 与警告中给出的指纹完全相同。
  - 被覆盖的记录不再报警告，计入豁免的使用次数；没有匹配任何内容的豁免仍报 `WAIVER-UNUSED`。
- **报告时机**：警告在"豁免应用"阶段之后发出（`rules::reportOutputCrossings`），警告的附注中给出指纹。
- **输出**：
  - JSON 结果：`cdc.outputCrossings` 数组；`cdc.stats.outputCrossings` 是未豁免的数量。
  - HTML 报告：新增"Output ports driven asynchronously to their output delay clock"一节，列出被豁免的记录及其豁免名。

## 2. 修改的文件

- `src/cdc/CdcModel.h`：`OutputCrossing`、`CdcResult::outputs`。
- `src/cdc/CdcAnalyzer.cpp`：`checkOutputs` 生成记录。
- `src/rules/Waiver.{h,cpp}`：输出记录的豁免匹配；`reportOutputCrossings`。
- `src/engine/AnalysisSession.cpp`：豁免之后报告。
- `src/engine/ResultJson.cpp`、`src/engine/Report.cpp`：JSON 与 HTML 输出。
- 测试：
  - `tests/engine/test_waivers.cpp`：按端口豁免只覆盖对应记录；另一规则的豁免不覆盖（`WAIVER-UNUSED`）；按指纹豁免；通过分析会话检查豁免文件、诊断和 JSON。
  - `tests/cdc/CdcTestUtil.h`：测试辅助函数在规则检查后报告输出记录。
- 文档：`docs/waivers.md`、`README.md`。

## 3. 构建与测试

见交付说明：全部测试、两套语料库、Clang 构建。

## 4. 已知限制

- GUI 通过 Messages 窗口显示这些警告。被豁免的记录在 GUI 中没有单独的列表，只出现在 JSON 和 HTML 报告中。

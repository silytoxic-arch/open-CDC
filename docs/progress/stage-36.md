# 阶段 36：豁免过期提醒

日期：2026-11-01　版本：0.36.0

## 1. 完成的功能

以前豁免过期后才报 `WAIVER-EXPIRED`，并且同时失效：前一天还被豁免的命中，第二天突然变成违例。现在会提前提醒。

- **新诊断 `WAIVER-EXPIRING`**（警告）：豁免仍然生效，但过期日期在提醒期内时报告。
  - 消息给出日期和剩余天数（"今天"或"N 天后"），并建议复查、延期或修复跨域。
  - 位置是豁免在文件中的行。
- **工程选项 `options.waiverExpiryWarningDays`**：
  - 默认 30 天，取值 0–3650，0 表示不提醒；
  - 等于默认值时不写入工程文件；
  - GUI 工程设置中有"Warn before waivers expire"一项，0 显示为"never"。
- **结果中的显示**：
  - `WaiverRecord::expiresInDays`：在提醒期内为剩余天数（0 表示今天），否则为 -1；
  - JSON 中的 `cdc.waivers[].expiresInDays`（不在提醒期内为 null）；
  - HTML 报告的豁免表中标出"expires in N day(s)"；
  - "Waiver application"阶段的摘要中给出"N expiring soon"。
- **GUI Waivers 视图**：
  - 新状态 expiring：豁免在使用中，且在提醒期内；
  - 过滤器增加"Expiring soon"；
  - 过期日期一列显示"(in N days)"或"(today)"。
  - 状态的优先级为：expired、unused、expiring、active。
- **日期计算**：`rules::daysBetween` 使用 days-from-civil 算法，不依赖平台的时间库，能识别闰年和格式错误的日期。

## 2. 真实工程验证

**Digilent 同步器示例（工程副本）**：用 `opencdc waive` 加入一个 10 天后过期的豁免（路径 #1 的 `CDC-010`）。
- **CLI**：报 `WAIVER-EXPIRING`，"expires on 2026-11-11 (in 10 day(s))"，位置指向豁免文件第 6 行；阶段摘要中有"1 expiring soon"；命中仍然被豁免。
- **GUI**：冒烟测试通过，Waivers 视图的各状态计数（含 expiring）与结果一致。

## 3. 修改的文件

- `src/cdc/CdcModel.h`：`WaiverRecord::expiresInDays`。
- `src/rules/Waiver.{h,cpp}`：`applyWaivers` 增加提醒天数参数；`daysBetween`。
- `src/engine/ProjectConfig.{h,cpp}`：`waiverExpiryWarningDays` 的读写与范围检查。
- `src/engine/AnalysisSession.cpp`：传入选项；阶段摘要。
- `src/engine/ResultJson.cpp`、`src/engine/Report.cpp`：`expiresInDays`。
- `src/gui/CdcViews.{h,cpp}`：expiring 状态与过滤器。
- `src/gui/ProjectEditor.{h,cpp}`：提醒天数设置。
- `src/gui/main.cpp`：
  - 编辑测试：设为 7 天，保存并重新打开后保留；
  - Waivers 检查包含 expiring。
- 测试：
  - `tests/engine/test_waivers.cpp`：
    - `daysBetween`：同日、跨月、闰年、跨年、负数；负例为月份 13、不足位数。
    - `applyWaivers`：19 天后和今天过期的豁免报告；负例：一年后过期、已过期（只报 EXPIRED）、没有日期、提醒天数为 0 时都不报。
    - 会话：默认 30 天时报告 5 天后过期的豁免，JSON 中有 `expiresInDays`；负例为把选项设为 2 天时不报。
  - `tests/engine/test_project_config.cpp`：默认值、往返、默认值不写入；负例为 5000 和字符串。
- 文档：`docs/waivers.md`（诊断表、Waivers 视图）、`README.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- "今天"取 UTC 日期，与本地日期可能相差一天。

## 6. 补丁 0.36.1：Windows CI 失败

0.35.0 和 0.36.0 的 Windows CI 失败，Linux 的三个作业都通过。

失败的测试是"updateWaiver and removeWaiver edit one entry and keep the rest of the file"。第二次 `updateWaiver` 和之后的 `removeWaiver` 都失败了。

- **原因**：
  - 测试中读取文件内容的 `std::ifstream` 一直保持打开；
  - 而写入是先写临时文件，再改名覆盖原文件；
  - Windows 不允许改名覆盖一个被打开的文件，Linux 允许，所以只在 Windows 上失败。
- **影响用户的同一问题**：编辑器或查看器打开着豁免文件或工程文件时，在 Windows 上保存会失败。
- **修复**：
  - 测试：读取后立即关闭文件。
  - 引擎：`WaiverFile` 的写入和 `saveProjectFile` 在改名失败时改为复制覆盖原文件，再删除临时文件。
  - 新测试：在文件被另一个流保持打开时追加、更新、删除豁免并保存工程，都必须成功，且不留下 `.tmp` 文件。这个测试在 Windows CI 上会走复制覆盖的路径。

# 阶段 37：命令行豁免审计（`opencdc waivers`）

日期：2026-11-01　版本：0.37.0

## 1. 完成的功能

豁免的状态（使用中、即将过期、未使用、已过期）以前只能在 GUI 或 HTML 报告中看到，CI 中没有办法让过时的豁免导致构建失败。

### 1.1 `opencdc waivers <工程>`

先运行分析（使用次数取决于分析结果），然后输出：
- **豁免诊断**：打不开的文件、无效 JSON、被跳过的条目（`WAIVER-INVALID`）、`WAIVER-EXPIRING`/`WAIVER-EXPIRED`/`WAIVER-UNUSED`。
- **摘要**："N 个豁免来自 M 个文件：a 个使用中、b 个即将过期、c 个未使用、d 个已过期；e 个无效条目或文件问题"。
- **每个豁免**：
  - 第一行：id、[状态]、uses=、选择器（rule/fingerprint/source/destination）、expires=、定义位置；
  - 第二行：原因和作者。

### 1.2 `--fail-on`：用于 CI

- **可选的状态**：`unused`、`expiring`、`expired`、`invalid`，用逗号分隔或重复给出。
- **失败时**：输出"Audit : FAILED (…)"，退出码为 **4**（新增，README 的退出码表已更新）。
- **通过时**：输出"Audit : passed"，退出码为 0。
- **其他退出码**：状态名写错时退出码为 2；分析没有运行（HDL 错误）时退出码为 1，并说明此时使用次数未知。

### 1.3 `--json <文件>`

写出豁免列表，每项包含 id、state、uses、选择器、reason、author、expires、expiresInDays、file、line，另有 invalid 计数。分析结果的 JSON 中，`cdc.waivers[]` 也增加了 `state`。

### 1.4 共用的状态函数

`rules::waiverState`：CLI、JSON 与 GUI 的 Waivers 视图使用同一个函数，优先级为 expired、unused、expiring、active。

## 2. 真实工程验证

**Digilent 同步器示例（工程副本）**：其中有阶段 36 加入的、10 天后过期的豁免。运行 `opencdc waivers --fail-on expiring,unused`：
- 输出 `WAIVER-EXPIRING` 和"0 active, 1 expiring, 0 unused, 0 expired"；
- W-1 [expiring] uses=1，显示指纹和过期日期；
- "Audit : FAILED (expiring)"，退出码 4。

## 3. 修改的文件

- `src/rules/Waiver.{h,cpp}`：`waiverState`。
- `src/cli/main.cpp`：`waivers` 子命令，退出码 4。
- `src/cli/CMakeLists.txt`：链接 nlohmann_json。
- `src/engine/ResultJson.cpp`：`cdc.waivers[].state`。
- `src/gui/CdcViews.cpp`：`WaiverView::stateOf` 改用 `waiverState`。
- 测试：`tests/cli/RunWaiverAudit.cmake`（新，`cli.waiver_audit`），使用 cdc_advanced 示例的源文件和 4 个豁免（使用中、未使用、已过期、无效）：
  - 没有 `--fail-on` 时：退出码 0，摘要、各行状态、`WAIVER-INVALID` 都正确；
  - `--fail-on unused` 时：退出码 4；
  - 负例 `--fail-on expiring`：没有即将过期的豁免，退出码 0；
  - `--fail-on invalid,expired --json` 时：退出码 4，JSON 中有 3 个豁免，第二个为 unused，invalid 为 1；
  - 负例：状态名写错时退出码 2。
- 文档：`docs/waivers.md`（新增"Auditing waivers"一节）、`README.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- `--fail-on` 的状态按给出的顺序列在 FAILED 后面。
- 只能审计整个工程，不能只审计某个豁免文件。

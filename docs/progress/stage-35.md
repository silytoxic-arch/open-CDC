# 阶段 35：管理已有的豁免（Waivers 视图、编辑、删除）

日期：2026-11-01　版本：0.35.0

## 1. 完成的功能

阶段 34 能生成豁免，但已有的豁免只能手工编辑 JSON 才能修改或删除。在 GUI 中也看不到哪些豁免已经失效（未使用或已过期）。

### 1.1 GUI：Waivers 视图

新的停靠窗口与 CDC Paths、Rule Violations、Output Ports 同组：
- **表格**：每个豁免一行，列为 id、状态、使用次数、规则、选择器（指纹/源/目标）、原因、作者、过期日期、定义位置。
- **状态**：
  - active：至少覆盖一个命中；
  - unused：什么也没有匹配，可能已过时或范围太窄；
  - expired：已过期。
  - 后两种状态的行有底色，并可以按状态过滤。
- **Edit...**：修改 id、原因、作者、过期日期。选择器只显示，不修改：要改选择器，应删除后重新生成。改为其他豁免已用的 id 时拒绝。
- **Remove**：确认后从它的豁免文件中删除。
- 修改或删除之后自动重新分析。
- **双击**：打开豁免在文件中的位置。

### 1.2 CLI

`opencdc waive <工程> --remove <id>`：从工程中含有该 id 的豁免文件里删除这个豁免；找不到时退出码为 2。添加豁免时，`--reason` 仍是必需的。

### 1.3 引擎

- `updateWaiver(file, id, record, taken)`：
  - 条目位置不变，原有的其他字段（如 `comment`）保留；
  - 清空的字段从条目中删除；
  - 可以改 id，但不能与其他豁免重复。
- `removeWaiver(file, id)`。
- 读写逻辑抽成 `loadForEdit` / `writeAtomically` / `setFields`，与 `appendWaiver` 共用。

## 2. 真实工程验证

- **DE2-115（注入错误约束，工程副本）**：GUI 测试依次完成：
  - 用对话框豁免 `CDC-005`；
  - 在 Waivers 视图中找到它并编辑原因；
  - 删除；
  - 删除后这个命中重新变为未豁免；
  - 再豁免输出记录。
  - 每一步之后，Waivers 视图的行数和状态过滤计数都与结果一致。
- **Digilent 同步器示例（工程副本）**：`opencdc waive --remove W-1` 删除阶段 34 生成的豁免后，"需要复查"回到 6 条；豁免文件保留，`waivers` 为空数组。

## 3. 修改的文件

- `src/engine/WaiverFile.{h,cpp}`：`updateWaiver`、`removeWaiver`，以及共用的读写函数。
- `src/cli/main.cpp`：`waive --remove`。
- `src/gui/WaiverDialog.{h,cpp}`：编辑模式（选择器只读，文件固定，自己的 id 不算重复）。
- `src/gui/CdcViews.{h,cpp}`：`WaiverView`。
- `src/gui/MainWindow.{h,cpp}`：Waivers 停靠窗口，`waiverDialogForEdit`、`updateWaiver`、`removeWaiver`。
- `src/gui/main.cpp`：
  - 冒烟测试新增 `checkWaiverView`：行数，以及每种状态的过滤计数与结果一致。
  - `--waive-test` 改为状态机，覆盖豁免、编辑、删除、输出豁免四步；负例包括编辑为其他豁免的 id。
- 测试：
  - `tests/engine/test_waivers.cpp`：`updateWaiver`/`removeWaiver`，检查位置不变、`comment` 保留、清空字段被删除；负例为未知 id、id 冲突、没有原因，失败时文件不变。
  - `tests/cli/RunWaive.cmake`：增加 `--remove` 及其负例。
- 文档：`docs/waivers.md`、`README.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 编辑时不能修改选择器。
- Waivers 视图只显示最近一次分析读到的豁免。读取失败的条目（`WAIVER-INVALID`）不在表中，只在 Messages 中显示。

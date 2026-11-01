# 阶段 34：生成豁免条目（GUI 与 CLI）

日期：2026-11-01　版本：0.34.0

## 1. 完成的功能

以前要豁免一个规则命中，需要手工编辑豁免文件的 JSON：从报告中抄指纹，自己保证 id 唯一，并把新文件加入工程。现在 GUI 与 CLI 都能直接生成。

生成的豁免按"规则 + 路径指纹"选择，只覆盖这一条路径。设计变化产生的新跨域路径不会被它悄悄豁免。

### 1.1 GUI：Waive... 按钮

- **入口**：Rule Violations 与 Output Ports 视图中各有一个 **Waive...** 按钮。选中一个尚未被豁免的规则命中或输出记录时，按钮才可用。
- **对话框**：
  - 只读显示规则和指纹；
  - 可编辑 id（预填下一个空闲的 `W-<n>`）、原因（必填）、作者（预填系统用户名）、过期日期（可选）；
  - 豁免文件可选工程中已有的豁免文件，或工程文件旁边的 `waivers.json`（标注"新建，并加入工程"）。
- **校验**：缺少原因、日期格式错误、id 已被任何一个豁免文件使用时，不能确定。
- **确定之后**：写入豁免文件；如果是新文件，加入工程并保存工程；然后重新分析，结果中这条命中显示为已豁免。
- **限制**：工程必须已经保存为文件，否则提示先保存。

### 1.2 CLI：`opencdc waive`

```
opencdc waive <工程> --fingerprint <指纹> --rule <规则> --reason "<原因>"
              [--author …] [--expires YYYY-MM-DD] [--id …] [--file …]
              [--source …] [--destination …]
```

- **写到哪个文件**：没有 `--file` 时写入工程的第一个豁免文件；工程没有豁免文件时，在工程文件旁新建 `waivers.json` 并加入工程（保存工程文件）。
- **id**：没有 `--id` 时取下一个空闲的 `W-<n>`。
- **退出码**：出错时为 2，并说明原因（缺少原因、id 重复、日期格式等）。
- **指纹从哪里抄**：CLI 的路径行在有规则命中时，末尾给出 `fingerprint=…`；输出端口警告中原本就有指纹。

### 1.3 引擎

`engine/WaiverFile`：
- `waiverProblem`：与读取豁免文件时相同的检查。
- `appendWaiver`：
  - 文件不存在时新建；
  - 已有文件保留原有条目、字段顺序和其他顶层字段（用 `nlohmann::ordered_json`）；
  - 先写临时文件再替换，写失败不会损坏原文件。
- `nextWaiverId`。
- `waiverFor(path, hit)` / `waiverFor(output)`：生成"规则 + 指纹"选择器。
- `addProjectWaiver`：
  - 选择文件；
  - 在工程所有豁免文件范围内检查 id 唯一；
  - 新文件加入工程。

## 2. 真实工程验证

- **Digilent 同步器示例**（`dl_sync_example`，工程副本）：用 `opencdc waive` 豁免路径 #1 的 `CDC-010`（两个时钟无关系约束），使用 CLI 打印的指纹。
  - 新建了 `waivers.json` 并加入工程；
  - 重新分析后路径 #1 变为 safe，"需要复查"从 6 条变为 5 条；
  - 没有 `WAIVER-UNUSED`。
- **DE2-115（注入错误约束）**：GUI 豁免测试在工程副本上依次通过对话框豁免一个规则命中（`CDC-005`，W-1）和一个输出记录（`CDC-OUTPUT-CLOCK`，W-2），两次重新分析后都显示为已豁免。

## 3. 修改的文件

- `src/engine/WaiverFile.{h,cpp}`：上述函数。
- `src/cli/main.cpp`：`waive` 子命令；路径行的 `fingerprint=`。
- `src/gui/WaiverDialog.{h,cpp}`（新）：豁免对话框。
- `src/gui/CdcViews.{h,cpp}`：
  - 两个视图的 Waive... 按钮；
  - `currentHit()`、`currentOutput()`；
  - `waiveRequested` 信号。
- `src/gui/MainWindow.{h,cpp}`：`waiverDialogForHit`、`waiverDialogForOutput`、`addWaiver`。
- `src/gui/main.cpp`：`--waive-test`。在工程副本上依次：
  - 用对话框豁免第一个未豁免的规则命中和输出记录；
  - 负例：没有原因、日期格式错误、id 重复；
  - 检查新文件加入工程（重新读取工程文件），并且重新分析后显示为已豁免。
- `src/gui/CMakeLists.txt`。
- 测试：
  - `tests/engine/test_waivers.cpp`：
    - `appendWaiver`：新建；追加时保留条目、字段顺序和其他字段；负例为 id 重复（文件内、工程内）、没有原因、没有选择器、日期错误、不是豁免文件，失败时不写入。
    - `nextWaiverId`。
    - 示例端到端：`cdc_advanced` 中有两条 `CDC-005` 路径，只豁免其中一条；另一条仍未豁免（负例）；第二个豁免写入同一文件，id 为 W-2。
  - `tests/cli/RunWaive.cmake`（新，`cli.waive`）：在示例副本上运行 `opencdc waive`，示例本身不被修改；负例为缺少原因、id 重复。
  - `tests/CMakeLists.txt`：`cli.waive`、`gui.waive_hit`、`gui.waive_output`。
- 文档：`docs/waivers.md`（新增"Adding a waiver"一节）、`README.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 生成的豁免总是"规则 + 指纹"。要按源或目标的通配符豁免，需要用 CLI 的 `--source`/`--destination`，或手工编辑文件。
- GUI 不能删除或修改已有的豁免。

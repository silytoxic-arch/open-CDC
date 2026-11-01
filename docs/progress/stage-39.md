# 阶段 39：豁免教程，文档中的命令作为测试运行

日期：2026-11-01　版本：0.39.0

## 1. 完成的功能

### 1.1 教程 `docs/tutorial-waivers.md`

在 `examples/cdc_advanced` 的副本上，从第一次分析走到一个被 CI 持续检查的豁免：
1. **分析**：路径 #7 的 `CDC-007`（`a_kick` 的单周期脉冲可能被漏采），路径行末尾给出指纹；说明指纹为什么稳定。
2. **豁免**：`opencdc waive --fingerprint … --rule CDC-007 --reason … --author … --expires …`。工程原本没有豁免文件，所以新建并加入工程。再次分析时，#7 变为 safe，"处于潜在风险"从 4 条减为 3 条，3 个错误违例保留（这些是示例中故意设置的真实错误）。
3. **审计**：
   - `opencdc waivers --fail-on unused,expired,invalid` 通过；
   - 加入一个选择已改名信号的过时豁免后，报 `WAIVER-UNUSED`，"Audit : FAILED (unused)"，退出码 4；
   - 用 `--remove` 删除后重新通过。
4. **过期日期**：`expires`、提醒期（`waiverExpiryWarningDays`）与 `--fail-on expiring`。
5. **评审用的报告**：`--html` 与 `--csv`，后者包括 `waivers.csv`。
6. **CI**：GitHub Actions 的两步示例，分别是分析（未豁免的错误违例使退出码为 3）和豁免审计。

`docs/waivers.md` 与 `README.md` 链接到教程。

### 1.2 文档测试：`tests/cli/RunDocConsole.cmake`

教程中每个 ```console 代码块都由测试运行，避免文档与程序不一致：
- `$ opencdc …` 是命令，用被测的可执行文件运行；
- 命令后面的行是期望输出，按顺序与实际输出匹配：
  - `...` 单独一行表示跳过任意行；
  - 行内的 `...` 表示任意文本，例如路径部分；
  - `(exit code N)` 指定退出码，默认 0。
- 以 CMake 脚本实现，Linux 和 Windows 都可以运行，无需 Python。
- 测试 `cli.waiver_tutorial` 先用 `TutorialSetup.cmake` 准备示例副本（不含豁免文件，与教程开头一致），然后运行教程的 9 条命令。

指纹由 FNV-1a 根据名字计算，与平台无关，所以教程中写出的指纹在 Windows 上也一致。

## 2. 验证

- 教程的 9 条命令全部给出文档中的输出和退出码。
- 编写过程中，测试发现了一处与程序不一致的地方：CLI 先输出 `Rules` 摘要，再输出路径列表，教程最初把顺序写反了。已按实际输出修正。
- **文档测试本身**（`cli.doc_runner_*`）：
  - 正确的文档通过；
  - 负例：期望行错误（"expected line not found"）、退出码错误（"exit code 0, the document says 3"）都会失败（`WILL_FAIL`）。已确认是因为这两个原因失败，而不是脚本错误。

## 3. 修改的文件

- `docs/tutorial-waivers.md`（新）。
- `tests/cli/RunDocConsole.cmake`（新）、`tests/cli/TutorialSetup.cmake`（新）、`tests/cli/docs/{right,wrong_line,wrong_exit}.md`（新）。
- `tests/CMakeLists.txt`：`cli.waiver_tutorial`、`cli.doc_runner_right`、`cli.doc_runner_wrong_line`、`cli.doc_runner_wrong_exit`。
- `docs/waivers.md`、`README.md`：链接。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- 文档测试只能运行 `opencdc` 命令；教程开头的复制步骤（`cp`、`rm`）由测试的准备脚本完成。
- CI 示例（YAML）不运行。

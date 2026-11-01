# 阶段 40：README 的示例输出由测试保证

日期：2026-11-01　版本：0.40.0

## 1. 完成的功能

README 的 Usage 部分以前只列出命令，没有给出结果。新增的示例输出采用阶段 39 的 `console` 格式，由测试运行，以后程序的输出一变，测试就会失败，README 不会悄悄过时。

新增的 4 条命令和期望输出：

| 命令 | 显示的结果 | 退出码 |
|---|---|---|
| `opencdc analyze examples/smoke/smoke.ocdc.json -q` | 2 个时钟（15/15 个寄存器）、7 条跨域路径、1 条潜在风险、4 条需复查、按时钟对的汇总 | 0 |
| `opencdc analyze examples/cdc_advanced/cdc_advanced_waived.ocdc.json -q` | 豁免阶段"3 rule hit(s) waived"，3 个错误违例、3 个已豁免命中 | 3 |
| `opencdc waivers examples/cdc_advanced/cdc_advanced_waived.ocdc.json --fail-on unused,expired,invalid` | 2 个豁免都在使用中，审计通过 | 0 |
| `opencdc analyze examples/primitives/primitives.ocdc.json -q` | MMCM 的两个派生时钟（200 MHz = clk_in × 0.5，50 MHz = clk_in × 2）、`FE-PRIMITIVES`、1 个错误违例和 1 个警告违例 | 3 |

测试 `cli.readme` 先把 `examples/` 复制到构建目录（准备脚本 `tests/cli/ReadmeSetup.cmake`），再从那里运行，与 README 中"从仓库根目录"运行的写法一致；仓库中的文件不会被修改。

## 2. 修改的文件

- `README.md`：Usage 后增加示例输出。
- `tests/cli/ReadmeSetup.cmake`（新）。
- `tests/CMakeLists.txt`：`cli.readme`。

## 3. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 4. CI 状态

0.39.0 的 CI 5 个作业都没有启动，GitHub 的提示是："The job was not started because recent account payments have failed or your spending limit needs to be increased"。这是 GitHub 账户的计费问题，不是代码问题，需要在仓库所有者账户的 Billing & plans 中处理。在此之前不能生成新的安装包，`dist` 停留在 0.38.0。

## 5. 已知限制

- README 中"Usage"的命令列表本身（```sh 代码块）不运行，只运行 ```console 代码块。

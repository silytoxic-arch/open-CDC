# 阶段 67：在 GUI 中按时序例外豁免；Quartus 示例中的准静态配置

日期：2026-11-08　版本：0.67.0

阶段 66 加入了按时序例外选择的豁免（`timingException`），但只能通过 CLI 或编辑豁免文件使用。这一阶段在 GUI 中加入对应的操作，并在 Quartus 示例中加入一个设计者声明为准静态的跨时钟路径，覆盖从约束到豁免的完整流程。

## 1. 完成的功能

### 1.1 GUI：Waive by timing exception...

规则违例查看器增加按钮 **Waive by timing exception...**：

- **启用条件**：选中的规则命中尚未豁免，且它的路径被时序例外覆盖。
- **生成的豁免**：选中命中的规则，加上时序例外的选择条件，例如 `set_false_path (*sys_top.sdc:34)`。
  - 约束文件只取文件名，不取绝对路径，豁免文件换一台机器仍然可用；
  - 不按指纹选择：同一条约束覆盖的所有路径，包括以后设计修改产生的新路径，都会被覆盖。
- **对话框内容**：
  - 显示时序例外条件；
  - 显示现在会覆盖多少个规则命中、多少条路径；
  - 说明 `set_false_path` 只表示不做时序分析，用户应确认值在使用期间不变。
- **复用的逻辑**：新的 `engine::waiverForException`。
- **原有按钮**：**Waive...** 仍按指纹豁免单条路径，说明文字不变。

### 1.2 Quartus 示例中的准静态配置

`examples/quartus_project` 增加一个配置寄存器 `cfg_mode`：
- 在核心时钟下、计数器停止时设置；
- 在外部振荡器时钟下读取；
- `demo.sdc` 用 Quartus 的裸名称声明它是准静态的：`set_false_path -from {cfg_mode[*]}`，与 MiSTer 的写法相同。

结果：3 条跨时钟路径（以前 2 条），其中 1 条未同步路径被时序例外覆盖。

## 2. 测试

| 测试 | 内容 |
|---|---|
| `gui.waive_timing_exception`（新） | GUI 自测试（在示例工程的副本上）：按时序例外豁免一个命中，重新分析后，同一规则在被同一时序例外覆盖的所有路径上都已豁免，没有时序例外的路径都不受影响；反例：没有时序例外的路径不提供这种豁免 |
| `cli.import_vivado`（扩展） | 导入 Quartus 示例为 OpenCDC 工程后：<br>1. 时序例外仍被识别（`derive_pll_clocks` 在前）；<br>2. `opencdc waive --timing-exception "*demo.sdc*"` 豁免了 `cfg_mode` 的路径；<br>3. 反例：选择另一个约束文件的豁免没有用到（`WAIVER-UNUSED`） |
| `cli.analyze_quartus_project` | 期望值更新：3 条路径，1 条被时序例外覆盖 |

## 3. 修改的文件

- `src/engine/WaiverFile.{h,cpp}`：`waiverForException`。
- `src/gui/CdcViews.{h,cpp}`：按钮和信号 `waiveExceptionRequested`。
- `src/gui/MainWindow.{h,cpp}`：`waiverDialogForException`（统计会覆盖的命中数）。
- `src/gui/WaiverDialog.cpp`：显示时序例外条件，说明文字按选择条件区分。
- `src/gui/main.cpp`：GUI 豁免自测试增加“按时序例外豁免”阶段。
- `examples/quartus_project/rtl/demo_top.sv`、`demo.sdc`：准静态配置。
- `tests/CMakeLists.txt`、`tests/cli/RunImport.cmake`：新测试和期望值。
- `docs/waivers.md`。
- `CMakeLists.txt`：版本 0.67.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：598 个测试全部通过（包括 GUI 自测试）。
  - 第一次运行中，`cli.import_vivado` 失败：我写的正则表达式中两段输出的顺序写反了（“1 hit(s) waived”在路径详细信息之前打印）；
  - 豁免本身是正确的，改正正则表达式后通过。
- SV 语料 24 个项目全部匹配，VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）结果见交付说明。

## 5. 已知限制

- 直接分析 Quartus/Vivado 工程时，GUI 添加豁免前需要先另存为 OpenCDC 工程（与以前一样）。
- 豁免对话框中的命中数是当前结果中的数目；之后设计修改产生的新路径也会被这个豁免覆盖。

## 6. 下一步

阶段 68：用更多真实 Quartus 工程验证（例如另一个 MiSTer 核心，或 Intel/Terasic 的参考设计），检查新的写法和误报。

# 阶段 66：时序例外在 GUI、HTML 报告和 CSV 中显示；按时序例外豁免

日期：2026-11-08　版本：0.66.0

阶段 65 把覆盖跨时钟路径的 `set_false_path`/`set_max_delay` 记在路径上，但只出现在 JSON 和路径说明里。这一阶段让用户在所有输出中看到并筛选这些路径，并可以用一条豁免确认设计者已经声明的路径。

## 1. 完成的功能

### 1.1 显示与筛选

| 输出 | 改动 |
|---|---|
| GUI 路径查看器 | 新列“Timing exception”（`set_false_path sys_top.sdc:34`）；新筛选：有时序例外 / 没有时序例外 |
| GUI 违例查看器 | 同上的列和筛选 |
| HTML 报告 | 路径总表新列“Timing exception”，每条路径的详细信息中增加一行，表下说明含义 |
| CSV | `paths.csv` 增加 `timing_exception` 列（放在最后，已有列的位置不变）；`waivers.csv` 增加 `timing_exception` 列 |

短格式（`shortException`）只保留文件名和行号，完整路径在 JSON 和详细信息中。

### 1.2 按时序例外豁免

豁免文件增加一个选择条件 `timingException`：对覆盖路径的时序例外做 glob 匹配。

```json
{ "id": "W-QS", "rule": "CDC-00[13]", "timingException": "*sys_top.sdc:34*",
  "reason": "cfg 只在核心复位期间改变，使用期间不变" }
```

- `"*"` 匹配所有被时序例外覆盖的路径，`"*sys_top.sdc:34*"` 只匹配这一条约束；
- 没有被时序例外覆盖的路径永远不匹配（反例测试）；
- 只用于跨时钟路径，不用于输出端口（`CDC-OUTPUT-CLOCK`）；
- 和其他选择条件一样，可以单独使用，也可以与 rule、source、destination 组合；
- CLI：`opencdc waive --timing-exception <glob>`；`opencdc waivers` 的文本和 JSON 输出、HTML 报告、GUI 豁免列表中都显示这个条件；
- `docs/waivers.md` 说明了用法和注意事项：`set_false_path` 只说明不做时序检查，用户确认值在使用期间不变后，才应豁免。

## 2. 真实工程验证

- Template_MiSTer：路径总表中 121 条未同步路径显示时序例外，例如 `cfg` → `set_false_path sys_top.sdc:34`、`hdmi_osd.rot` → `set_false_path sys_top.sdc:50`。
- 用 `"timingException": "*"` 的豁免可以一次覆盖这些路径。不过 MiSTer 直接分析 `.qpf`，而直接分析厂商工程时 CLI 不写入豁免文件（阶段 59）。需要先用 `opencdc import` 导入为 OpenCDC 工程，再添加豁免。
- 其他语料结果不变。

## 3. 修改的文件

- `src/engine/Report.{h,cpp}`：`shortException`；HTML 列和详细信息行；CSV 列。
- `src/gui/CdcViews.{h,cpp}`：两个查看器的列和筛选；豁免列表显示新条件。
- `src/cdc/CdcModel.h`：`WaiverRecord::timingException`。
- `src/rules/Waiver.cpp`：匹配。
- `src/engine/WaiverFile.cpp`：读写、校验（新条件也算“至少一个选择条件”）。
- `src/engine/ResultJson.cpp`：豁免的 JSON。
- `src/cli/main.cpp`：`--timing-exception`，`waivers` 输出。
- `tests/frontend/test_quartus_compat.cpp`：
  - 新增 1 个测试用例：按时序例外豁免，包括反例；
  - 阶段 65 的测试用例增加 CSV 和 HTML 的检查。
- `docs/waivers.md`。
- `CMakeLists.txt`：版本 0.66.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：597 个测试全部通过（包括 GUI 测试）。
- SV 语料 24 个项目全部匹配，VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）结果见交付说明。

## 5. 已知限制

- GUI 的“Waive...”按钮仍按单条路径的指纹生成豁免，没有“按时序例外豁免”的按钮，需要用 CLI 或编辑豁免文件。
- 直接分析 Quartus/Vivado 工程时，CLI 不写入豁免文件，需要先导入。

## 6. 下一步

阶段 67：GUI 的豁免对话框支持按时序例外生成豁免（预填 `timingException` 和覆盖的路径数）。之后继续用更多真实工程（例如其他 MiSTer 核心或 Intel 参考设计）验证 Quartus 支持。

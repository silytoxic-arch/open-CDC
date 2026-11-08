# 阶段 65：Quartus 约束中的裸名称；在跨时钟路径上标出设计者声明的时序例外

日期：2026-11-08　版本：0.65.0

阶段 64 发现：MiSTer 在 `sys_top.sdc` 中用 38 条 `set_false_path` 把配置寄存器、视频尺寸、OSD 参数等声明为准静态，而这些声明对报告没有任何作用。原因有两个：

- 写法是 Quartus 的裸名称（`set_false_path -from {cfg[*]}`），以前没有求值，记为“已记录、未使用”；
- 寄存器级的例外只用于时钟关系未知的路径（第 25.5 节），而 MiSTer 用 `set_clock_groups` 声明了时钟关系。

这一阶段让这些声明被读取，并显示在它覆盖的跨时钟路径上。路径是否同步的判断不变。

## 1. 完成的功能

### 1.1 Quartus 的裸名称

在 Quartus 的 SDC 中，`-from`/`-to` 的值如果不是查询（`[get_registers ...]`）、变量或 `*`，而是名称，Quartus 把它读作 keeper（寄存器、端口、引脚）集合：

```tcl
set_false_path -to   {cfg[*]}
set_false_path -from {*_osd|rot*}
set_false_path -from {WIDTH[*] HFP[*] HS[*] HBP[*] HEIGHT[*] VFP[*] VS[*] VBP[*]}
```

这些名称按 Quartus 的命名匹配（与 `get_keepers` 相同：顶层实体以下，用 `|` 分隔层次）。以下情况按此求值：

- 工程来自 Quartus（直接分析 `.qpf`/`.qsf`）：新的 `SdcOptions::quartus`；
- 或者约束文件中前面出现过 Quartus 专有命令（`derive_pll_clocks`、`derive_clock_uncertainty`、`get_registers`、`get_keepers`、`get_nodes`）：用 `opencdc import` 导入后保存的工程也能识别；
- 或者名称含 Quartus 的层次分隔符 `|`，这种名称在 Vivado 中不会出现。

其他情况（例如 Vivado 约束中的裸名称）仍记为“已记录、未使用”（反例测试）。

匹配不到的名称报告 `CON-UNMATCHED`，与 Quartus 一致。Template_MiSTer 中有两处：
- `set_false_path -from {get_ports {SW[*]}}`：MiSTer 把查询写在了花括号里，Quartus 把它当作两个名称 `get_ports` 和 `SW[*]`。前者匹配不到，Quartus 同样会警告；后者匹配到端口 `SW`。
- HPS 原语内部的引脚（`sysmem|fpga_interfaces|clocks_resets|f2h*`）：HPS 原语按黑盒建模，内部引脚不存在。

### 1.2 在跨时钟路径上标出时序例外

时钟关系已知的跨时钟路径，如果被约束中的 `set_false_path` 或 `set_max_delay -datapath_only` 覆盖：

- 路径的判断不变：没有同步器仍然是违例；
- 路径上记录覆盖它的例外（`timingException`，例如 `set_false_path (sys_top.sdc:34)`），JSON 中输出，并加一条说明：设计者把这条路径排除在时序分析之外（通常是使用期间不变的值）；除非源值在目的端采样时不会变化，仍然需要同步结构；
- CLI 汇总中增加一行：被时序例外覆盖的未同步路径数。

原因：`set_false_path` 是时序约束，说明这条路径不需要满足建立/保持时间，但不能证明数据在采样时稳定，所以不能代替同步器或豁免。显示出来后，用户可以先审查设计者已经声明的路径。确认后，可以用豁免文件（waiver）关闭这些违例。

时钟关系未知的路径照旧：例外使它按异步分析（阶段 18）。

## 2. 真实工程验证

| | Template_MiSTer | NES_MiSTer |
|---|---|---|
| 求值的 `set_false_path` | 38（以前 0） | 38（以前 0） |
| 未同步路径 | 432 | 498 |
| 其中被时序例外覆盖 | 121 | 119 |

抽查（Template_MiSTer）：

| 源寄存器 | 路径数 | 覆盖它的例外 |
|---|---|---|
| `cfg` | 10 | `sys_top.sdc:34` |
| `WIDTH` | 4 | `sys_top.sdc:55` |
| `hdmi_osd.rot` | 21 | `sys_top.sdc:50` |
| `areset` | 8 | `sys_top.sdc:60` |
| `scaler_out` | 72 | 无（没有声明） |

其他语料（包括使用 Quartus SDC 的 DE2-115 以太网示例）结果不变。

## 3. 修改的文件

- `src/constraints/Sdc.{h,cpp}`：`SdcOptions::quartus`；Quartus 专有命令的识别；裸名称读作 keeper 集合（`implicitKeepers`、`keeperCollection`）。
- `src/engine/ProjectConfig.{h,cpp}`：`isQuartusProject`。
- `src/engine/AnalysisSession.cpp`：Quartus 工程的约束按 Quartus 读取。
- `src/cdc/CdcModel.h`、`src/cdc/CdcAnalyzer.cpp`：`CdcPath::timingException` 和说明。
- `src/engine/ResultJson.cpp`：JSON 的 `timingException`。
- `src/cli/main.cpp`：汇总行。
- `tests/frontend/test_quartus_compat.cpp`：1 个测试用例（3 个部分：Quartus 约束中的裸名称、含 `|` 的名称、反例）。
- `tools/ci/sv-corpus.sh`：两个 MiSTer 工程检查被例外覆盖的路径数。
- `README.md`、`docs/architecture.md`（第 25.5 节）。
- `CMakeLists.txt`：版本 0.65.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：596 个测试全部通过。
- SV 语料 24 个项目全部匹配，VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- GitHub Actions Windows x64 / VS2022（提交 98cd56d，运行 42459541932）：
  - Release：596/596 通过，生成 `OpenCDC-0.65.0-windows-x64.zip` 与 `.exe`；
  - Debug：596/596 通过。

## 5. 已知限制

- HTML 报告和 GUI 通过路径的说明显示时序例外，没有单独的列或筛选条件；CSV 中没有这一列。
- Quartus 名称匹配中 `*` 是否跨越层次分隔符 `|`，沿用 `get_keepers` 的现有实现。
- 被时序例外覆盖的违例不会自动豁免，需要用户用豁免文件确认。

## 6. 下一步

阶段 66：在 GUI 的违例查看器和 HTML 报告中加入“时序例外”列和筛选条件，CSV 加入对应列，方便用户先审查设计者已声明的路径，并为它们批量生成豁免。

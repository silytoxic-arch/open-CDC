# 阶段 7 完成报告：工程化（豁免、报告、性能、打包）

日期：2026-10-21　版本：0.7.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T7.1 豁免（waiver） | **匹配**：按规则（可用通配）、路径指纹、源/目标寄存器名通配匹配；必须写原因，可选作者和到期日。规格见 `docs/waivers.md`。<br>**应用**：作为独立阶段"Waiver application"在规则检查之后运行。命中保留并标记豁免编号；路径状态由未被豁免的命中重新计算；全部命中都被豁免的路径标为 waived。<br>**诊断**：过期（`WAIVER-EXPIRED`）和未命中（`WAIVER-UNUSED`）给出警告；文件错误给出 `WAIVER-OPEN`、`WAIVER-SCHEMA`、`WAIVER-INVALID`，并定位到条目所在行。<br>**输出**：CLI、JSON、HTML、CSV、GUI（违规表新增"Waiver"列和过滤器，规则面板显示豁免原因）都显示豁免。CLI 退出码只计未被豁免的违规。 |
| T7.2 报告 | **HTML**（`--html`，GUI 中为"File > Export HTML Report"）：单个自包含文件，内联 CSS 和 SVG，不需要脚本。内容包括摘要卡片、各阶段及耗时、时钟和关系矩阵、规则统计（命中/已豁免）、路径表、同步结构、每条路径的详情（层次路径、同步条件、规则命中和修复建议、豁免原因、原理图）、豁免表、诊断信息、未建模的构造。<br>**CSV**（`--csv <dir>`，GUI 中为"File > Export CSV"）：`paths.csv` 和 `violations.csv`，RFC 4180 引号规则，带 UTF-8 BOM，便于 Excel 打开。<br>**阶段耗时**：每个阶段的耗时在 CLI、JSON、HTML 中输出。 |
| T7.3 性能 | 10⁶ 寄存器的设计：分析 18.8 s，峰值 2.6 GB；10⁵ 寄存器：1.5 s，271 MB。<br>GUI 在 10⁶ 寄存器（17.8 万条路径）的设计上 22 s 完成分析并填充全部视图。<br>新增基准生成器 `tools/bench/gen_bench.py` 和基准文档 `docs/benchmarks.md`；CTest 中加入基准设计的同步器数量回归测试。 |
| T7.4 打包 | **安装规则**：CMake `install()` 只安装 `OpenCDC` 组件，FetchContent 依赖自带的安装规则被排除。<br>**Windows**：安装时由 `qt_generate_deploy_app_script` 运行 windeployqt，把 Qt 运行库部署到 `opencdc-gui.exe` 旁边。<br>**CPack**：Windows 生成 ZIP 和 NSIS 安装包（开始菜单项，可选加入 PATH）；Linux 生成 TGZ 和 DEB。<br>**测试**：CTest `package.install_smoke` 安装到临时目录，然后运行安装后的 CLI（版本号、分析安装后的示例）和 GUI 自测；在 Windows 上还检查 Qt DLL 和平台插件是否已部署。<br>**CI**：Windows 和 Linux 两个作业都执行安装测试、cpack，并上传安装包。Linux 作业改用 Qt 6.8.3，因为 Ubuntu 24.04 自带的 Qt 6.4 低于最低要求 6.5。 |

## 2. 修改的文件

**新建**
- `src/rules/Waiver.{h,cpp}`、`src/engine/WaiverFile.{h,cpp}`、`src/engine/Report.{h,cpp}`
- `cmake/Packaging.cmake`
- `tools/bench/gen_bench.py`
- `tests/engine/test_waivers.cpp`、`tests/engine/test_report.cpp`、`tests/package/InstallSmoke.cmake`
- `examples/cdc_advanced/waivers.json`、`cdc_advanced_waived.ocdc.json`
- `docs/waivers.md`、`docs/benchmarks.md`
- `docs/progress/stage-7.md`、`stage-7-gui.png`、`stage-7-report.html`、`stage-7-report-path.png`

**修改**
- 根 `CMakeLists.txt`：
  - 版本 0.7.0；
  - `GNUInstallDirs`；
  - 安装组件和数据目录；
  - 非 Windows 平台保留外部库的 RPATH；
  - `include(Packaging)`。
- `src/cdc/CdcModel.h`：`RuleHit::waiver`、`CdcPath::waived`、`WaiverRecord`、`CdcResult::waivers`、`CdcStats::waivedHits`。
- `src/cdc/AdvancedAnalysis.cpp`：
  - 握手/FIFO 匹配改为索引；
  - 下游读者批量预取；
  - 删除不再使用的反向 BFS（`reaches`）。
- `src/rules/RuleEngine.cpp`：新增 `makeHit()`。
- `src/engine/`：
  - `ProjectConfig`：`waivers` 字段；
  - `AnalysisSession`：豁免阶段、阶段计时、`waivedHits()`、`pathsWithStatus()`；
  - `ResultJson`：流式 `writeJson()`，`toJson()` 输出不变；
  - `CMakeLists.txt`。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`maxGenerateSteps` 提高到 2²⁴。
- `src/cli/main.cpp`：`--html`、`--csv`，JSON 流式写出，显示豁免信息和阶段耗时。
- `src/gui/`：
  - `CdcViews.{h,cpp}`：路径表和违规表改为 model/view，并加入豁免列和过滤器；
  - `MainWindow.{h,cpp}`：导出 HTML/CSV；报告页的同步结构超过 300 个时按类型汇总；
  - `main.cpp`：自测增加豁免和导出检查，大设计时抽样检查，并输出耗时；
  - `CMakeLists.txt`：安装和 Qt 部署。
- `tests/CMakeLists.txt`：`cli.analyze_waived`、`cli.analyze_reports`、`package.install_smoke`、`bench.*`、`gui.cdc_advanced_waived`。
- `.github/workflows/ci.yml`：cpack、上传安装包，Linux 作业改用 Qt 6.8.3。
- 文档：`README.md`、`docs/architecture.md`（状态行，第 14 节新增 9 条）、`docs/rules/CDC-0xx.md`（新增"Waivers"一节，删除"以后会提供豁免"的说明）。

已有接口没有删除，也没有改变语义。唯一的签名变化是 `CdcPathView::table()` / `ViolationView::table()` 的返回类型由 `QTableWidget*` 改为 `QTableView*`；这两个函数只在 GUI 内部使用。另外新增了 `rowCount()` 和 `visibleRows()`。

## 3. 关键算法

### 3.1 豁免应用
1. 读取豁免，并按今天的 UTC 日期标记已过期的条目（`expires < today`，到期当天仍然有效）。
2. 对每条路径的每个命中，按文件顺序找第一个未过期、且所有已给出的选择条件都匹配的豁免。
3. 路径状态取未被豁免命中的最高状态。全部命中被豁免时状态为 safe，并标记 waived。
4. 统计每个豁免的使用次数；使用次数为 0 时给出 `WAIVER-UNUSED`。

通配符匹配用迭代算法，失配时回溯到最近的 `*`，最坏 O(n·m)，不会出现指数级回溯。

### 3.2 握手/FIFO 识别的索引化
原实现对每条受限定数据路径扫描全部路径，再对每个候选做两级反向 BFS，复杂度 O(Q×P)。新实现：
1. 只建一次索引：单比特同步链路径按源寄存器建索引；Gray 路径单独建索引。
2. 应答候选：先查源寄存器就是请求同步器某一级的路径，再查同步器各级下游 2 级以内的寄存器。下游关系使用缓存的读者表，并在循环开始前按层批量预取，每层只扫描一次全图。
3. FIFO：在读数据目标寄存器的控制锥（使能、选择，2 级以内）中找寄存器，再在 Gray 索引中查它们。

选择顺序与原来相同（直接来自请求同步器的优先），候选按节点号排序，结果是确定的。语料库和全部测试的结果与优化前一致。

### 3.3 流式 JSON
`JsonWriter` 按 `nlohmann::json::dump(indent)` 的格式写出：
- 小成员整体转储，再给其中的换行补上缩进（字符串内的换行都已转义，所以可以安全地按换行补缩进）；
- 寄存器表和路径表逐个元素生成并写出；
- 键按字典序排列，与 `nlohmann::json` 的 `std::map` 顺序一致。

在 10⁵ 寄存器的设计上，新旧输出除时间戳外逐字节相同。

### 3.4 HTML 规模控制
- 路径数不超过 `maxPaths`（5000）时，列出全部路径。
- 否则只列"有规则命中或状态不是 safe"的路径，并说明完整数据在 CSV 和 JSON 中。
- 同步结构表只列包含已列出路径的结构；指向未列出路径的编号显示为普通文本，不做链接。

## 4. 编译与测试结果

| 环境 | 构建 | 测试 |
|---|---|---|
| Linux x64，GCC 13.3，Release，`-Werror` | ✅ 零警告 | ✅ 229/229 |
| Linux x64，GCC 13.3，Debug，`-Werror` | ✅ 零警告 | ✅ 229/229 |
| Linux 安装树、TGZ、DEB（cpack） | ✅ 安装后的 CLI 和 GUI 自测通过 | ✅ |
| Linux x64，Clang 18，Debug，`-Werror` | ✅ 零警告 | ✅ 229/229 |
| Linux x64，GCC，ASan + UBSan + `_GLIBCXX_DEBUG`（检查迭代器的 STL） | ✅ | ✅ 225/225（不含 GUI），真实工程和基准设计均无报错 |
| Windows x64 + VS2022 | ⚠️ 仍未实际验证；CI 配置和一键脚本 `tools/ci/verify-windows.ps1` 已就绪 | — |

229 个 CTest 测试：
- 212 个 Catch2 用例，本阶段新增 7 个，主要在 `test_waivers.cpp` 和 `test_report.cpp`；
- 11 个 CLI 端到端测试；
- 4 个 GUI 无头测试；
- 1 个安装测试；
- 1 个基准生成器测试。

真实工程回归：verilog-axis 全部模块和 verilog-ethernet 的 4 个 MAC+FIFO 顶层，结果与阶段 6 相同。

性能结果见 `docs/benchmarks.md`。HTML 报告用 Chromium 渲染核对过（`stage-7-report-path.png`）。

## 5. 已知限制

1. **Windows/MSVC 仍未实际编译。** 本环境无法访问 GitHub，也无法安装 Windows 交叉编译工具链。已经做的替代验证：
   - GCC 和 Clang 两种编译器零警告；
   - ASan/UBSan 加检查迭代器的 STL（对应 MSVC Debug 的迭代器检查）全部通过；
   - 静态检查了 Windows 专用代码路径（UTF-8 路径转换、`gmtime_s`、控制台代码页、`/utf-8`）。

   在 Windows 上一条命令即可验证：`tools/ci/verify-windows.ps1`，它会依次完成配置、Debug/Release 构建（`/W4 /WX`）、全部测试和打包，并写出日志。第一次构建可能需要修正 MSVC 特有的警告。
2. 分析是单线程的。在 10⁶ 寄存器的设计上，前端（slang）约占四分之一的时间。
3. 豁免按名称和指纹匹配。寄存器改名后指纹会变，豁免会变成 `WAIVER-UNUSED`。这是有意的设计，但需要人工更新豁免。
4. GUI 的层次树仍然为每个实例创建一个条目（34 万个实例时填充约 2 s）。
5. Linux 安装包依赖系统的 Qt ≥ 6.5（Ubuntu 24.04 的 Qt 是 6.4，需要另外安装）。
6. 项目本身还没有选定许可证（`LICENSE` 文件），安装包中只附带第三方组件的说明。

## 6. 整体总结（阶段 1–7）

| 阶段 | 内容 | 版本 |
|---|---|---|
| 1 | 工程骨架：项目文件，slang 前端，模块和实例层次，诊断，CLI/GUI 骨架，JSON | 0.1 |
| 2 | RTL 抽取：字级 Design IR，always 块的符号执行，寄存器、锁存器、存储器，结构哈希和无用逻辑清理 | 0.2 |
| 3 | 时钟：SDC/XDC 子集，时钟追溯（门控、MUX、分频），关系矩阵（含"未知"），GUI 时钟视图 | 0.3 |
| 4 | CDC 核心：时序依赖图，跨域路径搜索与分组，N 级同步器和受限定数据识别，CDC-001/002/003/010，路径与违规视图 | 0.4 |
| 5 | 高级 CDC：toggle、Gray（证明 ±1 步进）、握手、异步 FIFO、复位同步器；重汇聚、多比特、脉冲丢失、混合时钟；CDC-004 至 009 | 0.5 |
| 6 | 完整 GUI：CDC 原理图（自研分层布局，无 Qt 依赖，可输出 SVG），视图联动，层次统计 | 0.6 |
| 7 | 工程化：豁免、HTML/CSV 报告、10⁶ 规模性能、安装包 | 0.7 |

- **代码量**：`src` 约 1.68 万行，`tests` 约 4500 行。
- **规则**：10 条，每条都有正例、反例测试和规格文档。
- **示例**：4 个示例工程；另外用 2 个开源真实工程（verilog-axis、verilog-ethernet）做回归。
- **架构**：所有分析库和 CLI 都不依赖 Qt，由构建时检查保证；GUI 与 CLI 共用 `AnalysisSession`。

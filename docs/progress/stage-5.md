# 阶段 5 完成报告：高级 CDC 分析

日期：2026-10-21　版本：0.5.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T5.1 模型扩展 | 新增同步器类型：`toggle`、`gray`、`handshake`、`async-fifo`、`reset-sync`。<br>路径新增字段：分组大小/成员、无时钟源、可能为脉冲、位是否被一起读取、同源拆分同步、混合时钟、重汇聚记录、所属结构。<br>新增 `CdcResult::structures`（同步结构列表，一个握手结构包含请求、应答和数据三条路径）。<br>分析器内部状态拆到 `AnalyzerState.h`，阶段 5 的分析在独立文件 `AdvancedAnalysis.cpp`。新增选项 `reconvergenceDepth`。 |
| T5.2 复位同步器与异步复位跨域 | 识别复位同步器：同一时钟的若干触发器，首级 D 为常量、共享同一异步复位，后级逐级直连（包括向量写法 `{r[1], r[0]} <= {r[0], 1'b1}`）；检查级数、扇出等条件，并统计它保护的寄存器数。<br>异步复位跨域从"只统计"改为生成路径，按（复位源、目标时钟、复位同步器）分组。无时钟域的输入作为复位源时同样生成路径。 |
| T5.3 协议同步器 | **Gray**：沿 XOR/NOT/常量移位/MUX 追溯，验证 `D[k] = X[k] ^ X[k+1]`。分支各自是 Gray 编码的 MUX 也能识别。并证明二进制值每次至多 ±1：加/减一位增量（含 Verilog 位宽扩展产生的常量 0），或"增量/保持"MUX。<br>**Toggle**：源寄存器自翻转（`t <= t ^ e` 或带使能的 `t <= ~t`），每位独立翻转的向量也算；检查目标端是否有边沿检测。<br>**握手**：受同步请求限定的数据，加上一条单比特应答路径反向回到源时钟（优先选择"应答 = 同步后的请求"）。<br>**异步 FIFO**：存储器读受 Gray 写指针限定，且存在反向的 Gray 读指针。 |
| T5.4 汇聚类分析 | **重汇聚**（CDC-005）：从同步器输出前向传播标签，按寄存器输入位记录，深度可配置。标签集合驻留去重、并集记忆化；伙伴计数按源时钟分类计数，复杂度 O(n·时钟数)。<br>**位汇合**（CDC-008）：逐位同步的多比特值，或同一源寄存器各位经各自的同步器，在目标域是否被同一寄存器一起读取。<br>**混合时钟**（CDC-009）。<br>**可能为脉冲的源**（CDC-007）。 |
| T5.5 规则 | CDC-004～009 全部实现，加上已有的共 10 条，每条都有规格文档 `docs/rules/CDC-00x.md`。<br>CDC-001/002/003 不再判断异步复位引脚。<br>时钟关系未知时，PotentialRisk 也降为 NeedsReview。 |
| T5.6 集成 | 引擎"高级 CDC 分析"阶段改为已实现，未实现阶段改为"报告生成（阶段 7）"。<br>JSON 格式版本 2：新增 `structures`、路径新字段、`summary.pathsPotentialRisk/pathsNeedReview`。<br>CLI 输出潜在风险数、需复核数和同步结构表。<br>GUI：同步器类型过滤新增 5 种；路径详情显示所属结构（可点击跳转到同结构的其他路径）、分组寄存器、重汇聚位置和伙伴路径、拆分同步、混合时钟、脉冲源；报告页列出同步结构表；状态栏增加潜在风险数和需复核数。 |
| T5.7 测试与验证 | 新增 37 个测试用例，覆盖每条新规则的正例、反例和每种同步结构。<br>新增示例 `examples/cdc_advanced`：7 个模块，正确的协议同步器加上每条新规则一个故意的缺陷。<br>新增 CLI 和 GUI 端到端测试。<br>用 verilog-axis、Cummings FIFO、verilog-ethernet 做了端到端验证，并测了性能。 |

## 2. 修改的文件

**新建**
- `src/cdc/AnalyzerState.h`、`src/cdc/AdvancedAnalysis.cpp`
- `docs/rules/CDC-004.md` … `CDC-009.md`
- `examples/cdc_advanced/`：`rtl/{sync_2ff,sync_vec,rst_sync,pulse_sync,gray_fifo,handshake,cdc_advanced}.v`、约束、工程文件、README
- `tests/cdc/CdcTestUtil.h`（从 `test_cdc.cpp` 抽出的公共辅助函数）、`tests/cdc/test_cdc_advanced.cpp`
- `docs/progress/stage-5.md`、`stage-5-gui.png`

**修改**
- `src/cdc/CdcModel.{h,cpp}`：新类型和字段（只增不删）。
- `src/cdc/CdcAnalyzer.{h,cpp}`：
  - 改用共享状态；
  - 异步复位路径分组；
  - 首级门控判定；
  - 链在"已达最小级数后被其他逻辑读取"的一级结束；
  - `findCrossings` 按源节点缓存跨域判断。
- `src/cdc/SeqGraph.{h,cpp}`：`Consumer::bitEnd`，合并连续且读取方式相同的读者位。
- `src/cdc/CMakeLists.txt`
- `src/rules/RuleEngine.cpp`：CDC-004～009；001/002/003 排除复位引脚；未知关系下的降级。
- `src/engine/{ProjectConfig,AnalysisSession,ResultJson}.*`：`reconvergenceDepth`、`pathsWithStatus()`、阶段状态、JSON v2。
- `src/cli/main.cpp`
- `src/gui/{CdcViews.h,CdcViews.cpp,MainWindow.cpp,main.cpp}`
- `tests/CMakeLists.txt`、`tests/cdc/test_cdc.cpp`、`tests/engine/test_analysis_session.cpp`
- 示例说明：`examples/{smoke,cdc_rules}/README.md`（补充阶段 5 的预期结果）
- 文档：`README.md`、`docs/architecture.md` 第 14 节（新增 16 条实现差异）、`docs/rules/CDC-00{1,2,3,10}.md`
- 根 `CMakeLists.txt`：版本 0.5.0

**接口变化**
- 已有字段和函数的语义没有改变。
- JSON 中 `stats.asyncResetCrossingsDeferred` 更名为 `asyncResetCrossings`，因此 `formatVersion` 升为 2。
- `SeqGraph::consumersOf` 返回的记录可能覆盖多个位（`bitEnd`）。逐位语义的使用者需要按区间处理；仓库内所有使用处已更新。

## 3. 关键算法

### 3.1 复位同步器与复位路径
1. 候选首级：有异步复位、无使能/同步复位、某位 D 为常量的寄存器位。
2. 按层批量查询读者，逐级前进：下一级必须同时钟、直连、无使能，且异步复位源集合与首级相同。
3. 末级的 AsyncReset 读者即受保护的寄存器。单级的常量触发器只有在驱动复位时才计入。
4. 跨域的异步复位连接按（源、目标时钟、复位同步器）分组为一条路径。目标是复位同步器的一级时，类型为 `reset-sync`，条件取自同步器；否则由 CDC-006 判断。

### 3.2 XOR 树追溯与 Gray/Toggle 识别
- `XorTracer` 把一个位展开为"异或叶子"集合（偶数次出现相互抵消），可以穿过 XOR/XNOR、NOT、常量移位，以及满足以下任一条件的 MUX：一臂为常量、一臂是保持值（寄存器自身 Q）、选择信号可常量折叠、两臂结果相同。结果分为"值 / 常量 / 保持"三类：只有保持没有变化时不算 toggle。
- **Gray**：末位叶子为 `X[N-1]`，其余位为 `{X[k], X[k+1]}`；不满足时，若全部位来自同一个字宽 MUX，就递归检查各臂。单一二进制来源且能证明 ±1 时为"必要条件已验证"；有多个来源（例如帧 FIFO 回退到提交指针）时为"协议条件未验证"。
- **Toggle**：每位的叶子都包含自身 Q。

### 3.3 重汇聚（CDC-005）
1. 每条同步链的末级打上路径标签。
2. 第 0 层，直接读者按"输入引脚位"获得标签；使能/同步复位作用于整个寄存器。
3. 第 1..K 层，沿目标域寄存器传播：读者读取的位区间内各位标签取并集。
4. 某一位上存在两条"不同首级、源时钟相关、且不属于同一拆分组"的路径时，该寄存器是汇聚点，在此停止传播。
5. 标签集合驻留成 id；4000 位宽数据通路的各位共享同一个集合。

### 3.4 首级逻辑的毛刺判定（CDC-004）
统计首级每一位锥中"异步位"（非目标时钟、非同步时钟或无时钟的源位）的数量：
- 为 1：只是目标域信号或常量的门控，不会产生毛刺，条件成立，并记录说明；
- 大于 1：报 CDC-004。

### 3.5 规则判定一览

| 规则 | 条件 | 严重度 / 状态 |
|---|---|---|
| CDC-004 | 链路径首级前的逻辑组合了 ≥2 个异步位 | Error / Violation |
| CDC-005 | 存在汇聚记录 | Warning / PotentialRisk |
| CDC-006 | 复位路径没有复位同步器，或复位同步器条件失败 | Error / Violation；无时钟域输入为 PotentialRisk |
| CDC-007 | 源可能为单周期脉冲，经电平同步；源周期 < 1.5 × 目标周期 | Warning / PotentialRisk；无周期信息为 NeedsReview |
| CDC-008 | 逐位同步的非 Gray 多比特值，或同源拆分同步 | Error / PotentialRisk（被一起读取）或 NeedsReview |
| CDC-009 | 已同步或已限定的路径上，逻辑混入了其他异步时钟的信号 | Warning / PotentialRisk |

## 4. 编译与测试结果

| 环境 | 构建 | 测试 |
|---|---|---|
| Linux x64，GCC 13.3，Release，`-Werror` | ✅ 零警告 | ✅ 207/207 |
| Linux x64，GCC 13.3，Debug，`-Werror` | ✅ 零警告 | ✅ 207/207 |
| Windows x64 + VS2022 | ⚠️ 仍未验证 | — |

207 个 CTest 测试：197 个 Catch2 用例（本阶段新增 37 个）、7 个 CLI 端到端测试、3 个 GUI 无头测试。

GUI 自测新增两项检查：每条路径的详情都能渲染，结构、重汇聚、分组等区块与结果一致；同步器类型过滤的结果与分析结果一致。

### 4.1 示例结果

| 示例 | 路径 | 命中 | 说明 |
|---|---|---|---|
| `cdc_advanced` | 16 | 004×2、005×2、006×1、007×1、008×1、009×2 | 11 个同步结构（异步 FIFO、握手、2 个复位同步器、2 个 toggle 等），全部与 README 一致；退出码 3 |
| `cdc_rules` | 12 | 001×2、002×2、003×1、004×2、005×6、010×1 | 新增的 CDC-005 来自 XOR 观测寄存器 `dout`（README 第 11 项） |
| `smoke` | 7 | 006×1、008×4 | `status[3:0]` 拆分同步（只经端口输出，判为需复核）；`rst_b_n` 直接复位 11 个寄存器 |
| `async_fifo`（Cummings） | 4 | 006×2 | 两个 Gray 指针都为"必要条件已验证"（`wbin + (winc & ~wfull)` 的 ±1 已证明） |

### 4.2 开源工程验证

**verilog-axis**（31 个模块，无约束）
- 29 个单时钟模块：0 条路径。
- `axis_async_fifo`：识别为 `async-fifo` 结构。两个 13 位 Gray 指针都为"必要条件已验证"，3 个 toggle 状态同步器识别正确。
- 外部复位 `s_rst`/`m_rst` 报 CDC-006（潜在风险）。在 verilog-axis 的约定中这两个复位与各自的时钟同步，用 `set_input_delay -clock` 声明后即消失。

**verilog-ethernet**（98 个 RTL 文件 + lib/axis，新增）。以 7 个多时钟顶层逐一分析，并对 `eth_mac_1g_fifo` 的 27 条路径逐条对照源码：
- 帧模式异步 FIFO：提交指针的传递识别为 `handshake`（`wr_ptr_update` toggle 请求 + `ack_sync` 应答）；读指针为已验证的 Gray；写指针为 Gray 但"协议条件未验证"。后者是正确的：丢帧时写指针会回退到提交指针，Gray 值可能跳变多位，源码在帧模式下也正是改用握手传递的提交指针。
- 错误状态的 2 位 toggle 向量（`rx_sync_reg_1 <= rx_sync_reg_1 ^ {...}`）识别为 `toggle`。
- 重汇聚 8 处（同一 FIFO 内的指针与 update/ack 信号在 `wr_ptr_update_valid_reg`、`m_depth_reg` 汇合），属实，作为需复核报告。

这次验证发现并修复了 5 个问题：
1. 首级被目标域复位信号清零的 MUX 被报为 CDC-004。改为按毛刺判定，见 3.4。
2. Gray 寄存器在多个分支以不同二进制值赋值时识别失败。新增按 MUX 递归识别，并且多来源时不声称 ±1。
3. 内层 MUX 两臂都是"保持"时，递归误判为失败。改为三态结果。
4. 多比特 toggle 向量被报为 CDC-008。
5. 握手把同方向的 13 位读指针当成了应答。现在要求应答是单比特，并优先选择"同步后的请求"。

另外，`ssio_sdr_in_diff.v` 引用了未声明的参数 `IODDR_STYLE`，这是上游源码的错误，OpenCDC 正确报错；该文件与所测顶层无关，已排除。

### 4.3 性能

| 设计 | Release | 峰值内存 |
|---|---|---|
| 2 万个实例、6 万个寄存器（192 万位），单时钟 | 1.61 s | 195 MB |
| 1 万条跨域路径 | 0.23 s | 62 MB |
| 4000 个同步器汇入 4000 位全相关加法器（重汇聚压力测试） | 1.72 s | 96 MB |
| 2000 个同步器两两异或汇聚 | 0.05 s | 24 MB |
| verilog-ethernet `eth_mac_1g_fifo` | 0.65 s | — |

压力测试暴露并修复了三个问题：
1. 阶段 4 起就存在的问题：`findCrossings` 对每个目标位的每个源做线性聚合查找，宽全相关单元会达到 O(位×源×源)。改为先按源节点缓存跨域判断，只有跨域的源才聚合，并改用哈希索引。耗时从 29 s 降到 1.7 s。
2. `consumersOf` 每个读者位一条记录，4000×4000 达到 1600 万条。改为合并连续且读取方式相同的位，内存从 537 MB 降到 96 MB。
3. CDC-005 每位最多保留 64 个标签，压力测试中只有 64 条路径被报告，属于漏报。改为驻留集合后不再设上限，4000 条全部报告；伙伴计数改为按时钟分类计数。

`eth_mac_phy_10g_fifo` 需要 17 s，其中绝大部分时间花在 slang 对 10G PHY 中 CRC/扰码器常量函数的求值（前端展开），CDC 分析本身不到 1 s。

## 5. 已知限制

1. Windows/MSVC 仍未实际编译。
2. 重汇聚只在目标域内前向搜索 K 级寄存器（默认 2），经过输出端口离开设计的汇聚找不到。读者位按源位区间（hull）聚合，可能多计。
3. CDC-004 统计异步位时按位区间计数，偏保守。同步链两级之间的逻辑会把链截断，按单级同步报告（CDC-002）。
4. Gray 的 ±1 证明只覆盖"加/减一位增量"和"增量/保持 MUX"。其他写法（例如查找表）判为"协议条件未验证"；目标端是否把 Gray 转回二进制不检查。
5. 握手只识别"请求同步 + 应答同步回源"的结构，不检查发送端是否在应答前保持数据（列为未验证条件）。
6. 帧模式 FIFO 的存储器读受提交指针握手保护，目前识别为"受限定数据"，没有归入 `async-fifo` 结构。
7. CDC-007 的判据是启发式的：无保持路径且由逻辑计算的寄存器视为可能为脉冲；只复制输入端口的寄存器不判断。
8. 外部复位若已在设计外同步，需要用 `set_input_delay -clock` 声明，否则报 CDC-006（潜在风险）。
9. 用 posedge 和 negedge 两个 always 块驱动同一寄存器的 DDR 模型（如 verilog-ethernet 的 `oddr` GENERIC 分支）仍按阶段 2 的规则报 `IR-MULTIDRIVER` 错误，与综合工具一致。
10. 豁免（waiver）、HTML/CSV 报告在阶段 7 实现；CDC 原理图在阶段 6 实现。

## 6. 下一阶段（阶段 6：完整 GUI）

按总提示词与架构文档第 12 节：
1. **CDC 原理图视图**：用自研分层布局在 `QGraphicsView` 中画出选中路径，包括源寄存器、组合逻辑、同步器各级和目标寄存器，按时钟域着色。同步结构（FIFO、握手）整组显示，重汇聚点高亮。
2. **视图联动**：层次树、路径表、违规表、原理图和源码之间双向选择与高亮。
3. **层次视图增强**：按实例统计跨域路径和违规数。
4. GUI 的无头测试与截图验证。

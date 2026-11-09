# 阶段 87：文件列表（.f）、taxi KC705 以太网示例与 CDC 分析的运行时间

日期：2026-11-09　版本：0.87.0

阶段 86 的下一步有两项：
- 继续验证多时钟域的真实设计，优先选择带以太网 MAC 的 SoC；
- 检查 CDC 规则在大型设计上的运行时间。

这一阶段选择 **fpganinja/taxi**（提交 cc70b27，MIT）的 KC705 以太网示例 `fpga_rgmii_1g`。设计包括：
- 1G RGMII MAC，带异步 FIFO；
- SFP 口的 1000BASE-X MAC；
- 统计模块；
- UART 上的 XFCP 调试总线；
- 时钟：板上 200 MHz、MMCM 的 125 MHz 与 125 MHz/90°、RGMII 接收时钟，以及 PCS/PMA IP（黑盒）给出的 SFP 时钟。

工程按作者的 `Makefile` 与 `config.tcl` 生成 OpenCDC 工程，约束是作者自己的 XDC 和各模块的 Tcl 约束脚本。

第一次分析：
- 不能运行。`Makefile` 中的源文件大多是 `.f` 文件列表，OpenCDC 把它们当作源文件读取。
- 支持文件列表后，有 4 个 `$size(实例数组)` 错误（slang 不把实例数组当作值）。
- 修复后能分析，但：
  - CDC 路径分析用了 29.3 s，而设计只有 1252 个寄存器；
  - XFCP 模块的 ID ROM 初始化中有 5 个 `IR-UNSUPPORTED-LOOP`。

修复后：
- 0 个错误，6 个告警（4 个 `FE-SIZE-INSTANCE-ARRAY`，2 个黑盒 IP）；
- 6 个时钟，1252/1252 个寄存器；
- 76 条跨时钟路径全部已同步，35 个同步结构；
- CDC 路径分析 0.55 s。

## 1. 完成的功能

### 1.1 Verilog 文件列表（新文件 `src/engine/FileList.{h,cpp}`，`ProjectConfig.cpp`）

- 工程的源文件以 `.f` 结尾时，按文件列表读取。条目相对于列表所在目录：
  - 源文件；
  - 嵌套的列表（`other.f`、`-f other.f`、`-F other.f`）；
  - `+incdir+dir`（可用 `+` 分隔多个）；
  - `+define+NAME=value`。
- 注释（`//`、`#`，到行尾）与空行跳过。其他选项（`-v`、`-y`、`+libext+` 等）不读，在提示中列出。
- 去重：多个列表都列出的文件，或既在列表中又被直接列出的文件，只在第一次出现的位置读一次（按规范路径比较）。
  - taxi 的 `taxi_sync_reset.sv` 同时出现在 `Makefile` 和 `taxi_eth_mac_1g_rgmii_fifo.f` 中。
- 列表互相引用（环）时每个列表只读一次；嵌套深度最多 32 层。
- `PRJ-FILELIST`：
  - 提示：从几个列表读了多少个源文件；未读的选项列在其中（最多 5 条）；
  - 错误：列表无法读取（包括嵌套的列表）。
- 没有使用列表的工程不受影响（例如同一文件列出两次时仍然保留两次）。

### 1.2 实例数组的 `$size`（`SlangFrontend.cpp`）

- 写法：`.PORTS($size(xfcp_sw_us))`，其中 `taxi_axis_if xfcp_sw_us[2]()` 是接口实例数组。
- (System)Verilog 不把实例数组当作值，slang 报错；Vivado 接受并求值。
- 处理：取 slang 的 `NotAValue` 诊断，把调用替换为声明中写的大小：
  - `[N]` → `(N)`；
  - `[a:b]` → 元素个数，`?:` 中的冒号不当作范围分隔符。
- 每处给一条 `FE-SIZE-INSTANCE-ARRAY` 告警。
- 只在调用形如 `$size(名字)`、声明是 `名字 [维度]` 时替换。单个实例的 `$size` 仍然是错误。
- 这类兼容替换以前在告警下附带一行空的 “note:”，现在没有注释时不再输出。

### 1.3 超过 64 位的常量折叠（`ConstFold.cpp`）

- 以前：常量折叠只处理不超过 64 位的值。
- XFCP 模块的 ID ROM 在 `initial` 中按字符串参数求长度：

  ```systemverilog
  for (i = 1; i <= 16; i++) if (j == i-1 && (XFCP_ID_STR >> (i*8)) > 0) j = i;
  for (i = 0; i <= j; i++) ...
  ```

  `"Statistics"` 是 80 位，`>>` 与 `>` 不折叠，`j` 不是常量，第二个循环无法展开（`IR-UNSUPPORTED-LOOP`、`IR-INITIAL-IGNORED`）。
- 现在：输入或输出超过 64 位、且操作数是完全确定的常量时，按位向量计算：
  - 按位运算；
  - 加、减、取负；
  - 移位（含有符号算术右移）；
  - 比较（有符号时最高位为符号）；
  - 归约；
  - 逻辑运算。
- 乘、除、取模、幂仍不折叠。含 X/Z 的值也不折叠。
- 顺带修正：输出超过 64 位时，窄常量的 `~` 以前只置低 64 位。

### 1.4 CDC 分析的运行时间

| 设计 | 修改前 | 修改后 | 主要原因 |
|---|---|---|---|
| taxi KC705 | 29.3 s | 0.55 s | 收敛分析逐位展开 163840 位的 FIFO 存储器 |
| SNES_MiSTer | 7.2 s | 3.0 s | 同一锥（cone）的位重复遍历 |
| ao486_MiSTer | 10.6 s | 约 9 s | 同上；剩余时间在收敛分析的标签传播 |
| corundum ZCU106 | 约 14 s | 约 14.5 s | 未修改（时序图构建） |

1. **宽存储器不逐位求数据候选（`SeqGraph::dataAlternatives`）。**
   - 原因：收敛分析在读取存储器（`DynSelect`）时，对存储器的每一位求一次“数据候选”。每一位又经过按位的写入单元（`Mux`、`DynInsert`）。taxi 的 MAC FIFO 是 163840 位，共计约 130 万次单元求值。
   - 修改：超过 4096 位的寄存器（存储器）不逐位求，用标签的并集。这与锥分析中超过 4096 位的单元按一个整体跟踪（`kWideCell`）是同一条界线。
2. **候选族的化简（`keepMaximal`，`AdvancedAnalysis.cpp`）。**
   - 原因：去掉被其他成员包含的成员是平方复杂度，taxi 上约有 12 亿次比较。
   - 修改：
     - 先比较标签和条件的个数，以及 64 位掩码，绝大多数成员对在这一步就排除；
     - 结果按族缓存（成员是驻留的，结果不变）。
   - 包含关系是严格偏序，保留的总是极大元，与比较顺序无关，所以结果不变。
3. **同一锥的位只遍历一次（`CdcAnalyzer.cpp`）。** 读取同一个驻留锥的位（存储器的字、宽总线的位）来源相同：
   - `findCrossings`：每个锥在它的第一位处理一次，用于所有这些位；之后每条路径的目的位排序，与以前逐位处理的顺序相同；
   - `findQualifier`：跳过重复的锥。
4. **收敛分析中多时钟寄存器的循环（`analyzeConvergence`）：** 已处理过的锥和节点不再重复处理。

结果不变：
- SV 语料其余 51 项、VHDL 语料 48 项的输出与修改前逐行相同；
- taxi 的结果在修改 1、2 前后也相同（76 条路径、50 个收敛点、9 条 CDC-005）。

## 2. 真实工程验证

### taxi KC705 `fpga_rgmii_1g`（加入语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | `.f` 被当作源文件 → 支持后 4 个 `$size` 错误 | 0 |
| 未分析的构造 | 5 个循环 + 2 个黑盒实例 | 2（PCS/PMA IP 黑盒） |
| 时钟 | — | 6，1252/1252 个寄存器 |
| 跨时钟路径 | — | 76，全部已同步（异步 FIFO ×7、握手 ×3、复位同步 ×20、翻转 ×3、两级 ×2） |
| CDC 路径分析 | 29.3 s | 0.55 s |

- 语料脚本用稀疏检出（`/src/`，不含 `tb` 和 Python 文件）。
  - `lib/taxi` 是指向仓库根目录的符号链接，在稀疏检出中可用。
  - 工程 JSON 由 `Makefile` 的 `SYN_FILES`/`XDC_FILES` 和 `config.tcl` 的参数生成。
- 9 条 CDC-005（潜在风险）：FIFO 的状态寄存器（`overflow_reg`、`m_depth_reg` 等）把两个 FIFO 的同步指针或指针与更新标志组合在一起。这是结构上真实的重新汇合；是否有害取决于这些状态的用途（统计与调试），因此列为潜在风险而不是违例。

### 其他语料

- SV 语料 52 项全部匹配：新增 taxi，其余 51 项与修改前逐行相同。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- 新增 `src/engine/FileList.{h,cpp}`；`src/engine/CMakeLists.txt`；`src/engine/ProjectConfig.cpp`。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`$size` 的兼容替换，空注释。
- `src/ir/ConstFold.cpp`：宽常量折叠。
- `src/cdc/CdcAnalyzer.cpp`、`src/cdc/AdvancedAnalysis.cpp`、`src/cdc/SeqGraph.{h,cpp}`：运行时间。`FamilyOps::subsumes` 换成 `keepMaximal`（一次处理整个族）。
- 测试（每个用例都含反例）：
  - 新增 `tests/engine/test_file_list.cpp`：
    - 列表的各种条目、嵌套、环、去重、未读的选项；
    - 无法读取的列表（包括嵌套的）是错误；
    - 工程中的展开与 `PRJ-FILELIST`；
    - 反例：没有列表的工程不变。
  - `tests/frontend/test_quartus_compat.cpp`：
    - `$size` 的 `[N]`、参数表达式与 `[a:b]`，生成的寄存器宽度为 3+4+5；反例：单个实例仍然报错；
    - 宽字符串参数决定循环次数的 `initial`（3、10、16 个字符）；反例：依赖输入的循环仍然报告。
  - `tests/ir/test_ir.cpp`：超过 64 位的移位、比较（含有符号）、加减、按位、归约；反例：含 X 的值与乘法不折叠。
  - `tests/cdc/test_cdc_advanced.cpp`：经过存储器的 CDC-005，128 位与 8192 位存储器结论相同；反例：只有一个同步信号写入时没有 CDC-005。
- `README.md`：文件列表；`$size`。
- `tools/ci/sv-corpus.sh`：taxi KC705。
- `CMakeLists.txt`：版本 0.87.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：688 个测试全部通过。
- 测试预期的两处修改：
  - 文件列表的用例中，行尾的 `# 注释` 最初被当作文件名。实现改为 `#` 开头的词之后是注释，与行首的 `#` 一致。
  - 宽常量用例最初用 `toUint()` 检查宽度，但它只对能放进 64 位的值有效。改为检查宽度。
- SV 语料 52 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 86 的 Windows CI（提交 85d3aed，运行 42622852774）：Release 与 Debug 均 678/678 通过，生成 `OpenCDC-0.86.0-windows-x64.zip` 与 `.exe`。
- 本阶段的 Windows CI（提交 1460817，运行 42632557900）：Release 与 Debug 均 688/688 通过，生成 `OpenCDC-0.87.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- 文件列表不支持 `-v`/`-y` 库搜索和环境变量（`$VAR`）。
- 超过 4096 位的存储器在收敛分析中使用标签并集。理论上可能比逐位求候选多报告收敛点；本阶段的语料和测试中没有出现差异。
- ao486 的 CDC 分析仍约 9 s（收敛分析的标签传播），corundum 约 14.5 s（时序图构建）；corundum 的前端解析约 32 s。
- 乘、除、取模、幂的宽常量不折叠。

## 6. 下一步

阶段 88：
- 继续多时钟域的真实设计，例如 taxi 的其他板（UltraScale+ 的 10G/25G MAC、PTP 时钟）或带视频采集的设计；
- 分析 corundum 的前端时间。

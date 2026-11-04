# 阶段 52：AXI Interconnect；IP 容器 .xcix；多维 unpacked 数组；输出端口连接常量

日期：2026-11-05　版本：0.52.0

真实工程是 Loongson chiplab（木兰宽松许可证第 2 版）在 loongson 板上的 SoC，CPU 为 open-la500（木兰宽松许可证第 2 版，gitee 子模块，使用 GitHub 上 loongson-community 的镜像，固定为子模块记录的提交）。SoC 的结构如下：

- **时钟**：CPU 在 50 MHz（`clk_out1_clk_pll_33`）；uncore 在 33 MHz，挂有 confreg、UART、SPI、DMA、以太网 MAC。
- **CPU 与 uncore 之间**：经 AXI Clock Converter（`.xcix` 容器）连接。
- **DDR3**：经 3 口 AXI Interconnect 连到 MIG 的 AXI 接口，在 `ui_clk` 中。
- **约束**：工程自带的 `soc_up.xdc` 使用 Vivado 的时钟名 `clk_pll_i`、`clk_out2_clk_pll_33`。

## 1. 完成的功能

### 1.1 `axi_interconnect`（AXI Interconnect 1.x，PG059）

- **时钟域**：每个从端口 `Sxx_AXI_*` 和主端口 `M00_AXI_*` 各在自己的时钟（`Sxx_AXI_ACLK`、`M00_AXI_ACLK`）。
- **复位**：复位输出 `Sxx_AXI_ARESET_OUT_N` 在本端口时钟；`INTERCONNECT_ARESETN` 在 `INTERCONNECT_ACLK`。
- **跨域**：IP 在内部完成。摘要列出配置为异步的端口（`C_Sxx_AXI_IS_ACLK_ASYNC`）。
- **端口来源**：来自实例（JSON `.xci`、组件文件或 VHDL 组件声明）。XML `.xci` 没有端口时明确报告，不推测端口。
- chiplab 中有两个实例：3 口的 `axi_interconnect_0`（DDR）和 2 口的 `axi_2x1_mux`。

### 1.2 IP 容器 `.xcix`

`.xcix` 是 Vivado 的 core container：IP 目录打包成的 ZIP，内含 `.xci`、组件文件和输出产物。

- 新增 `core/Zip`：不依赖 zlib，自己实现 DEFLATE 解压（RFC 1951：stored、固定和动态 Huffman）和 ZIP 中央目录的读取。不支持加密和 ZIP64，遇到时明确报错。
- `.xcix` 被解到临时目录后，再按 `.xci` 建模，模型的说明中写出容器名。
- 防止 zip slip：绝对路径和含 `..` 的条目不写出。
- chiplab 中有 3 个 `.xcix`：AXI Clock Converter，以及 CPU cache 的两个块 RAM。

### 1.3 多维 unpacked 数组

- **问题**：open-la500 的 cache 中有 `wire [7:0] way_bank_addra [1:0][3:0]` 这样的声明，在嵌套的 generate 循环中按 `[i][j]` 赋值。以前报 `IR-UNSUPPORTED-TYPE`，这个工程因此有 284 个不支持的结构。
- **修正**：展平为外层数组，每个元素的宽度是整个内层数组的宽度。`a[i][j]` 先选外层，再在内层中选，常量下标和动态下标都支持。
- 不支持的结构从 284 个降到 0 个。

### 1.4 输出端口连接常量

- **问题**：chiplab 的 `soc_top.v` 中有 `.M00_AXI_ARQOS (4'b0)`，这是 Interconnect 的一个输出。在 SystemVerilog 中这是错误（expression is not assignable），整个设计因此无法分析；Vivado 只是忽略这个连接。
- **修正**：
  - 只有当诊断位置的前面是 `.名字(` 时，即端口连接，才报警告 `FE-OUTPUT-PORT-CONSTANT`，该输出不连接；
  - 其他位置的同类错误仍然是错误。

## 2. 真实工程验证（Loongson chiplab `soc_top`）

| 时钟（已解析寄存器） | 结果 |
|---|---|
| 6（1293/1293） | 0 错误，1 个警告（输出端口连接常量），132 条路径，72 条已同步；36 个错误违例、24 个警告违例，11 条潜在风险 |

- **时钟与工程约束一致**：
  - `clk_out1_clk_pll_33` 为 20 ns，`clk_out2_clk_pll_33` 为 30.303 ns（33 MHz），MIG 的 `ui_clk` 为 10 ns（别名 `clk_pll_i`）；
  - 工程 xdc 中的 `set_false_path`（以太网 MII 时钟与 uncore 时钟之间）都匹配上了。
- **CPU 与 uncore 之间、uncore 与 DDR 之间没有跨时钟路径**：它们只经过 AXI Clock Converter 和 AXI Interconnect，跨域在 IP 内部完成。
- **60 条未同步路径都在以太网 MAC 中**（uncore 33 MHz 与 `mrxclk_0`/`mtxclk_0` 之间）：
  - MAC 的跨域握手只用一级寄存器，例如 `rc.v` 中的 `stop_r <= stopi;`、`focl_r <= focl;`，之后直接参与逻辑（报 CDC-002，只有 1 级同步）；
  - 还有直接组合使用的信号（CDC-001），例如 `csr6_fd` 到 `U_BD` 的 `crs_r`/`icoll`。
  - 这是这个 MAC 的设计方式，按两级同步的规则是真实的违例。
- **对比**：本阶段修正之前，这个工程无法分析（`.xcix` 不能读取、输出端口连接常量报错、284 个不支持的结构）。
- 加入 SystemVerilog 语料库：
  - 源文件列表直接取自工程的 `system_run.xpr`，另加 CPU 源文件和它的 `.xcix`；
  - 约束使用工程自带的 `soc_up.xdc`。

## 3. 修改的文件

- `src/core/Zip.{h,cpp}`（新增）：DEFLATE 解压和 ZIP 读取。
- `src/frontend/XciModels.cpp`：`.xcix` 解包、`axiInterconnect`。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：输出端口连接常量。
- `src/frontend/slang_adapter/IrBuilder.cpp`：多维 unpacked 数组；连接常量的输出不连接。
- 测试：
  - `tests/core/test_zip.cpp`（新增）：
    - stored、固定和动态 Huffman 三种块（用 Python zlib 生成的数据）；ZIP 中的 stored 和 deflate 条目，跳过目录条目；
    - 负例：截断的流、保留的块类型、超过上限、不是 ZIP、截断的 ZIP。
  - `tests/frontend/test_xci.cpp`：
    - `.xcix` 的建模，以及它在设计中的分析结果与 `.xci` 相同；负例：不是 ZIP、没有 `.xci`、zip slip 条目没有写出。
    - AXI Interconnect 各端口的时钟域；负例：端口不混在别的接口的时钟中；没有端口时不建模。
  - `tests/ir_extract/test_ir_extract.cpp`：二维数组的 generate 赋值、动态读写，偏移逐项核对；负例：`real` 元素的数组仍报告。
  - `tests/frontend/test_slang_frontend.cpp`：输出连接常量时报警告，设计照常展开；负例：连接 net 时没有警告。
- `tools/ci/sv-corpus.sh`：chiplab 和 open-la500 的下载，以及 `chiplab_soc` 工程。
- 文档：`docs/vivado-ip.md`、`docs/architecture.md`、`README.md`。
- `CMakeLists.txt`：0.52.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- XML 格式的 AXI Interconnect 没有组件文件时不建模，因为它的端口（各端口的 ID 宽度等）不容易从参数中准确推出。
- ZIP 不支持加密和 ZIP64。`.xcix` 不会用到这两种格式。
- 输出端口连接常量的识别依赖源代码文本（诊断位置之前是 `.名字(`）。

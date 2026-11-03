# 阶段 49：IEEE 802.3 Clause 74 FEC IP 模型；SEM 暂缓

日期：2026-11-03　版本：0.49.0

## 1. 完成的功能

### 1.1 `ieee802d3_clause74_fec`（IEEE 802.3 Clause 74 FEC，PG303）

端口来自实例。SURF 的 `Pgp4GtyUsIpFec` 是 JSON `.xci`，端口直接取自其中的 boundary。

- **`tx_clk` 时钟域（编码器）**：`tx_reset`、`tx_din`、`tx_din_start`、`tx_dout`、`tx_dout_start`。
- **`rx_clk` 时钟域（解码器）**：`rx_reset`、`rx_din`、`rx_din_slip`、`rx_dout`、`rx_dout_start`、`ctrl_rx_*`、`stat_rx_*`。
- **复位**：PG303 的端口表把 `tx_reset` 列在 `tx_clk` 域、`rx_reset` 列在 `rx_clk` 域，所以复位按同步于各自时钟的数据处理。
- **IP 内部**：编码和解码两条通路相互独立，内部没有跨时钟路径。
- **不认识的端口**：明确报 `FE-XCI-UNSUPPORTED` 并列出端口名。

### 1.2 SEM（Soft Error Mitigation）暂不建模

SURF 中剩下的另一个 IP 是 `sem`（`SemCore`）。它的模型依赖两个配置原语，目前都没有模型：

- **`FRAME_ECCE2`**：输出（`SYNDROMEVALID`、`FAR` 等）没有时钟端口。UG953 的原语说明也没有写它们同步于哪个时钟，只能推测是 ICAP 的回读时钟。
- **`ICAPE2`**：也需要新的原语模型。

按"不伪造结果"的原则，在确认 `FRAME_ECCE2` 输出的时钟关系之前，不建模 SEM。

## 2. 真实工程验证（SURF `Pgp3GtyUsIpWrapper`，25 Gb/s）

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| `EN_FEC_G = true`（带 Clause 74 FEC） | 6（216/216） | 0 错误 0 警告，32 条路径，22 条已同步，0 个违例 |
| `EN_FEC_G = false`（对照） | 6（202/202） | 同样的 32 条路径，0 个违例 |

- **FEC 的位置**：SURF 把 FEC 的 `tx_clk` 接到 `txUsrClk2`、`rx_clk` 接到 `rxUsrClk2`，FEC 位于 gearbox 和收发器之间。
- **对照结果**：两种配置的跨时钟路径数相同，说明 FEC 和两侧逻辑在同一时钟域，没有引入新的跨时钟路径。
- **10 条未同步路径**：与以前的 PGPv3 GTH 封装相同，9 条潜在风险和 1 条需复查，都在 AXI-Lite 和 DRP 的异步桥中。
- 带 FEC 的工程加入 VHDL 语料库。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`clause74Fec` 模型和分派。
- `tests/frontend/test_xci.cpp`：
  - 用 JSON `.xci` 测试编码器和解码器的时钟域；
  - 负例：TX 端口不在 RX 时钟中，反之亦然；没有异步源；不认识的端口被报告。
- `tools/ci/vhdl-corpus.sh`：SURF PGPv4 GTY 25G FEC 工程。
- 文档：`docs/vivado-ip.md`、`README.md`。
- `CMakeLists.txt`：0.49.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- SEM 不建模（见 1.2）。
- 至此，SURF 中所有 `.xci` 涉及的 IP 类型都已建模，只剩 SEM。

## 6. 补充：Mac 构建中语料库下载失败

- **现象**：Mac 上第一次构建 0.49.0 时，编译、508 个测试、安装包和 SV 语料库都通过，但 VHDL 语料库在下载 SURF 时出错：`无法访问 'https://github.com/slaclab/surf/'：SSL connection timeout`（退出码 128），所以 0 项运行。
- **原因**：语料库脚本每次运行都向 GitHub 执行 `git fetch`，即使本地已经是固定的提交，一次网络超时就会让整个语料库失败。代码本身没有问题。
- **修正**（`tools/ci/vhdl-corpus.sh`、`tools/ci/sv-corpus.sh`）：
  - 本地已检出固定的提交且工作区没有修改时，直接使用，不访问网络。在 Mac 上检查过，9 个 VHDL 语料库都已在固定提交且没有修改。
  - 需要下载时，失败后最多重试 3 次（间隔 10、20、30 秒），仍失败则给出 `::error::cannot fetch ...` 并退出。
- **验证**：本地测试了两种情况：已在固定提交时不访问网络（0.02 s）；地址无效时重试 3 次后报错。

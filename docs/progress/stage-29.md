# 阶段 29：XADC Wizard 与更多 Digilent 板卡演示

日期：2026-10-27　版本：0.29.0

## 1. 完成的功能

### 1.1 XADC Wizard（xadc_wiz）的 .xci 模型

在 Digilent 的 XADC 演示工程中，XADC 是以 `xadc_wiz` 的 `.xci` 给出的。以前报 FE-XCI-UNSUPPORTED，设计因未知模块无法分析。

现在按配置生成模型（DRP 接口）：
- 动态重配置口（`di_in`、`daddr_in`、`den_in`、`dwe_in`、`do_out`、`drdy_out`）和状态输出（`channel_out`、`eoc_out`、`eos_out`、`busy_out`、`alarm_out` 以及所选的各个告警）都是 `dclk_in` 的寄存器，与 XADC 原语模型一致。
- 模拟输入（`vp_in`/`vn_in`、所选的 `vauxp<n>`/`vauxn<n>`）不读取。
- JTAG 状态输出没有时钟域。
- 端口由参数推导：`C_HAS_DRP`、`C_HAS_DCLK`、`C_HAS_RESET`、`C_HAS_CONVST`、`C_VAUX<n>`、各告警的 `C_HAS_*_ALARM`、`C_HAS_CHANNEL/EOC/EOS/BUSY/TEMP_BUS` 等。
- `alarm_out`（告警的或）总是存在：Basys 3 的演示在没有选任何告警时也连接了它。
- AXI4-Lite 接口：单时钟 `s_axi_aclk`，端口取自 JSON `.xci` 或组件文件。
- AXI4-Stream 输出不建模。

## 2. 真实工程验证

新增 3 个 Digilent 演示工程，加入 SV 语料回归，均固定到具体提交：

| 工程 | 结果 |
|---|---|
| Arty A7-100 XADC | 以前：FE-XCI-UNSUPPORTED，未知模块错误。现在：1 个时钟，14/14 个寄存器，0 个警告，0 个跨域 |
| Basys 3 XADC | 以前同上。现在：2 个时钟（含一个分频时钟），24/24，0 个警告，0 个跨域 |
| Zybo Z7-10 XADC | 以前同上。现在：1 个时钟，19/19，0 个跨域；1 个警告是设计没有连接 `vp_in`/`vn_in` 等端口 |

另外试了 Arty A7-100 GPIO（VHDL）和 Cmod S7-25 OOB（Verilog），均无错误，0 个跨域。Cmod S7 有一个 `'\r'` 转义序列的警告，确实不是 Verilog 标准中的转义，Vivado 接受。Nexys A7-100T XADC 的仓库里缺少 `xadc_wiz_0.xci`，不能分析。

## 3. 修改的文件

- `src/frontend/XciModels.cpp`：`xadcWizard`，分发到 `xadc_wiz`。
- `src/frontend/MixedSupport.h`：未知模块提示中的 IP 列表。
- `tests/frontend/test_xci.cpp`：
  - 端口推导：所选与未选的通道和告警；模拟输入不读取；
  - AXI4-Stream 不建模，AXI4-Lite 没有端口表时不建模；
  - 端到端：同时钟读取 DRP 数据不跨域（负例），另一时钟读取跨域（正例）。
- 语料库：`tools/ci/sv-corpus.sh`（3 个 XADC 演示）。
- 文档：`docs/vivado-ip.md`、`README.md`。

## 4. 已知限制

- `reset_in`、`convst_in` 按 `dclk_in` 下采样的输入处理；XADC 本身对它们是异步的。从另一个时钟域驱动时会被报告。
- AXI4-Stream 接口不建模。

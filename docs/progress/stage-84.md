# 阶段 84：Microchip PolarFire 与 Cologne Chip GateMate —— SERV 的最后两块板

日期：2026-11-09　版本：0.84.0

阶段 83 之后，SERV（`servant.core`）的 40 个目标中只有两个综合目标还不能分析：
- `polarfire_splashkit`（Microchip PolarFire）：`PLL`、`CLKINT` 没有模型，`VCC`/`GND` 的端口是 `Y`（OpenCDC 的 `VCC`/`GND` 是 Xilinx 的，端口为 `P`/`G`）；
- `gmm7550`（Cologne Chip GateMate）：`CC_PLL`、`CC_USR_RSTN` 没有模型。

这一阶段为它们建模。两个目标现在都是 0 错误。GateMate 目标有一条复位路径：用户复位 `CC_USR_RSTN` 来自配置逻辑，相对 PLL 输出时钟异步释放，报告为 CDC-006（潜在风险），这是设计中真实存在的结构。

## 1. 完成的功能

### 1.1 Microchip 模型（新文件 `src/frontend/primitives/microchip_cologne.v`）

| 原语 | 模型 |
|---|---|
| `PLL`（PolarFire，Libero 为 `PF_CCC` 写出的 PLL） | `OUT0`..`OUT3` 由 `REF_CLK_0` 派生：f(VCO) = f(输入) × (`FB_INT_VAL` + `FB_FRAC_VAL` / 2^24) / `RFDIV`，f(`OUTn`) = f(VCO) / (4 × `DIVn_VAL`)；`LOCK` 没有时钟域；DRI 端口在 `DRI_CLK` 时钟域 |
| `CLKINT`、`CLKINT_PRESERVE`、`CLKBUF` | 全局时钟缓冲（连线） |
| `VCC`、`GND` | Xilinx 的单元，另加 Microchip 的输出 `Y` |

- 参数表取自 Libero 生成的 PLL 实例（SERV 的 `servant_pf_clock_gen.v` 写出了全部参数）。
- 以 SERV 的设置核对公式：50 MHz × 2496 / 25 = 4992 MHz，4992 / (4 × 39) = 32 MHz，与文件中的注释一致。
- 参考时钟选择、旁路和相位不建模。

### 1.2 Cologne Chip 模型

| 原语 | 模型 |
|---|---|
| `CC_PLL` | `CLK0`、`CLK90`、`CLK180`、`CLK270` 由 `CLK_REF` 派生，周期比为 `REF_CLK` / `OUT_CLK`（MHz 字符串）；`CLK_REF_OUT` 是参考时钟；锁定输出没有时钟域 |
| `CC_USR_RSTN` | 配置逻辑给出的用户复位：没有时钟域 |
| `CC_BUFG` | 全局缓冲（连线） |

`docs/primitives.md` 增加 “Microchip and Cologne Chip” 一节，“Not modelled” 一节相应修改（并删去早已建模的 iCE40 `SB_IO`）。

## 2. 真实工程验证

### SERV

| 目标 | 结果 |
|---|---|
| `polarfire_splashkit` | 0 错误，6 条 `FE-PORT-NET-DEFAULT`（阶段 83）；时钟 `pll_inst_0.OUT0` 由 `i_clk` 派生（输入未约束，周期未知，比例已知）；66 个寄存器中 65 个有时钟，剩下的 1 个是 DRI 端口的寄存器，`DRI_CLK` 接地，属于未使用的接口 |
| `gmm7550` | 0 错误；`pll_inst.CLK0` 为 32 MHz（31.25 ns，来自 `REF_CLK`/`OUT_CLK`）；1 条跨时钟路径：用户复位在 `CLK0` 时钟域异步释放（CDC-006，潜在风险） |

至此 SERV 的 38 个综合目标全部 0 错误（另外两个是仿真目标）。两个目标加入语料库。

### 其他语料

- SV 语料 48 项全部匹配：原有 46 项与修改前逐行相同，新增 SERV 的两个目标。
- VHDL 语料 48 个全部匹配，与修改前相同。

## 3. 修改的文件

- 新增 `src/frontend/primitives/microchip_cologne.v`；`src/frontend/CMakeLists.txt`（嵌入的模型文件）。
- `src/frontend/primitives/xilinx_unisim.v`：`VCC`、`GND` 增加输出 `Y`。
- 测试（含反例）：`tests/frontend/test_primitives.cpp`：1 个用例（PolarFire PLL 的两个输出周期、`CLKINT`、`VCC`/`GND` 的 `Y`、`CC_PLL` 跟随实际输入时钟并报告 `CON-INPUT-PERIOD`、同一 PLL 的输出之间没有跨时钟路径、两个 PLL 之间有、用户复位的 CDC-006）。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：SERV 的 `polarfire_splashkit`、`gmm7550`。
- `CMakeLists.txt`：版本 0.84.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：672 个测试全部通过。
- 测试预期的一处修改：`CC_PLL` 的两个输出（`CLK0`、`CLK90`）都被使用，`CON-INPUT-PERIOD` 每个输出报告一次，共 2 条，不是我最初写的 1 条。
- SV 语料 48 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 83 的 Windows CI（提交 60b3f7a，运行 42591420469）：Release 与 Debug 均 671/671 通过，生成 `OpenCDC-0.83.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- PolarFire 的 `CCC` 宏、收发器和 I/O 单元没有模型；PLL 的参考时钟选择（`REF_CLK_1`）不建模，输出总是由 `REF_CLK_0` 派生。
- GateMate 的 `CC_PLL` 只用 `REF_CLK`/`OUT_CLK` 两个字符串参数；`CLK270_DOUB`、`CLK180_DOUB`（倍频输出）不建模。

## 6. 下一步

阶段 85：一个多时钟域的真实设计（SERV 和 ibex 演示系统的跨时钟路径都很少）。候选：带以太网 MAC、SDRAM/DDR 控制器或视频输出的开源 SoC，优先选择不需要运行生成器的工程。

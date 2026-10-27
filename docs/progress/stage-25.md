# 阶段 25：存储器接口（MIG）模型

日期：2026-10-27　版本：0.25.0

## 1. 完成的功能

### 1.1 mig_7series 的 .xci 模型

7 系列存储器接口（一个 DDR2 或 DDR3 控制器，原生接口或 AXI4 接口）现在从 `.xci` 生成 CDC 模型，不再是黑盒。

- **ui_clk**：由系统时钟（`sys_clk_i`，差分时为 `sys_clk_p`/`sys_clk_n` 经 IBUFDS）派生，与系统时钟是同步关系，和 Vivado 一致。Vivado 给它的名字 `clk_pll_i` 记为别名，约束可以直接用。
- **周期**：
  - 有存储器配置文件（`.xci` 的 `XML_INPUT_FILE`，通常是同目录下的 `mig_a.prj`）时，按"存储器时钟周期 × PHY 比例"相对配置的输入时钟给出比例。实际输入时钟不是配置值时报 CON-INPUT-PERIOD，周期跟随实际输入，与时钟向导相同。
  - 没有该文件时，用 `.xci` 中配置的用户接口频率 `FREQ_HZ` 作为固定周期，仍与系统时钟相关；FE-XCI-MODEL 附注说明没找到配置文件。
- **附加时钟** `ui_addn_clk_<n>`：频率取自 `MMCM_CLKOUT<n>_FREQ`，处理方式同上。
- **用户接口**：`app_*`（或 `s_axi_*` 与 `aresetn`）全部属于 ui_clk，输入在 ui_clk 下采样，输出是 ui_clk 寄存器。
- **复位**：`sys_rst`（按配置的极性）经复位同步器进入 ui_clk。同步器的层级和名字与 IP 相同：`u_<ip>_mig/u_<mem>_infrastructure/rstdiv0_sync_r`，所以引用它的约束能匹配上。`ui_clk_sync_rst` 和 `mmcm_locked` 都取自它。
- 端口按配置推导：存储器类型、各位宽、片选/数据屏蔽/ODT 是否存在、系统时钟和参考时钟的类型、温度监测方式（外部为 `device_temp_i` 输入，内部为 `device_temp` 输出）。JSON 格式直接使用 `.xci` 中的端口表。
- 不建模：其他存储器类型（QDR II+、RLDRAM、LPDDR2）、ECC、多个控制器，这些报 FE-XCI-UNSUPPORTED。

### 1.2 时钟分析：IP 给定的周期与名字

派生时钟寄存器上新增两个属性（供 IP 模型使用）：
- `OPENCDC_CLOCK_PERIOD`：固定周期，不随输入时钟变化，但仍保留派生关系；
- `OPENCDC_CLOCK_ALIAS`：额外的时钟别名。

## 2. 真实工程验证：Nexys A7 OOB

此前 MIG 是黑盒：`ui_clk` 是来源不明、周期未知的独立时钟。PDM 数据从 `ui_clk` 进入 108 MHz 显示时钟域的路径被报为 CDC-007/010。

现在：
- `ui_clk` 为 75.01 MHz，由时钟向导的 197.9 MHz 输出派生，与工程其他时钟同属一个来源。Vivado 也按相关时钟对这些路径做时序分析。
- 4 个时钟，383/383 个寄存器解析；0 个警告（此前 2 个，其中一个是 MIG 不支持）。
- 跨域路径：4 条（2 条已同步，2 条潜在风险），都是 MMCM `LOCKED` 和外部复位按键直接作为异步复位（CDC-006），这是设计本身的问题。
- 约束 `set_false_path ... -to [get_pins Inst_Audio/DDR/Inst_DDR/u_ddr_mig/u_ddr2_infrastructure/rstdiv0_sync_r*/PRE]` 匹配到模型中的复位同步器。

该工程仓库里没有 `mig_a.prj`，所以用的是 `FREQ_HZ` 给出的周期。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：
  - `memoryInterface`：MIG 模型；
  - 读取 `.prj` 中的时钟配置；
  - `modelFromXciText(text, dir)`：在 `.xci` 所在目录查找它引用的文件。
- `src/clock/ClockAnalyzer.cpp`：`OPENCDC_CLOCK_PERIOD`、`OPENCDC_CLOCK_ALIAS`。
- `src/frontend/MixedSupport.h`：未知模块提示中的 IP 列表。
- `tests/frontend/test_xci.cpp`，三个测试：
  - 端口推导：DDR2 原生接口；DDR3 AXI、差分时钟、参考时钟、附加时钟；不支持的类型和多控制器；
  - ui_clk 与系统时钟相关：系统时钟寄存器进入用户接口、复位都不跨域（负例）；读数据在无关时钟下采样会跨域（正例）；用 `clk_pll_i` 声明异步后跨域，复位同步器被识别（正例）；对复位同步器的约束能匹配，名字错误时报 CON-UNMATCHED；
  - 有 `.prj` 时按比例派生；输入时钟不是配置值时报 CON-INPUT-PERIOD，周期跟随输入。
- 语料库：`tools/ci/vhdl-corpus.sh`，更新 Nexys A7 OOB 的期望。
- 文档：`docs/vivado-ip.md`、`README.md`。

## 4. 已知限制

- 存储器内部 PHY 的时钟（存储器时钟、参考时钟）不建模；存储器引脚由 ui_clk 寄存器驱动。
- `device_temp_i` 不读取：IP 自己对温度做滤波。
- UltraScale 的存储器接口（`ddr4`、`ddr3` 新版 IP）不建模。

# 阶段 79：MachXO2 —— SCUBA 存储器网表、DP8KC/PDPW8KC/SP8KC、模型中接常量的时钟

日期：2026-11-09　版本：0.79.0

阶段 78 的下一步是验证一个 MachXO2/MachXO3 的 Diamond 工程。这一阶段选择 **Yellowstone/Liron 软盘控制器**（steve-chamberlin/fpga-disk-controller，提交 3148876，CC BY-SA 4.0）。它是一块 Apple II 扩展卡，工程文件是 `lattice/liron.ldf`，器件为 LCMXO2-1200HC。

它和前面的工程有几点不同：
- 程序 ROM 是 IPexpress 生成的 SCUBA 网表 `codeROM.v`：由 4 个 `DP8KC` 拼成，参数（包括 ROM 内容 `INITVAL_00..1F`）用写在实例之前的 `defparam` 设置；
- 每个 `DP8KC` 的端口 B 都没有使用，时钟接常量 0；
- IWM 状态寄存器用 Apple II 总线选通信号 `_devsel` 的下降沿作为时钟。

第一次分析有 176 个错误：每条 defparam 一个“标识符在声明之前使用”。原因是 `DP8KC` 没有模型，实例无法细化，slang 只在 defparam 之前查找实例。

修复后的结果：
- 0 个错误，0 个告警，0 个未分析的构造；
- 17 条跨时钟路径。

工程已加入语料库。

## 1. 完成的功能

### 1.1 MachXO2/XO3 的 9 kbit RAM 模型（`lattice_gowin.v`）

| 原语 | 模型 |
|---|---|
| `DP8KC` | 真双口：端口 A 在 `CLKA`、端口 B 在 `CLKB`；每个端口写自己的数组，读出看到两者（与 `DP16KD` 相同）。字地址取 `ADx12..ADx3` |
| `PDPW8KC` | 伪双口：在 `CLKW` 写，在 `CLKR` 读 |
| `SP8KC` | 单口，在 `CLK` 上读写 |

- 三个模型都声明了 `INITVAL_00..1F`；`DP16KD` 声明了 `INITVAL_00..3F`。SCUBA 网表用 defparam 设置这些参数，以前模型中没有，会报告“参数不存在”。
- VHDL 组件包 `components` 现在也属于 `machxo2`、`machxo3l`、`machxo3d` 库。
- `docs/primitives.md` 已更新。

### 1.2 写在黑盒实例之前的 defparam（`FE-DEFPARAM-FORWARD`）

- 什么时候发生：一个实例的模块未知，在 `unknownModules: blackbox` 下作为黑盒。slang 只在 defparam 之前查找实例，所以写在它之前的 defparam 报告“使用在声明之前”。
- 处理：把该模块的所有 defparam 移到 `endmodule` 之前。原位置替换为空格，行号不变。Verilog 工具也是这样解析 defparam 的。报告一条告警。
- 实例的模块有模型或定义时，slang 能正常解析，不做任何修改（测试中确认）。
- 反例测试：指向不存在的实例的 defparam 仍然是错误。

### 1.3 模型中接常量的时钟（`CLK-UNUSED-INTERFACE`）

- 以前：内置原语模型中，时钟接常量的寄存器报告为 `CLK-CONSTANT` 告警，例如作为 ROM 使用的 RAM 的端口 B。
- 现在：改为提示 `CLK-UNUSED-INTERFACE`，说明该接口未使用，与阶段 71 中“时钟输入未连接”的处理一致。
- 设计自身的寄存器接常量时钟，仍然是 `CLK-CONSTANT` 告警（反例测试）。
- 语料中受影响的项目：`surf_gigeth_gtp7w`。`GTPE2_COMMON` 模型中 2 个寄存器改为提示，`CLK-CONSTANT` 从 34 个变为 32 个，预期值已更新。

## 2. 真实工程验证

### Yellowstone/Liron（直接分析 `liron.ldf`，加入语料库）

| | 第一次分析 | 修复后 |
|---|---|---|
| 错误 | 176（defparam 在实例之前） | 0 |
| 未知原语 | `DP8KC` ×4 | 0（使用模型 `DP8KC`、`VHI`、`VLO`） |
| 寄存器 | — | 31（23 个有时钟；8 个是 ROM 端口 B 的输出寄存器，属于未使用的接口） |
| 时钟 | — | `fclk`、`_devsel`（下降沿） |
| 跨时钟路径 | — | 17 |

17 条路径都是 IWM 寄存器 `q6`/`q7`/`motorOn`（在 `_devsel` 下降沿锁存）到 `fclk` 域，没有同步，另有 2 条是复位输入。这个设计依赖 Apple II 总线时序，这些路径需要设计者复核。LPF 中没有时钟约束，两个时钟的关系未知。

### 其他语料

- SV 语料 33 个项目全部匹配（新增 Liron），其余结果与修改前逐行相同。
- VHDL 语料 48 个全部匹配，其中 `surf_gigeth_gtp7w` 的预期值按 1.3 更新。

## 3. 修改的文件

- `src/frontend/primitives/lattice_gowin.v`：`DP8KC`、`PDPW8KC`、`SP8KC`，`INITVAL_*` 参数。
- `src/frontend/primitives/lattice_components.vhd`：组件与 MachXO2/XO3 库名。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：`FE-DEFPARAM-FORWARD`。
- `src/clock/ClockAnalyzer.cpp`：模型中接常量的时钟。
- 测试（每个用例都含反例）：
  - `tests/frontend/test_primitives.cpp`：1 个用例，并更新原语清单；
  - `tests/frontend/test_defparam.cpp`：1 个用例。
- `README.md`、`docs/primitives.md`。
- `tools/ci/sv-corpus.sh`：Liron；`tools/ci/vhdl-corpus.sh`：`surf_gigeth_gtp7w` 的预期值。
- `CMakeLists.txt`：版本 0.79.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：651 个测试全部通过。
- 我自己的一个错误判断，已改正：
  - 最初以为 defparam 写在实例之前本身就会出错，测试也按这个写。
  - 测试结果表明：实例的模块已知时，slang 能正常解析。错误只在实例是黑盒时出现。
  - 已据此修改诊断信息和测试。
- SV 语料 33 个、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 78 的 Windows CI（提交 d18bd31，运行 42575537920）：Release 与 Debug 均 649/649 通过，生成 `OpenCDC-0.78.0-windows-x64.zip` 与 `.exe`。
- 本阶段的 Windows CI（提交 576d886，运行 42577411641）：Release 与 Debug 均 651/651 通过，生成 `OpenCDC-0.79.0-windows-x64.zip` 与 `.exe`。

## 5. 已知限制

- MachXO2 的 `EFB`（嵌入式功能块：SPI/I2C/定时器/闪存）、`BB`（双向缓冲）、`OSCH`（内部振荡器）没有模型。
- 存储器模型不使用 `INITVAL_*` 的内容（与 CDC 无关）。
- Liron 的两个时钟没有约束，关系未知。

## 6. 下一步

阶段 80：继续用真实工程验证。候选：
- 为 MachXO2 的 `OSCH`、`EFB`、`BB` 建模；
- 一个使用 yosys/nextpnr 流程（Makefile 与 `.lpf`/`.pcf`）的 iCE40 或 ECP5 工程，例如 ULX3S 或 iCEBreaker 上的设计。

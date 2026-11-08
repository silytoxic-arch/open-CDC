# 阶段 64：顶层三态引脚；VHDL 选择赋值的完整性；常量位选择表达式

日期：2026-11-08　版本：0.64.0

阶段 63 之后，这一阶段检查 NES_MiSTer 剩下的锁存器和违例，逐类判断是真实问题还是误报。找到并修复了三处误报的根源。

## 1. 完成的功能

### 1.1 顶层三态引脚（误报：来源被错误归属）

NES_MiSTer 报告了 38 条从音频输出的 I2S 寄存器（音频时钟）到 NES 处理器数据总线（核心时钟）的未同步路径。实际上 I2S 输出不会进入 NES 核心。追踪后发现路径是这样的：

```verilog
assign USER_IO[2] = !(SW[1] ? HDMI_I2S : user_out[2]) ? 1'b0 : 1'bZ;   // 开漏输出
user_in <= USER_IO;                                                  // 读回引脚，送入核心（手柄等）
```

`USER_IO` 是顶层的 `inout` 引脚。以前，引脚的值取自内部驱动，所以读取引脚的逻辑“看到”了 I2S 数据。但在硬件中，输入缓冲器读到的是引脚上的电平，即外部输入；内部的三态驱动只决定引脚输出什么。

- **三态引脚**：顶层实例的 `inout` 端口中，由三态写法驱动的位（`oe ? d : 'z`，或常量 `'z`）。
- **读取**：读取三态引脚的逻辑看到的是引脚，即主输入，没有时钟域，归入 `CDC-NO-DOMAIN`（可以用 `set_input_delay` 指定时钟）。
- **输出检查**：对 `set_output_delay` 的检查仍然追踪三态引脚的内部驱动，检查设计驱动出去的值（`CDC-OUTPUT-CLOCK`）。
- **不受影响**：
  - 当普通输出使用的 `inout`（`assign plain = r`，没有 `'z`）仍按内部驱动处理（反例测试）；
  - 子模块的 `inout` 端口（阶段 60 的不动点）也不受影响。

### 1.2 VHDL 选择赋值的完整性（误报：假锁存器）

```vhdl
with Set_BusA_To select BusA <= DI when Set_BusA_To_DI, ... (others => '-') when Set_BusA_To_DONTCARE;   -- T65
```

这个枚举类型有 11 个值，编码为 4 位。选择列出了全部 11 个值，但 4 位编码有 16 种，以前因此推断出锁存器。

- VHDL 要求 `case` 和选择赋值的选择覆盖选择表达式子类型的所有值（否则是编译错误），所以它们不可能不完整。
- `case` 语句以前已经按这条规则处理：最后一个选择承担其余编码。现在选择信号赋值也这样处理。
- 带 `unaffected` 的选择仍然会保持原值，推断锁存器（反例测试）。

### 1.3 选择表达式中的常量位（误报：假锁存器）

```verilog
always_comb case ({state[3][2], 1'b0, state[3][0]})   // NES_MiSTer Mapper162
  0: ...  1: ...  4: ...  5: ...
endcase
```

中间位是常量 0，所以选择表达式只可能取 0、1、4、5 四个值，这个 case 是完整的。以前要求覆盖全部 8 个值。

- 现在只要求覆盖选择表达式可能取到的值：常量位不同的值不会出现。
- 中间位为常量 1 时，这些标签永远不匹配，仍然推断锁存器（反例测试）。

## 2. 真实工程验证

### NES_MiSTer

| | 阶段 63 | 阶段 64 |
|---|---|---|
| 锁存器 | 4 | **0** |
| 未分析的构造 | 0 | 0 |
| 跨时钟路径 | 603 | 556 |
| 未同步 | 545 | 498 |
| 错误级违例 | 519 | 472 |

- 减少的 47 条都是经过三态引脚 `USER_IO` 读回的路径（I2S、用户输出到核心），现在作为主输入（`CDC-NO-DOMAIN`）报告。
- Template_MiSTer 的核心不读取 `USER_IO`，结果不变。

### 违例分类（按时钟对）

| 时钟对 | 违例 | 判断 |
|---|---|---|
| 核心 → HDMI | 188 | 主要是缩放器的 `scaler_out`（67）、OSD 的 `rot`/`info`/坐标、视频尺寸寄存器 `WIDTH`/`HEIGHT`。结构上确实没有同步器；其中许多源寄存器在 `sys_top.sdc` 中用 `set_false_path` 标为准静态（见第 5 节） |
| 核心 → 音频 | 97 | 音频配置（`cfg`、`aflt_*`）和音频数据。配置寄存器有 `set_false_path` |
| 50 MHz 振荡器 1 ↔ 2 | 95 | 复位请求和 HDMI PLL 重配置 |
| 其他 | 92 | HPS、SPI 等 |

## 3. 修改的文件

- `src/graph/DesignView.{h,cpp}`：三态引脚（`isPad`、`padInternal`），读取三态引脚得到主输入。
- `src/cdc/CdcAnalyzer.cpp`：输出检查追踪三态引脚的内部驱动。
- `src/frontend/vhdl/VhdlStmt.cpp`：选择赋值的完整性。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：选择表达式中的常量位。
- 测试：
  - `tests/frontend/test_quartus_compat.cpp`：三态引脚；
  - `tests/vhdl/test_vhdl_elab.cpp`：枚举选择赋值；
  - `tests/ir_extract/test_ir_extract.cpp`：常量位。
- `tools/ci/sv-corpus.sh`：NES_MiSTer 的新结果。
- `CMakeLists.txt`：版本 0.64.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：595 个测试全部通过（新增 3 个测试用例）。
- SV 语料 24 个项目全部匹配（NES_MiSTer 的期望值已更新），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）结果见交付说明。

## 5. 已知限制

- **三态写法的识别**：只识别直接赋给顶层 `inout` 位的 `'z` 和一层 `?:`（多路选择的一个输入是 `'z`）。更复杂的写法仍按内部驱动处理。
- **准静态寄存器上的 `set_false_path`**：寄存器级的例外只用于时钟关系未知的路径（第 25.5 节的设计），而 MiSTer 用 `set_clock_groups -asynchronous` 声明了时钟关系。此外，Quartus 的裸名称写法（`set_false_path -to {cfg[*]}`）没有求值，记为“已记录、未使用”（Template_MiSTer 中 38 条）。设计者声明为准静态的路径仍按错误级违例报告。

## 6. 下一步

阶段 65：
- 求值 Quartus 约束中的裸名称集合（`{cfg[*]}`、`{*_osd|rot*}`）；
- 在违例中注明覆盖它的 `set_false_path`（设计者声明为准静态），让用户区分“设计者已声明”和“未声明”的跨时钟路径。不改变路径是否同步的判断。

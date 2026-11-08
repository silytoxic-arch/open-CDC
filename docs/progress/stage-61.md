# 阶段 61：没有默认值、由 defparam 赋值的参数；混合语言的位置参数；原语库启动时间

日期：2026-11-08　版本：0.61.0

阶段 60 的已知限制把 NES_MiSTer 无法分析归因于“链式 defparam”。这一阶段先做最小复现，发现原因不是链式，而是另一个更普遍的问题。修复后 NES_MiSTer 完整分析，并加入语料库。同时修复了在复现中发现的另外两个问题。

## 1. 完成的功能

### 1.1 隐式类型、没有默认值、由 defparam 赋值的参数（根本原因）

最小复现（纯 Verilog）：

```verilog
module dmc #(parameter [9:0] IDX) (...);   // 没有默认值
  leaf #(IDX) u_leaf (...);                 // 引用 IDX
endmodule
module top (...);
  dmc u_dmc (...);
  defparam u_dmc.IDX = 5;
endmodule
```

结果：`u_dmc.IDX` 的值是 5，但 `u_leaf.IDX` 没有值，依赖它的寄存器丢失，**没有任何诊断**。

逐步排除：
- 有默认值时正确；显式类型（`parameter integer IDX`）时正确；
- 链式与否无关，单层 defparam 也出错；
- 出错的是三种隐式类型：`parameter [9:0] P`、`parameter P`、`parameter signed P`。

原因在 slang（`DeclaredType::resolveType`）：隐式类型的参数从初始化表达式推断类型。没有初始化表达式时，类型被设为错误类型，即使声明给出了范围或符号。defparam 只设置参数的值，不设置类型。之后每个引用该参数的表达式都因错误类型求值失败，而且不报告。

修复（`third_party/patches/slang-0001-implicit-parameter-type-from-defparam.patch`）：defparam 为这种参数赋值时，按 IEEE 1800-2017 6.20.2 确定类型：
- 有范围：使用声明的范围和符号（`logic [9:0]`，或 `logic signed [9:0]`）；
- 没有范围：使用值的类型（整数的位宽和符号，`signed` 声明时为有符号；实数；字符串）。

### 1.2 依赖补丁的应用方式

- 新增 `cmake/ApplyPatches.cmake`：取得依赖源码后，按顺序应用 `third_party/patches/<依赖>-NNNN-*.patch`。
- 幂等：已应用的补丁跳过，重新配置和已有构建目录都可以用。离线镜像（`FETCHCONTENT_SOURCE_DIR_SLANG`）也会打补丁。
- 补丁既不能应用、也不是已应用状态时，配置失败：升级依赖时不会悄悄丢掉修复。
- `.gitattributes` 把补丁固定为 LF 换行。slang 自身也固定为 LF，所以 Windows 上换行一致；CRLF 版本的补丁也验证过可以应用。
- `third_party/README.md` 记录每个补丁修复的问题。

### 1.3 VHDL 实体的位置参数保持位置

Verilog 按位置给 VHDL 实体传参数时（`eReg_SavestateV #(IDX, DEF)`），如果某个值 OpenCDC 无法表示，以前会把它丢掉，后面的值整体前移一位：`DEF` 的值被当成 `Adr`，报告的错误也指向错误的泛型。

- 现在该位置保留为“值未知”，后面的参数位置不变；
- 错误报告具体的泛型：`the value of generic 'A' given by the instance is not known`；
- 转换失败的消息使用泛型名，不再出现空名字（`parameter value for ''`）。

### 1.4 原语库的启动时间

复现中，一个 5 个模块的混合语言设计，前端花了 3.3 秒（Debug 20 秒）。原因：原语库首次使用时，为每对模型（196 × 195 对）各编译并运行一个正则表达式，查找模型之间的实例化关系。阶段 60 加入 Cyclone V 原语后模型数增加，这个开销变得明显。混合语言设计和使用原语的设计都要付出这个代价。

- 改为每个模型扫描一遍标识符，判断条件与原来的正则表达式相同（后面是 `#`，或者是实例名加 `(`）；
- 对 196 个模型逐一比较新旧结果，完全相同；
- 前端时间：3.3 秒降到 0.006 秒（Release），20 秒降到 0.026 秒（Debug）。

## 2. 真实工程验证

### NES_MiSTer（直接分析 `NES.qpf`，加入语料库）

- **导入**：103 个源文件，13 个 IP 文件列表，2 个 SDC，顶层 `sys_top`。
- **参数**：存档寄存器 `eReg_SavestateV`（VHDL）的地址来自 `regs_savestates` 包，经 `nes.v` → `APU` → `DmcChan` 两层 defparam 传入，都是 `parameter [9:0]`，没有默认值。现在全部正确：APU 的四个存档寄存器地址分别是 16、17、18、19，与包中定义一致。
- **规模**：457 个实例，3720 个寄存器（931613 位），38020 个单元。
- **时钟**：11 个时钟，3664/3720 个寄存器已解析。
  - 核心 PLL 输出 85.91 MHz、42.95 MHz、21.48 MHz（NES 主时钟），与该核心的设计一致；
  - 音频 24.58 MHz、HDMI 148.5 MHz，三个 50 MHz 振荡器，HPS 100 MHz，SPI 和 I2C 时钟。
- **结果**：0 个错误，461 条跨时钟路径，34 条已同步，427 条未同步，77 个同步器结构；404 个错误级违例。
- **前端时间**：1.2 秒；整个分析 2.8 秒。
- **语料库**：只稀疏检出 `rtl/` 和 `sys/`（不含已发布的比特流），`build_id.v` 由脚本写入（与 Template_MiSTer 相同）。

### 其他语料

见第 4 节：SV 语料 24 个项目（新增 NES_MiSTer），VHDL 语料 48 个，结果与阶段 60 一致。

## 3. 修改的文件

- `third_party/patches/slang-0001-implicit-parameter-type-from-defparam.patch`（新）：slang 的修复。
- `cmake/ApplyPatches.cmake`（新）、`cmake/Dependencies.cmake`：应用补丁。
- `.gitattributes`（新）：补丁使用 LF。
- `third_party/README.md`：补丁说明。
- `src/frontend/slang_adapter/IrBuilder.cpp`：值未知的位置参数保留位置。
- `src/frontend/vhdl/VhdlMixed.cpp`、`VhdlElab.cpp`、`VhdlElab.h`：`fromForeign` 使用泛型名，报告值未知的参数。
- `src/frontend/Primitives.cpp`：模型依赖改为线性扫描。
- `tests/frontend/test_defparam.cpp`（新）、`tests/CMakeLists.txt`：测试。
- `tools/ci/sv-corpus.sh`：NES_MiSTer。
- `CMakeLists.txt`：版本 0.61.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：582 个测试全部通过（新增 3 个测试用例，23 个断言）。
- 撤销 slang 补丁后，新测试的 5 个断言失败（确认测试能发现这个问题）；重新应用后全部通过。
- SV 语料 24 个项目全部匹配（新增 NES_MiSTer），VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- Windows（GitHub Actions）结果见交付说明。

## 5. 已知限制

- **阶段 60 的诊断有误**：阶段 60 记录的“链式 defparam”限制不存在。链式 defparam 本身一直正确，真正的原因是 1.1 节的问题，已修复。
- **slang 补丁**：需要随 slang 版本更新维护。补丁不能应用时配置会失败并给出提示。这个修复也应提交给 slang 上游。
- **值无法表示的参数**：传给 VHDL 实体的非整数、非实数、非字符串值（结构体、非压缩数组）仍然不支持，报告 `IR-UNSUPPORTED-PARAM` 和对应的泛型错误。
- **NES_MiSTer 的警告**：334 个警告，主要是不支持的表达式和过程块（`IR-UNSUPPORTED-EXPR/PROCESS/LVALUE`）、混合赋值、锁存器推断，涉及的寄存器按保守方式建模。

## 6. 下一步

阶段 62：减少 NES_MiSTer 中的 `IR-UNSUPPORTED-*`（47 个表达式、33 个过程块、29 个左值），先按出现次数找出最常见的构造并逐个支持，每个都用最小复现和语料验证。

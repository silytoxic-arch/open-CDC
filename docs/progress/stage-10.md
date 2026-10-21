# 阶段 10 完成报告：时钟生成器频率、存储器/DDR 原语、SystemVerilog 接口

日期：2026-10-21　版本：0.10.0

## 1. 已完成的功能

| 任务 | 内容 |
|---|---|
| T10.1 MMCM/PLL 输出周期 | 每个输出的周期 = 输入周期 × `DIVCLK_DIVIDE` × `CLKOUTn_DIVIDE[_F]` / `CLKFBOUT_MULT[_F]`。输入周期取输入时钟的约束；输入时钟没有约束时取实例的 `CLKIN1_PERIOD`。时钟按引脚命名（`top.mmcm.CLKOUT0`），CDC-007 可以用这些周期判断脉冲丢失。原语实例未连接的输出不再报警告（一个 MMCM 原来有 15 条）。 |
| T10.2 更多原语 | **Xilinx**：`IDDR`/`IDDRE1`、`ODDR`/`ODDRE1`、`IDELAYE2/3`、`ODELAYE2/3`、`IDELAYCTRL`（`RDY` 没有时钟域）、`RAMB18E1`、`RAMB36E1`、`RAMB18E2`、`RAMB36E2`。<br>**XPM**：`xpm_memory_sdpram`、`_tdpram`、`_spram`、`_dpdistram`（独立时钟模式跨域，共用时钟模式不跨域）。<br>**Intel**：`altsyncram`、`altddio_in`、`altddio_out`。<br>**宽容处理**：模型不用的参数被忽略；连接到模型没有的引脚（级联、ECC）时给出 `FE-PRIMITIVE-PORT` 信息而不是错误，Verilog 和 VHDL 一致；VHDL 中 `INIT_00 => (others => '0')` 这类没有类型上下文的 generic 被跳过。 |
| T10.3 SystemVerilog 接口 | 接口实例展平到声明它的模块（成员 `ia.data`，接口内部的 always/assign 和接口端口一起分析）；接口端口（modport、通用 `interface`、接口数组、继续向下传递）拆成"每个用到的成员一个端口"（`b.data`），方向取 modport，否则按使用判断。只含断言的 always 块不再报不支持。 |

## 2. 修改的文件

**新建**
- `tests/frontend/test_sv_interfaces.cpp`
- `tools/ci/sv-corpus.sh`
- `docs/progress/stage-10.md`

**修改**
- `src/frontend/Primitives.cpp`：时钟生成器模型（频率参数及默认值、时钟属性）。
- `src/frontend/primitives/`：
  - `xilinx_unisim.v`：DDR、I/O 延迟、`IDELAYCTRL`、块 RAM；
  - `xilinx_xpm.v`：XPM 存储器；
  - `intel.v`：`altsyncram`、`altddio_*`；
  - 三个 VHDL 组件包新增对应组件。
- `src/clock/ClockAnalyzer.cpp`：读取时钟属性，计算分频时钟周期（`resolveDividerPeriods`）。
- `src/frontend/AutoFrontend.cpp`：过滤原语的未连接输出和未知参数警告；传递宽容模块列表。
- `src/frontend/MixedSupport.h`、`slang_adapter/SlangFrontend.cpp`：`lenientModules`，`PortDoesNotExist` 降级为 `FE-PRIMITIVE-PORT`。
- `src/frontend/vhdl/VhdlElab.h`、`VhdlMixed.cpp`：VHDL 侧的宽容端口和聚合 generic。
- `src/frontend/slang_adapter/IrBuilder.{h,cpp}`：接口实例展平、接口端口、层次引用到接口成员的读写、接口数组。
- `src/frontend/slang_adapter/ProcessLowering.cpp`：跳过只含断言的 always 块；不支持的 always 块给出语句类型。
- `tests/CMakeLists.txt`、`tests/frontend/test_primitives.cpp`。
- `.github/workflows/ci.yml`（SV 语料步骤）、`CMakeLists.txt`（版本 0.10.0）、`README.md`、`docs/architecture.md`（第 14 节新增 2 条，新增第 17 节）。

已有接口没有删除，也没有改变语义。

## 3. 关键算法

### 3.1 生成器周期
模型里的比例是 slang 按实例参数求值的属性表达式，例如 `(1.0 * DIVCLK_DIVIDE * CLKOUT1_DIVIDE) / CLKFBOUT_MULT_F`。每种参数组合有自己的模块体，所以每个实例的比例都正确。时钟分析在生成时钟的主时钟解析之后，按固定点迭代计算：周期 = 主时钟周期 × 比例；主时钟没有周期时用 `CLKIN1_PERIOD` × 比例。

### 3.2 双时钟存储器模型
每个写端口写自己的数组，每个读端口读所有数组（按位异或合并）。读端口寄存器的数据锥里既有本端口写入的数组（同时钟），也有另一端口写入的数组（另一时钟），所以跨端口的数据流成为 CDC 路径，同端口的不会。共用时钟模式下两个端口用同一个时钟，没有跨域。

### 3.3 接口展平
- **接口实例**：声明接口实例的模块体对接口体执行 `declareScope`/`lowerScope`，成员名带实例前缀，接口端口的连接变成连续赋值。
- **接口端口**：模块体中对 `b.data` 的引用（slang 的 `HierarchicalValue`，或 modport 端口）解析到接口成员；该成员不在本模块体内时，建立名为"端口名.成员路径"的线网，登记为虚拟端口。在模块体结束时确定方向：有 modport 方向就用它，否则看本模块（或经接口端口驱动它的子模块）是否驱动这个线网。
- **父模块绑定**：在连接的接口实例（或数组元素，按 `[i]` 后缀匹配）中按成员路径查找成员，实参是父模块中该成员的线网。父模块中的这个线网可能是本地展平的成员，也可能是父模块自己的虚拟端口，所以接口可以逐层向下传递。

## 4. 构建与测试结果

### 4.1 构建
Linux GCC Debug/Release（`-Werror`）通过。Windows MSVC 等 CI 作业的结果在推送之后确认。

### 4.2 测试
全部 329 个 CTest 用例通过（阶段 9 结束时 317 个）。新增用例：
- **`test_primitives.cpp`**：
  - MMCM/PLL 周期（正例，以及周期足够长时不报 CDC-007 的负例）；
  - 输入时钟无约束时使用 `CLKIN1_PERIOD`；
  - XPM 存储器：独立时钟跨域、共用时钟不跨域（负例）、真双口；
  - 块 RAM/DDR/延迟：E1 和 E2 块 RAM、未建模引脚、`IDELAYCTRL.RDY`；
  - 用户模块的未知端口仍是错误（负例）；
  - Intel 存储器和 DDR：双时钟跨域、单时钟不跨域（负例）；
  - VHDL 中的块 RAM 和 XPM 存储器（含未知端口和聚合 `INIT_00`）。
- **`test_sv_interfaces.cpp`**：
  - modport 接口；
  - 接口内部逻辑、端口向下传递和模块体共享；
  - 通用接口端口和接口数组；
  - 普通层次引用仍不支持（负例）；
  - 只含断言的 always 块。

### 4.3 真实工程
| 工程 | 结果 |
|---|---|
| PULP `axi_cdc_intf`（AXI_BUS 接口、modport、包、typedef 宏、common_cells Gray FIFO） | 87 个实例，80 个寄存器全部解析，0 个不支持的构造（阶段 9 有 101 个）。52 条路径：五个通道的 Gray 指针全部为"必要条件已验证"，数据为握手结构，只有两个顶层复位输入报潜在风险（这两个输入没有复位同步，本来就应报告；外部已同步时可用 `set_input_delay` 声明）。 |
| surf `ClockManager7`（MMCM/PLL） | 输出周期 4/8/16 ns，与 `CLKIN_PERIOD_G = 8.0`、倍频 8、分频 4/8/16 一致 |
| VHDL 语料（open-logic、neorv32、surf，约 1900 个文件） | 与阶段 9 完全一致 |

PULP AXI CDC 已加入 CI（`tools/ci/sv-corpus.sh`）。

## 5. 已知限制
- **接口**：接口中的函数/任务、modport 表达式（`.a(x[3:0])`）、clocking block、虚接口不支持；跨模块的普通层次引用仍按不支持处理。
- **周期**：MMCM 的相移、占空比、`CLKOUT4_CASCADE`、`CLKOUTn_USE_FINE_PS` 不建模。
- **块 RAM**：`ISERDESE2`/`OSERDESE2`、GT 收发器、URAM 没有模型。块 RAM 的级联和 ECC 引脚忽略；ECC 输出为常量 0。
- **XPM 存储器**：异步读（`READ_LATENCY = 0`）只在 `dpdistram` 中建模。

## 6. 下一步建议
1. `ISERDESE2`/`OSERDESE2`、URAM（`URAM288`）、Intel `altera_syncram`、Lattice/Gowin 常用原语。
2. SystemVerilog 接口中的函数和 modport 表达式。
3. 报告中按时钟域显示频率，并在 GUI 的时钟视图中标出生成器派生的时钟树。

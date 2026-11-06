# 阶段 57：XML 格式的块设计；IP 的综合存根；Vivado 的时钟覆盖规则；ODDR2/IDDR2

日期：2026-11-06　版本：0.57.0

真实工程是 Domipheus 的 ArtyS7-RPU-SoC（Apache-2.0），Arty S7-50 板上用 VHDL 写的 RISC-V SoC：

- **RPU 核心**：git 子模块，Apache-2.0；
- **5 个块设计**（Vivado 2018.1，XML 格式）：两个 Clocking Wizard（主时钟和 HDMI 视频时钟）和三个 Block Memory；
- **其他**：MIG DDR3（`.xci`）、HDMI 输出（TMDS，Spartan-6 写法的 `ODDR2`）、SPI、UART。

## 1. 完成的功能

### 1.1 XML 格式的块设计（Vivado 2018.2 及以前）

阶段 53 的文档写的是".bd 自 2014.1 起是 JSON"，这不对：JSON 格式从 2018.3 开始，之前是 IP-XACT XML 仓库（`bd:repository`）。文档已更正。

XML 仓库的内容：

- **组件**：每个层次有一个 `spirit:component`，含端口和总线接口。视图中的 `hierarchyRef` 指向它的 `spirit:design`。
- **设计**：每个 `spirit:design` 包含组件实例（`bd:xciName` 给出 IP 配置）、`adHocConnection`（网络）、`interconnection`（接口网络），以及 `hierConnection`（层次接口与内部单元之间的接口网络，`interfaceRef="S/网络名"`）。

读取方式：

- 转换成与 JSON 格式相同的结构，再用同一个生成器生成模块，所以层次模块、接口展开、位宽和默认值的规则都相同。
- Vivado 写出的字节序标记（BOM）会被跳过。
- **报错**：找不到顶层组件，或不是 `bd:repository` 时，报 `FE-BD-PARSE`。

### 1.2 IP 的综合存根优先于组件文件

- **问题**：这个工程的 MIG 组件文件 `mig_7series_0.xml` 已经过时。
  - 它写 `app_rd_data` 是 32 位，实际是 128 位（4:1 PHY，16 位 DQ）；
  - 它也没有 `device_temp` 输出。
  - 结果 VHDL 顶层的端口映射报错，整个设计无法分析。
- **修正**：读取 Vivado 写出的综合存根 `<instance>_stub.v`（综合设计时看到的黑盒声明）。存在时优先用它的端口，查找位置包括 `.xci` 旁边和 `<project>.gen`。
- **MIG 模型也随之修正**：`device_temp` 输出总是存在；`TEMP_MON_CONTROL = EXTERNAL` 时另有 `device_temp_i` 输入。这与存根一致，以前的模型在外部 XADC 时缺少 `device_temp`。

### 1.3 Vivado 的时钟覆盖规则

Digilent 的板级 xdc 把 12 MHz 和 100 MHz 两个振荡器都命名为 `sys_clk_pin`，100 MHz 的还定义了两次，三行都带 `-add`。

- **以前**：带 `-add` 时都保留，生成 `sys_clk_pin`、`sys_clk_pin#2`、`sys_clk_pin#3` 三个时钟。同一个端口上有两个时钟，所有寄存器都被重复计入，结果有 233 条"需要审查"的跨时钟路径，都是虚假的。
- **现在与 Vivado 相同**（UG903；Vivado 报 Constraints 18-1056 "completely overrides"）：
  - 同名的时钟，即使带 `-add`，也完全覆盖先前的定义；
  - 不带 `-add` 时，同一源上的时钟也被覆盖；
  - 两种情况都报 `CON-DUPLICATE-CLOCK` 警告。
- 所以在这个工程中，与 Vivado 一样，12 MHz 时钟被覆盖，`CLK12MHZ` 成为没有约束的时钟。

### 1.4 原语 `ODDR2`、`IDDR2`

Spartan-6 的 DDR 寄存器，Vivado 会把它们重定向为 `ODDR`/`IDDR`。

- 两半都在 `C0` 的时钟域（`C1` 是 `C0` 的反相）。
- 同时加入 VHDL 组件声明。

## 2. 真实工程验证（ArtyS7-RPU-SoC）

| 时钟（已解析寄存器） | 结果 |
|---|---|
| 6（369/369） | 0 错误，3 个警告，10 条跨时钟路径，均未同步，需要审查 |

- **时钟**：
  - `sys_clk_pin` 10 ns；
  - MMCM 输出 `clk_core` 10 ns、`clk_pixel` 13.468 ns（74.25 MHz）、`clk_5xpixel` 2.6936 ns；
  - MIG 的 `ui_clk` 12.3 ns；
  - `CLK12MHZ` 没有约束。
- **10 条路径都在调试跟踪逻辑中**：
  - CPU（`clk_core`）的 `memctl.we`、`memctl.addr` 进入 12 MHz 域的 `trace_addr_triggered`、`trace_dump_enabled`；
  - 反方向，`trace_dump_enabled` 进入 100 MHz 域的 `trace_state`、`trace_ena_0` 等；
  - 都没有同步，是多位的跨时钟路径；
  - 它们标为"需要审查"，是因为两个振荡器的关系没有约束。在硬件上这两个时钟是异步的，所以这些路径是真实的问题。
- **3 个警告**：2 个是上面的时钟覆盖；1 个是 xdc 中 `get_ports -filter` 不支持。
- **视频与内存**：CPU 与视频（`clk_pixel`）、MIG（`ui_clk`）之间没有跨时钟路径。视频经过双口 Block Memory，各端口在自己的时钟中。
- **对比**：修正前这个工程无法分析：XML 块设计不能读，MIG 端口不匹配，`ODDR2` 是未知原语。
- 加入 SystemVerilog 语料库：只检出需要的目录，RPU 子模块按记录的提交检出。

## 3. 修改的文件

- `src/frontend/BlockDesign.{h,cpp}`：`xmlBlockDesign` 等 XML 读取函数，以及 BOM 处理。
- `src/frontend/XciModels.cpp`：`parseStubPorts`；存根优先；MIG 的 `device_temp`。
- `src/constraints/Sdc.cpp`：时钟覆盖规则。
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：`ODDR2`、`IDDR2`。
- 测试：
  - `tests/frontend/test_block_design.cpp`：
    - XML 块设计：BOM、层次、interconnection、hierConnection、ad-hoc 连接经过层次端口；
    - 负例：没有顶层组件、不是块设计的 XML。
  - `tests/frontend/test_xci.cpp`：
    - 存根优先于过时的组件文件；负例：没有存根时用组件文件；
    - MIG 外部 XADC 时有 `device_temp`。
  - `tests/constraints/test_sdc.cpp`：
    - 时钟覆盖：同名带 `-add`、同源不带 `-add`；负例：不同名带 `-add`、不同端口都保留；
    - 测试用的解析器改为每个端口一个位，以前所有端口同一个位。
  - `tests/frontend/test_primitives.cpp`：`ODDR2`/`IDDR2` 的时钟域；负例：同一时钟中读取不算跨时钟。
- `tools/ci/sv-corpus.sh`：`artys7_rpu_soc` 工程。
- 文档：`docs/vivado-ip.md`（XML 格式、存根）、`docs/primitives.md`、`docs/architecture.md`（时钟覆盖）、`README.md`。
- `CMakeLists.txt`：0.57.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- XML 块设计中的 module reference 按 `componentRef` 的名字识别，没有在真实工程中验证。
- ad-hoc 连接中带位段的端口引用（`spirit:left`/`spirit:right` 属性）不处理。
- 存根中只识别 `input`/`output`/`inout` 声明及其位宽。

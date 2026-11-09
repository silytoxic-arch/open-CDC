# 阶段 88：taxi 的全部以太网示例与 cndm 网卡；存储器写入锥的共享

日期：2026-11-09　版本：0.88.0

阶段 87 只分析了 taxi 的 KC705 1G 示例。这一阶段按同样的方法分析 taxi（提交 cc70b27）中所有带 `Makefile` 的 FPGA 工程：
- `src/eth/example` 的 108 个以太网示例：22 块板，1G/10G/25G，7 系列、UltraScale、UltraScale+、Zynq UltraScale+；
- `src/cndm*/board` 的 42 个 cndm 网卡工程：PCIe DMA、多队列、PTP，最大的有 858 个实例、约 1.2 万个寄存器。

每个工程由脚本从作者的 `Makefile`（`SYN_FILES`、`XDC_FILES`）和 `config.tcl`（参数）生成 OpenCDC 工程。Vivado 生成的 IP（PCIe、收发器、PCS/PMA）按黑盒分析。

| | 第一次分析 | 修复后 |
|---|---|---|
| 以太网示例：有错误的工程 | 87 / 108（114 个错误） | 0 / 108 |
| 以太网示例：约束中未匹配的端口 | 包括所有非压缩数组端口 | 只剩设计中确实没有的端口 |
| cndm：有错误的工程 | 0 / 42 | 0 / 42 |
| cndm：CDC 路径分析 | 50–180 s | 1.7–13.8 s |

## 1. 完成的功能

### 1.1 常量表达式中的层次名（`SlangFrontend.cpp`）

- 写法：`.DATA_W(axis_qsfp_tx[0].DATA_W)`、`.ID_W(axis_stat.ID_W)`，即接口实例（或实例数组元素）的参数。
- slang 默认报错（`ConstEvalHierarchicalName`），Vivado 接受。这一个错误阻止了 87 个以太网示例的分析。
- 现在打开 slang 的 `AllowHierarchicalConst`。层次名引用的信号仍然不是常量，依然报错。

### 1.2 非压缩数组端口的约束名（`Sdc.cpp`）

- Vivado 按元素给非压缩数组端口命名：
  - `output logic sfp_led[2]` 的端口是 `sfp_led[0]`、`sfp_led[1]`；
  - `output logic [1:0] sfp_rs[4]` 的一位是 `sfp_rs[1][0]`。
- 以前 `get_ports {sfp_led[*]}` 什么也不匹配（`CON-UNMATCHED`），这些端口上的伪路径和输出延迟都不生效。
- 现在 `get_ports`/`get_pins` 的名字表中，每个数组端口另有一项对应每个元素。元素的下标按声明的范围给出（偏移 0 的元素是右边的下标），元素内的位可以再按位选择。

### 1.3 输出端口连接上的赋值模式（`IrBuilder.cpp`）

- 写法：`.xcvr_txp('{sfp_tx_p})`，连接到 `output wire logic xcvr_txp[CNT]`（KC705 10G、KR260）。
- 以前报 `IR-UNSUPPORTED-LVALUE`，该输出不连接。
- 现在 `'{a, b}` 作为赋值目标，按拼接处理：第一个元素对应左边的下标。slang 把每个元素表示为 “元素 = 它的部分”，取赋值的左边。

### 1.4 存储器写入锥的共享（`SeqGraph.cpp`）

- 原因：cndm 的 CDC 路径分析用了 50–180 s，几乎都在构建时序图的锥（cone）。
  - 存储器写入是按位的 `DynInsert` 单元，每一位读取 A 的这一位、全部写数据和写地址。
  - 一个 4096 位以内的存储器因此有“位数 × 数据宽度”次输入解析和合并。
- 修改：
  1. 写数据和写地址合在一个共享的锥中（键的位为 `kShared`），每个写入单元计算一次。每一位只合并 A 的这一位和这个共享锥。
  2. 合并时，读取同一个驻留锥、标志相同的输入只处理一次。
- 结果不变：锥是来源的并集，并集与合并顺序无关；写地址仍带“选择”标志。
  - 用修改前后的两个程序比较 SNES、ao486、corundum、zybo_sobel、chiplab：JSON 结果（去掉时间字段）完全相同，文本输出除时间外相同。
  - 两个语料库与修改前一致。
- 运行时间（CDC 路径分析）：

| 设计 | 修改前 | 修改后 |
|---|---|---|
| cndm ADM-PCIE-9V3 | 176 s | 12 s |
| cndm Alveo AU200 | 170 s | 12.7 s |
| corundum ZCU106 | 14.5 s | 7.2 s |
| ao486 | 约 9 s | 7.9 s |
| SNES | 3.0 s | 2.3 s |

## 2. 真实工程验证

### taxi 以太网示例（108 个工程）

- 修复后全部 0 错误。
- 剩余告警：
  - `FE-SIZE-INSTANCE-ARRAY`（阶段 87）；
  - 收发器与 PCS/PMA 的黑盒；
  - `CON-UNMATCHED`。
- 检查过的 `CON-UNMATCHED` 都是工程本身的问题：
  - XDC 引用的端口在 RTL 中被注释掉（RK_XCKU5P_F 的 `qsfp_i2c_*`、ZCU102 的 `i2c1_*`、fb2CG 的 `pps_*`）；
  - NT40E3 的 `sfp.xdc` 写成 `get_ports {get_ports sfp_tx_disable[*]}`；
  - 库脚本 `taxi_eth_mac_fifo.tcl` 在没有使用 MAC FIFO 的设计中找不到实例。
- 10G/25G 示例中的 CDC-004、CDC-008 是真实的结构：
  - 自协商结果 `an_lp_usxgmii_speed = mode ? reg[11:9] : 3'b011` 经过选择器后进入同步器；
  - 多位配置值逐位同步。
  - 这些是准静态信号，是否可接受需要设计者判断。
- 未加入语料库：阶段 87 的 KC705 1G 示例已经覆盖了这个库的以太网部分。

### cndm 网卡（42 个工程，ADM-PCIE-9V3 加入语料库）

- 全部 0 错误。
- 89–844 条跨时钟路径；没有 10G 选项的工程全部已同步，10G 版本有 4–17 条未同步路径（与以太网示例中的 CDC-004/008 来源相同）。
- 语料项：ADM-PCIE-9V3 的 25G 版本。
  - 858 个实例，11886 个寄存器，21 个时钟；
  - 603 条跨时钟路径全部已同步，439 个同步结构；
  - 45 条潜在风险（CDC-005，几乎都在 PTP 时间分发：各通道 `ptp_td_leaf` 的 `ts_rel_ns_reg` 处多个同步信号重新汇合），16 条需复查。

### 其他语料

- SV 语料 53 项（新增 cndm）、VHDL 语料 48 个全部匹配，其余结果与修改前相同。

## 3. 修改的文件

- `src/frontend/slang_adapter/SlangFrontend.cpp`：`AllowHierarchicalConst`。
- `src/constraints/Sdc.cpp`：数组端口的元素名。
- `src/frontend/slang_adapter/IrBuilder.cpp`：赋值模式作为赋值目标。
- `src/cdc/SeqGraph.{h,cpp}`：共享的写入锥，重复锥只合并一次。
- 测试（每个用例都含反例）：
  - `tests/frontend/test_quartus_compat.cpp`：
    - 接口数组元素与单个接口的参数决定宽度；反例：层次名引用的信号仍然报错；
    - `'{a, b}` 连接输出数组端口，并检查元素顺序（`a` 来自 `q[0]`）。
  - `tests/constraints/test_sdc.cpp`：
    - 一位元素与向量元素的端口名、元素的偏移、`[*][*]`、实例引脚；
    - 反例：越界下标、普通向量端口不接受第二个下标。
  - `tests/cdc/test_cdc.cpp`：存储器写数据与写地址的跨时钟路径（地址带选择标志）；反例：常量地址、输入数据的写入没有来自另一时钟域的路径。
- `README.md`：数组端口名；常量表达式中的层次名。
- `tools/ci/sv-corpus.sh`：工程生成改为通用函数（变量替换、跳过含 `$` 的参数）；cndm ADM-PCIE-9V3。
- `CMakeLists.txt`：版本 0.88.0。

## 4. 构建与测试结果

- 本地 Linux GCC Debug：692 个测试全部通过。
- 测试预期的一处修改：数组端口的用例最初写在 `.v` 文件中并使用 `logic`。Verilog 文件不使用 SystemVerilog 关键字，`logic` 是标识符。改为 `wire`。
- SV 语料 53 项、VHDL 语料 48 个全部匹配。
- Clang 构建无告警。
- 阶段 87 的 Windows CI（提交 1460817，运行 42632557900）：Release 与 Debug 均 688/688 通过。
- 本阶段中间提交的 Windows CI（提交 9f5db5d，运行 42640571601）：Release 与 Debug 均 691/691 通过。
- 中间提交 c5b53bd 的 Windows CI（运行 42654358692）构建失败：`SeqGraph.cpp` 中 `std::pair<std::size_t, std::uint8_t>{1, kLogic | kSelect}` 的 `int` 到 `unsigned char` 转换在 MSVC 中是 C4244 告警（告警视为错误），GCC 与 Clang 不报。已改为显式的 `std::uint8_t` 常量。

## 5. 已知限制

- 同一个接口实例连接到一个模块的两个接口端口（cndm 的事件环回 `.s_axis_event(e), .m_axis_event(e)`）时，`IR-INPUT-DRIVEN` 报告输入端口在模块内被驱动。
  - 信号仍按模块内驱动分析，结果正确，但告警有误导。
  - 一次按 modport 方向修正的尝试没有效果，已撤回。
- 不在 slang 中修改常量函数的求值。corundum 前端约 32 s 中约 19 s 用于 `lfsr_mask` 的常量求值；按参数和实参缓存试过，没有效果：每次调用的参数或实参都不同。
- 数组端口的元素名只对一维非压缩数组生成。

## 6. 下一步

阶段 89：
- 修正同一接口实例连接两个端口时的端口方向；
- 继续真实设计验证，例如带视频采集或 SerDes 的设计。

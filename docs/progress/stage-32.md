# 阶段 32：current_instance 与按位的触发器名字

日期：2026-10-27　版本：0.32.0

## 1. 完成的功能

### 1.1 current_instance

Vivado 的约束常用 `current_instance <实例>` 把一段约束限定在某个实例内（Digilent 的同步器示例、IP 的 XDC）。以前这个命令被忽略，后面的相对名字全部找不到。

现在：
- **相对名字**：`current_instance` 之后，`get_cells`/`get_pins`/`get_nets` 的名字相对于当前实例，`-hierarchical` 只在它下面查找。
- **实例的写法**：
  - 对象，如 `[get_cells u0]`；
  - 相对路径，如 `u1/inner`，可以带 `..`（上一级）和 `.`；
  - 名字可以是通配符，但必须正好匹配一个层次实例。
- **回到顶层**：
  - 不带实例的 `current_instance`（`-quiet` 也一样）回到顶层；
  - 每个约束文件结束时也回到顶层，与 Vivado 相同。
- **返回值**：当前实例的名字。
- **get_ports**：
  - 普通的 `get_ports` 仍然是顶层端口（Vivado：端口不受 `current_instance` 影响）；
  - `get_ports -scoped_to_current_instance` 返回当前实例的引脚；
  - `-prop_thru_buffers` 接受（模型中端口与逻辑之间没有 I/O 缓冲）。
- **在局部约束文件中**（`read_xdc -ref`）：`current_instance` 只能在该实例内移动；不带实例时回到该实例。
- **错误**：实例不存在、`..` 超出顶层、目标不是层次实例时报 `CON-UNMATCHED` 警告，当前实例不变。

### 1.2 get_ports -of_objects <线网>

返回线网上的端口（与线网有共同位的端口，只取这些位），用于 open-logic 的 `olo_intf_sync.tcl`：

```
get_ports -scoped_to_current_instance -prop_thru_buffers -of_objects [get_nets -of_objects $in_ffs]
```

以前报"不支持"，返回空集合。

### 1.3 按位的触发器

Vivado 中向量寄存器的每一位是一个触发器单元，名字为 `x_reg[3]`；`std_logic_vector(0 downto 0)` 这样的一位向量也是 `x_reg[0]`。以前整个向量寄存器是一个单元 `x_reg`，所以 `-filter {NAME =~ */iData_int_reg[*]}` 什么也匹配不到。

现在：
- `get_cells` 对向量寄存器每一位返回一个单元，`get_cells -regexp` 按位匹配时也是每位一个单元；
- 触发器引脚每位一个（`x_reg[3]/C`）；
- 过滤器中的 `NAME` 是按位的名字；
- `get_pins -hierarchical *`（只用 `-filter` 筛选时）也包含触发器引脚；
- 没有给名字的 `get_pins`/`get_cells`/`get_nets`/`get_ports` 等同于 `*`。

为此 IR 的线网记录了"声明为向量"（`ir::Net::vector`，VHDL 与 SystemVerilog 前端都设置），用来区分一位向量与标量：标量寄存器 `s_reg` 不能写成 `s_reg[0]` 过滤。

## 2. 真实工程验证

### 2.1 Digilent 同步器示例（vivado-library `module/synchronizers/example`）

它的 `top.xdc` 用 `current_instance [get_cells SyncAsync1]` 等为 5 个同步器实例分别加约束，并用 `get_pins -hier -filter {NAME =~ ...oSyncStages_reg[0]/C}` 和 `get_cells -hier -filter {NAME =~ *HandshakeData*/iData_int_reg[*]}` 查找触发器。

| | 以前 | 现在 |
|---|---|---|
| 警告 | 8 个：`-scoped_to_current_instance` 是未知选项（5 个）；过滤器找不到触发器（2 个）；周期为空导致 `expr` 出错（1 个）。另有 `current_instance x11` 被忽略 | 0 个 |
| 约束 | 实例内的约束写到了顶层，名字找不到 | 全部找到 |
| 跨域 | 7 条，都需要复查 | 7 条。握手数据通路 `iData_int -> oData` 被 XDC 中的 `set_max_delay -datapath_only` 覆盖，成为安全路径；其余 6 条仍需复查（两个时钟的关系没有约束） |

### 2.2 open-logic `olo_intf_sync`

以 `olo_intf_sync` 为顶层，加入它自带的局部约束 `olo_intf_sync.tcl`（`read_xdc -ref`）：0 个警告。约束中的
- `get_pins -of_objects [get_cells *Reg0*] -filter {REF_PIN_NAME == D}` 得到 4 个引脚（`Width_g = 4`，每位一个，与 Vivado 相同）；
- `get_ports ... -of_objects [get_nets ...]` 得到 `DataAsync` 的 4 位。

这两个结果是在约束文件副本末尾加入检查后确认的。

两个工程都加入了 VHDL 语料回归。`olo_intf_spi_master` 的 `ClkFreq_g` 没有默认值，需要另配参数，没有加入。

## 3. 修改的文件

- `src/constraints/Sdc.h`：
  - `IObjectResolver::cellBits`；
  - `DesignObjectResolver::perBit`、`Named::vector`。
- `src/constraints/Sdc.cpp`：
  - `current_instance` 命令（`currentInstanceCmd`）；
  - 作用域状态：`root_`/`fileScope_`/`curScope_`/`setScope`/`runInScopes`；
  - `ScopedResolver` 的 `portScope`；
  - `get_ports -scoped_to_current_instance`/`-prop_thru_buffers`/`-of_objects`；
  - 按位的单元与引脚；没有名字时等同于 `*`；
  - `read_xdc -cells` 接受名字。
- `src/ir/Design.h`：`Net::vector`。
- `src/frontend/vhdl/VhdlElab.cpp`、`src/frontend/slang_adapter/IrBuilder.cpp`：设置 `vector`。
- 测试：
  - `tests/constraints/test_sdc.cpp`，4 个新用例：
    - `current_instance`：对象、相对路径、`..`、不带实例、返回值、`get_ports` 与 `-scoped_to_current_instance`；文件结束时复位；负例：实例不存在、`..` 超出顶层、目标是触发器、没有选项时 `get_ports` 看不到实例引脚。
    - 局部约束中的 `current_instance`：负例为超出实例。
    - `get_ports -of_objects`：两个实例各得到一条路径例外；负例为实例内部的线网。
    - 按位过滤：`v_reg[2]`、`v_reg[*]/C`（4 个）、一位向量 `one_reg[0]`；负例为标量 `sc_reg[0]`。
  - 原有用例按 Vivado 的按位结果更新：`cnt_reg_reg\[\d+\]` 现在是 2 个单元；`get_ports -of_objects` 现在受支持。
- 语料库：`tools/ci/vhdl-corpus.sh`（Digilent 同步器示例、`olo_intf_sync`）。
- 文档：`README.md`。

## 4. 构建与测试结果

见交付说明：全部测试、两套语料库、Clang 构建。

## 5. 已知限制

- `current_instance` 的名字不接受以 `/` 开头的绝对路径（Vivado 也没有这种写法）。
- 内存数组元素（`m_reg[3]`）不再按位拆分（Vivado 中是 `m_reg[3][7]`）。
- `get_ports -of_objects` 只接受线网。

## 6. 下一步

- GUI 中新建和修改约束条目的模块（阶段 31 的限制）。

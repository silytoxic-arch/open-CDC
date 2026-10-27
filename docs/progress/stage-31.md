# 阶段 31：局部约束（read_xdc -ref）

日期：2026-10-27　版本：0.31.0

## 1. 完成的功能

### 1.1 局部约束

IP 库常为每个模块附带一份局部约束文件（Vivado 的 `read_xdc -ref <模块>` / `SCOPED_TO_REF`）：文件在该模块的每个实例中各执行一次，名字相对于实例，`get_ports` 指实例的端口。open-logic（`olo_base_cc_*.tcl`）和 hdl-modules（`resync_*.tcl`）都是这样写的。以前这类文件无法使用：名字找不到，`read_xdc` 是未知命令。

现在：
- **两种用法**：
  - 约束脚本中的 `read_xdc -ref <模块> <文件>`（也支持 `-cells <实例>`；`-quiet`、`-unmanaged` 接受并忽略；不带 `-ref` 时就是普通约束文件）；
  - 工程文件中的约束条目 `{"file": "x.tcl", "ref": "模块"}`。GUI 编辑工程时保留条目的模块。
- **实例范围**：按 `REF_NAME`/`ORIG_REF_NAME` 找到模块的全部实例（不区分大小写）；模块就是顶层时，名字原样使用。每次执行使用独立的 Tcl 变量。
- **相对名字**：在实例内，`get_cells`/`get_pins`/`get_nets` 的名字相对于实例；`get_ports` 返回实例的引脚；`-hierarchical` 只在实例之下查找；`-regexp` 加上实例前缀。
- **没有实例**：报 `CON-UNMATCHED` 提示（info），与 Vivado 一致。库为所有模块附带约束时这是正常情况。

### 1.2 命令缩写

Vivado 接受唯一的命令缩写。open-logic 的约束写 `get_cell`。现在 `get_cell`、`get_pin`、`get_port`、`get_net`、`get_clock` 等同于复数形式：
- 能作为命令替换求值；
- 在 `set_false_path`/`set_max_delay` 的 `-from/-to` 中能被识别。

以前缩写形式被当作未知命令，`[get_cell RegIn*]` 原样成为文字，导致名字不匹配。

## 2. 真实工程验证：open-logic

VHDL 语料中的 6 个 open-logic 工程，现在都加入 open-logic 自带的 5 个局部约束文件（cc_reset、cc_bits、cc_simple、reset_gen、ram_sdp，与它的 `olo_base_constraints_amd.tcl` 相同）：
- 6 个工程都是 0 错误、0 警告，跨域结果不变。
- `olo_base_cc_status` 内部的 6 个 `olo_base_cc_bits` 实例各执行了一次 `olo_base_cc_bits.tcl`（`set_bus_skew x6`）。其中的 `set_max_delay -datapath_only -from <时钟> -to [get_cell Reg0*]` 按实例生成路径例外。
- 设计中没有的模块（reset_gen、ram_sdp）只给出提示。

## 3. 修改的文件

- `src/constraints/Sdc.{h,cpp}`：
  - `ScopedResolver`（实例范围的对象解析）；
  - `Interp::runScoped`、`read_xdc` 命令；
  - `SdcReader::readFile(file, scopeRef)`；
  - `IObjectResolver::topModules`；
  - 命令缩写。
- `src/engine/ProjectConfig.{h,cpp}`：`constraintScopes`，条目 `{"file", "ref"}` 的读写。
- `src/engine/AnalysisSession.cpp`：按条目的模块读取约束。
- `src/gui/ProjectEditor.cpp`：保留约束条目的模块。
- 测试：
  - `tests/constraints/test_sdc.cpp`：两个实例各得到一条路径例外，指向各自实例的寄存器；时钟取自实例端口；`-hierarchical` 只在实例内查找；缩写 `get_cell`；模块不存在时给出提示；作用域为顶层；不带 `-ref` 的 `read_xdc`。
  - `tests/engine/test_project_config.cpp`：`{"file", "ref"}` 条目的读写往返。
- 语料库：`tools/ci/vhdl-corpus.sh`（open-logic 的局部约束）。
- 文档：`README.md`。

## 4. 已知限制

- GUI 不能新建或修改约束条目的模块，只能保留；新建需要编辑工程文件。
- `read_xdc -mode`、`-of_objects` 被忽略。
- 局部约束中 `current_instance` 不支持。

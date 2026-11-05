# 阶段 54：块设计的层次单元保留为模块，实例路径与 Vivado 相同

日期：2026-11-05　版本：0.54.0

## 1. 完成的功能

阶段 53 把块设计中的层次单元展平，实例名是单元路径中的 `/` 换成 `_`。例如 `sys_bus/s00_couplers/auto_cc` 变成 `sys_bus_s00_couplers_auto_cc`。这样做有两个缺点：

- 约束中写出完整层次路径的对象（`get_cells grp/cons/r_reg*`）匹配不到；
- 报告中的路径与 Vivado 网表的路径不一样。

现在每个层次单元生成一个模块（`<设计名>_<路径>_imp`），以单元名实例化，实例路径与 Vivado 生成的网表相同（`soc_inst/sys_bus/s00_couplers/auto_cc`）。

- **层次模块的端口**：端点既在层次内、又在层次外的网络成为该模块的端口。
  - 驱动端在层次内时为输出，双向端口为 inout，其余为输入；
  - 端口名沿用网络名。
- **只在内部使用的网络**：只在一个子层次内部使用的网络不出现在上层模块中。
- **叶单元**：实例名就是单元名。
- **不变的部分**：多驱动或无驱动的说明、未连接输入的默认值、端口的位段拼接，与阶段 53 相同。

## 2. 真实工程验证（Fuxi SoC）

- 结果与阶段 53 相同：8 个时钟（1095/1095），0 错误，9 个警告，1 条跨时钟路径（已同步），0 违例。
- 22 个层次单元现在各是一个模块，例如 `SoC.soc_inst.sys_bus.s00_couplers.auto_cc`。
- 语料库中 Fuxi 的期望只把摘要中的"flattened"一词去掉。

## 3. 修改的文件

- `src/frontend/BlockDesign.{h,cpp}`：
  - 按层次生成模块；
  - 每个网络记录它出现在哪些模块中，据此判断它是否穿过某个层次；
  - 网络在每个模块内按该模块中的声明取位段。
- `src/frontend/AutoFrontend.cpp`：`FE-BD-MODULE` 的措辞改为"N hierarchical cell(s)"。
- 测试 `tests/frontend/test_block_design.cpp`：
  - 正例：层次模块的端口和方向、子层次中的实例；嵌套两层的层次（`d_h1_imp`、`d_h1_h2_imp`），层次内部的网络不成为上层的端口。
  - 端到端：按 Vivado 完整路径写的 `set_false_path -to [get_cells grp/cons/r_reg*]` 能匹配，没有 `CON-UNMATCHED`；负例：阶段 53 展平后的名字 `grp_cons/...` 匹配不到。
- `tools/ci/sv-corpus.sh`：Fuxi 的期望。
- 文档：`docs/vivado-ip.md`、`docs/architecture.md`、`README.md`。
- `CMakeLists.txt`：0.54.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 层次模块的名字（`<设计名>_<路径>_imp`）与 Vivado 的不同。Vivado 用 `<单元名>_imp_<散列>`，模块名只在 `get_cells -filter {REF_NAME == ...}` 这类约束中才会用到。
- AXI TFT 控制器仍没有模型：PG095 中各端口的时钟域没有查到可靠的说明。

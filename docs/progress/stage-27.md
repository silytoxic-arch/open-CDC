# 阶段 27：IP 仓库中的自定义 IP；组合块中的混合赋值

日期：2026-10-27　版本：0.27.0

## 1. 完成的功能

### 1.1 从 IP 仓库取自定义 IP

OpenCDC 不建模的 IP（自己的 IP、板卡厂商的 IP，例如 Digilent 的 `pmod_bridge`、`PWM`）可以像 Vivado 的 `ip_repo_paths` 一样，从 IP 仓库取得。工程文件新增字段：

```json
"ipRepositories": ["../vivado-library/ip"]
```

处理方式：
- 在仓库目录中（最多 8 层）查找 `component.xml`，按 `.xci` 的厂商、库、名称、版本匹配（厂商和库不区分大小写）。每个仓库只扫描一次。
- 从 `component.xml` 取：
  - 综合视图（优先 `xilinx_anylanguagesynthesis`，其次 Verilog/VHDL 综合视图）的顶层模块，以及该视图文件集中的 HDL 文件。Verilog、SystemVerilog、VHDL（带库名）会加入；约束等其他文件不加入；文件集中的 `.xci` 继续按 IP 建模。出现 SystemVerilog 文件时分析切换为 SystemVerilog。
  - 组件声明的模型参数，取 `.xci` 中配置的值，按类型写成 Verilog 表达式：字符串、整数、位串（`reg`/`std_logic_vector`）、布尔。
- 生成一个以实例名为名的包装模块，用这些参数例化顶层模块：
  - 端口取自实例的端口表（JSON `.xci`，或 XML `.xci` 旁的组件文件）；
  - 配置禁用的端口：输入接组件声明的默认值，输出悬空，与 Vivado 生成的包装一致。
- FE-XCI-MODEL 中说明来自哪个仓库文件、顶层模块和文件数。没有实例端口表时不建模。

GUI 工程设置的"Sources"页增加了 IP 仓库列表。

### 1.2 组合块中的阻塞与非阻塞混合赋值

Digilent PWM 的 AXI 读多路选择器在组合块中混用了 `=` 和 `<=`（`default: reg_data_out <= 0;`）。以前两种赋值分开跟踪：没有执行非阻塞赋值的路径会保持旧值，形成变量依赖自身的组合环路（PmodAMP2 报 128 个 GR-COMB-LOOP）。

现在组合块里两种方式都赋过值的变量，其非阻塞赋值按阻塞赋值处理，与综合工具一致。仍报 IR-MIXED-ASSIGN 警告，并说明按阻塞处理。只用非阻塞赋值的组合块，以及时钟块，行为不变。

## 2. 真实工程验证：Digilent Pmod

以 vivado-library 的 `ip` 目录作为 IP 仓库，不再需要黑盒：

| 工程 | 结果 |
|---|---|
| PmodGPIO、PmodCOLOR、PmodAD2、PmodBT2、PmodESP32、PmodWIFI | 0 个警告（以前每个工程有 pmod_bridge 不支持与黑盒两个警告），0 个跨域 |
| PmodSF3、PmodNAV | 0 个警告；10 条跨域全部已同步，6 条因 OOC 约束没有声明时钟关系而需复核 |
| PmodAMP2 | PWM IP（SystemVerilog）也来自仓库：39/39 个寄存器（以前 21 个，PWM 是黑盒）；只剩 PWM 代码本身的 IR-MIXED-ASSIGN 警告，组合环路消失 |
| PmodHYGRO | 只剩块设计网表没有连接 axi_timer 输入的警告 |

语料回归中的 Pmod 工程改为使用 IP 仓库，并加入 PmodAMP2，共 5 个。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：
  - `XciContext`、`modelFromXci(file, context)`；
  - IP 仓库扫描（`componentsIn`，按仓库缓存）、`userIp`（包装模块、参数、禁用端口）；
  - `XciModel::extraSources`；
  - 分发函数中未建模的 IP 再到仓库查找。
- `src/frontend/AutoFrontend.cpp`：IP 带来的文件按队列插入在其 `.xci` 之后，去重；SystemVerilog 文件切换语言。
- `src/frontend/Frontend.h`、`src/engine/ProjectConfig.{h,cpp}`：`ipRepositories`（读取、保存、传给前端）。
- `src/frontend/slang_adapter/ProcessLowering.cpp`、头文件：组合块中混合赋值的处理。
- GUI：`src/gui/ProjectEditor.{h,cpp}`（IP 仓库列表）、`src/gui/main.cpp`（冒烟测试检查该列表）。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 正例：仓库中的自定义 IP，包括综合视图（不取仿真视图）、三种参数类型、禁用端口按默认值连接、只加入 HDL 文件；端到端时 IP 的寄存器在另一时钟读取会跨域、同时钟不跨域、文件集中嵌套的 `.xci` 也被建模；
    - 负例：没有仓库、版本不符、没有实例端口表。
  - `tests/ir_extract/test_ir_extract.cpp`：组合块混合赋值没有锁存器和组合环路；负例为只用非阻塞赋值的组合块、时钟块。关掉修正后，该测试因 16 个组合环路失败。
  - `tests/engine/test_project_config.cpp`：`ipRepositories` 读写往返。
- 语料库：`tools/ci/vhdl-corpus.sh`。
- 文档：`docs/vivado-ip.md`（新增"IP from an IP repository"一节）、`README.md`。

## 4. 已知限制

- 布尔参数写成 `1'b1`/`1'b0`；VHDL 顶层的布尔泛型能否这样接收，没有真实工程验证。
- 不处理组件文件里的 include 文件和子核（`subCoreRef`）。
- 仓库扫描深度限制为 8 层。

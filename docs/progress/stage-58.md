# 阶段 58：只知道接口时钟的 IP；Zynq UltraScale+ MPSoC；仿真网表的转义；负沿触发器与 MMCM_ADV

日期：2026-11-06　版本：0.58.0

真实工程来自 ATaylorCEngFIET 的 Hackster 仓库（Hackster.io 文章的工程，没有许可证，只为检查而下载）：

- **u96_linux_hackster**：Ultra96 上的 Zynq UltraScale+ MPSoC 块设计（Vivado 2018.2，JSON 格式）；
- **s7_tdm114**：Spartan-7 上的 MicroBlaze 和 MIPI 摄像头（视频去马赛克、视频时序控制器）；
- **Sobel_zybo_z7**：Zybo Z7 上的 Sobel 视频滤波器（HLS IP）、视频 DMA、HDMI 输入输出；
- **Zybo_EV_Platform**：与 Sobel 工程相同的视频通路（只用于对照，未加入语料库）。

## 1. 完成的功能

### 1.1 只知道接口时钟的 IP

- **问题**：有的 IP 既没有模型，输出产品也不能用。
  - Sobel 工程的 HLS IP `image_filter` 导出时没有输出产品；
  - 视频去马赛克 `v_demosaic` 的 HDL 和仿真网表都已加密。
  - 以前这些 IP 是未知模块，整个设计无法分析（`FE-UNKNOWN-MODULE` 错误）。
- **现在**：组件文件（或 JSON `.xci`）用 `ASSOCIATED_BUSIF` 把总线接口关联到时钟输入时，按这些关联建模，与 Vivado 的时序分析一致：
  - 关联到某个时钟的接口，其所有端口都在这个时钟中（输入被采样，输出是这个时钟的寄存器）；
  - 其他端口（复位、中断、`ap_start` 等块级控制）没有时钟域，不检查，在注释中列出。
- **限制**：IP 内部的跨时钟路径不分析，假定 IP 自己处理。所以报 `FE-XCI-INTERFACE-MODEL` 警告，并附上输出产品不能用的原因。
- **不适用**：有输出时钟的 IP（周期和关系未知），以及没有任何关联的 IP，仍报 `FE-XCI-UNSUPPORTED`。
- **Fuxi SoC 的变化**：AXI TFT 控制器（`axi_tft`，其 `ipshared` HDL 不在仓库中）现在也这样建模。
  - VGA 时钟 `clk_vga` 的寄存器因此被识别：时钟数 8 → 9，寄存器 1095 → 1134；
  - 警告 9 → 7；
  - 跨时钟路径仍为 1 条，已同步。

### 1.2 Zynq UltraScale+ MPSoC 处理系统（`zynq_ultra_ps_e`，PG201）

- `pl_clk0..3` 是独立的时钟，周期取自 `PSU__CRL_APB__PLn_REF_CTRL__ACT_FREQMHZ`，带 Vivado 的名字 `clk_pl_0..3`。
- 每个 AXI 端口在其接口关联的时钟中（`maxigp0_*` 在 `maxihpm0_fpd_aclk`，`saxigp2_*` 在 `saxihp0_fpd_aclk`……），依据组件文件或 JSON `.xci` 的 `ASSOCIATED_BUSIF` 和端口映射。没有这些信息时不建模。
- EMIO GPIO 输出是 LPD 外设时钟 `lpd_lsbus` 的寄存器（周期取自 `PSU__CRL_APB__LPD_LSBUS_CTRL__ACT_FREQMHZ`）。
- `pl_resetn`、中断和其他 EMIO 信号没有逻辑侧时钟域；输入到 PS 的信号在 PS 中同步。
- XML `.xci` 的端口时钟现在也从组件文件读取（`portClock`），供上述两个模型使用。

### 1.3 输出产品的综合视图引用多个文件集

Vivado 的综合视图先引用 IP 所用的库（如 `axi_lite_ipif`）的文件集，再引用 IP 自己的。以前只读第一个文件集，现在读视图中的所有 `localName`，按顺序加入。

### 1.4 仿真网表中的转义

Vivado 的仿真网表把单元路径写进属性字符串（`"\AXI_LITE_REG_INTERFACE_I/..."`），每个视频 IP 产生数百个 `FE-SLANG-UnknownEscapeCode` 警告。文件名为 `*_sim_netlist.v` 时不再报告；其他源文件中照常报告。

### 1.5 原语

- **负沿触发器**：`FDRE_1`、`FDSE_1`、`FDCE_1`、`FDPE_1`、`FD_1`、`FDE_1`、`FDR_1`、`FDS_1`、`FDC_1`、`FDP_1`（MicroBlaze 调试模块 MDM 使用）。
- **Virtex-6 的 `MMCM_BASE`、`MMCM_ADV`**：Vivado 在 7 系列上把它们重定向为 MMCME2，端口和分频参数相同（7:1 串行化时钟发生器使用）。
- **仿真网表中的切片原语**：`SRLC16E`、`AND2B1L`、`OR2L`、`MUXCY`、`MUXCY_L`、`XORCY`、`MULT_AND`。
- 都加入了 VHDL 组件声明。

## 2. 真实工程验证

| 工程 | 时钟（已解析寄存器） | 结果 |
|---|---|---|
| u96_mpsoc | 1（93/93） | 0 错误，0 警告，0 条跨时钟路径 |
| s7_mipi_microblaze | — | 1 个错误（真实问题，见下），1 个警告；CDC 未运行 |
| zybo_sobel | 7（11735/11735） | 0 错误，3 个警告，295 条跨时钟路径，238 条已同步，57 条未同步，294 条需要审查 |

- **u96**：`pl_clk0`（`clk_pl_0`，10 ns）驱动全部 93 个寄存器。
- **s7**：
  - 摄像头接口 `CAM_interface` 的 `pclk_out` 有两个驱动，`assign pclk_out = cam_de;` 和 `BUFG_PixelClk` 的输出 `O`。这是设计中的真实错误，Vivado 也会报多驱动。
  - 以前它被未知原语（`FDC_1`、`FDRE_1`、`MMCM_ADV`）和未知模块（`v_demosaic`）掩盖。
- **Sobel**：
  - 视频 DMA 和两个视频时序控制器的 HDL 已加密，用它们的仿真网表分析；
  - HLS 滤波器按接口时钟建模（18 个端口在 `ap_clk` 中）；
  - 294 条路径需要审查，因为工程没有约束 PS 的 `FCLK_CLK0` 与视频像素时钟（`axi_dynclk` 的 `pxl_clk`、HDMI 输入的 `PixelClk`）的关系；
  - 未同步的路径主要在视频 IP 的寄存器接口与像素时钟之间。
- **EV 平台**（对照）：4 个时钟，145 条跨时钟路径；`rgb2dvi` 引用的 `ResetBridge` 在仓库中缺失，报告为未解析实例。
- 三个工程加入 SystemVerilog 语料库：
  - 用非锥形稀疏检出，不下载设计检查点（`.dcp`）、VHDL 网表（`.vhdl`）和头文件；
  - `fetch_sparse` 在参数中有 `!` 排除模式时自动用 `--no-cone`。

## 3. 修改的文件

- `src/frontend/XciModels.{h,cpp}`：
  - `interfaceModel`、`XciModel::interfaceOnly`；
  - `zynqUltraPs`；
  - 组件文件中的端口时钟；
  - 综合视图的多个文件集。
- `src/frontend/AutoFrontend.cpp`：`FE-XCI-INTERFACE-MODEL` 警告。
- `src/frontend/slang_adapter/SlangFrontend.cpp`：仿真网表的 `UnknownEscapeCode`。
- `src/frontend/Primitives.cpp`：`MMCM_BASE`、`MMCM_ADV`。
- `src/frontend/primitives/xilinx_unisim.v`、`unisim_vcomponents.vhd`：负沿触发器和切片原语。
- 测试：
  - `tests/frontend/test_xci.cpp`：
    - 接口时钟模型（模型内容；设计中跨时钟路径被发现；警告级别）；负例：没有关联、输出时钟；
    - MPSoC（PL 时钟、AXI 端口、EMIO GPIO）；负例：中断和 EMIO 输入不读，没有关联时不建模；
    - 综合视图的库文件集；负例：没有库文件集时模块未知。
  - `tests/frontend/test_primitives.cpp`：
    - 负沿触发器与 `MMCM_ADV`（输出周期 20 ns；跨时钟）；负例：同一时钟的下降沿不算跨时钟；
    - 切片原语与 `SRLC16E`（经过进位链和门不算跨时钟）；
    - 仿真网表的转义；负例：其他文件照常报告。
  - Windows：`tests/frontend/test_primitives.cpp` 不再用 `std::regex::multiline`（MSVC 标准库没有）。
- `tools/ci/sv-corpus.sh`：三个 Hackster 工程；非锥形稀疏检出；Fuxi 的期望值。
- `.github/workflows/ci.yml`：Windows x64 / VS2022 作业（Debug、Release、打包），推送到 `main` 时运行。
- 文档：`docs/vivado-ip.md`（接口时钟模型、MPSoC、仿真网表转义）、`docs/primitives.md`、`README.md`。
- `CMakeLists.txt`：0.58.0。

## 4. 构建与测试结果

见交付说明。

## 5. 已知限制

- 接口时钟模型不分析 IP 内部的跨时钟路径；HLS IP 的 `ap_ctrl` 块级控制端口（实际在 `ap_clk` 中）不检查。
- MPSoC 的 PS 时钟只建模 `lpd_lsbus`；其他 PS 内部时钟上的 EMIO 外设没有时钟域。
- `MMCM_ADV` 的相位、占空比和 DRP 不建模（与 MMCME2 相同）。

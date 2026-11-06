#!/usr/bin/env bash
# SystemVerilog / Verilog regression on real open-source code, pinned to fixed commits.
#
#   tools/ci/sv-corpus.sh <bin-dir> [work-dir]
#
# Analyzes PULP's AXI clock-domain crossing through its interface wrapper
# (axi_cdc_intf: AXI_BUS interfaces with modports, packages, typedef macros,
# common_cells Gray FIFOs), alexforencich's verilog-ethernet Arty example with
# its own Vivado Tcl constraint scripts, its NexysVideo example (RGMII) with
# its Vivado scripts, its DE2-115 example with its own
# Quartus SDC (source + procedures), and the verilog-ethernet RGMII MAC with
# its asynchronous frame FIFOs (Xilinx IDDR/ODDR/BUFR/BUFIO, four clocks), and
# Analog Devices' AD9361 core as the fmcomms2 ZedBoard block design connects it
# (l_clk -> clk) with ADI's own IP constraints (toggle and reset synchronizers,
# register read-back across many synchronized status words); the CDC
# summaries must match.
set -euo pipefail

BIN=$(cd "$1" && pwd)
WORK=${2:-$(mktemp -d)}
mkdir -p "$WORK"
cd "$WORK"

fetch() { # name url commit
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    # a global Git LFS filter (git lfs install) without git-lfs on PATH must not
    # break the checkout: the analyzed sources are no LFS objects
    git -C "$1" config filter.lfs.required false
    git -C "$1" config filter.lfs.process ""
    git -C "$1" config filter.lfs.smudge cat
    git -C "$1" config filter.lfs.clean cat
    # already at the pinned commit with a clean tree: no network needed
    if [ "$(git -C "$1" rev-parse -q --verify HEAD 2>/dev/null)" = "$3" ] &&
       [ -z "$(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        return 0
    fi
    # a network hiccup (TLS or connection timeout) must not fail the run: retry
    local n
    for n in 1 2 3 4; do
        git -C "$1" fetch -q --depth 1 origin "$3" && break
        [ "$n" = 4 ] && { echo "::error::cannot fetch $2 ($3)"; return 1; }
        echo "fetch $1 failed (attempt $n), retrying in $((n * 10)) s"
        sleep $((n * 10))
    done
    git -C "$1" checkout -q -f FETCH_HEAD # -f: files of an interrupted earlier checkout
}
fetch axi          https://github.com/pulp-platform/axi          39f5f2d51c5e524f6fc5cf8b6e901f7dcc5622d7
fetch common_cells https://github.com/pulp-platform/common_cells c27bce39ebb2e6bae52f60960814a2afca7bd4cb
fetch verilog-ethernet https://github.com/alexforencich/verilog-ethernet 77320a9471d19c7dd383914bc049e02d9f4f1ffb
fetch adi-hdl      https://github.com/analogdevicesinc/hdl       c9a501d87a420d2e28aa9d931557bdb4122f3c4f
fetch arty-xadc    https://github.com/Digilent/Arty-A7-100-XADC  2511692cc01c8b38cb8aa7d492b4e6dda97eb3c9
fetch basys3-xadc  https://github.com/Digilent/Basys-3-XADC      fdfff92eca14c9e4df1ccc38317411e62a2f05d0
fetch zybo-xadc    https://github.com/Digilent/Zybo-Z7-10-XADC   a55ca0d6f669d84bf1061d7b3e5a439397698634
# only some directories of a large repository: blobs fetched for the checked-out paths only
fetch_sparse() { # name url commit dir...
    local name=$1 url=$2 commit=$3
    shift 3
    if [ ! -d "$name/.git" ]; then
        git init -q "$name"
        git -C "$name" remote add origin "$url"
    fi
    git -C "$name" config filter.lfs.required false
    git -C "$name" config filter.lfs.process ""
    git -C "$name" config filter.lfs.smudge cat
    git -C "$name" config filter.lfs.clean cat
    # directories (cone mode), or patterns with exclusions ("/dir/" "!*.dcp": non-cone mode)
    local mode=
    case " $* " in *" !"*) mode=--no-cone ;; esac
    git -C "$name" sparse-checkout set $mode "$@"
    if [ "$(git -C "$name" rev-parse -q --verify HEAD 2>/dev/null)" = "$commit" ] &&
       [ -z "$(git -C "$name" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        return 0
    fi
    local n
    for n in 1 2 3 4; do
        git -C "$name" fetch -q --depth 1 --filter=blob:none origin "$commit" && break
        [ "$n" = 4 ] && { echo "::error::cannot fetch $url ($commit)"; return 1; }
        echo "fetch $name failed (attempt $n), retrying in $((n * 10)) s"
        sleep $((n * 10))
    done
    git -C "$name" checkout -q -f FETCH_HEAD
}
# NonTrivialMIPS (MIT) in the Loongson NSCSCC AXI SoC, and its AES submodule (secworks/aes, BSD-2)
fetch_sparse nontrivial-mips https://github.com/trivialmips/nontrivial-mips 8e83643c22a3ba7612a9bb9cec93292dad618ab5 \
    loongson/soc_axi_func/rtl src
fetch nontrivial-mips/src/asic/aes https://github.com/secworks/aes a2502af731600cb20ddf6d8ed11a95804f011536
# Loongson chiplab (Mulan PSL v2): the SoC of the loongson board with open-la500 (Mulan PSL v2, the GitHub mirror of
# the gitee submodule at the recorded commit)
fetch_sparse chiplab https://github.com/loongson-community/chiplab 810be643c83a47c26c7f06de2c3be652c9f44f73 \
    IP/AMBA IP/APB_DEV IP/CONFREG IP/DEBUG IP/DMA IP/MAC IP/SPI IP/xilinx_ip/2023.2 chip/soc_demo/loongson fpga/loongson
fetch chiplab/IP/myCPU https://github.com/loongson-community/open-la500 9f42fa69b7210693ba3aa663fce33a78de3824b8
# Fuxi (GPL-3): a Vivado block design SoC (soc.bd) with its IP output products; the CPU itself is generated by
# Chisel and not in the repository, so it stays a black box
fetch_sparse Fuxi https://github.com/MaxXSoft/Fuxi 5588a4db18298a8a5ff2c3373446b46047ec7439 soc verilog
# ArtyS7-RPU-SoC (Apache-2.0): a RISC-V SoC in VHDL with five XML block designs (Vivado 2018.1: clocking wizards, block
# memories), MIG DDR3 and HDMI; the RPU core is a submodule (Apache-2.0)
fetch_sparse ArtyS7-RPU-SoC https://github.com/Domipheus/ArtyS7-RPU-SoC a2826f5ea72b12eac3c32a40d2ba641ca9e0e32b \
    ArtyS7_RPU_SoC.srcs/sources_1/bd ArtyS7_RPU_SoC.srcs/sources_1/imports ArtyS7_RPU_SoC.srcs/constrs_1 \
    ArtyS7_RPU_SoC.srcs/sources_1/ip/mig_7series_0 hdl
fetch ArtyS7-RPU-SoC/RPU https://github.com/Domipheus/RPU 4b16f9bf6f0e176bb436666e6e1501cb953436c4
# cteqeu/SoC (course projects without a license, only fetched for this check): Zynq-7000 block designs with the
# processing system, SmartConnect, AXI GPIO, XADC; Vivado 2019.2 (.srcs) and 2020.2+ (component files in .gen)
cq=cteqeu-SoC
fetch_sparse $cq https://github.com/cteqeu/SoC 8aeef06a58fd1986c3f65ef407860444508bc407 \
    Pynq-Z2/SoC_7_Timers_GIC/SoC_7_Timers_GIC.srcs/sources_1/bd Pynq-Z2/SoC_7_Timers_GIC/SoC_7_Timers_GIC.gen/sources_1/bd \
    MiniZED/SoC_Multiple_IRQ_F2P/SoC_Multiple_IRQ_F2P.srcs MiniZED/SoC_Multiple_IRQ_F2P/SoC_Multiple_IRQ_F2P.gen/sources_1/bd \
    MiniZED/eFPGA_XADC/Minized_XADC.srcs/sources_1/bd MiniZED/eFPGA_BRAM/eFPGA_BRAM.srcs/sources_1/bd \
    MiniZED/Vitis_Custom_AXI_IP2021/Vitis_Custom_AXI_IP.srcs/sources_1/bd \
    Pynq-Z2/SoC_AXI_Neopixel/SoC_AXI_NeoPixel/SoC_AXI_NeoPixel.srcs \
    Pynq-Z2/SoC_AXI_Neopixel/SoC_AXI_NeoPixel/SoC_AXI_NeoPixel.gen/sources_1/bd
# ATaylorCEngFIET/Hackster (Hackster.io projects without a license, only fetched for this check): block designs
# with the Zynq UltraScale+ MPSoC (Ultra96), MicroBlaze on Spartan-7 with a MIPI camera, a Sobel filter (HLS IP
# exported without its output products) on the Zybo Z7; design checkpoints and VHDL netlists are not fetched
hk=Hackster
fetch_sparse $hk https://github.com/ATaylorCEngFIET/Hackster 430230fc0cfca0c2108baed7470be8551cf9165c \
    /u96_linux_hackster/u96_linux_hackster.srcs/sources_1/bd/ /s7_tdm114/s7_tdm114.srcs/sources_1/bd/ \
    /Sobel_zybo_z7/block_compile.srcs/sources_1/bd/ /Sobel_zybo_z7/block_compile.srcs/constrs_1/ \
    '!*.dcp' '!*.vhdl' '!*.h'

cat > axi_cdc_top.sv <<'SV'
module top (input logic src_clk, input logic src_rst_n, input logic dst_clk, input logic dst_rst_n);
  AXI_BUS #(.AXI_ADDR_WIDTH(32), .AXI_DATA_WIDTH(32), .AXI_ID_WIDTH(4), .AXI_USER_WIDTH(1)) s ();
  AXI_BUS #(.AXI_ADDR_WIDTH(32), .AXI_DATA_WIDTH(32), .AXI_ID_WIDTH(4), .AXI_USER_WIDTH(1)) m ();
  axi_cdc_intf #(.AXI_ID_WIDTH(4), .AXI_ADDR_WIDTH(32), .AXI_DATA_WIDTH(32), .AXI_USER_WIDTH(1), .LOG_DEPTH(2))
    i_cdc (.src_clk_i(src_clk), .src_rst_ni(src_rst_n), .src(s), .dst_clk_i(dst_clk), .dst_rst_ni(dst_rst_n), .dst(m));
endmodule
SV
cat > axi_cdc.sdc <<'SDC'
create_clock -name src -period 10 [get_ports src_clk]
create_clock -name dst -period 7 [get_ports dst_clk]
set_clock_groups -asynchronous -group [get_clocks src] -group [get_clocks dst]
SDC
cat > axi_cdc.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "axi_cdc", "top": "top",
 "includeDirs": ["axi/include", "common_cells/include"],
 "sources": ["common_cells/src/cf_math_pkg.sv", "axi/src/axi_pkg.sv", "axi/src/axi_intf.sv",
  "common_cells/src/binary_to_gray.sv", "common_cells/src/gray_to_binary.sv", "common_cells/src/sync.sv",
  "common_cells/src/spill_register_flushable.sv", "common_cells/src/spill_register.sv",
  "common_cells/src/cdc_fifo_gray.sv", "axi/src/axi_cdc_src.sv", "axi/src/axi_cdc_dst.sv", "axi/src/axi_cdc.sv",
  "axi_cdc_top.sv"],
 "constraints": ["axi_cdc.sdc"]}
JSON

cat > fuxi_soc.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "fuxi_soc", "top": "SoC",
 "sources": ["Fuxi/verilog/FuxiWrapper.v", "Fuxi/soc/peripherals/ddr/ResetSynchronizer.v",
  "Fuxi/soc/peripherals/clint/AXIBridge.v", "Fuxi/soc/peripherals/clint/Clint.v",
  "Fuxi/soc/peripherals/gpio/confreg.v", "Fuxi/soc/soc.srcs/sources_1/new/iobuf.v",
  "Fuxi/soc/soc.srcs/sources_1/bd/soc/soc.bd", "Fuxi/soc/soc.srcs/sources_1/new/SoC.v"],
 "ipRepositories": ["Fuxi/soc/peripherals/lcd"],
 "constraints": ["Fuxi/soc/soc.srcs/constrs_1/new/io.xdc", "Fuxi/soc/soc.srcs/constrs_1/new/timing.xdc"],
 "options": {"unknownModules": "blackbox"}}
JSON

python3 - <<'PY'
import json
P = "ArtyS7-RPU-SoC/"
S = P + "ArtyS7_RPU_SoC.srcs/sources_1/"
src = [S + "bd/%s/%s.bd" % (b, b) for b in ["clk_video", "BRAM_64KB", "clk_main", "BRAM_trace", "BRAM_cache"]]
src += [P + "hdl/BRAM_64KB_wrapper.vhd", S + "imports/hdl/BRAM_trace_wrapper.vhd", S + "imports/hdl/clk_main_wrapper.vhd",
        S + "imports/hdl/clk_video_wrapper.vhd", S + "bd/BRAM_cache/hdl/BRAM_cache_wrapper.vhd"]
src += [P + "hdl/soc/gfx/" + f for f in ["TDMS_encoder.vhd", "char_generator.vhd", "dvid.vhd", "test_font_ram.vhd",
                                        "vga_gen.vhd"]]
src += [P + "RPU/vhdl/" + f for f in ["constants.vhd", "control_unit.vhd", "core.vhd", "csr_unit.vhd", "lint_unit.vhd",
                                     "mem_controller.vhd", "pc_unit.vhd", "register_set.vhd", "unit_alu_RV32_I.vhd",
                                     "unit_decoder_RV32I.vhd", "alu_int32_div.vhd"]]
src += [P + "hdl/soc/spi_master.vhd", P + "hdl/xilinx/uart_rx6.vhd", P + "hdl/rpu_top.vhd", P + "hdl/soc/clocking.vhd",
        P + "hdl/cache.vhd", S + "ip/mig_7series_0/mig_7series_0.xci"]
json.dump({"format": "opencdc-project", "version": 1, "name": "artys7_rpu_soc", "top": "rpu_top", "sources": src,
           "constraints": [P + "ArtyS7_RPU_SoC.srcs/constrs_1/imports/Arty-S7-50-Rev-B-Master.xdc"]},
          open("artys7_rpu_soc.ocdc.json", "w"), indent=1)
PY

python3 - <<'PY'
import json
def proj(name, bd, wrapper, xdc=None):
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": "design_1_wrapper",
               "sources": [bd, wrapper], "constraints": [xdc] if xdc else []}, open(name + ".ocdc.json", "w"), indent=1)
q = "cteqeu-SoC/"
t7 = q + "Pynq-Z2/SoC_7_Timers_GIC/SoC_7_Timers_GIC."
proj("zynq_timers_gic", t7 + "srcs/sources_1/bd/design_1/design_1.bd", t7 + "gen/sources_1/bd/design_1/hdl/design_1_wrapper.vhd")
ir = q + "MiniZED/SoC_Multiple_IRQ_F2P/SoC_Multiple_IRQ_F2P."
proj("zynq_irq_f2p", ir + "srcs/sources_1/bd/design_1/design_1.bd", ir + "gen/sources_1/bd/design_1/hdl/design_1_wrapper.vhd",
     ir + "srcs/constrs_1/new/SoC_Multiple_IRQ_F2P.xdc")
xa = q + "MiniZED/eFPGA_XADC/Minized_XADC.srcs/sources_1/bd/design_1/"
proj("zynq_xadc", xa + "design_1.bd", xa + "hdl/design_1_wrapper.vhd")
br = q + "MiniZED/eFPGA_BRAM/eFPGA_BRAM.srcs/sources_1/bd/design_1/"
proj("zynq_bram", br + "design_1.bd", br + "hdl/design_1_wrapper.vhd")
# user IP whose repository is not in the project: from their generated HDL (synth/ wrapper + ipshared/)
ma = q + "MiniZED/Vitis_Custom_AXI_IP2021/Vitis_Custom_AXI_IP.srcs/sources_1/bd/design_1/"
proj("zynq_math_ip", ma + "design_1.bd", ma + "hdl/design_1_wrapper.vhd")
np = q + "Pynq-Z2/SoC_AXI_Neopixel/SoC_AXI_NeoPixel/SoC_AXI_NeoPixel."
proj("zynq_axi_neopixel", np + "srcs/sources_1/bd/design_1/design_1.bd",
     np + "gen/sources_1/bd/design_1/hdl/design_1_wrapper.vhd", np + "srcs/constrs_1/new/SoC_AXI_NeoPixel.xdc")
h = "Hackster/"
u9 = h + "u96_linux_hackster/u96_linux_hackster.srcs/sources_1/bd/design_1/"
proj("u96_mpsoc", u9 + "design_1.bd", u9 + "hdl/design_1_wrapper.vhd")
s7 = h + "s7_tdm114/s7_tdm114.srcs/sources_1/bd/design_1/"
proj("s7_mipi_microblaze", s7 + "design_1.bd", s7 + "hdl/design_1_wrapper.vhd")
sb = h + "Sobel_zybo_z7/block_compile.srcs/"
proj("zybo_sobel", sb + "sources_1/bd/design_1/design_1.bd", sb + "sources_1/bd/design_1/hdl/design_1_wrapper.vhd",
     sb + "constrs_1/new/cosnt.xdc")
PY

cat > rgmii.sdc <<'SDC'
create_clock -name gtx_clk -period 8 [get_ports gtx_clk]
create_generated_clock -name gtx_clk90 -source [get_ports gtx_clk] -divide_by 1 [get_ports gtx_clk90]
create_clock -name logic_clk -period 6.4 [get_ports logic_clk]
create_clock -name rgmii_rx_clk -period 8 [get_ports rgmii_rx_clk]
set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks gtx_clk] -group logic_clk -group rgmii_rx_clk
SDC
cat > rgmii.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "rgmii", "top": "eth_mac_1g_rgmii_fifo",
 "sources": ["verilog-ethernet/rtl/eth_mac_1g_rgmii_fifo.v", "verilog-ethernet/rtl/eth_mac_1g_rgmii.v",
  "verilog-ethernet/rtl/rgmii_phy_if.v", "verilog-ethernet/rtl/ssio_ddr_in.v", "verilog-ethernet/rtl/oddr.v",
  "verilog-ethernet/rtl/iddr.v", "verilog-ethernet/rtl/eth_mac_1g.v", "verilog-ethernet/rtl/axis_gmii_rx.v",
  "verilog-ethernet/rtl/axis_gmii_tx.v", "verilog-ethernet/rtl/lfsr.v", "verilog-ethernet/rtl/mac_ctrl_rx.v",
  "verilog-ethernet/rtl/mac_ctrl_tx.v", "verilog-ethernet/rtl/mac_pause_ctrl_rx.v",
  "verilog-ethernet/rtl/mac_pause_ctrl_tx.v", "verilog-ethernet/lib/axis/rtl/axis_async_fifo_adapter.v",
  "verilog-ethernet/lib/axis/rtl/axis_async_fifo.v", "verilog-ethernet/lib/axis/rtl/axis_adapter.v"],
 "parameters": {"TARGET": "\"XILINX\"", "IODDR_STYLE": "\"IODDR\"", "CLOCK_INPUT_STYLE": "\"BUFR\""},
 "constraints": ["rgmii.sdc"]}
JSON

# verilog-ethernet's Arty example (MMCM, MII, UDP/IP) with the project's own
# Vivado Tcl constraint scripts (foreach over instances by REF_NAME, -regexp
# filters, netlist traversal of reset sources, per-crossing max delays)
cat > arty_eth.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "arty_eth", "top": "fpga",
 "sources": [
 "verilog-ethernet/example/Arty/fpga/rtl/fpga.v", "verilog-ethernet/example/Arty/fpga/rtl/fpga_core.v",
  "verilog-ethernet/example/Arty/fpga/rtl/debounce_switch.v",
  "verilog-ethernet/example/Arty/fpga/rtl/sync_signal.v", "verilog-ethernet/rtl/ssio_sdr_in.v",
  "verilog-ethernet/rtl/mii_phy_if.v", "verilog-ethernet/rtl/eth_mac_mii_fifo.v",
  "verilog-ethernet/rtl/eth_mac_mii.v", "verilog-ethernet/rtl/eth_mac_1g.v",
  "verilog-ethernet/rtl/axis_gmii_rx.v", "verilog-ethernet/rtl/axis_gmii_tx.v", "verilog-ethernet/rtl/lfsr.v",
  "verilog-ethernet/rtl/eth_axis_rx.v", "verilog-ethernet/rtl/eth_axis_tx.v",
  "verilog-ethernet/rtl/udp_complete.v", "verilog-ethernet/rtl/udp_checksum_gen.v",
  "verilog-ethernet/rtl/udp.v", "verilog-ethernet/rtl/udp_ip_rx.v", "verilog-ethernet/rtl/udp_ip_tx.v",
  "verilog-ethernet/rtl/ip_complete.v", "verilog-ethernet/rtl/ip.v", "verilog-ethernet/rtl/ip_eth_rx.v",
  "verilog-ethernet/rtl/ip_eth_tx.v", "verilog-ethernet/rtl/ip_arb_mux.v", "verilog-ethernet/rtl/arp.v",
  "verilog-ethernet/rtl/arp_cache.v", "verilog-ethernet/rtl/arp_eth_rx.v", "verilog-ethernet/rtl/arp_eth_tx.v",
  "verilog-ethernet/rtl/eth_arb_mux.v", "verilog-ethernet/lib/axis/rtl/arbiter.v",
  "verilog-ethernet/lib/axis/rtl/priority_encoder.v", "verilog-ethernet/lib/axis/rtl/axis_fifo.v",
  "verilog-ethernet/lib/axis/rtl/axis_async_fifo.v", "verilog-ethernet/lib/axis/rtl/axis_async_fifo_adapter.v",
  "verilog-ethernet/lib/axis/rtl/sync_reset.v"],
 "constraints": [
 "verilog-ethernet/example/Arty/fpga/fpga.xdc", "verilog-ethernet/syn/vivado/mii_phy_if.tcl",
  "verilog-ethernet/syn/vivado/eth_mac_fifo.tcl", "verilog-ethernet/lib/axis/syn/vivado/axis_async_fifo.tcl",
  "verilog-ethernet/lib/axis/syn/vivado/sync_reset.tcl"]}
JSON

# verilog-ethernet's DE2-115 example (Cyclone IV: altpll, altddio) with the
# project's own Quartus SDC: `source` of the library scripts and calls of the
# procedures they define, get_registers with '|' names, a PLL output pin named
# inside the IP
cat > de2_115.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "de2_115", "top": "fpga",
 "sources": [
 "verilog-ethernet/example/DE2-115/fpga/rtl/fpga.v", "verilog-ethernet/example/DE2-115/fpga/rtl/fpga_core.v",
  "verilog-ethernet/example/DE2-115/fpga/rtl/debounce_switch.v",
  "verilog-ethernet/example/DE2-115/fpga/rtl/sync_signal.v",
  "verilog-ethernet/example/DE2-115/fpga/rtl/hex_display.v", "verilog-ethernet/rtl/iddr.v",
  "verilog-ethernet/rtl/oddr.v", "verilog-ethernet/rtl/ssio_ddr_in.v", "verilog-ethernet/rtl/ssio_ddr_out.v",
  "verilog-ethernet/rtl/rgmii_phy_if.v", "verilog-ethernet/rtl/eth_mac_1g_rgmii_fifo.v",
  "verilog-ethernet/rtl/eth_mac_1g_rgmii.v", "verilog-ethernet/rtl/eth_mac_1g.v",
  "verilog-ethernet/rtl/axis_gmii_rx.v", "verilog-ethernet/rtl/axis_gmii_tx.v", "verilog-ethernet/rtl/lfsr.v",
  "verilog-ethernet/rtl/eth_axis_rx.v", "verilog-ethernet/rtl/eth_axis_tx.v",
  "verilog-ethernet/rtl/udp_complete.v", "verilog-ethernet/rtl/udp_checksum_gen.v",
  "verilog-ethernet/rtl/udp.v", "verilog-ethernet/rtl/udp_ip_rx.v", "verilog-ethernet/rtl/udp_ip_tx.v",
  "verilog-ethernet/rtl/ip_complete.v", "verilog-ethernet/rtl/ip.v", "verilog-ethernet/rtl/ip_eth_rx.v",
  "verilog-ethernet/rtl/ip_eth_tx.v", "verilog-ethernet/rtl/ip_arb_mux.v", "verilog-ethernet/rtl/ip_mux.v",
  "verilog-ethernet/rtl/arp.v", "verilog-ethernet/rtl/arp_cache.v", "verilog-ethernet/rtl/arp_eth_rx.v",
  "verilog-ethernet/rtl/arp_eth_tx.v", "verilog-ethernet/rtl/eth_arb_mux.v", "verilog-ethernet/rtl/eth_mux.v",
  "verilog-ethernet/lib/axis/rtl/arbiter.v", "verilog-ethernet/lib/axis/rtl/priority_encoder.v",
  "verilog-ethernet/lib/axis/rtl/axis_fifo.v", "verilog-ethernet/lib/axis/rtl/axis_async_fifo.v",
  "verilog-ethernet/lib/axis/rtl/axis_async_fifo_adapter.v", "verilog-ethernet/lib/axis/rtl/sync_reset.v"],
 "constraints": ["verilog-ethernet/example/DE2-115/fpga/fpga.sdc"],
 "tclSourceDirs": ["verilog-ethernet/example/DE2-115/fpga/fpga"]}
JSON

# verilog-ethernet's NexysVideo example (RGMII: IDDR/ODDR in unnamed generate
# blocks, IDELAYE2 in FIXED mode, MMCM) with its own Vivado Tcl scripts
cat > nexys_video.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "nexys_video", "top": "fpga",
 "sources": [
 "verilog-ethernet/example/NexysVideo/fpga/rtl/fpga.v",
  "verilog-ethernet/example/NexysVideo/fpga/rtl/fpga_core.v",
  "verilog-ethernet/example/NexysVideo/fpga/rtl/debounce_switch.v",
  "verilog-ethernet/example/NexysVideo/fpga/rtl/sync_signal.v", "verilog-ethernet/rtl/iddr.v",
  "verilog-ethernet/rtl/oddr.v", "verilog-ethernet/rtl/ssio_ddr_in.v", "verilog-ethernet/rtl/ssio_ddr_out.v",
  "verilog-ethernet/rtl/rgmii_phy_if.v", "verilog-ethernet/rtl/eth_mac_1g_rgmii_fifo.v",
  "verilog-ethernet/rtl/eth_mac_1g_rgmii.v", "verilog-ethernet/rtl/eth_mac_1g.v",
  "verilog-ethernet/rtl/axis_gmii_rx.v", "verilog-ethernet/rtl/axis_gmii_tx.v", "verilog-ethernet/rtl/lfsr.v",
  "verilog-ethernet/rtl/eth_axis_rx.v", "verilog-ethernet/rtl/eth_axis_tx.v",
  "verilog-ethernet/rtl/udp_complete.v", "verilog-ethernet/rtl/udp_checksum_gen.v",
  "verilog-ethernet/rtl/udp.v", "verilog-ethernet/rtl/udp_ip_rx.v", "verilog-ethernet/rtl/udp_ip_tx.v",
  "verilog-ethernet/rtl/ip_complete.v", "verilog-ethernet/rtl/ip.v", "verilog-ethernet/rtl/ip_eth_rx.v",
  "verilog-ethernet/rtl/ip_eth_tx.v", "verilog-ethernet/rtl/ip_arb_mux.v", "verilog-ethernet/rtl/arp.v",
  "verilog-ethernet/rtl/arp_cache.v", "verilog-ethernet/rtl/arp_eth_rx.v", "verilog-ethernet/rtl/arp_eth_tx.v",
  "verilog-ethernet/rtl/eth_arb_mux.v", "verilog-ethernet/lib/axis/rtl/arbiter.v",
  "verilog-ethernet/lib/axis/rtl/priority_encoder.v", "verilog-ethernet/lib/axis/rtl/axis_fifo.v",
  "verilog-ethernet/lib/axis/rtl/axis_async_fifo.v", "verilog-ethernet/lib/axis/rtl/axis_async_fifo_adapter.v",
  "verilog-ethernet/lib/axis/rtl/sync_reset.v"],
 "constraints": [
 "verilog-ethernet/example/NexysVideo/fpga/fpga.xdc", "verilog-ethernet/example/NexysVideo/fpga/eth.xdc",
  "verilog-ethernet/syn/vivado/rgmii_phy_if.tcl", "verilog-ethernet/syn/vivado/eth_mac_1g_rgmii.tcl",
  "verilog-ethernet/syn/vivado/eth_mac_fifo.tcl", "verilog-ethernet/lib/axis/syn/vivado/axis_async_fifo.tcl",
  "verilog-ethernet/lib/axis/syn/vivado/sync_reset.tcl"]}
JSON

# fmcomms2 (projects/fmcomms2/common/fmcomms2_bd.tcl): "ad_connect axi_ad9361/l_clk axi_ad9361/clk"
python3 - <<'PY'
import re
src = open("adi-hdl/library/axi_ad9361/axi_ad9361.v").read()
hdr = src[src.index("module axi_ad9361"):]
hdr = hdr[:hdr.index(");")]
decl, conn = [], []
for line in hdr.splitlines():
    line = re.sub(r"//.*", "", line).strip().rstrip(",")
    m = re.match(r"(input|output|inout)\s*(\[[^\]]*\])?\s*(\w+)$", line)
    if not m:
        continue
    d, w, n = m.group(1), m.group(2) or "", m.group(3)
    if n == "clk":
        conn.append("    .clk (l_clk)")
        continue
    decl.append(f"  {d} {w} {n}")
    conn.append(f"    .{n} ({n})")
open("fmcomms2_sys.v", "w").write(
    "module fmcomms2_sys (\n" + ",\n".join(decl) + ");\n"
    "  axi_ad9361 #(.ID(0)) i_axi_ad9361 (\n" + ",\n".join(conn) + ");\nendmodule\n")
PY
cat > ps7_clocks.xdc <<'XDC'
# clocks the Zynq PS7 IP creates on FCLK_CLK0 (sys_cpu_clk) and FCLK_CLK1 (IODELAY reference)
create_clock -name clk_fpga_0 -period 10 [get_ports s_axi_aclk]
create_clock -name clk_fpga_1 -period 5 [get_ports delay_clk]
XDC
cat > fmcomms2.ocdc.json <<'JSON'
{"format": "opencdc-project", "version": 1, "name": "fmcomms2_zed", "top": "fmcomms2_sys",
 "sources": ["adi-hdl/library/common/ad_rst.v", "adi-hdl/library/xilinx/common/ad_data_clk.v",
  "adi-hdl/library/xilinx/common/ad_data_in.v", "adi-hdl/library/xilinx/common/ad_data_out.v",
  "adi-hdl/library/xilinx/common/ad_dcfilter.v", "adi-hdl/library/xilinx/common/ad_mul.v",
  "adi-hdl/library/common/ad_pnmon.v", "adi-hdl/library/common/ad_dds_cordic_pipe.v",
  "adi-hdl/library/common/ad_dds_sine_cordic.v", "adi-hdl/library/common/ad_dds_sine.v",
  "adi-hdl/library/common/ad_dds_2.v", "adi-hdl/library/common/ad_dds_1.v", "adi-hdl/library/common/ad_dds.v",
  "adi-hdl/library/common/ad_datafmt.v", "adi-hdl/library/common/ad_iqcor.v", "adi-hdl/library/common/ad_addsub.v",
  "adi-hdl/library/common/ad_tdd_control.v", "adi-hdl/library/common/ad_pps_receiver.v",
  "adi-hdl/library/common/up_axi.v", "adi-hdl/library/common/up_xfer_cntrl.v",
  "adi-hdl/library/common/up_xfer_status.v", "adi-hdl/library/common/up_clock_mon.v",
  "adi-hdl/library/common/up_delay_cntrl.v", "adi-hdl/library/common/up_adc_common.v",
  "adi-hdl/library/common/up_adc_channel.v", "adi-hdl/library/common/up_dac_common.v",
  "adi-hdl/library/common/up_dac_channel.v", "adi-hdl/library/common/up_tdd_cntrl.v",
  "adi-hdl/library/axi_ad9361/xilinx/axi_ad9361_lvds_if.v", "adi-hdl/library/axi_ad9361/xilinx/axi_ad9361_cmos_if.v",
  "adi-hdl/library/axi_ad9361/axi_ad9361_rx_pnmon.v", "adi-hdl/library/axi_ad9361/axi_ad9361_rx_channel.v",
  "adi-hdl/library/axi_ad9361/axi_ad9361_rx.v", "adi-hdl/library/axi_ad9361/axi_ad9361_tx_channel.v",
  "adi-hdl/library/axi_ad9361/axi_ad9361_tx.v", "adi-hdl/library/axi_ad9361/axi_ad9361_tdd.v",
  "adi-hdl/library/axi_ad9361/axi_ad9361_tdd_if.v", "adi-hdl/library/axi_ad9361/axi_ad9361.v",
  "fmcomms2_sys.v"],
 "constraints": ["ps7_clocks.xdc",
  "adi-hdl/library/xilinx/common/up_xfer_cntrl_constr.xdc", "adi-hdl/library/xilinx/common/ad_rst_constr.xdc",
  "adi-hdl/library/xilinx/common/up_xfer_status_constr.xdc", "adi-hdl/library/xilinx/common/up_clock_mon_constr.xdc",
  "adi-hdl/library/axi_ad9361/axi_ad9361_constr.xdc", "adi-hdl/projects/fmcomms2/zed/system_constr.xdc"]}
JSON

fail=0
# Loongson NSCSCC soc_axi_func with NonTrivialMIPS: the CPU in cpu_clk (50 MHz), the AXI RAM and confreg in sys_clk
# (100 MHz) behind the AXI Clock Converter; the converter, both crossbars, the AXI block RAM and the floating point
# IP modelled from their XML .xci; set_clock_groups as the project's own constraints (false paths between them)
nt=nontrivial-mips
nr=$nt/loongson/soc_axi_func/rtl
cat > soc_axi_func.sdc <<'SDC'
create_clock -name clk -period 10 [get_ports clk]
set_clock_groups -asynchronous -group [get_clocks -of_objects [get_pins pll.clk_pll/cpu_clk]] -group [get_clocks -of_objects [get_pins pll.clk_pll/sys_clk]]
SDC
python3 - "$nt" "$nr" <<'PY'
import glob, json, sys
nt, nr = sys.argv[1], sys.argv[2]
src = sorted(glob.glob(nt + "/src/**/*.sv", recursive=True)) + \
      sorted(p for p in glob.glob(nt + "/src/**/*.v", recursive=True) if "/toolruns/" not in p and "/tb/" not in p) + \
      [nr + "/soc_axi_lite_top.v", nr + "/CONFREG/confreg.v", nr + "/axi_wrap/axi_wrap.v", nr + "/ram_wrap/axi_wrap_ram.v",
       nr + "/myCPU/mycpu_top.v"] + sorted(glob.glob(nr + "/**/*.xci", recursive=True)) + \
      sorted(glob.glob(nt + "/src/**/*.xci", recursive=True))
json.dump({"format": "opencdc-project", "version": 1, "name": "soc_axi_func", "top": "soc_axi_lite_top", "sources": src,
           "includeDirs": [nt + "/src", nt + "/src/cpu", nt + "/src/utils"], "constraints": ["soc_axi_func.sdc"]},
          open("soc_axi_func.ocdc.json", "w"), indent=1)
PY
# chiplab soc_top (loongson board): the source list of the Vivado project (system_run.xpr), the CPU sources and its
# block RAM .xcix, the project's own constraints (soc_up.xdc, with Vivado's clock names clk_pll_i and clk_out2_clk_pll_33)
python3 - chiplab <<'PY'
import glob, json, re, sys
c = sys.argv[1]
x = open(c + "/fpga/loongson/2023.2/system_run.xpr").read()
paths = sorted(set(p.replace("$PPRDIR/../../../", c + "/") for p in re.findall(r'Path="([^"]*)"', x)))
src = [p for p in paths if p.endswith((".v", ".xci", ".xcix")) and
       not p.endswith("axi_clock_converter_0/axi_clock_converter_0/axi_clock_converter_0.xci")]
src += sorted(glob.glob(c + "/IP/myCPU/*.v")) + sorted(glob.glob(c + "/IP/myCPU/IP/*.xcix"))
json.dump({"format": "opencdc-project", "version": 1, "name": "chiplab_soc", "top": "soc_top", "sources": src,
           "includeDirs": [c + "/chip/soc_demo/loongson", c + "/IP/myCPU", c + "/IP/APB_DEV/URT"],
           "constraints": [c + "/fpga/loongson/soc_up.xdc"]}, open("chiplab_soc.ocdc.json", "w"), indent=1)
PY
check() { # project expected-regex
    local out
    out=$("$BIN/opencdc" analyze "$1" -q 2>&1 || true)
    # grep reads a here-string: with pipefail, `echo | grep -q` fails when grep
    # stops reading early (SIGPIPE in echo), which made matching runs fail
    if grep -Eq "$2" <<<"${out//$'\n'/ }"; then
        echo "ok   $1: $(echo "$out" | grep -E '^CDC     :')"
    else
        echo "::error::$1 does not match '$2'"; echo "$out" | tail -30; fail=1
    fi
}
check axi_cdc.ocdc.json "80 registers.*0 unsupported construct.*80/80 registers resolved.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 52 crossing path\(s\), 50 synchronized.*Rules   : 0 error violation"
check rgmii.ocdc.json "206/206 registers resolved.*async-fifo x2.*built-in models used for vendor primitives: BUFIO, BUFR, IDDR, ODDR.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 30 crossing path\(s\), 29 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 1 path\(s\) at potential risk, 1 path\(s\) need review"
check arty_eth.ocdc.json "608/608 registers resolved.*Summary : 0 error\(s\), 9 warning\(s\).*CDC     : 27 crossing path\(s\), 27 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 0 path\(s\) at potential risk, 0 path\(s\) need review"
check de2_115.ocdc.json "Clocks  : 13 \(618/618 registers resolved\).*enet0_tx_clk_125m  \[generated\] source=fpga.ENET0_GTX_CLK master=fpga.altpll_component.clk\[0\].*CDC-SOURCE-SYNC.*Summary : 0 error\(s\), 18 warning\(s\).*CDC     : 16 crossing path\(s\), 16 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 2 path\(s\) at potential risk, 0 path\(s\) need review"
check nexys_video.ocdc.json "Clocks  : 4 \(645/645 registers resolved\).*built-in models used for vendor primitives: BUFG, BUFIO, BUFR, IBUFG, IDDR, IDELAYCTRL, IDELAYE2, MMCME2_BASE, ODDR.*Summary : 0 error\(s\), 9 warning\(s\).*CDC     : 16 crossing path\(s\), 16 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 2 path\(s\) at potential risk, 0 path\(s\) need review"
check fmcomms2.ocdc.json "Clocks  : 3 \(1667/1667 registers resolved\).*built-in models used for vendor primitives: BUFG, DSP48E1, IBUFDS, IBUFGDS, IDELAYCTRL, MULT_MACRO, OBUF, OBUFDS.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 78 crossing path\(s\), 77 synchronized, 1 unsynchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 21 path\(s\) at potential risk, 0 path\(s\) need review"
# Digilent XADC demos: XADC Wizard given as .xci (DRP in dclk_in)
for p in "arty_xadc XADCdemo arty-xadc" "basys3_xadc XADCdemo basys3-xadc" "zybo_xadc top zybo-xadc"; do
    set -- $p
    python3 - "$1" "$2" "$3" <<'PY'
import glob, json, sys
name, top, d = sys.argv[1:]
json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": top,
           "sources": sorted(glob.glob(d + "/src/hdl/*.v")) + sorted(glob.glob(d + "/src/ip/*/*.xci")),
           "constraints": sorted(glob.glob(d + "/src/constraints/*.xdc"))}, open(name + ".ocdc.json", "w"))
PY
done
check soc_axi_func.ocdc.json "Clocks  : 3 \(991/991 registers resolved\).*CLKOUT0  \[divider\] alias=cpu_clk_clk_pll .*period=20ns.*axi_clock_converter:2.1.*AXI3, s_axi_aclk to m_axi_aclk, asynchronous clocks.*IP 'axi_crossbar_1x2' .*AXI3, 1 x 2, one clock.*blk_mem_gen:8.4.*AXI4 slave, 262144 x 32.*floating_point:7.1.*Summary : 0 error\(s\), 6 warning\(s\).*CDC     : 0 crossing path"
# chiplab: the CPU (50 MHz) and the uncore (33 MHz) only through the AXI clock converter (.xcix), the DDR3 behind the
# AXI interconnect in the MIG's ui_clk; the 60 unsynchronized paths are the Ethernet MAC's one-flop handshakes between
# the uncore clock and the MII clocks (stop_r <= stopi, focl_r <= focl, ...)
check chiplab_soc.ocdc.json "0 unsupported construct.*Clocks  : 6 \(1293/1293 registers resolved\).*ui_clk  \[divider\] alias=c1_clk0 alias=clk_pll_i.*CLKOUT1  \[divider\] alias=clk_out2_clk_pll_33 .*period=30.303ns.*axi_clock_converter:2.1.*axi_interconnect:1.7.*mig_7series:4.2.*AXI4 interface.*data_bank_sram.xcix.*FE-OUTPUT-PORT-CONSTANT.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 132 crossing path\(s\), 72 synchronized.*Rules   : 36 error violation\(s\), 24 warning violation"
# Fuxi: the block design (45 cells, 22 hierarchical cells as modules) with all IP but the TFT controller modelled; every
# crossing between the clocks happens inside the IP (clock converters, Quad SPI FIFOs, MIG reset synchronizer)
check fuxi_soc.ocdc.json "Clocks  : 9 \(1134/1134 registers resolved\).*ui_clk  \[divider\] alias=ddr_controller_ui_clk alias=clk_pll_i.*CLKOUT0  \[divider\] alias=clk_cpu_soc_clk_wiz_0_0 .*period=20ns.*FE-BD-MODULE.*45 cell\(s\), 22 hierarchical cell\(s\), 41 IP configuration\(s\).*axi_quad_spi:3.2.*AXI4 memory interface on s_axi4_aclk.*axi_intc:4.1.*4 interrupt input\(s\) \(1 asynchronous\).*proc_sys_reset:5.0.*axi_tft:2.0\) known only by the clocks of its interfaces: 54 port\(s\) of its bus interfaces in 3 clock\(s\).*Summary : 0 error\(s\), 7 warning\(s\).*CDC     : 1 crossing path\(s\), 1 synchronized, 0 unsynchronized"
# Zynq-7000 block designs: one fabric clock (clk_fpga_0, Vivado's name, period from the PS configuration); the AXI
# peripherals behind the PS's M_AXI_GP0 in it; the EMIO GPIO outputs in the PS clock cpu_1x
check zynq_timers_gic.ocdc.json "Clocks  : 1 \(99/99 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=10ns.*FE-BD-MODULE.*7 cell\(s\), 4 hierarchical cell\(s\), 7 IP configuration\(s\).*processing_system7:5.5.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zynq_irq_f2p.ocdc.json "Clocks  : 2 \(98/98 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=20ns.*cpu_1x  \[black-box\].*FE-BD-MODULE.*7 cell\(s\).*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zynq_xadc.ocdc.json "Clocks  : 2 \(82/82 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=20ns.*FE-BD-MODULE.*4 cell\(s\).*xadc_wiz:3.3.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zynq_bram.ocdc.json "Clocks  : 2 \(118/118 registers resolved\).*FE-BD-MODULE.*6 cell\(s\), 6 IP configuration\(s\).*smartconnect:1.0.*1 clock\(s\): aclk: M00_AXI, M01_AXI, S00_AXI.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zynq_math_ip.ocdc.json "Clocks  : 2 \(74/74 registers resolved\).*FE-BD-MODULE.*Math_IP:1.0\) analyzed from its output products: its generated HDL \(synthesis view, 4 file\(s\)\).*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
# AXI_NeoPixel (generated HDL): the AXI registers (clk_fpga_0, 100 MHz) read straight by the LED logic in clk_fpga_1
# (50 MHz): 64 registers x 24 bits without synchronization (the relation of the two PS clocks is not constrained)
check zynq_axi_neopixel.ocdc.json "Clocks  : 2 \(179/179 registers resolved\).*FCLK_CLK1  \[black-box\] alias=clk_fpga_1 .*period=20ns.*AXI_NeoPixel:1.0\) analyzed from its output products.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 64 crossing path\(s\), 0 synchronized, 64 unsynchronized.*64 path\(s\) need review"
# ArtyS7-RPU-SoC: the XML block designs; the board XDC names both oscillators sys_clk_pin (the 100 MHz one twice), so
# as in Vivado the 12 MHz clock is overridden and CLK12MHZ is an unconstrained clock; the CPU (clk_core, from the
# 100 MHz MMCM) and the trace logic in CLK12MHZ exchange trace_dump_enabled / memory address and write enable without
# synchronization; the MIG with the ports of its stub (128-bit user data, device_temp and device_temp_i)
check artys7_rpu_soc.ocdc.json "Clocks  : 6 \(369/369 registers resolved\).*sys_clk_pin  \[constraint\] source=rpu_top.CLK100MHZ period=10ns.*CLK12MHZ  \[primary\].*alias=clk_core_clk_main_clk_wiz_0_0.*alias=clk_pixel_clk_video_clk_wiz_0_0 .*period=13.468ns.*FE-BD-MODULE.*block design 'clk_video'.*mig_7series:4.1.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 10 crossing path\(s\), 0 synchronized, 10 unsynchronized.*10 path\(s\) need review"
check arty_xadc.ocdc.json "Clocks  : 1 \(14/14 registers resolved\).*IP 'xadc_wiz_0' \(xilinx.com:ip:xadc_wiz:3.3\) modelled.*DRP interface.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check basys3_xadc.ocdc.json "Clocks  : 2 \(24/24 registers resolved\).*IP 'xadc_wiz_0'.*DRP interface.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zybo_xadc.ocdc.json "Clocks  : 1 \(19/19 registers resolved\).*IP 'xadc_wiz_0'.*DRP interface.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 0 crossing path"
# Zynq UltraScale+ MPSoC: pl_clk0 with Vivado's name clk_pl_0, the AXI ports in their interface clocks
check u96_mpsoc.ocdc.json "Clocks  : 1 \(93/93 registers resolved\).*zynq_ultra_ps_e_0.pl_clk0  \[black-box\] alias=clk_pl_0 .*period=10ns.*zynq_ultra_ps_e:3.2\) modelled from its configuration: PL clocks pl_clk0.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
# MicroBlaze (simulation netlist: slice gates, SRLC16E), its debug module (FDC_1/FDRE_1), MMCM_ADV, an encrypted IP known by
# its interface clocks; the camera interface drives pclk_out twice (an assignment and a BUFG): a real error
check s7_mipi_microblaze.ocdc.json "v_demosaic:1.0\) known only by the clocks of its interfaces: 35 port\(s\).*error\[IR-MULTIDRIVER\]: 'pclk_out' bit \[0\] in module 'CAM_interface' has multiple drivers.*Summary : 1 error\(s\), 1 warning\(s\).*CDC analysis: not run"
# the Sobel filter (an HLS IP exported without output products) known by its interface clock; the video DMA and
# timing controllers from their simulation netlists (their HDL is encrypted)
check zybo_sobel.ocdc.json "Clocks  : 7 \(11735/11735 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=5ns.*axi_vdma:6.3\) analyzed from its output products: its simulation netlist.*image_filter:1.0\) known only by the clocks of its interfaces: 18 port\(s\) of its bus interfaces in 1 clock\(s\) \(ap_clk\).*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 295 crossing path\(s\), 238 synchronized, 57 unsynchronized.*294 path\(s\) need review"
exit $fail

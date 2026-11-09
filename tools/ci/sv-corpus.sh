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
# MiSTer-devel/Template_MiSTer (GPL-2.0): the MiSTer framework (DE10-Nano, Cyclone V with its HPS) around a
# template core, a Quartus project; its pre-flow script writes build_id.v (the build date), done here as it does
fetch Template_MiSTer https://github.com/MiSTer-devel/Template_MiSTer e901a9fa6548f4eb024619357b89d48be94fd9f8
printf '`define BUILD_DATE "261108"' > Template_MiSTer/build_id.v
# MiSTer-devel/NES_MiSTer (GPL-3.0, only fetched for this check): a whole core on the MiSTer framework,
# SystemVerilog with VHDL (savestates, the VM2413 sound chip), parameters without a default value set
# through two levels of defparams; rtl/ and sys/ only (not the released bitstreams)
fetch_sparse NES_MiSTer https://github.com/MiSTer-devel/NES_MiSTer 49a0a662e244469ca77b2155746a066df704ffae rtl sys
printf '`define BUILD_DATE "261108"' > NES_MiSTer/build_id.v
# MiSTer-devel/Genesis_MiSTer (GPL-3.0, only fetched for this check): a 68000 (FX68K, SystemVerilog with
# tasks and unique case), a Z80 (T80, VHDL with concurrent procedure calls), the VDP (VHDL) and Intel MLAB RAM
# (altdpram); rtl/ and sys/ only
fetch_sparse Genesis_MiSTer https://github.com/MiSTer-devel/Genesis_MiSTer adc0c42cfb1fa5d484cc8566767f7d68982bc44a rtl sys
printf '`define BUILD_DATE "261108"' > Genesis_MiSTer/build_id.v
# MiSTer-devel/SNES_MiSTer (GPL-3.0, only fetched for this check): the 65C816, PPU and DSP (VHDL) with the
# cartridge chips (CX4, SA1, GSU, SPC7110: lpm_mult, lpm_divide, scfifo), if / else if chains without a final
# else, a type conversion in a port map formal; rtl/ and sys/ only
fetch_sparse SNES_MiSTer https://github.com/MiSTer-devel/SNES_MiSTer 3ac236af3013a4c96bdaf11b5a917a117f25dff6 rtl sys
printf '`define BUILD_DATE "261108"' > SNES_MiSTer/build_id.v
# MiSTer-devel/ao486_MiSTer (rtl/ BSD, sys/ GPL MiSTer framework, only fetched for this check): an
# x86 core with L1/L2 caches; '// synthesis translate_off' around simulation code, assign ... = 'Z next to the
# instance that drives the same SDRAM pins, a hierarchical reference into a generate block; rtl/ and sys/ only
fetch_sparse ao486_MiSTer https://github.com/MiSTer-devel/ao486_MiSTer 628ddbcb101f836812981b06367b5080161fad4f rtl sys
printf '`define BUILD_DATE "261108"' > ao486_MiSTer/build_id.v
# MiSTer-devel/C64_MiSTer (only fetched for this check, not redistributed): VHDL (fpga64, VIC-II)
# with SystemVerilog (SID, OPL3, disk drives): function ports declared as wire, true dual-port RAMs written from
# two always blocks, mem[i][j] writes, '&' on a constrained array type; rtl/ and sys/ only
fetch_sparse C64_MiSTer https://github.com/MiSTer-devel/C64_MiSTer 88354ba0b69c7168f2f7439266f88d936ae64de3 rtl sys
printf '`define BUILD_DATE "261108"' > C64_MiSTer/build_id.v
# MiSTer-devel/Atari800_MiSTer (only fetched for this check, not redistributed): almost all VHDL (87 files: CPU, ANTIC,
# GTIA, POKEY, cartridge emulation); a signal driven twice with the same value, RAM arrays of 9M bits; rtl/ and sys/ only
fetch_sparse Atari800_MiSTer https://github.com/MiSTer-devel/Atari800_MiSTer 98d73fa3e349bd06c9a1ddf310c7a445b5c4dbdc rtl sys
printf '`define BUILD_DATE "261109"' > Atari800_MiSTer/build_id.v
# sipeed/TangNano-20K-example (only fetched for this check, not redistributed): NESTang on the Tang Nano 20K (Gowin
# GW2AR-18C), analyzed from its Gowin project (.gprj): no top module (the deepest root), SystemVerilog 2017 for the
# .v files, Gowin cells (rPLL, CLKDIV, OSER10, ELVDS_OBUF), '//' comments in the SDC; nestang/ only
fetch_sparse TangNano-20K-example https://github.com/sipeed/TangNano-20K-example e23949a0a77381b94960cbc4e97a7c5e5ba8d222 nestang
# f32c/f32c (BSD-2-Clause): the f32c SoC on the ULX3S (ECP5) from its Lattice Diamond project (.ldf), VHDL: a CPU
# whose clocked processes AND several enables with the edge, a true dual-port RAM in a shared variable, a ROM read from
# an SREC file, two EHXPLLL; the preference file (.lpf) blocks the paths between the CPU and the pixel clock; rtl/ only
fetch_sparse f32c https://github.com/f32c/f32c 546d817ee66a58dd282c44c8cc845b608b9b111c rtl
# steve-chamberlin/fpga-disk-controller (only fetched for this check): the Yellowstone/Liron Apple II disk controller
# on a MachXO2 from its Lattice Diamond project; the ROM is a SCUBA netlist of four DP8KC whose port B is tied off,
# the IWM registers are clocked by the bus strobe _devsel; lattice/ only
fetch_sparse fpga-disk-controller https://github.com/steve-chamberlin/fpga-disk-controller 31488767c7ec94b6d9338dc95fc917e2e37d70c5 \
    /lattice/ /top.v /yellowstone_blink.ldf /yellowstone_blink.lpf '!*.ncd'
# lowRISC/ibex-demo-system (Apache-2.0, only fetched for this check): Ibex with the PULP debug module on the Arty A7,
# analyzed from its FuseSoC core (60 cores, lowRISC's primgen written by OpenCDC); without the software, the documents
# and the test suites vendored into Ibex
fetch_sparse ibex-demo-system https://github.com/lowRISC/ibex-demo-system d37beb753e19482eb054aefac73a27289bf72e47 \
    '/*' '!/sw/' '!/doc/' '!/vendor/lowrisc_ibex/doc/' '!/vendor/lowrisc_ibex/vendor/riscv-*/' \
    '!/vendor/lowrisc_ibex/vendor/google_riscv-dv/'
# olofk/serv (ISC): the SERV bit-serial RISC-V and its servant SoC, analyzed from servant.core for boards of every
# vendor (FuseSoC targets: Xilinx PLLE2/PLL_BASE/DCM_SP, iCE40 PLL from the icepll generator, ECP5, Intel altpll and
# Cyclone 10 LP, Gowin flow); without the software and Zephyr
fetch_sparse serv https://github.com/olofk/serv f200eb2ed7b69ac1c6b8eddd47654522aeee5ce8 \
    '/*' '!/sw/' '!/zephyr/' '!/doc/'
# lawrie/ulx3s_zx_spectrum (only fetched for this check): a ZX Spectrum (TV80 Z80, HDMI, USB host, SPI OSD) on the
# ULX3S, built with yosys/nextpnr from a Makefile: the OpenCDC project takes its VERILOG list and the .lpf
fetch_sparse ulx3s_zx_spectrum https://github.com/lawrie/ulx3s_zx_spectrum 19f242c057e25b38254004b1ea57306a28d85414 src ulx3s
python3 - <<'PY'
import json, re
d = "ulx3s_zx_spectrum/ulx3s/"
mk = open(d + "Makefile").read().replace("\\\n", " ")
files = re.search(r"^VERILOG\s*=\s*(.*)$", mk, re.M).group(1).split()
json.dump({"format": "opencdc-project", "version": 1, "name": "zx_spectrum", "top": "spectrum",
           "sources": files, "constraints": ["ulx3s_v20.lpf"]}, open(d + "zx.ocdc.json", "w"), indent=1)
PY
# smunaut/ice40-playground riscv_usb (CERN-OHL-P-2.0 / LGPL, only fetched for this check): a PicoRV32 SoC with USB on
# the iCEBreaker (iCE40 UltraPlus), yosys/nextpnr; the no2fpga cores are git submodules, fetched at the pinned commits;
# the OpenCDC project takes the Makefiles' source lists (no clock constraint: nextpnr's clocks.py is not read)
fetch_sparse ice40-playground https://github.com/smunaut/ice40-playground d2fa0050129c14a7fc42f64f115366f6f2a51669 \
    projects/riscv_usb
for m in no2ice40:97413633237ef5ad899f04e0b4b22679c5f6f6bb no2misc:eb5448b60a2de550c01c6002be4531c2495af8da \
         no2usb:d359102614762fbd6506024ab19331e30da27cf5; do
    fetch_sparse "ice40-playground/cores/${m%%:*}" "https://github.com/no2fpga/${m%%:*}" "${m##*:}" rtl
done
python3 - <<'PY'
import json
r = "ice40-playground/"
def files(d, names):
    return [d + n + ".v" for n in names.split()]
src = (files("cores/no2misc/rtl/", "delay fifo_sync_ram fifo_sync_shift glitch_filter i2c_master i2c_master_wb "
             "muacm2wb prims pdm pwm ram_sdp stream2wb uart2wb uart_rx uart_tx uart_wb xclk_strobe xclk_wb") +
       files("cores/no2ice40/rtl/", "ice40_ebr ice40_i2c_wb ice40_rgb_wb ice40_spi_wb ice40_spram_gen ice40_spram_wb "
             "ice40_iserdes ice40_oserdes ice40_serdes_crg ice40_serdes_dff ice40_serdes_sync") +
       files("cores/no2usb/rtl/", "usb usb_crc usb_ep_buf usb_ep_status usb_phy usb_rx_ll usb_rx_pkt usb_trans "
             "usb_tx_ll usb_tx_pkt") +
       files("projects/riscv_usb/rtl/", "dfu_helper picorv32 picorv32_ice40_regs soc_bram soc_picorv32_base "
             "soc_picorv32_bridge soc_spram soc_usb sysmgr top"))
json.dump({"format": "opencdc-project", "version": 1, "name": "riscv_usb", "top": "top", "sources": src,
           "includeDirs": ["cores/no2usb/rtl", "projects/riscv_usb/rtl"], "defines": {"BOARD_ICEBREAKER": "1"}},
          open(r + "riscv_usb.ocdc.json", "w"), indent=1)
PY
# corundum/corundum (BSD-2-Clause-Views, only fetched for this check): the open-source NIC on the ZCU106 (UltraScale+),
# PCIe and 10G Ethernet; the Vivado IP (PCIe, transceivers) is generated from Tcl and not part of the sources: black
# boxes. Zero-width selects in branches of 'if (constant)' that are never taken, a 2^14 x 128 bit RAM, and the
# per-instance Tcl constraint scripts (get_cells -hier -regexp in loops, set_max_delay -datapath_only)
fetch_sparse corundum https://github.com/corundum/corundum 1ca0151b97af85aa5dd306d74b6bcec65904d2ce \
    fpga/mqnic/ZCU106/fpga_pcie fpga/common fpga/lib
python3 - <<'PY'
import json, re
d = "corundum/fpga/mqnic/ZCU106/fpga_pcie/"
mk = open(d + "fpga/Makefile").read()
json.dump({"format": "opencdc-project", "version": 1, "name": "corundum_zcu106", "top": "fpga",
           "sources": re.findall(r"^SYN_FILES \+?= (\S+)", mk, re.M),
           "constraints": re.findall(r"^XDC_FILES \+?= (\S+)", mk, re.M),
           "options": {"unknownModules": "blackbox"}}, open(d + "corundum.ocdc.json", "w"), indent=1)
PY

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
def proj(name, bd, wrapper, xdc=None):
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": "design_1_wrapper",
               "sources": [bd, wrapper], "constraints": [xdc] if xdc else []}, open(name + ".ocdc.json", "w"), indent=1)
# the other cteqeu projects and ArtyS7-RPU-SoC are analyzed from their Vivado projects (.xpr)
q = "cteqeu-SoC/"
# user IP whose repository is not in the project: from their generated HDL (synth/ wrapper + ipshared/)
ma = q + "MiniZED/Vitis_Custom_AXI_IP2021/Vitis_Custom_AXI_IP.srcs/sources_1/bd/design_1/"
proj("zynq_math_ip", ma + "design_1.bd", ma + "hdl/design_1_wrapper.vhd")
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
check_target() { # core target expected-regex
    local out
    out=$("$BIN/opencdc" analyze "$1" --target "$2" -q 2>&1 || true)
    if grep -Eq "$3" <<<"${out//$'\n'/ }"; then
        echo "ok   $1 ($2): $(echo "$out" | grep -E '^CDC     :')"
    else
        echo "::error::$1 ($2) does not match '$3'"; echo "$out" | tail -30; fail=1
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
check chiplab_soc.ocdc.json "0 unsupported construct.*Clocks  : 6 \(1293/1293 registers resolved\).*ui_clk  \[divider\] alias=c1_clk0 alias=clk_pll_i.*CLKOUT1  \[divider\] alias=clk_out2_clk_pll_33 .*period=30.303ns.*axi_clock_converter:2.1.*axi_interconnect:1.7.*mig_7series:4.2.*AXI4 interface.*data_bank_sram.xcix.*FE-OUTPUT-PORT-CONSTANT.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 138 crossing path\(s\), 78 synchronized.*Rules   : 36 error violation\(s\), 24 warning violation"
# Fuxi: the block design (45 cells, 22 hierarchical cells as modules) with all IP but the TFT controller modelled; every
# crossing between the clocks happens inside the IP (clock converters, Quad SPI FIFOs, MIG reset synchronizer)
check fuxi_soc.ocdc.json "Clocks  : 9 \(1134/1134 registers resolved\).*ui_clk  \[divider\] alias=ddr_controller_ui_clk alias=clk_pll_i.*CLKOUT0  \[divider\] alias=clk_cpu_soc_clk_wiz_0_0 .*period=20ns.*FE-BD-MODULE.*45 cell\(s\), 22 hierarchical cell\(s\), 41 IP configuration\(s\).*axi_quad_spi:3.2.*AXI4 memory interface on s_axi4_aclk.*axi_intc:4.1.*4 interrupt input\(s\) \(1 asynchronous\).*proc_sys_reset:5.0.*axi_tft:2.0\) known only by the clocks of its interfaces: 54 port\(s\) of its bus interfaces in 3 clock\(s\).*Summary : 0 error\(s\), 7 warning\(s\).*CDC     : 1 crossing path\(s\), 1 synchronized, 0 unsynchronized"
# Zynq-7000 block designs: one fabric clock (clk_fpga_0, Vivado's name, period from the PS configuration); the AXI
# peripherals behind the PS's M_AXI_GP0 in it; the EMIO GPIO outputs in the PS clock cpu_1x
check $cq/Pynq-Z2/SoC_7_Timers_GIC/SoC_7_Timers_GIC.xpr "Clocks  : 1 \(99/99 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=10ns.*FE-BD-MODULE.*7 cell\(s\), 4 hierarchical cell\(s\), 7 IP configuration\(s\).*processing_system7:5.5.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check $cq/MiniZED/SoC_Multiple_IRQ_F2P/SoC_Multiple_IRQ_F2P.xpr "Clocks  : 2 \(98/98 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=20ns.*cpu_1x  \[black-box\].*FE-BD-MODULE.*7 cell\(s\).*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check $cq/MiniZED/eFPGA_XADC/Minized_XADC.xpr "Clocks  : 2 \(82/82 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=20ns.*FE-BD-MODULE.*4 cell\(s\).*xadc_wiz:3.3.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check $cq/MiniZED/eFPGA_BRAM/eFPGA_BRAM.xpr "Clocks  : 2 \(118/118 registers resolved\).*FE-BD-MODULE.*6 cell\(s\), 6 IP configuration\(s\).*smartconnect:1.0.*1 clock\(s\): aclk: M00_AXI, M01_AXI, S00_AXI.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zynq_math_ip.ocdc.json "Clocks  : 2 \(74/74 registers resolved\).*FE-BD-MODULE.*Math_IP:1.0\) analyzed from its output products: its generated HDL \(synthesis view, 4 file\(s\)\).*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
# AXI_NeoPixel (generated HDL): the AXI registers (clk_fpga_0, 100 MHz) read straight by the LED logic in clk_fpga_1
# (50 MHz): 64 registers x 24 bits without synchronization (the relation of the two PS clocks is not constrained)
check $cq/Pynq-Z2/SoC_AXI_Neopixel/SoC_AXI_NeoPixel/SoC_AXI_NeoPixel.xpr "Clocks  : 2 \(179/179 registers resolved\).*FCLK_CLK1  \[black-box\] alias=clk_fpga_1 .*period=20ns.*PRJ-XPR-MISSING\]: IP repository not found: .*ip_repo/AXI_NeoPixel_1.0.*Vivado project imported: 2 source\(s\), 1 constraint file\(s\), top .design_1_wrapper.*AXI_NeoPixel:1.0\) analyzed from its output products.*Summary : 0 error\(s\), 2 warning\(s\).*CDC     : 64 crossing path\(s\), 0 synchronized, 64 unsynchronized.*64 path\(s\) need review"
# ArtyS7-RPU-SoC: the XML block designs; the board XDC names both oscillators sys_clk_pin (the 100 MHz one twice), so
# as in Vivado the 12 MHz clock is overridden and CLK12MHZ is an unconstrained clock; the CPU (clk_core, from the
# 100 MHz MMCM) and the trace logic in CLK12MHZ exchange trace_dump_enabled / memory address and write enable without
# synchronization; the MIG with the ports of its stub (128-bit user data, device_temp and device_temp_i)
# analyzed from its Vivado project (.xpr), as the cteqeu projects above
check ArtyS7-RPU-SoC/ArtyS7_RPU_SoC.xpr "Clocks  : 6 \(369/369 registers resolved\).*sys_clk_pin  \[constraint\] source=rpu_top.CLK100MHZ period=10ns.*CLK12MHZ  \[primary\].*alias=clk_core_clk_main_clk_wiz_0_0.*alias=clk_pixel_clk_video_clk_wiz_0_0 .*period=13.468ns.*FE-BD-MODULE.*block design 'clk_video'.*mig_7series:4.1.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 10 crossing path\(s\), 0 synchronized, 10 unsynchronized.*10 path\(s\) need review"
check arty_xadc.ocdc.json "Clocks  : 1 \(14/14 registers resolved\).*IP 'xadc_wiz_0' \(xilinx.com:ip:xadc_wiz:3.3\) modelled.*DRP interface.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check basys3_xadc.ocdc.json "Clocks  : 2 \(24/24 registers resolved\).*IP 'xadc_wiz_0'.*DRP interface.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check zybo_xadc.ocdc.json "Clocks  : 1 \(19/19 registers resolved\).*IP 'xadc_wiz_0'.*DRP interface.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 0 crossing path"
# Zynq UltraScale+ MPSoC: pl_clk0 with Vivado's name clk_pl_0, the AXI ports in their interface clocks
# a Quartus project (.qpf) as it is: Quartus' leniencies (nets assigned procedurally, generate-block names used
# in the module), Cyclone V HPS atoms, PLL clocks with Quartus' names (sys_top.sdc's clock groups match them)
check Template_MiSTer/Template.qpf "Clocks  : 9 \(1575/1648 registers resolved\).*sysmem\|fpga_interfaces\|h2f_user0_clk  \[constraint\].*period=10ns.*alias=emu\|pll\|pll_inst\|altera_pll_i\|general\[0\].gpll~PLL_OUTPUT_COUNTER\|divclk.*Quartus project imported: 46 source\(s\), 2 constraint file\(s\), top 'sys_top'.*FE-NET-PROCEDURAL-ASSIGN\]: net 'next_state_write'.*FE-GENERATE-SCOPE\]: 'kbd_rd'.*Summary : 0 error\(s\).*CDC     : 485 crossing path\(s\), 53 synchronized, 432 unsynchronized.*121 unsynchronized path\(s\) covered by a timing exception"
check NES_MiSTer/NES.qpf "Instances: 457.*Clocks  : 11 \(3830/3880 registers resolved\).*emu\|pll\|pll_inst\|altera_pll_i\|general\[2\].gpll~PLL_OUTPUT_COUNTER\|divclk.*period=46.5608ns.*Quartus project imported: 103 source\(s\), 2 constraint file\(s\), top 'sys_top'.*Summary : 0 error\(s\).*CDC     : 556 crossing path\(s\), 58 synchronized, 498 unsynchronized.*119 unsynchronized path\(s\) covered by a timing exception"
check Genesis_MiSTer/Genesis.qpf "Instances: 381.*RTL     : 265 module bodies, 3371 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 10 \(3321/3371 registers resolved\).*VHDL-LENGTH-LOGIC.*built-in models used for vendor primitives: altddio_out, altdpram,.*Summary : 0 error\(s\).*CDC     : 591 crossing path\(s\), 52 synchronized, 539 unsynchronized.*109 unsynchronized path\(s\) covered by a timing exception"
check SNES_MiSTer/SNES.qpf "Instances: 367.*RTL     : 234 module bodies, 3496 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 11 \(3446/3496 registers resolved\).*CX4Map.vhd:135:24: warning\[VHDL-LENGTH-LOGIC\].*built-in models used for vendor primitives: altddio_out, altdpram,.*Summary : 0 error\(s\).*CDC     : 617 crossing path\(s\), 65 synchronized, 552 unsynchronized.*113 unsynchronized path\(s\) covered by a timing exception"
check ao486_MiSTer/ao486.qpf "Instances: 661.*RTL     : 302 module bodies, 4487 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 13 \(4437/4487 registers resolved\).*Summary : 0 error\(s\).*CDC     : 1556 crossing path\(s\), 704 synchronized, 852 unsynchronized.*213 unsynchronized path\(s\) covered by a timing exception"
check C64_MiSTer/C64.qpf "Instances: 317.*RTL     : 190 module bodies, 4328 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 11 \(4278/4328 registers resolved\).*FE-WIRE-FUNCTION-PORT.*Summary : 0 error\(s\).*CDC     : 515 crossing path\(s\), 65 synchronized, 450 unsynchronized.*113 unsynchronized path\(s\) covered by a timing exception"
check Atari800_MiSTer/Atari800.qpf "Instances: 514.*RTL     : 241 module bodies, 3608 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 11 \(3535/3608 registers resolved\).*address_decoder.vhdl:159:2: warning\[IR-SAME-DRIVERS\].*Summary : 0 error\(s\).*CDC     : 516 crossing path\(s\), 53 synchronized, 463 unsynchronized.*118 unsynchronized path\(s\) covered by a timing exception"
check ice40-playground/riscv_usb.ocdc.json "RTL     : 50 module bodies, 359 registers .* 0 latches, .* 0 unsupported construct\\(s\\).*Clocks  : 3 \\(358/359 registers resolved\\).*IR-INPUT-DRIVEN\\]: input port 'sio_mosi_i'.*built-in models used for vendor primitives: SB_GB, SB_IO, SB_LEDDA_IP, SB_PLL40_2F_PAD, SB_RAM40_4K, SB_RAM40_4KNR, SB_RGBA_DRV, SB_SPI, SB_SPRAM256KA, SB_WARMBOOT.*Summary : 0 error\\(s\\), 8 warning\\(s\\).*CDC     : 1 crossing path\\(s\\), 0 synchronized, 1 unsynchronized"
# the two JTAG TAPs of the debug module (BSCANE2 on chains 3 and 4) share the one TCK; the debug module's CDC (dmi)
check ibex-demo-system/ibex_demo_system.core "RTL     : 78 module bodies, 271 registers .* 0 latches, .* 1 unsupported construct\(s\).*Clocks  : 3 \(271/271 registers resolved\).*i_tap_dtmcs.TCK  \[black-box\] alias=top_artya7.u_ibex_demo_system.gen_dm_top.u_dm_top.dap.i_dmi_jtag_tap.i_tap_dmi.TCK.*PRJ-CORE-GENERATED\]: 9 file\(s\) of FuseSoC generators.*FuseSoC core imported: 160 source\(s\), 1 constraint file\(s\), top 'top_artya7' \(target 'synth', tool vivado, 60 core\(s\), macro SYNTHESIS defined.*built-in models used for vendor primitives: BSCANE2, BUFG, BUFGCE, IBUF, PLLE2_ADV.*Summary : 0 error\(s\), 1 warning\(s\).*CDC     : 26 crossing path\(s\), 24 synchronized, 2 unsynchronized"
# SERV: one clock domain behind each board's clock generator
check_target serv/servant.core arty_a7_35t "RTL     : 24 module bodies, 68 registers .* 0 unsupported construct\\(s\\).*Clocks  : 2 \\(68/68 registers resolved\\).*PLLE2_BASE_inst.CLKOUT0  \\[divider\\].*period=62.5ns.*FuseSoC core imported: 28 source\\(s\\), 1 constraint file\\(s\\), top 'servix'.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core icebreaker "Clocks  : 2 \\(61/61 registers resolved\\).*pll.PLLOUTCORE  \\[divider\\].*PRJ-CORE-GENERATED\\]: 1 file\\(s\\).*pll.vh \\(icepll, 12 -> 15.9375 MHz\\).*built-in models used for vendor primitives: SB_PLL40_PAD.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core ulx3s_85 "Clocks  : 2 \\(64/64 registers resolved\\).*clk  \\[constraint\\].*period=40ns.*CLKOP  \\[divider\\].*period=64ns.*vendor primitives: EHXPLLL.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core de0_nano "Clocks  : 2 \\(65/65 registers resolved\\).*clk\\[0\\]  \\[divider\\].*period=62.5ns.*vendor primitives: altpll.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core cyc1000 "Clocks  : 2 \\(65/65 registers resolved\\).*clk\\[0\\]  \\[divider\\].*period=31.2499ns.*vendor primitives: cyclone10lp_pll.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core nexys_2_500 "Clocks  : 2 \\(70/70 registers resolved\\).*DCM_SP_inst.CLKFX  \\[divider\\].*period=62.5ns \\(16 MHz\\) = the configured input period x 3.125.*vendor primitives: DCM_SP.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core pipistrello "Clocks  : 2 \\(68/68 registers resolved\\).*PLL_BASE_inst.CLKOUT1  \\[divider\\].*period=62.5ns.*vendor primitives: PLL_BASE.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core go_board "Clocks  : 1 \\(60/60 registers resolved\\).*FE-PORT-NET-DEFAULT\\]: output net port 'o_led4'.*Summary : 0 error\\(s\\), 3 warning\\(s\\).*CDC     : 0 crossing path"
check_target serv/servant.core tangnano9k "Clocks  : 1 \\(61/61 registers resolved\\).*target 'tangnano9k', tool gowin.*Summary : 0 error\\(s\\), 0 warning\\(s\\).*CDC     : 0 crossing path"
check fpga-disk-controller/yellowstone_blink.ldf "Clocks  : 1 \(1/1 registers resolved\).*blink.internal_oscillator_inst.OSC  \[black-box\].*period=480.769ns \(2.08 MHz\).*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
check ulx3s_zx_spectrum/ulx3s/zx.ocdc.json "Instances: 39.*RTL     : 29 module bodies, 270 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 6 \(270/270 registers resolved\).*clk25_mhz  \[constraint\].*period=40ns.*built-in models used for vendor primitives: EHXPLLL, ODDRX1F.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 1 crossing path\(s\), 0 synchronized, 1 unsynchronized"
check fpga-disk-controller/lattice/liron.ldf "RTL     : 10 module bodies, 31 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 2 \(23/31 registers resolved\).*Lattice Diamond project imported: 4 source\(s\), 1 constraint file\(s\), top 'top' \(implementation 'fpgatop', device LCMXO2-1200HC-4TG100C\).*built-in models used for vendor primitives: DP8KC, VHI, VLO.*CLK-UNUSED-INTERFACE\]: 8 register\(s\).*DP8KC.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 17 crossing path\(s\), 0 synchronized, 17 unsynchronized"
check f32c/rtl/proj/lattice/ulx3s/sdram_dv/ulx3s_sdram_dv.ldf "RTL     : 24 module bodies, 350 registers .* 0 latches, .* 1 unsupported construct\(s\).*Clocks  : 5 \(350/350 registers resolved\).*clk_25m  \[constraint\].*period=40ns.*CLKOS3  \[divider\].*\(74.25 MHz\).*Lattice Diamond project imported: 22 source\(s\), 1 constraint file\(s\), top 'top_sdram_dv'.*rom.vhd:146:12: warning\[IR-UNSUPPORTED-INIT\].*built-in models used for vendor primitives: EHXPLLL, ODDRX1F, USRMCLK, VHI, VLO.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 15 crossing path\(s\), 3 synchronized, 12 unsynchronized"
check TangNano-20K-example/nestang/nes.gprj "Instances: 122.*RTL     : 89 module bodies, 638 registers .* 0 latches, .* 0 unsupported construct\(s\).*Clocks  : 12 \(638/638 registers resolved\).*NES_Tang20k.clk_div.clkdiv_inst.CLKOUT  \[divider\].*= clk_p5 x 5.*Gowin project imported: 35 source\(s\), 1 constraint file\(s\), top not set.*FE-AUTO-TOP\]: no top module given: 'NES_Tang20k' is the top.*built-in models used for vendor primitives: CLKDIV, ELVDS_OBUF, OSER10, rPLL.*Summary : 0 error\(s\).*CDC     : 27 crossing path\(s\), 0 synchronized, 27 unsynchronized"
check corundum/fpga/mqnic/ZCU106/fpga_pcie/corundum.ocdc.json "Instances: 532.*RTL     : 192 module bodies, 5936 registers .* 0 latches, .* 3 unsupported construct\(s\).*Clocks  : 9 \(5936/5936 registers resolved\).*pcie_tlp_fifo_raw.v:450:57: warning\[FE-SLANG-ValueMustBePositive\].*FE-BLACKBOX-MODULE.*Summary : 0 error\(s\).*CDC     : 219 crossing path\(s\), 209 synchronized, 10 unsynchronized.*10 unsynchronized path\(s\) covered by a timing exception"
check u96_mpsoc.ocdc.json "Clocks  : 1 \(93/93 registers resolved\).*zynq_ultra_ps_e_0.pl_clk0  \[black-box\] alias=clk_pl_0 .*period=10ns.*zynq_ultra_ps_e:3.2\) modelled from its configuration: PL clocks pl_clk0.*Summary : 0 error\(s\), 0 warning\(s\).*CDC     : 0 crossing path"
# MicroBlaze (simulation netlist: slice gates, SRLC16E), its debug module (FDC_1/FDRE_1), MMCM_ADV, an encrypted IP known by
# its interface clocks; the camera interface drives pclk_out twice (an assignment and a BUFG): a real error
check s7_mipi_microblaze.ocdc.json "v_demosaic:1.0\) known only by the clocks of its interfaces: 35 port\(s\).*error\[IR-MULTIDRIVER\]: 'pclk_out' bit \[0\] in module 'CAM_interface' has multiple drivers.*Summary : 1 error\(s\), 1 warning\(s\).*CDC analysis: not run"
# the Sobel filter (an HLS IP exported without output products) known by its interface clock; the video DMA and
# timing controllers from their simulation netlists (their HDL is encrypted)
check zybo_sobel.ocdc.json "Clocks  : 7 \(11735/11735 registers resolved\).*FCLK_CLK0  \[black-box\] alias=clk_fpga_0 .*period=5ns.*axi_vdma:6.3\) analyzed from its output products: its simulation netlist.*image_filter:1.0\) known only by the clocks of its interfaces: 18 port\(s\) of its bus interfaces in 1 clock\(s\) \(ap_clk\).*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 295 crossing path\(s\), 238 synchronized, 57 unsynchronized.*294 path\(s\) need review"
exit $fail

#!/usr/bin/env bash
# VHDL regression on real open-source code, pinned to fixed commits.
#
#   tools/ci/vhdl-corpus.sh <bin-dir> [work-dir]
#
# 1. Parses every .vhd/.vhdl file of seven projects (~2300 files); only the
#    files listed in REJECT (documentation snippets, templates, VHDL-2019)
#    may fail, and they must fail.
# 2. Elaborates and analyzes the open-logic clock-crossing components, a
#    fully featured neorv32 SoC and surf's Xilinx clock manager (MMCM/PLL
#    primitive models) and async FIFO, and the microwatt POWER SoC (VHDL-2008,
#    Verilog UART, Lattice ECP5 PLL declared as a local component; and the Arty
#    A7 configuration with LiteDRAM, LiteEth and LiteSDCard, five clock
#    domains), and two Digilent IP cores that use Vivado IP from their .xci
#    configuration (ZmodScopeController: FIFO Generator, MMCM, OSERDES;
#    MIPI CSI-2 receiver: AXI-Stream FIFOs and ILA debug cores), plus each
#    FIFO .xci of the Digilent library on its own, and two Digilent board
#    demos with their Vivado IP (Cmod A7 GPIO: Clocking Wizard; Nexys A7 out-of-
#    box demo: Clocking Wizard, block ROM, CORDIC, XADC, the DDR2 memory
#    interface), and five Pmod IP cores (Vivado block design netlists: AXI
#    GPIO, IIC, UART 16550, Quad SPI, Timer, Slice, Concat, Constant; ports from
#    the component files; Digilent's pmod_bridge and PWM from the library as IP
#    repository); the CDC summaries must match.
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
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
}
fetch open-logic https://github.com/open-logic/open-logic d9f123aae1c6022288782668067311f9794da0a2
fetch neorv32    https://github.com/stnolting/neorv32    6536b6f43b8372654fff56bc448852bb8f128094
fetch PoC        https://github.com/VLSI-EDA/PoC         8c39b2407a97ec81b8601c4b64e4d2e2141ab9cf
fetch osvvm      https://github.com/OSVVM/OSVVM          716d751566b4fb6479f69c38cc01790744c911b7
fetch surf       https://github.com/slaclab/surf         f0b5e3a7b5ab191be1cb5f4453e418299181c9d8
fetch microwatt  https://github.com/antonblanchard/microwatt f341bed306a9a7281bf65200df8cc1f0a002617d
fetch vivado-library https://github.com/Digilent/vivado-library 0362ce72d39eebc400fdabacff706ee7fa8ad564
fetch cmod-a7-gpio https://github.com/Digilent/Cmod-A7-35T-GPIO 793bb5f4fbf4218389922627eda009fcea75440c
fetch nexys-a7-oob https://github.com/Digilent/Nexys-A7-100T-OOB ad719f27d37dea4a7af3224657f41f7c909b66d2

# ---- 1. parser -------------------------------------------------------------
REJECT=(
    open-logic/doc/axi/slave/RbExample.vhd        # documentation snippet, not a design file
    osvvm/OsvvmSettingsPkg_template.vhd           # template with placeholders
    osvvm/DynamicVectorGenericPkg.vhd             # VHDL-2019 syntax
    microwatt/litedram/gen-src/no-init-mem.vhdl   # architecture without 'begin' (not legal VHDL)
)
fail=0
for repo in open-logic neorv32 PoC osvvm surf microwatt vivado-library cmod-a7-gpio nexys-a7-oob; do
    # (no mapfile: macOS ships bash 3.2)
    files=()
    while IFS= read -r -d '' f; do files+=("$f"); done < <(find "$repo" \( -name '*.vhd' -o -name '*.vhdl' \) -print0 | LC_ALL=C sort -z)
    ok=()
    for f in "${files[@]}"; do
        skip=0
        for r in "${REJECT[@]}"; do [ "$f" = "$r" ] && skip=1; done
        [ $skip = 0 ] && ok+=("$f")
    done
    if ! "$BIN/opencdc-vhdl-parse" "${ok[@]}" > "parse-$repo.log"; then
        echo "::error::VHDL parse errors in $repo"; grep -E "error" "parse-$repo.log" | head -20; fail=1
    fi
    tail -1 "parse-$repo.log"
done
for r in "${REJECT[@]}"; do
    if "$BIN/opencdc-vhdl-parse" "$r" > /dev/null; then
        echo "::error::$r was expected to be rejected"; fail=1
    fi
done

# ---- 2. elaboration and CDC analysis ---------------------------------------
python3 - "$WORK" <<'EOF'
import glob, json, os, sys
work = sys.argv[1]
olo = os.path.join(work, "open-logic/src/base/vhdl")
files = sorted(glob.glob(olo + "/olo_base_pkg_*.vhd")) + sorted(f for f in glob.glob(olo + "/*.vhd") if "pkg" not in f)
open(os.path.join(work, "olo.sdc"), "w").write(
    "create_clock -name in_clk -period 10 [get_ports In_Clk]\n"
    "create_clock -name out_clk -period 7 [get_ports Out_Clk]\n"
    "set_clock_groups -asynchronous -group [get_clocks in_clk] -group [get_clocks out_clk]\n")
for top, gen in [("olo_base_cc_bits", {"Width_g": 4}), ("olo_base_cc_pulse", {"NumPulses_g": 2}),
                 ("olo_base_cc_simple", {"Width_g": 8}), ("olo_base_cc_status", {"Width_g": 8}),
                 ("olo_base_cc_handshake", {"Width_g": 8}), ("olo_base_fifo_async", {"Width_g": 8, "Depth_g": 16})]:
    json.dump({"format": "opencdc-project", "version": 1, "name": top, "top": top,
               "sources": [{"file": f, "library": "olo"} for f in files],
               "parameters": gen,
               # open-logic's own scoped constraints (read_xdc -ref, as olo_base_constraints_amd.tcl does)
               "constraints": ["olo.sdc"] + [{"file": os.path.join(work, "open-logic/src/base/tcl", m + ".tcl"),
                                              "ref": m}
                                             for m in ("olo_base_cc_reset", "olo_base_cc_bits", "olo_base_cc_simple",
                                                       "olo_base_reset_gen", "olo_base_ram_sdp")]},
              open(os.path.join(work, top + ".ocdc.json"), "w"), indent=1)
neo = os.path.join(work, "neorv32/rtl/core")
nfiles = [neo + "/neorv32_package.vhd"] + sorted(f for f in glob.glob(neo + "/*.vhd") if "package" not in f)
params = {"CLOCK_FREQUENCY": 100000000, "IO_GPIO_NUM": 8, "IO_PWM_NUM": 4}
for g in ("OCD_EN RISCV_ISA_C RISCV_ISA_M RISCV_ISA_U RISCV_ISA_Zaamo RISCV_ISA_Zalrsc RISCV_ISA_Zba RISCV_ISA_Zbb "
          "RISCV_ISA_Zbc RISCV_ISA_Zbkb RISCV_ISA_Zbkc RISCV_ISA_Zbkx RISCV_ISA_Zbs RISCV_ISA_Zcb RISCV_ISA_Zfinx "
          "RISCV_ISA_Zicntr RISCV_ISA_Zicond RISCV_ISA_Zihpm RISCV_ISA_Zknd RISCV_ISA_Zkne RISCV_ISA_Zknh "
          "RISCV_ISA_Zksed RISCV_ISA_Zksh RISCV_ISA_Xcfu ICACHE_EN DCACHE_EN XBUS_EN IO_CLINT_EN IO_UART0_EN "
          "IO_UART1_EN IO_SPI_EN IO_SDI_EN IO_TWI_EN IO_TWD_EN IO_WDT_EN IO_TRNG_EN IO_CFS_EN IO_NEOLED_EN "
          "IO_ONEWIRE_EN IO_DMA_EN IO_SLINK_EN IO_TRACEBUF_EN IMEM_EN DMEM_EN").split():
    params[g] = "true"
open(os.path.join(work, "neo.sdc"), "w").write("create_clock -name clk -period 10 [get_ports clk_i]\n")
json.dump({"format": "opencdc-project", "version": 1, "name": "neorv32", "top": "neorv32_top",
           "sources": [{"file": f, "library": "neorv32"} for f in nfiles],
           "parameters": params, "constraints": ["neo.sdc"]},
          open(os.path.join(work, "neorv32.ocdc.json"), "w"), indent=1)
# surf: Xilinx clock manager (MMCM/PLL primitives through unisim) and the inferred async FIFO
surf = ["base/general/rtl/StdRtlPkg.vhd", "base/general/rtl/TextUtilPkg.vhd", "axi/axi-lite/rtl/AxiLitePkg.vhd",
        "xilinx/7Series/general/rtl/ClockManager7Pkg.vhd", "base/general/rtl/RstPipeline.vhd",
        "base/general/rtl/RegisterVector.vhd", "base/sync/rtl/Synchronizer.vhd", "base/sync/rtl/SynchronizerVector.vhd",
        "base/sync/rtl/SynchronizerEdge.vhd", "base/sync/rtl/SynchronizerOneShot.vhd", "base/sync/rtl/RstSync.vhd",
        "base/ram/inferred/DualPortRam.vhd", "base/ram/inferred/LutRam.vhd",
        "base/ram/inferred/SimpleDualPortRamInferred.vhd", "base/ram/inferred/TrueDualPortRamInferred.vhd",
        "base/ram/xilinx/SimpleDualPortRamXpm.vhd", "base/ram/xilinx/TrueDualPortRamXpm.vhd",
        "base/ram/dummy/SimpleDualPortRamAlteraMfDummy.vhd", "base/ram/dummy/TrueDualPortRamXpmAlteraMfDummy.vhd",
        "base/ram/rtl/SimpleDualPortRam.vhd", "base/ram/rtl/TrueDualPortRam.vhd", "base/fifo/rtl/inferred/FifoWrFsm.vhd",
        "base/fifo/rtl/inferred/FifoRdFsm.vhd", "base/fifo/rtl/inferred/FifoAsync.vhd",
        "base/fifo/rtl/inferred/FifoSync.vhd", "base/fifo/rtl/FifoOutputPipeline.vhd", "base/fifo/rtl/xilinx/FifoXpm.vhd",
        "base/fifo/rtl/dummy/FifoAlteraMfDummy.vhd", "base/fifo/rtl/Fifo.vhd", "axi/axi-lite/rtl/AxiLiteAsync.vhd",
        "axi/bridge/rtl/AxiLiteToDrp.vhd", "xilinx/general/tb/MmcmEmulation.vhd",
        "xilinx/7Series/general/rtl/ClockManager7.vhd"]
open(os.path.join(work, "cm7.sdc"), "w").write(
    "create_clock -name clkIn -period 8 [get_ports clkIn]\n"
    "create_clock -name axilClk -period 6.4 [get_ports axilClk]\n"
    "set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks clkIn] -group [get_clocks axilClk]\n")
open(os.path.join(work, "gty.sdc"), "w").write(
    "create_clock -name ref -period 6.4 [get_ports {qPllRefClk[0]}]\n"
    "create_clock -name axil -period 6.4 [get_ports axilClk]\n"
    "set_clock_groups -asynchronous -group ref -group axil\n")
surf.append("xilinx/UltraScale+/gtyUs+/rtl/GtyUltraScaleQuadPll.vhd")
open(os.path.join(work, "fifo.sdc"), "w").write(
    "create_clock -name wr -period 8 [get_ports wr_clk]\n"
    "create_clock -name rd -period 6 [get_ports rd_clk]\n"
    "set_clock_groups -asynchronous -group [get_clocks wr] -group [get_clocks rd]\n")
for name, top, gen, sdc in [
        ("surf_cm7_mmcm", "ClockManager7", {"TYPE_G": '"MMCM"', "NUM_CLOCKS_G": 3, "CLKIN_PERIOD_G": "8.0",
                                            "CLKFBOUT_MULT_F_G": "8.0", "CLKOUT0_DIVIDE_F_G": "4.0",
                                            "CLKOUT1_DIVIDE_G": 8, "CLKOUT2_DIVIDE_G": 16}, "cm7.sdc"),
        ("surf_cm7_pll", "ClockManager7", {"TYPE_G": '"PLL"', "NUM_CLOCKS_G": 3, "CLKFBOUT_MULT_G": 8,
                                           "CLKOUT0_DIVIDE_G": 4, "CLKOUT1_DIVIDE_G": 8, "CLKOUT2_DIVIDE_G": 16},
         "cm7.sdc"),
        ("surf_fifo_async", "FifoAsync", {"DATA_WIDTH_G": 8, "ADDR_WIDTH_G": 4}, "fifo.sdc"),
        ("surf_gty_qpll", "GtyUltraScaleQuadPll", {}, "gty.sdc")]:
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": top,
               "sources": [{"file": os.path.join(work, "surf", f), "library": "surf"} for f in surf],
               "parameters": gen, "constraints": [sdc]},
              open(os.path.join(work, name + ".ocdc.json"), "w"), indent=1)
# surf transceivers: Transceivers Wizard (gtwizard_ultrascale) .xci models in their SURF wrappers
sw = os.path.join(work, "surf")
def surf_project(name, top, files, sdc, gen=None):
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": top,
               "sources": [{"file": os.path.join(sw, f), "library": "surf"} if f.endswith(".vhd") else os.path.join(sw, f)
                           for f in files],
               "parameters": gen or {}, "constraints": [sdc]},
              open(os.path.join(work, name + ".ocdc.json"), "w"), indent=1)
pgp3_xci = ["protocols/pgp/pgp3/gthUs+/ip/Pgp3GthUsIp%s/Pgp3GthUsIp%s.xci" % (r, r) for r in ["10G", "12G", "15G", "3G", "6G"]]
open(os.path.join(work, "pgp3_wrapper.sdc"), "w").write(
    "create_clock -name stable -period 6.4 [get_ports stableClk]\n"
    "create_clock -name axil -period 8 [get_ports axilClk]\n"
    "set_clock_groups -asynchronous -group stable -group axil\n")
surf_project("surf_pgp3_gth_wrapper", "Pgp3GthUsIpWrapper",
             surf[:-1] + ["protocols/pgp/pgp3/gthUs+/rtl/Pgp3GthUsIpWrapper.vhd"] + pgp3_xci, "pgp3_wrapper.sdc")
# the whole PGPv3 core on the GTH (Pgp3GthUs), DRP and monitoring enabled
open(os.path.join(work, "pgp3_core.sdc"), "w").write(
    "create_clock -name stable -period 6.4 [get_ports stableClk]\n"
    "create_clock -name axil -period 8 [get_ports axilClk]\n"
    "# the transceiver's user clocks (TX and RX) and the fabric clocks are unrelated\n"
    "set_clock_groups -asynchronous -group [get_clocks stable] -group [get_clocks axil] \\\n"
    "    -group [get_clocks -include_generated_clocks -of_objects [get_pins -hier *U_Pgp3GthUsIp/gtwiz_userclk_tx_srcclk_out]] \\\n"
    "    -group [get_clocks -include_generated_clocks -of_objects [get_pins -hier *U_Pgp3GthUsIp/gtwiz_userclk_rx_srcclk_out]]\n")
pgp3_core = [
    "base/general/rtl/StdRtlPkg.vhd", "base/general/rtl/TextUtilPkg.vhd", "axi/axi-lite/rtl/AxiLitePkg.vhd",
    "axi/axi-stream/rtl/AxiStreamPkg.vhd", "axi/axi4/rtl/AxiPkg.vhd", "protocols/ssi/rtl/SsiPkg.vhd",
    "protocols/pgp/pgp3/core/rtl/Pgp3Pkg.vhd", "protocols/packetizer/rtl/AxiStreamPacketizer2Pkg.vhd",
    "base/crc/rtl/CrcPkg.vhd", "base/general/rtl/ArbiterPkg.vhd", "axi/axi-lite/rtl/AxiLiteCrossbar.vhd",
    "base/general/rtl/RegisterVector.vhd", "axi/axi-lite/rtl/AxiLiteAsync.vhd", "axi/bridge/rtl/AxiLiteToDrp.vhd",
    "protocols/pgp/pgp3/gthUs+/rtl/Pgp3GthUsIpWrapper.vhd", "protocols/packetizer/rtl/AxiStreamPacketizer2.vhd",
    "axi/axi-stream/rtl/AxiStreamMux.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3TxProtocol.vhd",
    "protocols/pgp/pgp3/core/rtl/Pgp3Tx.vhd", "base/sync/rtl/SynchronizerOneShot.vhd",
    "base/sync/rtl/SynchronizerOneShotCnt.vhd", "base/sync/rtl/SynchronizerOneShotCntVector.vhd",
    "base/sync/rtl/SyncStatusVector.vhd", "base/sync/rtl/SyncClockFreq.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3AxiL.vhd",
    "axi/axi-stream/rtl/AxiStreamDeMux.vhd", "base/general/rtl/Scrambler.vhd", "base/crc/rtl/Crc32Parallel.vhd",
    "base/ram/inferred/TrueDualPortRamInferred.vhd", "base/ram/inferred/LutRam.vhd", "base/ram/inferred/DualPortRam.vhd",
    "base/crc/rtl/Crc32.vhd", "axi/axi-stream/rtl/AxiStreamPipeline.vhd",
    "protocols/packetizer/rtl/AxiStreamDepacketizer2.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3RxGearboxAligner.vhd",
    "base/ram/inferred/SimpleDualPortRamInferred.vhd", "base/ram/xilinx/SimpleDualPortRamXpm.vhd",
    "base/ram/rtl/SimpleDualPortRam.vhd", "base/fifo/rtl/FifoOutputPipeline.vhd", "base/sync/rtl/SynchronizerVector.vhd",
    "base/fifo/rtl/inferred/FifoWrFsm.vhd", "base/fifo/rtl/inferred/FifoRdFsm.vhd", "base/fifo/rtl/inferred/FifoAsync.vhd",
    "base/sync/rtl/SynchronizerFifo.vhd", "base/sync/rtl/RstSync.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3RxEb.vhd",
    "base/sync/rtl/Synchronizer.vhd", "base/sync/rtl/SynchronizerEdge.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3RxProtocol.vhd",
    "protocols/pgp/pgp3/core/rtl/Pgp3Rx.vhd", "protocols/pgp/pgp3/core/rtl/Pgp3Core.vhd",
    "protocols/pgp/pgp3/gthUs+/rtl/Pgp3GthUs.vhd"] + pgp3_xci
surf_project("surf_pgp3_gth_core", "Pgp3GthUs", pgp3_core, "pgp3_core.sdc", {"EN_PGP_MON_G": "true", "EN_DRP_G": "true"})
# PGP2fc on the GTY: user clocking outside the IP, the buffer bypass helper, CPLL ports
open(os.path.join(work, "pgp2fc.sdc"), "w").write(
    "create_clock -name txUsrClk -period 5.385 [get_ports txUsrClk]\n"
    "create_clock -name rxUsrClk -period 5.385 [get_ports rxUsrClk]\n"
    "create_clock -name stableClk -period 6.4 [get_ports stableClk]\n"
    "create_clock -name axilClk -period 6.4 [get_ports axilClk]\n"
    "create_clock -name gtUserRefClk -period 2.692 [get_ports gtUserRefClk]\n"
    "set_clock_groups -asynchronous -group txUsrClk -group rxUsrClk -group stableClk -group axilClk -group gtUserRefClk\n")
fc = "protocols/pgp/pgp2fc/gtyUltraScale+/"
surf_project("surf_pgp2fc_gty_wrapper", "Pgp2fcGtyCoreWrapper",
             surf[:3] + ["base/general/rtl/ArbiterPkg.vhd"] + surf[3:-1] +
             ["axi/axi-lite/rtl/AxiLiteCrossbar.vhd", "axi/axi-lite/rtl/AxiLiteMaster.vhd",
              "base/sync/rtl/SynchronizerFifo.vhd", "base/sync/rtl/SyncClockFreq.vhd", "base/sync/rtl/SyncStatusVector.vhd",
              "base/sync/rtl/SyncTrigRate.vhd", "axi/axi-lite/rtl/AxiDualPortRam.vhd",
              "xilinx/general/rtl/GtRxAlignCheck.vhd", fc + "rtl/Pgp2fcGtyCoreWrapper.vhd",
              fc + "ip/Pgp2fcGtyCore/Pgp2fcGtyCore.xci", fc + "ip/Pgp2fcGtyCore_refclk2x/Pgp2fcGtyCore_refclk2x.xci"],
             "pgp2fc.sdc")
# PGPv3 on 7-series GTX/GTP: the 7-series Transceivers Wizard (gtwizard), ports from the
# wrapper's VHDL component declarations (the XML .xci has no port list)
for typ, rates, gen in [("gtx7", ["3G", "6G", "10G"], {}),
                        ("gtp7", ["3G", "6G"], {"CLKIN_PERIOD_G": "2.56", "BANDWIDTH_G": '"OPTIMIZED"',
                                                "CLKFBOUT_MULT_G": "3", "CLKOUT0_DIVIDE_G": "12",
                                                "CLKOUT1_DIVIDE_G": "3", "CLKOUT2_DIVIDE_G": "6"})]:
    T = "Pgp3" + typ.capitalize()
    d = "protocols/pgp/pgp3/%s/" % typ
    extra = [d + "rtl/%sTxGearbox.vhd" % T, d + "rtl/%sRxGearbox.vhd" % T] if typ == "gtp7" else []
    open(os.path.join(work, typ + ".sdc"), "w").write(
        "create_clock -name stable -period 6.4 [get_ports stableClk]\n"
        "create_clock -name axil -period 8 [get_ports axilClk]\n"
        "create_clock -name txPll -period 3.103 [get_ports {txPllClk[*]}]\n"
        "# the receiver clocks are derived from the transceiver's RXOUTCLK\n"
        "set_clock_groups -asynchronous -group [get_clocks stable] -group [get_clocks axil] -group [get_clocks txPll] "
        "-group [get_clocks -include_generated_clocks -of_objects [get_pins -hier *U_%sIp*/gt0_rxoutclk_out]]\n" % T)
    surf_project("surf_pgp3_%s_wrapper" % typ, T + "IpWrapper",
                 surf[:-1] + extra + [d + "rtl/%sIpWrapper.vhd" % T] + [d + "ip/%sIp%s.xci" % (T, r) for r in rates],
                 typ + ".sdc", gen)
# every Transceivers Wizard .xci of surf alone (the IP is the top)
gtx = []
for x in sorted(glob.glob(os.path.join(sw, "**", "*.xci"), recursive=True)):
    if "gtwizard_ultrascale" in open(x, errors="replace").read():
        gtx.append(x)
        n = "gtx_%d" % len(gtx)
        json.dump({"format": "opencdc-project", "version": 1, "name": n,
                   "top": os.path.splitext(os.path.basename(x))[0], "sources": [x]},
                  open(os.path.join(work, n + ".ocdc.json"), "w"), indent=1)
open(os.path.join(work, "gtx_count.txt"), "w").write(str(len(gtx)))
# microwatt for the ECP5-EVN board (Makefile: FPGA_TARGET=ECP5-EVN, synth_files + uart_files)
mw = os.path.join(work, "microwatt")
core = ("decode_types common wishbone_types fetch1 utils plrufn cache_ram icache predecode decode1 helpers "
        "insn_helpers control decode2 register_file cr_file crhelpers ppc_fx_insns rotator logical countbits "
        "multiply multiply-32s divider execute1 loadstore1 mmu dcache writeback core_debug core fpu pmu bitsort "
        "wishbone_arbiter wishbone_bram_wrapper sync_fifo wishbone_debug_master xics syscon gpio soc spi_rxtx "
        "spi_flash_ctrl").split()
# git.vhdl is generated by the Makefile from git.vhdl.in
open(os.path.join(work, "mw_git.vhdl"), "w").write(
    open(os.path.join(mw, "git.vhdl.in")).read().replace("@hash@", "f341bed306a9a7").replace("@dirty@", "0"))
mwsrc = ([os.path.join(mw, f + ".vhdl") for f in core] + [os.path.join(work, "mw_git.vhdl")] +
         [os.path.join(mw, f) for f in ["fpga/soc_reset.vhdl", "fpga/pp_fifo.vhd", "fpga/pp_soc_uart.vhd",
                                        "fpga/main_bram.vhdl", "nonrandom.vhdl", "fpga/clk_gen_ecp5.vhd",
                                        "fpga/top-generic.vhdl", "dmi_dtm_dummy.vhdl"]] +
         sorted(glob.glob(os.path.join(mw, "uart16550/*.v"))))
open(os.path.join(work, "mw.sdc"), "w").write("create_clock -name ext_clk -period 83.333 [get_ports ext_clk]\n")
json.dump({"format": "opencdc-project", "version": 1, "name": "microwatt", "top": "toplevel",
           "language": "verilog-2005", "sources": mwsrc, "includeDirs": [os.path.join(mw, "uart16550")],
           "parameters": {"MEMORY_SIZE": "8192",
                          "RAM_INIT_FILE": '"%s"' % os.path.join(mw, "hello_world/hello_world.hex"),
                          "RESET_LOW": "true", "CLK_INPUT": "12000000", "CLK_FREQUENCY": "40000000"},
           "constraints": ["mw.sdc"]},
          open(os.path.join(work, "microwatt.ocdc.json"), "w"), indent=1)
# microwatt for the Arty A7-35 (microwatt.core target arty_a7-35: filesets and parameters)
core = ("decode_types wishbone_types common fetch1 predecode decode1 helpers decode2 register_file cr_file crhelpers "
        "ppc_fx_insns sim_console logical countbits bitsort control execute1 fpu loadstore1 mmu dcache divider rotator "
        "pmu writeback insn_helpers core icache plrufn cache_ram core_debug utils wishbone_arbiter "
        "wishbone_debug_master wishbone_bram_wrapper soc xics gpio syscon sync_fifo spi_rxtx spi_flash_ctrl").split()
arty = ([os.path.join(mw, f + ".vhdl") for f in core] + [os.path.join(work, "mw_git.vhdl")] +
        [os.path.join(mw, f) for f in ["fpga/main_bram.vhdl", "fpga/soc_reset.vhdl", "fpga/pp_fifo.vhd",
                                       "fpga/pp_soc_uart.vhd", "fpga/pp_utilities.vhd", "nonrandom.vhdl",
                                       "xilinx-mult.vhdl", "xilinx-mult-32s.vhdl", "dmi_dtm_xilinx.vhdl",
                                       "fpga/clk_gen_plle2.vhd", "fpga/top-arty.vhdl",
                                       "litedram/generated/arty/litedram_core.v",
                                       "litedram/generated/arty/litedram-initmem.vhdl",
                                       "litedram/extras/litedram-wrapper-l2.vhdl",
                                       "liteeth/generated/arty/liteeth_core.v",
                                       "litesdcard/generated/xilinx.100e6/litesdcard_core.v"]] +
        [os.path.join(mw, "uart16550", f) for f in ["raminfr.v", "uart_receiver.v", "uart_regs.v", "uart_rfifo.v",
                                                    "uart_sync_flops.v", "uart_tfifo.v", "uart_top.v",
                                                    "uart_transmitter.v", "uart_wb.v"]])
arty_params = {"MEMORY_SIZE": "16384", "RAM_INIT_FILE": '"%s"' % os.path.join(mw, "fpga/firmware.hex"),
               "USE_LITEDRAM": "true", "USE_LITEETH": "true", "USE_LITESDCARD": "true", "NO_BRAM": "true",
               "SPI_FLASH_OFFSET": "3145728", "LOG_LENGTH": "512", "UART_IS_16550": "true", "HAS_UART1": "true",
               "HAS_FPU": "false", "HAS_BTC": "false", "USE_LCD": "false"}
def arty_project(name, sources):
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": "toplevel", "language": "verilog-2005",
               "sources": sources, "includeDirs": [os.path.join(mw, "uart16550")], "parameters": arty_params,
               "constraints": [os.path.join(mw, "fpga/arty_a7.xdc")]},
              open(os.path.join(work, name + ".ocdc.json"), "w"), indent=1)
arty_project("microwatt_arty", arty)
# top-arty.vhdl drives ddram_clk_p_vec/ddram_clk_n_vec and uart1_txd twice (real multiple drivers,
# reported as IR-MULTIDRIVER); the corrected copy lets the analysis run to the end
top = open(os.path.join(mw, "fpga/top-arty.vhdl")).read()
for a, b in [("\tddram_clk_p_vec <= (others => ddram_clk_p);\n", "\tddram_clk_p <= ddram_clk_p_vec(0);\n"),
             ("\tddram_clk_n_vec <= (others => ddram_clk_n);\n", "\tddram_clk_n <= ddram_clk_n_vec(0);\n"),
             ("    uart1_txd <= '1';\n", "")]:
    assert a in top, a
    top = top.replace(a, b)
open(os.path.join(work, "mw_top_arty_fixed.vhdl"), "w").write(top)
arty_project("microwatt_arty_fixed",
             [os.path.join(work, "mw_top_arty_fixed.vhdl") if f.endswith("fpga/top-arty.vhdl") else f for f in arty])
# with the JTAG clock constrained (as --suggest-sdc proposes): no path needs review; the two
# errors are real (sys_reset releases the JTAG-domain registers; the DMI acknowledge is muxed before its synchronizer)
open(os.path.join(work, "mw_jtag.sdc"), "w").write(
    "create_clock -name tck -period 30 [get_pins {soc0/dtm/bscan/TCK}]\n"
    "set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks sys_clk_pin] "
    "-group [get_clocks tck]\n")
j = json.load(open(os.path.join(work, "microwatt_arty_fixed.ocdc.json")))
j["name"] = "microwatt_arty_jtag"
j["constraints"].append("mw_jtag.sdc")
json.dump(j, open(os.path.join(work, "microwatt_arty_jtag.ocdc.json"), "w"), indent=1)
# Digilent IP cores with Vivado IP given as .xci
dl = os.path.join(work, "vivado-library/ip")
zs = os.path.join(dl, "Zmods/ZmodScopeController/src")
json.dump({"format": "opencdc-project", "version": 1, "name": "zmod_scope", "top": "ZmodScopeController",
           "sources": [os.path.join(zs, f) for f in ["PkgZmodADC.vhd", "SyncBase.vhd", "SyncAsync.vhd", "ResetBridge.vhd",
                                                     "HandshakeData.vhd", "ADI_SPI.vhd", "ConfigADC.vhd", "ConfigRelays.vhd",
                                                     "GainOffsetCalib.vhd", "DataPath.vhd", "ZmodScopeController.vhd",
                                                     "ZmodADC_SynchonizationFIFO/ZmodADC_SynchonizationFIFO.xci"]],
           "constraints": [os.path.join(zs, "ConstrZmodADC_ooc.xdc")]},
          open(os.path.join(work, "zmod_scope.ocdc.json"), "w"), indent=1)
mp = os.path.join(dl, "MIPI_CSI_2_RX/hdl")
json.dump({"format": "opencdc-project", "version": 1, "name": "mipi_csi2_rx", "top": "mipi_csi2_rx_top",
           "sources": [os.path.join(mp, f) for f in ["DebugLib.vhd", "SyncAsync.vhd", "SyncAsyncReset.vhd", "ECC.vhd",
                                                     "CRC16_behavioral.vhd", "SimpleFIFO.vhd", "LM.vhd", "LLP.vhd",
                                                     "MIPI_CSI2_Rx.vhd", "MIPI_CSI_2_RX_v1_0_S_AXI_LITE.vhd",
                                                     "MIPI_CSI2_RxTop.vhd", "cdc_fifo/cdc_fifo.xci",
                                                     "line_buffer/line_buffer.xci", "ila_rxclk/ila_rxclk.xci",
                                                     "ila_vidclk/ila_vidclk.xci", "ila_rxclk_lane/ila_rxclk_lane.xci"]],
           "constraints": [os.path.join(mp, "mipi_csi2_rx_ooc.xdc"), os.path.join(mp, "mipi_csi2_rx.xdc")]},
          open(os.path.join(work, "mipi_csi2_rx.ocdc.json"), "w"), indent=1)
# every FIFO .xci of the library as its own top
fifos = []
for root, _, names in os.walk(dl):
    for n in sorted(names):
        if n.endswith(".xci"):
            path = os.path.join(root, n)
            text = open(path, encoding="utf-8", errors="replace").read()
            if 'spirit:name="fifo_generator"' in text or 'spirit:name="axis_data_fifo"' in text:
                fifos.append(path)
for k, path in enumerate(sorted(fifos)):
    name = os.path.splitext(os.path.basename(path))[0]
    json.dump({"format": "opencdc-project", "version": 1, "name": name, "top": name, "sources": [path]},
              open(os.path.join(work, "dl_fifo_%02d.ocdc.json" % k), "w"), indent=1)
open(os.path.join(work, "dl_fifo_count.txt"), "w").write(str(len(fifos)))
# Pmod IP cores: Vivado block design netlists whose IP is given as .xci (ports
# from the component file next to each .xci); Digilent's own IP (pmod_bridge,
# PWM) comes from the library as IP repository
for pm in ["PmodGPIO", "PmodSF3", "PmodCOLOR", "PmodBT2", "PmodAMP2"]:
    d = glob.glob(os.path.join(dl, "Pmods", pm + "_v1_*", "src"))[0]
    json.dump({"format": "opencdc-project", "version": 1, "name": pm, "top": pm,
               "sources": [os.path.join(d, pm + ".v")] + sorted(glob.glob(os.path.join(d, "*", "*.xci"))),
               "constraints": glob.glob(os.path.join(d, "*_ooc.xdc")), "ipRepositories": [dl]},
              open(os.path.join(work, pm.lower() + ".ocdc.json"), "w"), indent=1)
# Digilent's synchronizer example: its XDC constrains each synchronizer with
# current_instance, get_ports -scoped_to_current_instance and per-bit filters
sy = os.path.join(work, "vivado-library/module/synchronizers")
json.dump({"format": "opencdc-project", "version": 1, "name": "dl_sync_example", "top": "top",
           "sources": [os.path.join(sy, f) for f in ["SyncAsync.vhd", "SyncBase.vhd", "ResetBridge.vhd",
                                                     "HandshakeData.vhd", "example/top.vhd"]],
           "constraints": [os.path.join(sy, "example/top.xdc")]},
          open(os.path.join(work, "dl_sync_example.ocdc.json"), "w"), indent=1)
# open-logic interface synchronizer with its scoped constraints (get_ports
# -scoped_to_current_instance -of_objects <nets>)
intf = os.path.join(work, "open-logic/src/intf")
open(os.path.join(work, "olo_intf.sdc"), "w").write("create_clock -name clk -period 10 [get_ports Clk]\n")
json.dump({"format": "opencdc-project", "version": 1, "name": "olo_intf_sync", "top": "olo_intf_sync",
           "sources": [{"file": f, "library": "olo"} for f in files + [os.path.join(intf, "vhdl/olo_intf_sync.vhd")]],
           "parameters": {"Width_g": 4},
           "constraints": ["olo_intf.sdc", {"file": os.path.join(intf, "tcl/olo_intf_sync.tcl"), "ref": "olo_intf_sync"}]},
          open(os.path.join(work, "olo_intf_sync.ocdc.json"), "w"), indent=1)
# Digilent board demos with their Vivado IP
cm = os.path.join(work, "cmod-a7-gpio/src")
json.dump({"format": "opencdc-project", "version": 1, "name": "cmod_a7_gpio", "top": "GPIO_demo",
           "sources": [os.path.join(cm, "hdl", f) for f in ["debouncer.vhd", "RGB_controller.vhd", "UART_TX_CTRL.vhd",
                                                            "GPIO.vhd"]] + [os.path.join(cm, "ip/clk_wiz_0/clk_wiz_0.xci")],
           "constraints": [os.path.join(cm, "constraints/CmodA7_Master.xdc")]},
          open(os.path.join(work, "cmod_a7_gpio.ocdc.json"), "w"), indent=1)
nx = os.path.join(work, "nexys-a7-oob/src")
json.dump({"format": "opencdc-project", "version": 1, "name": "nexys_a7_oob", "top": "Nexys4DdrUserDemo",
           "sources": sorted(glob.glob(os.path.join(nx, "hdl/*.vhd"))) +
                      [os.path.join(nx, "ip", n, n + ".xci") for n in ["ClkGen", "BRAM_1", "Square_Root", "ddr"]],
           "constraints": [os.path.join(nx, "constraints/Nexys-A7-100T-Master.xdc")],
           "options": {"unknownModules": "blackbox"}},
          open(os.path.join(work, "nexys_a7_oob.ocdc.json"), "w"), indent=1)
EOF

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
check olo_base_cc_bits.ocdc.json      "Summary : 0 error.*CDC     : 1 crossing path\(s\), 1 synchronized"
check olo_base_cc_pulse.ocdc.json     "Summary : 0 error.*CDC     : 5 crossing path\(s\), 5 synchronized"
check olo_base_cc_simple.ocdc.json    "Summary : 0 error.*CDC     : 6 crossing path\(s\), 6 synchronized"
check olo_base_cc_status.ocdc.json    "handshake x1.*Summary : 0 error.*CDC     : 11 crossing path\(s\), 11 synchronized"
check olo_base_cc_handshake.ocdc.json "handshake x1.*Summary : 0 error.*CDC     : 11 crossing path\(s\), 11 synchronized"
check olo_base_fifo_async.ocdc.json   "0 latches.*gray x2.*Summary : 0 error.*CDC     : 7 crossing path\(s\), 7 synchronized"
check surf_cm7_mmcm.ocdc.json         "Clocks  : 5.*reset-sync x3.*MMCME2_ADV.*Summary : 0 error.*CDC     : 3 crossing path\(s\), 3 synchronized"
check surf_cm7_pll.ocdc.json          "Clocks  : 5.*reset-sync x3.*PLLE2_ADV.*Summary : 0 error.*CDC     : 3 crossing path\(s\), 3 synchronized"
check surf_fifo_async.ocdc.json       "Summary : 0 error.*CDC     : 7 crossing path\(s\).*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 0 path\(s\) at potential risk"
check surf_gty_qpll.ocdc.json         "GTYE4_COMMON.*Summary : 0 error\(s\), 0 warning\(s\)"
check microwatt.ocdc.json             "RTL     : 50 module bodies, 1217 registers.*0 latches.*CLKOP  \\[divider\\].*\\(40 MHz\\).*1217/1217 registers resolved.*EHXPLLL.*Summary : 0 error\\(s\\), 1 warning"
check microwatt_arty.ocdc.json        "IR-MULTIDRIVER.*ddram_clk_p_vec.*IR-MULTIDRIVER.*ddram_clk_n_vec.*IR-MULTIDRIVER.*uart1_txd.*Summary : 3 error\\(s\\), 2 warning"
check microwatt_arty_fixed.ocdc.json  "Clocks  : 8 \\(2445/2445 registers resolved\\).*bscan.TCK.*Summary : 0 error\\(s\\), 2 warning.*CDC     : 173 crossing path\\(s\\), 170 synchronized"
check microwatt_arty_jtag.ocdc.json   "hits: CDC-004 x1, CDC-005 x3, CDC-006 x2.*Rules   : 2 error violation\\(s\\), 0 warning violation\\(s\\), 4 path\\(s\\) at potential risk, 0 path\\(s\\) need review"
check neorv32.ocdc.json               "Instances: 110.*RTL     : 77 module bodies, 7[0-9][0-9] registers.*0 latches.*Summary : 0 error"
# ADI_SPI.vhd instantiates IOBUF through unisim.vcomponents (declared since 0.41)
check zmod_scope.ocdc.json  "Clocks  : 6 \\(170/170 registers resolved\\).*async-fifo x1, handshake x1, reset-sync x6, two-flop x8.*IP 'ZmodADC_SynchonizationFIFO' \\(xilinx.com:ip:fifo_generator:13.2\\) modelled.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 21 crossing path\\(s\\), 18 synchronized"
check mipi_csi2_rx.ocdc.json "Clocks  : 3 \\(94/94 registers resolved\\).*async-fifo x1, reset-sync x3, two-flop x5.*IP 'cdc_fifo'.*AXI-Stream.*IP 'line_buffer'.*IP 'ila_rxclk'.*Summary : 0 error\\(s\\), 1 warning.*CDC     : 12 crossing path\\(s\\), 11 synchronized, 1 unsynchronized"
check cmod_a7_gpio.ocdc.json "Clocks  : 2 \\(37/37 registers resolved\\).*alias=clk_out1_clk_wiz_0.*\\(100 MHz\\).*IP 'clk_wiz_0' \\(xilinx.com:ip:clk_wiz:6.0\\) modelled.*MMCM, input 12 MHz.*Summary : 0 error\\(s\\), 0 warning"
check nexys_a7_oob.ocdc.json "Clocks  : 4 \\(383/383 registers resolved\\).*Inst_DDR.ui_clk  \\[divider\\] alias=mem_ui_clk alias=clk_pll_i.*\\(75.01 MHz\\).*IP 'ClkGen'.*IP 'BRAM_1'.*single-port ROM.*IP 'Square_Root'.*cordic.*IP 'ddr' \\(xilinx.com:ip:mig_7series:4.1\\) modelled.*DDR2 x16, native interface.*BUFG, MMCME2_ADV, XADC.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 4 crossing path\\(s\\), 2 synchronized, 2 unsynchronized"
check pmodgpio.ocdc.json "Clocks  : 1 \\(10/10 registers resolved\\).*IP 'PmodGPIO_axi_gpio_0_0' \\(xilinx.com:ip:axi_gpio:2.0\\) modelled.*IP 'PmodGPIO_pmod_bridge_0_0' \\(digilentinc.com:ip:pmod_bridge:1.0\\) modelled.*from the IP repository.*top module 'pmod_concat'.*IP 'PmodGPIO_xlconcat_0_0'.*IP 'PmodGPIO_xlslice_0_0'.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 0 crossing path"
check pmodsf3.ocdc.json  "Clocks  : 2 \\(51/51 registers resolved\\).*async-fifo x2, reset-sync x4.*IP 'PmodSF3_axi_quad_spi_0_0'.*asynchronous FIFOs.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 10 crossing path\\(s\\), 10 synchronized, 0 unsynchronized"
check pmodcolor.ocdc.json "Clocks  : 1 \\(25/25 registers resolved\\).*IP 'PmodCOLOR_axi_iic_0_0' \\(xilinx.com:ip:axi_iic:2.0\\) modelled.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 0 crossing path"
check pmodbt2.ocdc.json  "Clocks  : 1 \\(28/28 registers resolved\\).*IP 'PmodBT2_axi_uart16550_0_0' \\(xilinx.com:ip:axi_uart16550:2.0\\) modelled.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 0 crossing path"
check pmodamp2.ocdc.json "Clocks  : 1 \\(39/39 registers resolved\\).*IP 'PmodAMP2_PWM_0_0' \\(digilentinc.com:IP:PWM:2.0\\) modelled.*top module 'PWM_v2_0'.*IR-MIXED-ASSIGN.*reg_data_out.*Summary : 0 error\\(s\\), 1 warning.*CDC     : 0 crossing path"
# the XDC's per-instance constraints all resolve; its datapath-only max delay
# makes the handshake's data path safe
check dl_sync_example.ocdc.json "Clocks  : 2 \\(17/17 registers resolved\\).*Summary : 0 error\\(s\\), 0 warning.*CDC     : 7 crossing path\\(s\\), 7 synchronized.*OneClk -> TwoClk \\(asynchronous\\): 5 path\\(s\\), 5 synchronized, 4 need review"
check olo_intf_sync.ocdc.json "Summary : 0 error\\(s\\), 0 warning"
# Transceivers Wizard models in SURF: the transceiver user clocks are clocks of their own
check surf_pgp3_gth_wrapper.ocdc.json "Clocks  : 6 \\(195/195 registers resolved\\).*IP 'Pgp3GthUsIp10G' \\(xilinx.com:ip:gtwizard_ultrascale:1.7\\) modelled.*GTHE4.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 32 crossing path\\(s\\), 22 synchronized"
# the core: the 9 errors are genuine in SURF's configuration (pgpRxClk is the TX usrclk2)
check surf_pgp3_gth_core.ocdc.json    "Clocks  : 6 \\(2158/2158 registers resolved\\).*Summary : 0 error\\(s\\), 1 warning.*CDC     : 209 crossing path\\(s\\), 145 synchronized.*Rules   : 9 error violation"
check surf_pgp2fc_gty_wrapper.ocdc.json "Clocks  : 5 \\(458/458 registers resolved\\).*IP 'Pgp2fcGtyCore'.*user clocking outside the IP.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 52 crossing path\\(s\\), 36 synchronized"
# ClockManager7's own DRP is left unconnected in SURF's wrapper (axilClk at its default '0'): CLK-CONSTANT
check surf_pgp3_gtx7_wrapper.ocdc.json "Clocks  : 6 \\(200/216 registers resolved\\).*IP 'Pgp3Gtx7Ip10G' \\(xilinx.com:ip:gtwizard:3.6\\) modelled.*gt0_rxoutclk_out 322.265625 MHz.*VHDL component declaration.*CLK-CONSTANT.*Summary : 0 error\\(s\\), 1 warning.*CDC     : 34 crossing path\\(s\\), 24 synchronized.*Rules   : 0 error violation\\(s\\), 0 warning violation\\(s\\), 9 path\\(s\\) at potential risk, 1 path\\(s\\) need review"
check surf_pgp3_gtp7_wrapper.ocdc.json "Clocks  : 6 \\(269/269 registers resolved\\).*IP 'Pgp3Gtp7Ip6G' \\(xilinx.com:ip:gtwizard:3.6\\) modelled.*7-series GTP.*gt0_rxoutclk_out 390.625 MHz.*Summary : 0 error\\(s\\), 0 warning.*CDC     : 43 crossing path\\(s\\), 31 synchronized"
n=$(cat gtx_count.txt)
[ "$n" = 30 ] || { echo "::error::expected 30 Transceivers Wizard .xci files in surf, found $n"; fail=1; }
for f in gtx_*.ocdc.json; do
    out=$("$BIN/opencdc" analyze "$f" -q 2>&1 || true)
    if ! grep -q "FE-XCI-MODEL" <<<"$out" || ! grep -q "Summary : 0 error" <<<"$out"; then
        echo "::error::$f: Transceivers Wizard .xci not modelled"; echo "$out" | tail -10; fail=1
    fi
done
echo "ok   $n surf Transceivers Wizard .xci models"
# each FIFO .xci alone: modelled; with independent clocks an asynchronous FIFO
n=$(cat dl_fifo_count.txt)
[ "$n" = 13 ] || { echo "::error::expected 13 FIFO .xci files in vivado-library, found $n"; fail=1; }
for f in dl_fifo_*.ocdc.json; do
    out=$("$BIN/opencdc" analyze "$f" -q 2>&1 || true)
    if ! grep -q "FE-XCI-MODEL" <<<"$out" || ! grep -q "Summary : 0 error" <<<"$out"; then
        echo "::error::$f: FIFO .xci not modelled"; echo "$out" | tail -10; fail=1
    elif grep -q "independent clocks" <<<"$out" && ! grep -q "async-fifo x1" <<<"$out"; then
        echo "::error::$f: independent-clock FIFO not recognized as an asynchronous FIFO"; fail=1
    fi
done
echo "ok   $n Digilent FIFO .xci models"
exit $fail

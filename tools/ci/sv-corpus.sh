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
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
}
fetch axi          https://github.com/pulp-platform/axi          39f5f2d51c5e524f6fc5cf8b6e901f7dcc5622d7
fetch common_cells https://github.com/pulp-platform/common_cells c27bce39ebb2e6bae52f60960814a2afca7bd4cb
fetch verilog-ethernet https://github.com/alexforencich/verilog-ethernet 77320a9471d19c7dd383914bc049e02d9f4f1ffb
fetch adi-hdl      https://github.com/analogdevicesinc/hdl       c9a501d87a420d2e28aa9d931557bdb4122f3c4f

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
check() { # project expected-regex
    local out
    out=$("$BIN/opencdc" analyze "$1" -q 2>&1 || true)
    if echo "$out" | tr '\n' ' ' | grep -Eq "$2"; then
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
exit $fail

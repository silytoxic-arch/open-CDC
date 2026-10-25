#!/usr/bin/env bash
# SystemVerilog / Verilog regression on real open-source code, pinned to fixed commits.
#
#   tools/ci/sv-corpus.sh <bin-dir> [work-dir]
#
# Analyzes PULP's AXI clock-domain crossing through its interface wrapper
# (axi_cdc_intf: AXI_BUS interfaces with modports, packages, typedef macros,
# common_cells Gray FIFOs), alexforencich's verilog-ethernet Arty example with
# its own Vivado Tcl constraint scripts, and the verilog-ethernet RGMII MAC with
# its asynchronous frame FIFOs (Xilinx IDDR/ODDR/BUFR/BUFIO, four clocks); the
# CDC summaries must match.
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
check rgmii.ocdc.json "179/179 registers resolved.*async-fifo x2.*Summary : 0 error\(s\), 3 warning\(s\).*CDC     : 30 crossing path\(s\), 29 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 1 path\(s\) at potential risk, 1 path\(s\) need review"
check arty_eth.ocdc.json "608/608 registers resolved.*Summary : 0 error\(s\), 9 warning\(s\).*CDC     : 27 crossing path\(s\), 27 synchronized.*Rules   : 0 error violation\(s\), 0 warning violation\(s\), 0 path\(s\) at potential risk, 0 path\(s\) need review"
exit $fail

#!/usr/bin/env bash
# SystemVerilog regression on real open-source code, pinned to fixed commits.
#
#   tools/ci/sv-corpus.sh <bin-dir> [work-dir]
#
# Analyzes PULP's AXI clock-domain crossing through its interface wrapper
# (axi_cdc_intf: AXI_BUS interfaces with modports, packages, typedef macros,
# common_cells Gray FIFOs); the CDC summary must match.
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
exit $fail

#!/usr/bin/env bash
# VHDL regression on real open-source code, pinned to fixed commits.
#
#   tools/ci/vhdl-corpus.sh <bin-dir> [work-dir]
#
# 1. Parses every .vhd/.vhdl file of five projects (~1900 files); only the
#    files listed in REJECT (documentation snippets, templates, VHDL-2019)
#    may fail, and they must fail.
# 2. Elaborates and analyzes the open-logic clock-crossing components and a
#    fully featured neorv32 SoC; the CDC summaries must match.
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
fetch open-logic https://github.com/open-logic/open-logic d9f123aae1c6022288782668067311f9794da0a2
fetch neorv32    https://github.com/stnolting/neorv32    6536b6f43b8372654fff56bc448852bb8f128094
fetch PoC        https://github.com/VLSI-EDA/PoC         8c39b2407a97ec81b8601c4b64e4d2e2141ab9cf
fetch osvvm      https://github.com/OSVVM/OSVVM          716d751566b4fb6479f69c38cc01790744c911b7
fetch surf       https://github.com/slaclab/surf         f0b5e3a7b5ab191be1cb5f4453e418299181c9d8

# ---- 1. parser -------------------------------------------------------------
REJECT=(
    open-logic/doc/axi/slave/RbExample.vhd        # documentation snippet, not a design file
    osvvm/OsvvmSettingsPkg_template.vhd           # template with placeholders
    osvvm/DynamicVectorGenericPkg.vhd             # VHDL-2019 syntax
)
fail=0
for repo in open-logic neorv32 PoC osvvm surf; do
    mapfile -d '' files < <(find "$repo" \( -name '*.vhd' -o -name '*.vhdl' \) -print0 | sort -z)
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
               "parameters": gen, "constraints": ["olo.sdc"]},
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
EOF

check() { # project expected-regex
    local out
    out=$("$BIN/opencdc" analyze "$1" -q 2>&1 || true)
    if echo "$out" | tr '\n' ' ' | grep -Eq "$2"; then
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
check neorv32.ocdc.json               "Instances: 110.*RTL     : 77 module bodies, 7[0-9][0-9] registers.*0 latches.*Summary : 0 error"
exit $fail

#!/usr/bin/env python3
"""Generates the OpenCDC models of Xilinx GT transceivers (channels, commons,
reference clock buffers). Output: Verilog text on stdout."""

def channel(name, prefix, txw, rxw, refclks, plls):
    ins = [(r, 1) for r in refclks] + [(p, 1) for p in plls] + [
        (prefix + "RXP", 1), (prefix + "RXN", 1),
        ("RXUSRCLK", 1), ("RXUSRCLK2", 1), ("TXUSRCLK", 1), ("TXUSRCLK2", 1),
        ("TXDATA", txw), ("TXCHARISK", txw // 8), ("TXCTRL0", 16), ("TXCTRL1", 16), ("TXCTRL2", 8),
        ("GTRXRESET", 1), ("GTTXRESET", 1), ("RXUSERRDY", 1), ("TXUSERRDY", 1),
        ("CPLLRESET", 1), ("CPLLPD", 1), ("CPLLLOCKDETCLK", 1), ("CPLLLOCKEN", 1),
        ("DRPCLK", 1), ("DRPEN", 1), ("DRPWE", 1), ("DRPADDR", 10), ("DRPDI", 16),
        ("TXOUTCLKSEL", 3), ("RXOUTCLKSEL", 3), ("RXPD", 2), ("TXPD", 2), ("LOOPBACK", 3),
        ("RXPOLARITY", 1), ("TXPOLARITY", 1), ("RXCOMMADETEN", 1), ("RXMCOMMAALIGNEN", 1),
        ("RXPCOMMAALIGNEN", 1), ("RX8B10BEN", 1), ("TX8B10BEN", 1), ("RXSYSCLKSEL", 2), ("TXSYSCLKSEL", 2),
        ("RXPROGDIVRESET", 1), ("TXPROGDIVRESET", 1), ("RXPMARESET", 1), ("TXPMARESET", 1),
        ("RXPCSRESET", 1), ("TXPCSRESET", 1), ("RXBUFRESET", 1), ("EYESCANRESET", 1),
    ]
    outs = [
        (prefix + "TXP", 1), (prefix + "TXN", 1),
        ("RXDATA", rxw), ("RXCHARISK", rxw // 8), ("RXCTRL0", 16), ("RXCTRL1", 16), ("RXCTRL2", 8),
        ("RXCTRL3", 8), ("RXDISPERR", rxw // 8), ("RXNOTINTABLE", rxw // 8),
        ("RXOUTCLK", 1), ("TXOUTCLK", 1), ("RXOUTCLKFABRIC", 1), ("TXOUTCLKFABRIC", 1),
        ("RXOUTCLKPCS", 1), ("TXOUTCLKPCS", 1),
        ("CPLLLOCK", 1), ("RXRESETDONE", 1), ("TXRESETDONE", 1), ("RXPMARESETDONE", 1), ("TXPMARESETDONE", 1),
        ("RXBYTEISALIGNED", 1), ("RXBYTEREALIGN", 1), ("RXCOMMADET", 1), ("RXCDRLOCK", 1),
        ("DRPDO", 16), ("DRPRDY", 1),
    ]
    def w(n):
        return f"[{n - 1}:0] " if n > 1 else ""
    ports = [f"input {w(n)}{p}" for p, n in ins] + [f"output {w(n)}{p}" for p, n in outs]
    t = [f"module {name} #(parameter SIM_RECEIVER_DETECT_PASS = \"TRUE\") ("]
    t.append("    " + ",\n    ".join(ports))
    t.append(");")
    t.append("    // Recovered and transmit clocks are clock sources of their own: their")
    t.append("    // relation to fabric clocks is unknown until constrained (CDC-010).")
    for clk in ("RXOUTCLK", "TXOUTCLK", "RXOUTCLKPCS", "TXOUTCLKPCS"):
        t.append(f"    (* OPENCDC_CLOCK_PIN = \"{clk}\" *) wire {clk.lower()}_src;")
        t.append(f"    opencdc_async_source u_{clk.lower()} ({clk.lower()}_src);")
        t.append(f"    assign {clk} = {clk.lower()}_src;")
    t.append("    assign RXOUTCLKFABRIC = rxoutclk_src;")
    t.append("    assign TXOUTCLKFABRIC = txoutclk_src;")
    t.append("    // transmit data is captured on TXUSRCLK2, receive data comes out on RXUSRCLK2")
    t.append(f"    reg [{txw - 1}:0] tx_q;")
    t.append("    always @(posedge TXUSRCLK2) tx_q <= TXDATA;")
    t.append(f"    assign {prefix}TXP = ^tx_q;")
    t.append(f"    assign {prefix}TXN = ~^tx_q;")
    t.append("    wire line;")
    t.append("    opencdc_async_source u_line (line); // serial data: no clock domain")
    t.append(f"    reg [{rxw - 1}:0] rx_q;")
    t.append(f"    always @(posedge RXUSRCLK2) rx_q <= {{{rxw}{{line}}}};")
    t.append("    assign RXDATA = rx_q;")
    t.append(f"    assign RXCHARISK = {rxw // 8}'d0;")
    t.append(f"    assign RXDISPERR = {rxw // 8}'d0;")
    t.append(f"    assign RXNOTINTABLE = {rxw // 8}'d0;")
    for p, n in (("RXCTRL0", 16), ("RXCTRL1", 16), ("RXCTRL2", 8), ("RXCTRL3", 8)):
        t.append(f"    assign {p} = {n}'d0;")
    t.append("    // status outputs settle asynchronously (the wizards synchronize them)")
    for st in ("CPLLLOCK", "RXRESETDONE", "TXRESETDONE", "RXPMARESETDONE", "TXPMARESETDONE", "RXCDRLOCK"):
        t.append(f"    opencdc_async_source u_{st.lower()} ({st});")
    t.append("    reg aligned;")
    t.append("    always @(posedge RXUSRCLK2) aligned <= line;")
    t.append("    assign RXBYTEISALIGNED = aligned;")
    t.append("    assign RXBYTEREALIGN = 1'b0;")
    t.append("    assign RXCOMMADET = aligned;")
    t.append("    assign DRPDO = DRPDI; // DRP: in the DRPCLK domain of the caller")
    t.append("    assign DRPRDY = DRPEN;")
    t.append("endmodule\n")
    return "\n".join(t)


def common(name, refclks, outs):
    ports = [f"input {r}" for r in refclks] + ["input QPLLLOCKDETCLK", "input QPLL0LOCKDETCLK", "input QPLL1LOCKDETCLK",
                                               "input QPLLRESET", "input QPLL0RESET", "input QPLL1RESET",
                                               "input QPLLPD", "input QPLL0PD", "input QPLL1PD",
                                               "input DRPCLK", "input DRPEN", "input DRPWE", "input [15:0] DRPDI",
                                               "input [9:0] DRPADDR", "output [15:0] DRPDO", "output DRPRDY"]
    ports += [f"output {o}" for o, _ in outs]
    t = [f"module {name} #(parameter SIM_RESET_SPEEDUP = \"TRUE\") ("]
    t.append("    " + ",\n    ".join(ports))
    t.append(");")
    t.append(f"    // PLL outputs: derived from {refclks[0]}; reference outputs pass it through")
    for o, kind in outs:
        if kind == "clk":
            t.append(f"    (* OPENCDC_CLOCK_PIN = \"{o}\" *) reg {o}_q = 1'b0;")
            t.append(f"    always @(posedge {refclks[0]}) {o}_q <= ~{o}_q;")
            t.append(f"    assign {o} = {o}_q;")
        elif kind == "ref":
            t.append(f"    assign {o} = {refclks[0]};")
        else:
            t.append(f"    opencdc_async_source u_{o.lower()} ({o});")
    t.append("    assign DRPDO = 16'd0;")
    t.append("    assign DRPRDY = 1'b0;")
    t.append("endmodule\n")
    return "\n".join(t)


out = ["", "// ---------------------------------------------------------------- GT transceivers",
       "// Generated by the OpenCDC model generator: the clock and data structure of",
       "// the channel and common primitives (pins a design connects that are not",
       "// listed here are accepted and ignored, see FE-PRIMITIVE-PORT).", ""]
s7 = ["GTREFCLK0", "GTREFCLK1", "QPLLCLK", "QPLLREFCLK"]
us = ["GTREFCLK0", "GTREFCLK1", "QPLL0CLK", "QPLL0REFCLK", "QPLL1CLK", "QPLL1REFCLK"]
out.append(channel("GTXE2_CHANNEL", "GTX", 64, 64, s7, []))
out.append(channel("GTHE2_CHANNEL", "GTH", 64, 64, s7, []))
out.append(channel("GTPE2_CHANNEL", "GTP", 32, 32, ["GTREFCLK0", "GTREFCLK1"], ["PLL0CLK", "PLL1CLK", "PLL0REFCLK", "PLL1REFCLK"]))
for n, p in (("GTHE3_CHANNEL", "GTH"), ("GTHE4_CHANNEL", "GTH"), ("GTYE3_CHANNEL", "GTY"), ("GTYE4_CHANNEL", "GTY")):
    out.append(channel(n, p, 128, 128, us, []))
c7 = [("QPLLOUTCLK", "clk"), ("QPLLOUTREFCLK", "ref"), ("QPLLLOCK", "async")]
cus = [("QPLL0OUTCLK", "clk"), ("QPLL0OUTREFCLK", "ref"), ("QPLL0LOCK", "async"),
       ("QPLL1OUTCLK", "clk"), ("QPLL1OUTREFCLK", "ref"), ("QPLL1LOCK", "async")]
out.append(common("GTXE2_COMMON", ["GTREFCLK0", "GTREFCLK1"], c7))
out.append(common("GTHE2_COMMON", ["GTREFCLK0", "GTREFCLK1"], c7))
out.append(common("GTPE2_COMMON", ["GTREFCLK0", "GTREFCLK1"],
                  [("PLL0OUTCLK", "clk"), ("PLL0OUTREFCLK", "ref"), ("PLL0LOCK", "async"),
                   ("PLL1OUTCLK", "clk"), ("PLL1OUTREFCLK", "ref"), ("PLL1LOCK", "async")]))
for n in ("GTHE3_COMMON", "GTHE4_COMMON", "GTYE3_COMMON", "GTYE4_COMMON"):
    out.append(common(n, ["GTREFCLK00", "GTREFCLK01", "GTREFCLK10", "GTREFCLK11"], cus))
for n in ("IBUFDS_GTE3", "IBUFDS_GTE4"):
    out.append(f"module {n} #(parameter REFCLK_EN_TX_PATH = 1'b0, parameter [1:0] REFCLK_HROW_CK_SEL = 2'b00,\n"
               f"                parameter [1:0] REFCLK_ICNTL_RX = 2'b00)\n"
               f"               (output O, output ODIV2, input CEB, input I, input IB);\n"
               f"    assign O = I;\n    assign ODIV2 = I; // HROW divider not modelled\nendmodule\n")
print("\n".join(out))

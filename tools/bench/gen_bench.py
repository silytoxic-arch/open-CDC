#!/usr/bin/env python3
"""Generates OpenCDC benchmark projects (Verilog-2001 + SDC + .ocdc.json).

  gen_bench.py soc --domains 8 --blocks 1250 --out bench/soc_100k    (~102k registers)
  gen_bench.py soc --domains 8 --blocks 12500 --out bench/soc_1m     (~1.02M registers)
  gen_bench.py pipeline --stages 340000 --width 1 --out bench/pipe_1m (~1.02M registers)

soc:      D clock domains (create_clock + set_clock_groups -asynchronous).
          Every domain has B datapath blocks (3 registers each: input,
          accumulator, output, W bits). Block i of domain d sends to block i
          of domain d+1 through a two-flop synchronized valid and a data bus
          captured under it (qualified data, safe). Every 16th block uses a
          Gray-pointer FIFO instead, every 64th has a deliberate bug
          (unsynchronized bus, CDC-003).
pipeline: one clock, S stages of 3 registers (W bits) with an
          asynchronous reset through a reset synchronizer: pure size test.
"""
import argparse
import json
import os

BLOCK = """
module dp_block #(parameter W = 8) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [W-1:0] din,
    input  wire         din_valid,
    output reg  [W-1:0] dout,
    output reg          dout_valid
);
    reg [W-1:0] r_in, acc;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) begin
            r_in <= {W{1'b0}}; acc <= {W{1'b0}}; dout <= {W{1'b0}}; dout_valid <= 1'b0;
        end else begin
            if (din_valid) r_in <= din;
            acc <= acc + r_in;
            dout <= acc ^ r_in;
            dout_valid <= din_valid;
        end
endmodule

module sync2 (input wire clk, input wire rst_n, input wire d, output wire q);
    (* ASYNC_REG = "TRUE" *) reg s1, s2;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) begin s1 <= 1'b0; s2 <= 1'b0; end
        else begin s1 <= d; s2 <= s1; end
    assign q = s2;
endmodule

module rst_sync (input wire clk, input wire rst_in_n, output wire rst_out_n);
    (* ASYNC_REG = "TRUE" *) reg [1:0] r;
    always @(posedge clk or negedge rst_in_n)
        if (!rst_in_n) r <= 2'b00; else r <= {r[0], 1'b1};
    assign rst_out_n = r[1];
endmodule

// Qualified-data link: the source holds data and toggles a valid level;
// the destination captures the data when the synchronized valid changes.
module link_qual #(parameter W = 8) (
    input  wire         sclk, input wire srst_n, input wire [W-1:0] sdata, input wire svalid,
    input  wire         dclk, input wire drst_n, output reg [W-1:0] ddata, output reg dvalid
);
    reg [W-1:0] hold; reg tog;
    always @(posedge sclk or negedge srst_n)
        if (!srst_n) begin hold <= {W{1'b0}}; tog <= 1'b0; end
        else if (svalid) begin hold <= sdata; tog <= ~tog; end
    wire tog_s; reg tog_d;
    sync2 u_s (.clk(dclk), .rst_n(drst_n), .d(tog), .q(tog_s));
    always @(posedge dclk or negedge drst_n)
        if (!drst_n) begin tog_d <= 1'b0; ddata <= {W{1'b0}}; dvalid <= 1'b0; end
        else begin
            tog_d <= tog_s;
            dvalid <= tog_s ^ tog_d;
            if (tog_s ^ tog_d) ddata <= hold;
        end
endmodule

// Deliberate bug: bus captured directly in the other domain (CDC-003).
module link_bad #(parameter W = 8) (
    input wire sclk, input wire [W-1:0] sdata,
    input wire dclk, output reg [W-1:0] ddata, output reg dvalid
);
    reg [W-1:0] hold;
    always @(posedge sclk) hold <= sdata;
    always @(posedge dclk) begin ddata <= hold; dvalid <= 1'b1; end
endmodule

// Gray-pointer FIFO (depth 4).
module link_fifo #(parameter W = 8) (
    input  wire         sclk, input wire srst_n, input wire [W-1:0] sdata, input wire svalid,
    input  wire         dclk, input wire drst_n, output reg [W-1:0] ddata, output reg dvalid
);
    reg [W-1:0] mem [0:3];
    reg [2:0] wbin, wgray, rbin, rgray;
    (* ASYNC_REG = "TRUE" *) reg [2:0] w2r1, w2r2, r2w1, r2w2;
    wire full = wgray == {~r2w2[2:1], r2w2[0]};
    wire empty = rgray == w2r2;
    wire [2:0] wbin_n = wbin + {2'b00, svalid & ~full};
    wire [2:0] rbin_n = rbin + {2'b00, ~empty};
    always @(posedge sclk or negedge srst_n)
        if (!srst_n) begin wbin <= 3'd0; wgray <= 3'd0; r2w1 <= 3'd0; r2w2 <= 3'd0; end
        else begin
            if (svalid && !full) mem[wbin[1:0]] <= sdata;
            wbin <= wbin_n; wgray <= wbin_n ^ (wbin_n >> 1);
            r2w1 <= rgray; r2w2 <= r2w1;
        end
    always @(posedge dclk or negedge drst_n)
        if (!drst_n) begin rbin <= 3'd0; rgray <= 3'd0; w2r1 <= 3'd0; w2r2 <= 3'd0; ddata <= {W{1'b0}}; dvalid <= 1'b0; end
        else begin
            rbin <= rbin_n; rgray <= rbin_n ^ (rbin_n >> 1);
            w2r1 <= wgray; w2r2 <= w2r1;
            dvalid <= ~empty;
            if (!empty) ddata <= mem[rbin[1:0]];
        end
endmodule
"""


def soc(domains, blocks, width):
    lines = [BLOCK]
    lines.append("module domain #(parameter B = 4, parameter W = 8) (")
    lines.append("    input wire clk, input wire rst_n,")
    lines.append("    input wire [B*W-1:0] din, input wire [B-1:0] din_valid,")
    lines.append("    output wire [B*W-1:0] dout, output wire [B-1:0] dout_valid);")
    lines.append("    genvar i;")
    lines.append("    generate for (i = 0; i < B; i = i + 1) begin : g")
    lines.append("        dp_block #(.W(W)) u (.clk(clk), .rst_n(rst_n), .din(din[i*W +: W]), .din_valid(din_valid[i]),")
    lines.append("                             .dout(dout[i*W +: W]), .dout_valid(dout_valid[i]));")
    lines.append("    end endgenerate")
    lines.append("endmodule\n")
    lines.append("module links #(parameter B = 4, parameter W = 8) (")
    lines.append("    input wire sclk, input wire srst_n, input wire [B*W-1:0] sdata, input wire [B-1:0] svalid,")
    lines.append("    input wire dclk, input wire drst_n, output wire [B*W-1:0] ddata, output wire [B-1:0] dvalid);")
    lines.append("    genvar i;")
    lines.append("    generate for (i = 0; i < B; i = i + 1) begin : g")
    lines.append("        if (i % 64 == 63) begin : bad")
    lines.append("            link_bad #(.W(W)) u (.sclk(sclk), .sdata(sdata[i*W +: W]), .dclk(dclk), .ddata(ddata[i*W +: W]), .dvalid(dvalid[i]));")
    lines.append("        end else if (i % 16 == 15) begin : fifo")
    lines.append("            link_fifo #(.W(W)) u (.sclk(sclk), .srst_n(srst_n), .sdata(sdata[i*W +: W]), .svalid(svalid[i]),")
    lines.append("                                  .dclk(dclk), .drst_n(drst_n), .ddata(ddata[i*W +: W]), .dvalid(dvalid[i]));")
    lines.append("        end else begin : qual")
    lines.append("            link_qual #(.W(W)) u (.sclk(sclk), .srst_n(srst_n), .sdata(sdata[i*W +: W]), .svalid(svalid[i]),")
    lines.append("                                  .dclk(dclk), .drst_n(drst_n), .ddata(ddata[i*W +: W]), .dvalid(dvalid[i]));")
    lines.append("        end")
    lines.append("    end endgenerate")
    lines.append("endmodule\n")
    ports = ", ".join(f"input wire clk{d}" for d in range(domains))
    lines.append(f"module soc_top ({ports}, input wire rst_n, input wire [{blocks}*{width}-1:0] din,")
    lines.append(f"                input wire [{blocks}-1:0] din_valid, output wire [{blocks}*{width}-1:0] dout,")
    lines.append(f"                output wire [{blocks}-1:0] dout_valid);")
    for d in range(domains):
        lines.append(f"    wire rst{d}_n;")
        lines.append(f"    rst_sync u_rst{d} (.clk(clk{d}), .rst_in_n(rst_n), .rst_out_n(rst{d}_n));")
        lines.append(f"    wire [{blocks}*{width}-1:0] in{d}, out{d};")
        lines.append(f"    wire [{blocks}-1:0] inv{d}, outv{d};")
        lines.append(f"    domain #(.B({blocks}), .W({width})) u_d{d} (.clk(clk{d}), .rst_n(rst{d}_n), .din(in{d}),"
                     f" .din_valid(inv{d}), .dout(out{d}), .dout_valid(outv{d}));")
    lines.append("    assign in0 = din;")
    lines.append("    assign inv0 = din_valid;")
    for d in range(domains - 1):
        lines.append(f"    links #(.B({blocks}), .W({width})) u_l{d} (.sclk(clk{d}), .srst_n(rst{d}_n), .sdata(out{d}),"
                     f" .svalid(outv{d}), .dclk(clk{d+1}), .drst_n(rst{d+1}_n), .ddata(in{d+1}), .dvalid(inv{d+1}));")
    lines.append(f"    assign dout = out{domains-1};")
    lines.append(f"    assign dout_valid = outv{domains-1};")
    lines.append("endmodule")
    sdc = [f"create_clock -name clk{d} -period {4 + d} [get_ports clk{d}]" for d in range(domains)]
    sdc.append("set_clock_groups -asynchronous " + " ".join(f"-group clk{d}" for d in range(domains)))
    return "\n".join(lines) + "\n", "\n".join(sdc) + "\n", "soc_top"


def pipeline(stages, width):
    v = f"""
module stage #(parameter W = {width}) (input wire clk, input wire rst_n, input wire [W-1:0] d, output reg [W-1:0] q);
    reg [W-1:0] a, b;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) begin a <= {{W{{1'b0}}}}; b <= {{W{{1'b0}}}}; q <= {{W{{1'b0}}}}; end
        else begin a <= d; b <= a ^ b; q <= b; end
endmodule

module rst_sync (input wire clk, input wire rst_in_n, output wire rst_out_n);
    reg [1:0] r;
    always @(posedge clk or negedge rst_in_n)
        if (!rst_in_n) r <= 2'b00; else r <= {{r[0], 1'b1}};
    assign rst_out_n = r[1];
endmodule

module pipe_top (input wire clk, input wire rst_n, input wire [{width}-1:0] d, output wire [{width}-1:0] q);
    wire rs_n;
    rst_sync u_rst (.clk(clk), .rst_in_n(rst_n), .rst_out_n(rs_n));
    wire [{width}-1:0] c [0:{stages}];
    assign c[0] = d;
    genvar i;
    generate for (i = 0; i < {stages}; i = i + 1) begin : g
        stage u (.clk(clk), .rst_n(rs_n), .d(c[i]), .q(c[i+1]));
    end endgenerate
    assign q = c[{stages}];
endmodule
"""
    return v, "create_clock -name clk -period 4 [get_ports clk]\n", "pipe_top"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="kind", required=True)
    s = sub.add_parser("soc")
    s.add_argument("--domains", type=int, default=4)
    s.add_argument("--blocks", type=int, default=256)
    s.add_argument("--width", type=int, default=8)
    p = sub.add_parser("pipeline")
    p.add_argument("--stages", type=int, default=10000)
    p.add_argument("--width", type=int, default=8)
    for x in (s, p):
        x.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.kind == "soc":
        v, sdc, top = soc(a.domains, a.blocks, a.width)
    else:
        v, sdc, top = pipeline(a.stages, a.width)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "design.v"), "w") as f:
        f.write(v)
    with open(os.path.join(a.out, "clocks.sdc"), "w") as f:
        f.write(sdc)
    with open(os.path.join(a.out, "bench.ocdc.json"), "w") as f:
        json.dump({"format": "opencdc-project", "version": 1, "name": os.path.basename(a.out.rstrip("/")),
                   "top": top, "sources": ["design.v"], "constraints": ["clocks.sdc"]}, f, indent=2)


if __name__ == "__main__":
    main()

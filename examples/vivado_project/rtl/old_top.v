// disabled in the project: not part of the design
module old_top(input clk, output reg q);
    always @(posedge clk) q <= ~q;
    this is not Verilog
endmodule

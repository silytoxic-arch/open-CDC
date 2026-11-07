// A small Vivado project: two clocks, a 2-flop synchronizer for a flag and a
// counter that crosses without one.
`include "demo_defs.vh"
module demo_top (
    input  wire                   clk_a,
    input  wire                   clk_b,
    input  wire                   rst_n,
    input  wire                   start,
    output wire                   busy_b,
    output wire [`DEMO_WIDTH-1:0] count_b
);
    reg start_a;
    reg [`DEMO_WIDTH-1:0] count_a;
    always @(posedge clk_a) begin
        start_a <= start;
        if (!rst_n)
            count_a <= 0;
        else if (start_a)
            count_a <= count_a + 1'b1;
    end
    // flag: synchronized (VHDL library entity)
    demo_lib_sync u_sync (.clk(clk_b), .d(start_a), .q(busy_b));
    // counter: read in clk_b without a synchronizer
    reg [`DEMO_WIDTH-1:0] count_q;
    always @(posedge clk_b)
        count_q <= count_a;
    assign count_b = count_q;
endmodule

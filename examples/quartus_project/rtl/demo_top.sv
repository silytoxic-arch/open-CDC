// A small Quartus project: a PLL with two outputs and a second oscillator;
// a flag crosses through a 2-flop synchronizer, a counter without one.
module demo_top (
    input  wire                  clk_50,
    input  wire                  clk_ext,
    input  wire                  start,
    output wire                  busy_ext,
    output wire [`DEMO_WIDTH-1:0] count_b
);
    wire clk_sys, clk_slow;
    demo_pll pll (.refclk(clk_50), .rst(1'b0), .outclk_0(clk_sys), .outclk_1(clk_slow), .locked());

    logic start_q;
    logic [`DEMO_WIDTH-1:0] count;
    always_ff @(posedge clk_sys) begin
        start_q <= start;
        if (start_q)
            count <= count + 1'b1;
    end
    // the PLL's outputs are related: no crossing
    logic [`DEMO_WIDTH-1:0] count_slow;
    always_ff @(posedge clk_slow) count_slow <= count;
    // flag: synchronized (VHDL entity of the library demo_lib)
    demo_sync u_sync (.clk(clk_ext), .d(start_q), .q(busy_ext));
    // counter: read with the other oscillator without a synchronizer
    logic [`DEMO_WIDTH-1:0] count_ext;
    always_ff @(posedge clk_ext) count_ext <= count_slow;
    assign count_b = count_ext;
endmodule

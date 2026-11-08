// A small Quartus project: a PLL with two outputs and a second oscillator;
// a flag crosses through a 2-flop synchronizer, a counter without one, and a
// configuration that the constraints declare quasi-static (set_false_path).
module demo_top (
    input  wire                  clk_50,
    input  wire                  clk_ext,
    input  wire                  start,
    input  wire [1:0]            mode,
    output wire                  busy_ext,
    output wire [1:0]            mode_b,
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
    // configuration: set while the counter is stopped, read with the other
    // oscillator; demo.sdc declares it quasi-static (set_false_path, as MiSTer's
    // framework does): the crossing is reported with that timing exception
    logic [1:0] cfg_mode;
    always_ff @(posedge clk_sys) if (!start_q) cfg_mode <= mode;
    logic [1:0] mode_ext;
    always_ff @(posedge clk_ext) mode_ext <= cfg_mode;
    assign mode_b = mode_ext;
endmodule

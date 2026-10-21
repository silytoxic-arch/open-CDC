`include "defs.vh"

// Smoke-test top level: two independent clock inputs.
module top (
    input  wire                 clk_a,
    input  wire                 rst_a_n,
    input  wire                 clk_b,
    input  wire                 rst_b_n,
    output wire [`DATA_W-1:0]   data_out,
    output wire [`STATUS_W-1:0] status_out
);
    wire [`DATA_W-1:0]   data_a;
    wire                 valid_a;
    wire [`STATUS_W-1:0] status_a;

    tx_domain u_tx (
        .clk_a   (clk_a),
        .rst_a_n (rst_a_n),
        .data    (data_a),
        .valid   (valid_a),
        .status  (status_a)
    );

    rx_domain u_rx (
        .clk_b      (clk_b),
        .rst_b_n    (rst_b_n),
        .data_in    (data_a),
        .valid_in   (valid_a),
        .status_in  (status_a),
        .data_out   (data_out),
        .status_out (status_out)
    );
endmodule

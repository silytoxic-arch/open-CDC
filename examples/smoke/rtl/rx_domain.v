`include "defs.vh"

// Destination clock domain.
//  * valid  is synchronized with a 2-FF synchronizer
//  * data   is captured directly (deliberately unsynchronized multi-bit crossing)
//  * status is synchronized bit by bit through a generate loop
module rx_domain (
    input  wire                 clk_b,
    input  wire                 rst_b_n,
    input  wire [`DATA_W-1:0]   data_in,
    input  wire                 valid_in,
    input  wire [`STATUS_W-1:0] status_in,
    output reg  [`DATA_W-1:0]   data_out,
    output wire [`STATUS_W-1:0] status_out
);
    wire valid_sync;

    sync_2ff u_valid_sync (
        .clk   (clk_b),
        .rst_n (rst_b_n),
        .d     (valid_in),
        .q     (valid_sync)
    );

    always @(posedge clk_b or negedge rst_b_n) begin
        if (!rst_b_n)
            data_out <= {`DATA_W{1'b0}};
        else if (valid_sync)
            data_out <= data_in;
    end

    genvar i;
    generate
        for (i = 0; i < `STATUS_W; i = i + 1) begin : g_status
            sync_2ff u_sync (
                .clk   (clk_b),
                .rst_n (rst_b_n),
                .d     (status_in[i]),
                .q     (status_out[i])
            );
        end
    endgenerate
endmodule

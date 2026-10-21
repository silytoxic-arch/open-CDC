// Toggle (pulse) synchronizer: one source pulse -> one destination pulse.
module pulse_sync (
    input  wire src_clk,
    input  wire src_rst_n,
    input  wire src_pulse,
    input  wire dst_clk,
    input  wire dst_rst_n,
    output reg  dst_pulse
);
    reg toggle;
    always @(posedge src_clk or negedge src_rst_n)
        if (!src_rst_n) toggle <= 1'b0;
        else            toggle <= toggle ^ src_pulse;

    wire t_sync;
    sync_2ff u_sync (.clk(dst_clk), .rst_n(dst_rst_n), .d(toggle), .q(t_sync));

    reg t_last;
    always @(posedge dst_clk or negedge dst_rst_n)
        if (!dst_rst_n) begin
            t_last    <= 1'b0;
            dst_pulse <= 1'b0;
        end else begin
            t_last    <= t_sync;
            dst_pulse <= t_sync ^ t_last;
        end
endmodule

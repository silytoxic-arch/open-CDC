// Two-flop level synchronizer with asynchronous reset.
module sync_2ff (
    input  wire clk,
    input  wire rst_n,
    input  wire d,
    output wire q
);
    (* ASYNC_REG = "TRUE" *) reg meta, sync;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            meta <= 1'b0;
            sync <= 1'b0;
        end else begin
            meta <= d;
            sync <= meta;
        end
    end
    assign q = sync;
endmodule

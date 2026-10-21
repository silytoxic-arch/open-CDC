// Two flip-flop single-bit synchronizer.
module sync_2ff #(
    parameter RESET_VAL = 1'b0
) (
    input  wire clk,
    input  wire rst_n,
    input  wire d,
    output wire q
);
    (* ASYNC_REG = "TRUE" *) reg meta;
    (* ASYNC_REG = "TRUE" *) reg sync;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            meta <= RESET_VAL;
            sync <= RESET_VAL;
        end else begin
            meta <= d;
            sync <= meta;
        end
    end

    assign q = sync;
endmodule

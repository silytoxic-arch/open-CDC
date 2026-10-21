// Reset synchronizer: asynchronous assertion, synchronous release.
module rst_sync (
    input  wire clk,
    input  wire rst_in_n,
    output wire rst_out_n
);
    (* ASYNC_REG = "TRUE" *) reg [1:0] r;
    always @(posedge clk or negedge rst_in_n) begin
        if (!rst_in_n)
            r <= 2'b00;
        else
            r <= {r[0], 1'b1};
    end
    assign rst_out_n = r[1];
endmodule

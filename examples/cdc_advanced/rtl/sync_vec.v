// Vector of two-flop synchronizers (one chain per bit).
module sync_vec #(parameter W = 4) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [W-1:0] d,
    output wire [W-1:0] q
);
    (* ASYNC_REG = "TRUE" *) reg [W-1:0] s1, s2;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1 <= {W{1'b0}};
            s2 <= {W{1'b0}};
        end else begin
            s1 <= d;
            s2 <= s1;
        end
    end
    assign q = s2;
endmodule

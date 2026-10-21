`include "defs.vh"

// Source clock domain: produces a data word, a one-cycle valid pulse and a
// status bus.
module tx_domain (
    input  wire                 clk_a,
    input  wire                 rst_a_n,
    output reg  [`DATA_W-1:0]   data,
    output reg                  valid,
    output reg  [`STATUS_W-1:0] status
);
    reg [3:0] cnt;

    always @(posedge clk_a or negedge rst_a_n) begin
        if (!rst_a_n) begin
            cnt    <= 4'd0;
            data   <= {`DATA_W{1'b0}};
            valid  <= 1'b0;
            status <= {`STATUS_W{1'b0}};
        end else begin
            cnt   <= cnt + 4'd1;
            valid <= (cnt == 4'hF);
            if (cnt == 4'hF)
                data <= data + 1'b1;
            case (cnt[1:0])
                2'd0:    status <= 4'b0001;
                2'd1:    status <= 4'b0010;
                2'd2:    status <= 4'b0100;
                default: status <= 4'b1000;
            endcase
        end
    end
endmodule

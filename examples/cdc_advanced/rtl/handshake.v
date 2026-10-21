// Four-phase request/acknowledge transfer of a data word.
module handshake #(parameter DW = 8) (
    input  wire          src_clk,
    input  wire          src_rst_n,
    input  wire          send,
    input  wire [DW-1:0] din,
    output wire          busy,
    input  wire          dst_clk,
    input  wire          dst_rst_n,
    output reg  [DW-1:0] dout,
    output reg           dvalid
);
    reg          req;
    reg [DW-1:0] data;
    wire         ack_s;
    always @(posedge src_clk or negedge src_rst_n)
        if (!src_rst_n) begin
            req  <= 1'b0;
            data <= {DW{1'b0}};
        end else if (!req && !ack_s && send) begin
            req  <= 1'b1;
            data <= din;
        end else if (req && ack_s) begin
            req <= 1'b0;
        end
    assign busy = req | ack_s;

    wire req_s;
    reg  ack;
    sync_2ff u_req (.clk(dst_clk), .rst_n(dst_rst_n), .d(req), .q(req_s));
    always @(posedge dst_clk or negedge dst_rst_n)
        if (!dst_rst_n) begin
            ack    <= 1'b0;
            dout   <= {DW{1'b0}};
            dvalid <= 1'b0;
        end else begin
            dvalid <= 1'b0;
            if (req_s && !ack) begin
                dout   <= data;
                ack    <= 1'b1;
                dvalid <= 1'b1;
            end else if (!req_s) begin
                ack <= 1'b0;
            end
        end
    sync_2ff u_ack (.clk(src_clk), .rst_n(src_rst_n), .d(ack), .q(ack_s));
endmodule

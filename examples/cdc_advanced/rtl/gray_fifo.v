// Asynchronous FIFO: Gray-coded pointers synchronized in both directions,
// registered read data qualified by the synchronized write pointer.
module gray_fifo #(parameter DW = 8, parameter AW = 3) (
    input  wire          wclk,
    input  wire          wrst_n,
    input  wire          winc,
    input  wire [DW-1:0] wdata,
    output wire          wfull,
    input  wire          rclk,
    input  wire          rrst_n,
    input  wire          rinc,
    output reg  [DW-1:0] rdata,
    output wire          rempty
);
    reg [DW-1:0] mem [0:(1<<AW)-1];

    // write side
    reg  [AW:0] wbin, wptr;
    wire [AW:0] rptr_w;                         // read pointer, synchronized to wclk
    wire [AW:0] wbin_next  = wbin + {{AW{1'b0}}, winc & ~wfull};
    wire [AW:0] wgray_next = (wbin_next >> 1) ^ wbin_next;
    assign wfull = wptr == {~rptr_w[AW:AW-1], rptr_w[AW-2:0]};
    always @(posedge wclk or negedge wrst_n)
        if (!wrst_n) begin
            wbin <= {(AW+1){1'b0}};
            wptr <= {(AW+1){1'b0}};
        end else begin
            wbin <= wbin_next;
            wptr <= wgray_next;
        end
    always @(posedge wclk)
        if (winc && !wfull)
            mem[wbin[AW-1:0]] <= wdata;

    // read side
    reg  [AW:0] rbin, rptr;
    wire [AW:0] wptr_r;                         // write pointer, synchronized to rclk
    assign rempty = rptr == wptr_r;
    wire [AW:0] rbin_next  = rbin + {{AW{1'b0}}, rinc & ~rempty};
    wire [AW:0] rgray_next = (rbin_next >> 1) ^ rbin_next;
    always @(posedge rclk or negedge rrst_n)
        if (!rrst_n) begin
            rbin  <= {(AW+1){1'b0}};
            rptr  <= {(AW+1){1'b0}};
            rdata <= {DW{1'b0}};
        end else begin
            rbin <= rbin_next;
            rptr <= rgray_next;
            if (rinc && !rempty)
                rdata <= mem[rbin[AW-1:0]];
        end

    sync_vec #(.W(AW+1)) u_w2r (.clk(rclk), .rst_n(rrst_n), .d(wptr), .q(wptr_r));
    sync_vec #(.W(AW+1)) u_r2w (.clk(wclk), .rst_n(wrst_n), .d(rptr), .q(rptr_w));
endmodule

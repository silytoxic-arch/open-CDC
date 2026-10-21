// Write pointer (Gray) synchronized into the read clock domain.
module sync_w2r #(parameter ASIZE = 4) (
    output reg [ASIZE:0] rq2_wptr,
    input      [ASIZE:0] wptr,
    input                rclk,
    input                rrst_n
);
    reg [ASIZE:0] rq1_wptr;
    always @(posedge rclk or negedge rrst_n)
        if (!rrst_n) {rq2_wptr, rq1_wptr} <= 0;
        else         {rq2_wptr, rq1_wptr} <= {rq1_wptr, wptr};
endmodule

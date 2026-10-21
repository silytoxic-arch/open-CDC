// Read pointer (Gray) synchronized into the write clock domain.
module sync_r2w #(parameter ASIZE = 4) (
    output reg [ASIZE:0] wq2_rptr,
    input      [ASIZE:0] rptr,
    input                wclk,
    input                wrst_n
);
    reg [ASIZE:0] wq1_rptr;
    always @(posedge wclk or negedge wrst_n)
        if (!wrst_n) {wq2_rptr, wq1_rptr} <= 0;
        else         {wq2_rptr, wq1_rptr} <= {wq1_rptr, rptr};
endmodule

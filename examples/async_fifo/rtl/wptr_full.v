// Write pointer and full flag (write clock domain).
module wptr_full #(parameter ASIZE = 4) (
    output reg             wfull,
    output     [ASIZE-1:0] waddr,
    output reg [ASIZE:0]   wptr,
    input      [ASIZE:0]   wq2_rptr,
    input                  winc,
    input                  wclk,
    input                  wrst_n
);
    reg  [ASIZE:0] wbin;
    wire [ASIZE:0] wgraynext, wbinnext;
    wire           wfull_val;

    always @(posedge wclk or negedge wrst_n)
        if (!wrst_n) {wbin, wptr} <= 0;
        else         {wbin, wptr} <= {wbinnext, wgraynext};

    assign waddr     = wbin[ASIZE-1:0];
    assign wbinnext  = wbin + (winc & ~wfull);
    assign wgraynext = (wbinnext >> 1) ^ wbinnext;

    assign wfull_val = (wgraynext == {~wq2_rptr[ASIZE:ASIZE-1], wq2_rptr[ASIZE-2:0]});
    always @(posedge wclk or negedge wrst_n)
        if (!wrst_n) wfull <= 1'b0;
        else         wfull <= wfull_val;
endmodule

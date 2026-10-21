// Read pointer and empty flag (read clock domain).
module rptr_empty #(parameter ASIZE = 4) (
    output reg             rempty,
    output     [ASIZE-1:0] raddr,
    output reg [ASIZE:0]   rptr,
    input      [ASIZE:0]   rq2_wptr,
    input                  rinc,
    input                  rclk,
    input                  rrst_n
);
    reg  [ASIZE:0] rbin;
    wire [ASIZE:0] rgraynext, rbinnext;
    wire           rempty_val;

    always @(posedge rclk or negedge rrst_n)
        if (!rrst_n) {rbin, rptr} <= 0;
        else         {rbin, rptr} <= {rbinnext, rgraynext};

    assign raddr     = rbin[ASIZE-1:0];
    assign rbinnext  = rbin + (rinc & ~rempty);
    assign rgraynext = (rbinnext >> 1) ^ rbinnext;

    assign rempty_val = (rgraynext == rq2_wptr);
    always @(posedge rclk or negedge rrst_n)
        if (!rrst_n) rempty <= 1'b1;
        else         rempty <= rempty_val;
endmodule

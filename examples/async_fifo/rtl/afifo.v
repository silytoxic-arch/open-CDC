// Asynchronous FIFO (Gray-coded pointers), after C. E. Cummings,
// "Simulation and Synthesis Techniques for Asynchronous FIFO Design", SNUG 2002.
module afifo #(
    parameter DSIZE = 8,
    parameter ASIZE = 4
) (
    output [DSIZE-1:0] rdata,
    output             wfull,
    output             rempty,
    input  [DSIZE-1:0] wdata,
    input              winc, wclk, wrst_n,
    input              rinc, rclk, rrst_n
);
    wire [ASIZE-1:0] waddr, raddr;
    wire [ASIZE:0]   wptr, rptr, wq2_rptr, rq2_wptr;

    sync_r2w #(ASIZE) u_sync_r2w (.wq2_rptr(wq2_rptr), .rptr(rptr), .wclk(wclk), .wrst_n(wrst_n));
    sync_w2r #(ASIZE) u_sync_w2r (.rq2_wptr(rq2_wptr), .wptr(wptr), .rclk(rclk), .rrst_n(rrst_n));

    fifomem #(DSIZE, ASIZE) u_mem (
        .rdata(rdata), .wdata(wdata), .waddr(waddr), .raddr(raddr),
        .wclken(winc), .wfull(wfull), .wclk(wclk));

    rptr_empty #(ASIZE) u_rptr_empty (
        .rempty(rempty), .raddr(raddr), .rptr(rptr), .rq2_wptr(rq2_wptr),
        .rinc(rinc), .rclk(rclk), .rrst_n(rrst_n));

    wptr_full #(ASIZE) u_wptr_full (
        .wfull(wfull), .waddr(waddr), .wptr(wptr), .wq2_rptr(wq2_rptr),
        .winc(winc), .wclk(wclk), .wrst_n(wrst_n));
endmodule

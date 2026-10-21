// Dual-port memory: written in the write clock domain, read asynchronously.
module fifomem #(
    parameter DSIZE = 8,
    parameter ASIZE = 4
) (
    output [DSIZE-1:0] rdata,
    input  [DSIZE-1:0] wdata,
    input  [ASIZE-1:0] waddr,
    input  [ASIZE-1:0] raddr,
    input              wclken,
    input              wfull,
    input              wclk
);
    localparam DEPTH = 1 << ASIZE;
    reg [DSIZE-1:0] mem [0:DEPTH-1];

    assign rdata = mem[raddr];

    always @(posedge wclk)
        if (wclken && !wfull)
            mem[waddr] <= wdata;
endmodule

// OpenCDC behavioral models of Xilinx UNIMACRO cells (7-series), used when a
// design instantiates them without a definition.

// P = A * B after LATENCY register stages (0: combinational)
module MULT_MACRO #(parameter DEVICE = "7SERIES", parameter integer LATENCY = 3,
                    parameter integer WIDTH_A = 18, parameter integer WIDTH_B = 18)
                   (output [WIDTH_A+WIDTH_B-1:0] P, input [WIDTH_A-1:0] A, input [WIDTH_B-1:0] B,
                    input CE, input CLK, input RST);
    wire [WIDTH_A+WIDTH_B-1:0] product = $signed(A) * $signed(B);
    generate
        if (LATENCY == 0) begin : g_comb
            assign P = product;
        end else begin : g_pipe
            reg [WIDTH_A+WIDTH_B-1:0] pipe [0:LATENCY-1];
            integer k;
            always @(posedge CLK)
                if (RST) begin
                    for (k = 0; k < LATENCY; k = k + 1) pipe[k] <= {(WIDTH_A+WIDTH_B){1'b0}};
                end else if (CE) begin
                    pipe[0] <= product;
                    for (k = 1; k < LATENCY; k = k + 1) pipe[k] <= pipe[k-1];
                end
            assign P = pipe[LATENCY-1];
        end
    endgenerate
endmodule

// RESULT = A +/- B + CARRYIN after LATENCY register stages
module ADDSUB_MACRO #(parameter DEVICE = "7SERIES", parameter integer LATENCY = 2, parameter integer WIDTH = 48)
                     (output CARRYOUT, output [WIDTH-1:0] RESULT, input [WIDTH-1:0] A, input ADD_SUB,
                      input [WIDTH-1:0] B, input CARRYIN, input CE, input CLK, input RST);
    wire [WIDTH:0] sum = ADD_SUB ? {1'b0, A} + {1'b0, B} + CARRYIN : {1'b0, A} - {1'b0, B} - CARRYIN;
    generate
        if (LATENCY == 0) begin : g_comb
            assign {CARRYOUT, RESULT} = sum;
        end else begin : g_pipe
            reg [WIDTH:0] pipe [0:LATENCY-1];
            integer k;
            always @(posedge CLK)
                if (RST) begin
                    for (k = 0; k < LATENCY; k = k + 1) pipe[k] <= {(WIDTH+1){1'b0}};
                end else if (CE) begin
                    pipe[0] <= sum;
                    for (k = 1; k < LATENCY; k = k + 1) pipe[k] <= pipe[k-1];
                end
            assign {CARRYOUT, RESULT} = pipe[LATENCY-1];
        end
    endgenerate
endmodule

// P accumulates A * B (LOAD: P = LOAD_DATA)
module MACC_MACRO #(parameter DEVICE = "7SERIES", parameter integer LATENCY = 3, parameter integer WIDTH_A = 25,
                    parameter integer WIDTH_B = 18, parameter integer WIDTH_P = 48)
                   (output reg [WIDTH_P-1:0] P, input [WIDTH_A-1:0] A, input ADDSUB, input [WIDTH_B-1:0] B,
                    input CARRYIN, input CE, input CLK, input LOAD, input [WIDTH_P-1:0] LOAD_DATA, input RST);
    reg [WIDTH_A+WIDTH_B-1:0] m;
    always @(posedge CLK)
        if (RST) begin
            m <= {(WIDTH_A+WIDTH_B){1'b0}};
            P <= {WIDTH_P{1'b0}};
        end else if (CE) begin
            m <= $signed(A) * $signed(B);
            P <= LOAD ? LOAD_DATA : (ADDSUB ? P + m + CARRYIN : P - m - CARRYIN);
        end
endmodule

// Dual-clock FIFO: Gray pointers synchronized into the other clock domain
// (the counter widths follow the 7-series table of FIFO_SIZE and DATA_WIDTH)
module FIFO_DUALCLOCK_MACRO #(parameter [12:0] ALMOST_EMPTY_OFFSET = 13'h0080,
                              parameter [12:0] ALMOST_FULL_OFFSET = 13'h0080, parameter integer DATA_WIDTH = 4,
                              parameter DEVICE = "7SERIES", parameter FIFO_SIZE = "18Kb",
                              parameter FIRST_WORD_FALL_THROUGH = "FALSE", parameter INIT = 72'h0,
                              parameter SIM_MODE = "SAFE", parameter SRVAL = 72'h0,
                              // counter width (derived; not a Xilinx parameter)
                              parameter integer CW = FIFO_SIZE == "36Kb"
                                  ? (DATA_WIDTH <= 4 ? 13 : DATA_WIDTH <= 9 ? 12 : DATA_WIDTH <= 18 ? 11 :
                                     DATA_WIDTH <= 36 ? 10 : 9)
                                  : (DATA_WIDTH <= 4 ? 12 : DATA_WIDTH <= 9 ? 11 : DATA_WIDTH <= 18 ? 10 : 9))
                             (output ALMOSTEMPTY, output ALMOSTFULL, output [DATA_WIDTH-1:0] DO, output EMPTY,
                              output FULL, output [CW-1:0] RDCOUNT, output RDERR, output [CW-1:0] WRCOUNT,
                              output WRERR, input [DATA_WIDTH-1:0] DI, input RDCLK, input RDEN, input RST,
                              input WRCLK, input WREN);
    localparam integer AW = CW;
    reg [DATA_WIDTH-1:0] mem [0:(1 << AW)-1];
    reg  [AW:0] wbin, wptr, rbin, rptr;
    reg  [AW:0] w2r1, w2r2, r2w1, r2w2;
    reg  [DATA_WIDTH-1:0] do_ff;
    wire full_i = wptr == {~r2w2[AW:AW-1], r2w2[AW-2:0]};
    wire empty_i = rptr == w2r2;
    wire [AW:0] wbin_next = wbin + {{AW{1'b0}}, WREN & ~full_i};
    wire [AW:0] rbin_next = rbin + {{AW{1'b0}}, RDEN & ~empty_i};
    always @(posedge WRCLK or posedge RST)
        if (RST) begin
            wbin <= {(AW+1){1'b0}};
            wptr <= {(AW+1){1'b0}};
        end else begin
            wbin <= wbin_next;
            wptr <= (wbin_next >> 1) ^ wbin_next;
        end
    always @(posedge WRCLK)
        if (WREN && !full_i) mem[wbin[AW-1:0]] <= DI;
    always @(posedge RDCLK or posedge RST)
        if (RST) begin
            rbin <= {(AW+1){1'b0}};
            rptr <= {(AW+1){1'b0}};
        end else begin
            rbin <= rbin_next;
            rptr <= (rbin_next >> 1) ^ rbin_next;
            if (RDEN && !empty_i) do_ff <= mem[rbin[AW-1:0]];
        end
    always @(posedge RDCLK) begin
        w2r1 <= wptr;
        w2r2 <= w2r1;
    end
    always @(posedge WRCLK) begin
        r2w1 <= rptr;
        r2w2 <= r2w1;
    end
    assign DO = do_ff;
    assign EMPTY = empty_i;
    assign FULL = full_i;
    assign ALMOSTEMPTY = empty_i;
    assign ALMOSTFULL = full_i;
    assign RDCOUNT = rbin[CW-1:0];
    assign WRCOUNT = wbin[CW-1:0];
    assign RDERR = 1'b0;
    assign WRERR = 1'b0;
endmodule

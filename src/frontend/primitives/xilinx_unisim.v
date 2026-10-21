// OpenCDC behavioral models of Xilinx UNISIM primitives (register-transfer
// behavior only: what matters for clock-domain-crossing analysis). Each
// module is added to a compilation only when a design instantiates it.

// ---------------------------------------------------------------- flip-flops
module FDRE #(parameter [0:0] INIT = 1'b0, parameter [0:0] IS_C_INVERTED = 1'b0,
              parameter [0:0] IS_D_INVERTED = 1'b0, parameter [0:0] IS_R_INVERTED = 1'b0)
             (output reg Q, input C, input CE, input D, input R);
    wire c = C ^ IS_C_INVERTED;
    initial Q = INIT;
    always @(posedge c)
        if (R ^ IS_R_INVERTED) Q <= 1'b0;
        else if (CE) Q <= D ^ IS_D_INVERTED;
endmodule

module FDSE #(parameter [0:0] INIT = 1'b1, parameter [0:0] IS_C_INVERTED = 1'b0,
              parameter [0:0] IS_D_INVERTED = 1'b0, parameter [0:0] IS_S_INVERTED = 1'b0)
             (output reg Q, input C, input CE, input D, input S);
    wire c = C ^ IS_C_INVERTED;
    initial Q = INIT;
    always @(posedge c)
        if (S ^ IS_S_INVERTED) Q <= 1'b1;
        else if (CE) Q <= D ^ IS_D_INVERTED;
endmodule

module FDCE #(parameter [0:0] INIT = 1'b0, parameter [0:0] IS_C_INVERTED = 1'b0,
              parameter [0:0] IS_D_INVERTED = 1'b0, parameter [0:0] IS_CLR_INVERTED = 1'b0)
             (output reg Q, input C, input CE, input CLR, input D);
    wire c = C ^ IS_C_INVERTED;
    wire clr = CLR ^ IS_CLR_INVERTED;
    initial Q = INIT;
    always @(posedge c or posedge clr)
        if (clr) Q <= 1'b0;
        else if (CE) Q <= D ^ IS_D_INVERTED;
endmodule

module FDPE #(parameter [0:0] INIT = 1'b1, parameter [0:0] IS_C_INVERTED = 1'b0,
              parameter [0:0] IS_D_INVERTED = 1'b0, parameter [0:0] IS_PRE_INVERTED = 1'b0)
             (output reg Q, input C, input CE, input D, input PRE);
    wire c = C ^ IS_C_INVERTED;
    wire pre = PRE ^ IS_PRE_INVERTED;
    initial Q = INIT;
    always @(posedge c or posedge pre)
        if (pre) Q <= 1'b1;
        else if (CE) Q <= D ^ IS_D_INVERTED;
endmodule

module FD #(parameter [0:0] INIT = 1'b0) (output reg Q, input C, input D);
    initial Q = INIT;
    always @(posedge C) Q <= D;
endmodule

module FDE #(parameter [0:0] INIT = 1'b0) (output reg Q, input C, input CE, input D);
    initial Q = INIT;
    always @(posedge C) if (CE) Q <= D;
endmodule

module FDR #(parameter [0:0] INIT = 1'b0) (output reg Q, input C, input D, input R);
    initial Q = INIT;
    always @(posedge C) if (R) Q <= 1'b0; else Q <= D;
endmodule

module FDS #(parameter [0:0] INIT = 1'b1) (output reg Q, input C, input D, input S);
    initial Q = INIT;
    always @(posedge C) if (S) Q <= 1'b1; else Q <= D;
endmodule

module FDC #(parameter [0:0] INIT = 1'b0) (output reg Q, input C, input CLR, input D);
    initial Q = INIT;
    always @(posedge C or posedge CLR) if (CLR) Q <= 1'b0; else Q <= D;
endmodule

module FDP #(parameter [0:0] INIT = 1'b1) (output reg Q, input C, input D, input PRE);
    initial Q = INIT;
    always @(posedge C or posedge PRE) if (PRE) Q <= 1'b1; else Q <= D;
endmodule

module FDCPE #(parameter [0:0] INIT = 1'b0) (output reg Q, input C, input CE, input CLR, input D, input PRE);
    initial Q = INIT;
    always @(posedge C or posedge CLR or posedge PRE)
        if (CLR) Q <= 1'b0;
        else if (PRE) Q <= 1'b1;
        else if (CE) Q <= D;
endmodule

// -------------------------------------------------------------------- latches
module LDCE #(parameter [0:0] INIT = 1'b0, parameter [0:0] IS_CLR_INVERTED = 1'b0,
              parameter [0:0] IS_G_INVERTED = 1'b0)
             (output reg Q, input CLR, input D, input G, input GE);
    always @*
        if (CLR ^ IS_CLR_INVERTED) Q = 1'b0;
        else if ((G ^ IS_G_INVERTED) && GE) Q = D;
endmodule

module LDPE #(parameter [0:0] INIT = 1'b1, parameter [0:0] IS_PRE_INVERTED = 1'b0,
              parameter [0:0] IS_G_INVERTED = 1'b0)
             (output reg Q, input D, input G, input GE, input PRE);
    always @*
        if (PRE ^ IS_PRE_INVERTED) Q = 1'b1;
        else if ((G ^ IS_G_INVERTED) && GE) Q = D;
endmodule

// ------------------------------------------------------- clock and I/O buffers
module BUFG (output O, input I);
    assign O = I;
endmodule

module BUFGCE #(parameter CE_TYPE = "SYNC", parameter [0:0] IS_CE_INVERTED = 1'b0,
                parameter [0:0] IS_I_INVERTED = 1'b0, parameter SIM_DEVICE = "ULTRASCALE")
               (output O, input CE, input I);
    assign O = (I ^ IS_I_INVERTED) & (CE ^ IS_CE_INVERTED);
endmodule

module BUFGCE_1 (output O, input CE, input I);
    assign O = I & CE;
endmodule

module BUFGMUX #(parameter CLK_SEL_TYPE = "SYNC") (output O, input I0, input I1, input S);
    assign O = S ? I1 : I0;
endmodule

module BUFGMUX_CTRL (output O, input I0, input I1, input S);
    assign O = S ? I1 : I0;
endmodule

module BUFGCTRL #(parameter INIT_OUT = 0, parameter PRESELECT_I0 = "FALSE", parameter PRESELECT_I1 = "FALSE")
                 (output O, input CE0, input CE1, input I0, input I1, input IGNORE0, input IGNORE1,
                  input S0, input S1);
    assign O = S1 ? (I1 & CE1) : (I0 & CE0);
endmodule

module BUFH (output O, input I);
    assign O = I;
endmodule

module BUFHCE #(parameter [0:0] INIT_OUT = 1'b0, parameter CE_TYPE = "SYNC") (output O, input CE, input I);
    assign O = I & CE;
endmodule

// BUFR divides by BUFR_DIVIDE ("BYPASS", "1" .. "8"): a derived clock whose
// period the clock analysis computes from the attribute.
module BUFR #(parameter BUFR_DIVIDE = "BYPASS", parameter SIM_DEVICE = "7SERIES")
             (output O, input CE, input CLR, input I);
    localparam integer DIV = BUFR_DIVIDE == "2" ? 2 : BUFR_DIVIDE == "3" ? 3 : BUFR_DIVIDE == "4" ? 4 :
                             BUFR_DIVIDE == "5" ? 5 : BUFR_DIVIDE == "6" ? 6 : BUFR_DIVIDE == "7" ? 7 :
                             BUFR_DIVIDE == "8" ? 8 : 1;
    generate
        if (DIV == 1) begin : g_bypass
            assign O = I;
        end else begin : g_div
            (* OPENCDC_CLOCK_PIN = "O", OPENCDC_CLOCK_RATIO = 1.0 * DIV *)
            reg O_q = 1'b0;
            always @(posedge I) O_q <= ~O_q;
            assign O = O_q;
        end
    endgenerate
endmodule

module BUFGCE_DIV #(parameter integer BUFGCE_DIVIDE = 1, parameter CE_TYPE = "SYNC",
                    parameter HARDSYNC_CLR = "FALSE", parameter [0:0] IS_CE_INVERTED = 1'b0,
                    parameter [0:0] IS_CLR_INVERTED = 1'b0, parameter [0:0] IS_I_INVERTED = 1'b0,
                    parameter SIM_DEVICE = "ULTRASCALE")
                   (output O, input CE, input CLR, input I);
    generate
        if (BUFGCE_DIVIDE <= 1) begin : g_bypass
            assign O = I ^ IS_I_INVERTED;
        end else begin : g_div
            (* OPENCDC_CLOCK_PIN = "O", OPENCDC_CLOCK_RATIO = 1.0 * BUFGCE_DIVIDE *)
            reg O_q = 1'b0;
            always @(posedge I) O_q <= ~O_q;
            assign O = O_q;
        end
    endgenerate
endmodule

module BUFIO (output O, input I);
    assign O = I;
endmodule

module BUFG_GT #(parameter SIM_DEVICE = "ULTRASCALE")
                (output O, input CE, input CEMASK, input CLR, input CLRMASK, input [2:0] DIV, input I);
    assign O = I;
endmodule

module IBUF #(parameter IBUF_LOW_PWR = "TRUE", parameter IOSTANDARD = "DEFAULT") (output O, input I);
    assign O = I;
endmodule

module IBUFG #(parameter IBUF_LOW_PWR = "TRUE", parameter IOSTANDARD = "DEFAULT") (output O, input I);
    assign O = I;
endmodule

module IBUFDS #(parameter DIFF_TERM = "FALSE", parameter IBUF_LOW_PWR = "TRUE", parameter IOSTANDARD = "DEFAULT")
               (output O, input I, input IB);
    assign O = I;
endmodule

module IBUFGDS #(parameter DIFF_TERM = "FALSE", parameter IBUF_LOW_PWR = "TRUE", parameter IOSTANDARD = "DEFAULT")
                (output O, input I, input IB);
    assign O = I;
endmodule

module IBUFDS_GTE2 (output O, output ODIV2, input CEB, input I, input IB);
    assign O = I;
    assign ODIV2 = I;
endmodule

module OBUF #(parameter DRIVE = 12, parameter IOSTANDARD = "DEFAULT", parameter SLEW = "SLOW") (output O, input I);
    assign O = I;
endmodule

module OBUFT #(parameter DRIVE = 12, parameter IOSTANDARD = "DEFAULT", parameter SLEW = "SLOW")
              (output O, input I, input T);
    assign O = I;
endmodule

module OBUFDS #(parameter IOSTANDARD = "DEFAULT", parameter SLEW = "SLOW") (output O, output OB, input I);
    assign O = I;
    assign OB = ~I;
endmodule

module IOBUF #(parameter DRIVE = 12, parameter IOSTANDARD = "DEFAULT", parameter SLEW = "SLOW")
              (output O, inout IO, input I, input T);
    assign O = IO;
endmodule

// --------------------------------------------------------- shift-register LUTs
module SRL16E #(parameter [15:0] INIT = 16'h0000, parameter [0:0] IS_CLK_INVERTED = 1'b0)
               (output Q, input A0, input A1, input A2, input A3, input CE, input CLK, input D);
    reg [15:0] sr = INIT;
    wire clk = CLK ^ IS_CLK_INVERTED;
    always @(posedge clk) if (CE) sr <= {sr[14:0], D};
    assign Q = sr[{A3, A2, A1, A0}];
endmodule

module SRLC32E #(parameter [31:0] INIT = 32'h00000000, parameter [0:0] IS_CLK_INVERTED = 1'b0)
                (output Q, output Q31, input [4:0] A, input CE, input CLK, input D);
    reg [31:0] sr = INIT;
    wire clk = CLK ^ IS_CLK_INVERTED;
    always @(posedge clk) if (CE) sr <= {sr[30:0], D};
    assign Q = sr[A];
    assign Q31 = sr[31];
endmodule

// ----------------------------------------------------------------------- LUTs
module LUT1 #(parameter [1:0] INIT = 2'h0) (output O, input I0);
    assign O = INIT[I0];
endmodule

module LUT2 #(parameter [3:0] INIT = 4'h0) (output O, input I0, input I1);
    assign O = INIT[{I1, I0}];
endmodule

module LUT3 #(parameter [7:0] INIT = 8'h00) (output O, input I0, input I1, input I2);
    assign O = INIT[{I2, I1, I0}];
endmodule

module LUT4 #(parameter [15:0] INIT = 16'h0000) (output O, input I0, input I1, input I2, input I3);
    assign O = INIT[{I3, I2, I1, I0}];
endmodule

module LUT5 #(parameter [31:0] INIT = 32'h00000000) (output O, input I0, input I1, input I2, input I3, input I4);
    assign O = INIT[{I4, I3, I2, I1, I0}];
endmodule

module LUT6 #(parameter [63:0] INIT = 64'h0000000000000000)
             (output O, input I0, input I1, input I2, input I3, input I4, input I5);
    assign O = INIT[{I5, I4, I3, I2, I1, I0}];
endmodule

module LUT6_2 #(parameter [63:0] INIT = 64'h0000000000000000)
               (output O5, output O6, input I0, input I1, input I2, input I3, input I4, input I5);
    assign O5 = INIT[{1'b0, I4, I3, I2, I1, I0}];
    assign O6 = INIT[{I5, I4, I3, I2, I1, I0}];
endmodule

module MUXF7 (output O, input I0, input I1, input S);
    assign O = S ? I1 : I0;
endmodule

module MUXF8 (output O, input I0, input I1, input S);
    assign O = S ? I1 : I0;
endmodule

module VCC (output P);
    assign P = 1'b1;
endmodule

module GND (output G);
    assign G = 1'b0;
endmodule

// ---------------------------------------------------------------- DDR I/O
module IDDR #(parameter DDR_CLK_EDGE = "OPPOSITE_EDGE", parameter [0:0] INIT_Q1 = 1'b0,
              parameter [0:0] INIT_Q2 = 1'b0, parameter [0:0] IS_C_INVERTED = 1'b0,
              parameter [0:0] IS_D_INVERTED = 1'b0, parameter SRTYPE = "SYNC")
             (output reg Q1, output reg Q2, input C, input CE, input D, input R, input S);
    wire c = C ^ IS_C_INVERTED;
    wire d = D ^ IS_D_INVERTED;
    reg qn; // captured on the falling edge
    always @(negedge c) if (CE) qn <= d;
    always @(posedge c)
        if (R) begin Q1 <= 1'b0; Q2 <= 1'b0; end
        else if (S) begin Q1 <= 1'b1; Q2 <= 1'b1; end
        else if (CE) begin Q1 <= d; Q2 <= qn; end
endmodule

module IDDRE1 #(parameter DDR_CLK_EDGE = "OPPOSITE_EDGE", parameter [0:0] IS_CB_INVERTED = 1'b0,
                parameter [0:0] IS_C_INVERTED = 1'b0)
               (output reg Q1, output reg Q2, input C, input CB, input D, input R);
    wire c = C ^ IS_C_INVERTED;
    reg qn;
    always @(negedge c) qn <= D;
    always @(posedge c or posedge R)
        if (R) begin Q1 <= 1'b0; Q2 <= 1'b0; end
        else begin Q1 <= D; Q2 <= qn; end
endmodule

module ODDR #(parameter DDR_CLK_EDGE = "OPPOSITE_EDGE", parameter [0:0] INIT = 1'b0,
              parameter [0:0] IS_C_INVERTED = 1'b0, parameter [0:0] IS_D1_INVERTED = 1'b0,
              parameter [0:0] IS_D2_INVERTED = 1'b0, parameter SRTYPE = "SYNC")
             (output Q, input C, input CE, input D1, input D2, input R, input S);
    wire c = C ^ IS_C_INVERTED;
    reg q1, q2;
    always @(posedge c)
        if (R) begin q1 <= 1'b0; q2 <= 1'b0; end
        else if (S) begin q1 <= 1'b1; q2 <= 1'b1; end
        else if (CE) begin q1 <= D1 ^ IS_D1_INVERTED; q2 <= D2 ^ IS_D2_INVERTED; end
    assign Q = q1 | q2; // both halves reach the pin
endmodule

module ODDRE1 #(parameter [0:0] IS_C_INVERTED = 1'b0, parameter [0:0] IS_D1_INVERTED = 1'b0,
                parameter [0:0] IS_D2_INVERTED = 1'b0, parameter SIM_DEVICE = "ULTRASCALE",
                parameter [0:0] SRVAL = 1'b0)
               (output Q, input C, input D1, input D2, input SR);
    wire c = C ^ IS_C_INVERTED;
    reg q1, q2;
    always @(posedge c or posedge SR)
        if (SR) begin q1 <= SRVAL; q2 <= SRVAL; end
        else begin q1 <= D1 ^ IS_D1_INVERTED; q2 <= D2 ^ IS_D2_INVERTED; end
    assign Q = q1 | q2;
endmodule

// ---------------------------------------------------------------- I/O delays
// The data paths are wires; the tap counters belong to the control clock.
module IDELAYE2 #(parameter CINVCTRL_SEL = "FALSE", parameter DELAY_SRC = "IDATAIN",
                  parameter HIGH_PERFORMANCE_MODE = "FALSE", parameter IDELAY_TYPE = "FIXED",
                  parameter integer IDELAY_VALUE = 0, parameter [0:0] IS_C_INVERTED = 1'b0,
                  parameter [0:0] IS_DATAIN_INVERTED = 1'b0, parameter [0:0] IS_IDATAIN_INVERTED = 1'b0,
                  parameter PIPE_SEL = "FALSE", parameter real REFCLK_FREQUENCY = 200.0,
                  parameter SIGNAL_PATTERN = "DATA")
                 (output reg [4:0] CNTVALUEOUT, output DATAOUT, input C, input CE, input CINVCTRL,
                  input [4:0] CNTVALUEIN, input DATAIN, input IDATAIN, input INC, input LD, input LDPIPEEN,
                  input REGRST);
    assign DATAOUT = DELAY_SRC == "DATAIN" ? DATAIN : IDATAIN;
    always @(posedge C)
        if (LD) CNTVALUEOUT <= CNTVALUEIN;
        else if (CE) CNTVALUEOUT <= INC ? CNTVALUEOUT + 5'd1 : CNTVALUEOUT - 5'd1;
endmodule

module ODELAYE2 #(parameter CINVCTRL_SEL = "FALSE", parameter DELAY_SRC = "ODATAIN",
                  parameter HIGH_PERFORMANCE_MODE = "FALSE", parameter [0:0] IS_C_INVERTED = 1'b0,
                  parameter [0:0] IS_ODATAIN_INVERTED = 1'b0, parameter ODELAY_TYPE = "FIXED",
                  parameter integer ODELAY_VALUE = 0, parameter PIPE_SEL = "FALSE",
                  parameter real REFCLK_FREQUENCY = 200.0, parameter SIGNAL_PATTERN = "DATA")
                 (output reg [4:0] CNTVALUEOUT, output DATAOUT, input C, input CE, input CINVCTRL, input CLKIN,
                  input [4:0] CNTVALUEIN, input INC, input LD, input LDPIPEEN, input ODATAIN, input REGRST);
    assign DATAOUT = ODATAIN;
    always @(posedge C)
        if (LD) CNTVALUEOUT <= CNTVALUEIN;
        else if (CE) CNTVALUEOUT <= INC ? CNTVALUEOUT + 5'd1 : CNTVALUEOUT - 5'd1;
endmodule

module IDELAYE3 #(parameter CASCADE = "NONE", parameter DELAY_FORMAT = "TIME", parameter DELAY_SRC = "IDATAIN",
                  parameter DELAY_TYPE = "FIXED", parameter integer DELAY_VALUE = 0,
                  parameter [0:0] IS_CLK_INVERTED = 1'b0, parameter [0:0] IS_RST_INVERTED = 1'b0,
                  parameter LOOPBACK = "FALSE", parameter real REFCLK_FREQUENCY = 300.0,
                  parameter SIM_DEVICE = "ULTRASCALE", parameter real SIM_VERSION = 2.0,
                  parameter UPDATE_MODE = "ASYNC")
                 (output CASC_OUT, output reg [8:0] CNTVALUEOUT, output DATAOUT, input CASC_IN,
                  input CASC_RETURN, input CE, input CLK, input [8:0] CNTVALUEIN, input DATAIN, input EN_VTC,
                  input IDATAIN, input INC, input LOAD, input RST);
    assign DATAOUT = DELAY_SRC == "DATAIN" ? DATAIN : IDATAIN;
    assign CASC_OUT = DATAOUT;
    always @(posedge CLK)
        if (LOAD) CNTVALUEOUT <= CNTVALUEIN;
        else if (CE) CNTVALUEOUT <= INC ? CNTVALUEOUT + 9'd1 : CNTVALUEOUT - 9'd1;
endmodule

module ODELAYE3 #(parameter CASCADE = "NONE", parameter DELAY_FORMAT = "TIME", parameter DELAY_TYPE = "FIXED",
                  parameter integer DELAY_VALUE = 0, parameter [0:0] IS_CLK_INVERTED = 1'b0,
                  parameter [0:0] IS_RST_INVERTED = 1'b0, parameter real REFCLK_FREQUENCY = 300.0,
                  parameter SIM_DEVICE = "ULTRASCALE", parameter real SIM_VERSION = 2.0,
                  parameter UPDATE_MODE = "ASYNC")
                 (output CASC_OUT, output reg [8:0] CNTVALUEOUT, output DATAOUT, input CASC_IN,
                  input CASC_RETURN, input CE, input CLK, input [8:0] CNTVALUEIN, input EN_VTC, input INC,
                  input LOAD, input ODATAIN, input RST);
    assign DATAOUT = ODATAIN;
    assign CASC_OUT = ODATAIN;
    always @(posedge CLK)
        if (LOAD) CNTVALUEOUT <= CNTVALUEIN;
        else if (CE) CNTVALUEOUT <= INC ? CNTVALUEOUT + 9'd1 : CNTVALUEOUT - 9'd1;
endmodule

// RDY rises some time after REFCLK runs and RST is released: no clock domain.
module IDELAYCTRL #(parameter SIM_DEVICE = "7SERIES") (output RDY, input REFCLK, input RST);
    opencdc_async_source u_rdy (RDY);
endmodule

// ---------------------------------------------------------------- block RAM
// Port A reads and writes on CLKARDCLK, port B on CLKBWRCLK. Each port's
// writes go to its own array and both arrays are read, so data written by
// one port and read by the other crosses between the two clocks (in simple
// dual-port mode port A is the read port and port B the write port).
module RAMB36E1 #(parameter integer READ_WIDTH_A = 0, parameter integer READ_WIDTH_B = 0,
                  parameter integer WRITE_WIDTH_A = 0, parameter integer WRITE_WIDTH_B = 0,
                  parameter integer DOA_REG = 0, parameter integer DOB_REG = 0,
                  parameter RAM_MODE = "TDP", parameter WRITE_MODE_A = "WRITE_FIRST",
                  parameter WRITE_MODE_B = "WRITE_FIRST", parameter SIM_DEVICE = "7SERIES")
                 (output CASCADEOUTA, output CASCADEOUTB, output DBITERR, output [31:0] DOADO,
                  output [31:0] DOBDO, output [3:0] DOPADOP, output [3:0] DOPBDOP, output [7:0] ECCPARITY,
                  output [8:0] RDADDRECC, output SBITERR,
                  input [15:0] ADDRARDADDR, input [15:0] ADDRBWRADDR, input CASCADEINA, input CASCADEINB,
                  input CLKARDCLK, input CLKBWRCLK, input [31:0] DIADI, input [31:0] DIBDI, input [3:0] DIPADIP,
                  input [3:0] DIPBDIP, input ENARDEN, input ENBWREN, input INJECTDBITERR, input INJECTSBITERR,
                  input REGCEAREGCE, input REGCEB, input RSTRAMARSTRAM, input RSTRAMB, input RSTREGARSTREG,
                  input RSTREGB, input [3:0] WEA, input [7:0] WEBWE);
    reg [35:0] mem_a [0:1023];
    reg [35:0] mem_b [0:1023];
    wire [9:0] aa = ADDRARDADDR[14:5];
    wire [9:0] ab = ADDRBWRADDR[14:5];
    reg [35:0] da, db;
    always @(posedge CLKARDCLK) if (ENARDEN && |WEA) mem_a[aa] <= {DIPADIP, DIADI};
    always @(posedge CLKBWRCLK) if (ENBWREN && |WEBWE) mem_b[ab] <= {DIPBDIP, DIBDI};
    // SDP (72-bit) mode writes through both data inputs on port B
    always @(posedge CLKARDCLK)
        if (RSTRAMARSTRAM) da <= 36'd0;
        else if (ENARDEN) da <= mem_a[aa] ^ mem_b[aa];
    always @(posedge CLKBWRCLK)
        if (RSTRAMB) db <= 36'd0;
        else if (ENBWREN) db <= mem_a[ab] ^ mem_b[ab];
    assign {DOPADOP, DOADO} = da;
    assign {DOPBDOP, DOBDO} = db;
    assign CASCADEOUTA = 1'b0;
    assign CASCADEOUTB = 1'b0;
    assign DBITERR = 1'b0;
    assign SBITERR = 1'b0;
    assign ECCPARITY = 8'd0;
    assign RDADDRECC = 9'd0;
endmodule

module RAMB18E1 #(parameter integer READ_WIDTH_A = 0, parameter integer READ_WIDTH_B = 0,
                  parameter integer WRITE_WIDTH_A = 0, parameter integer WRITE_WIDTH_B = 0,
                  parameter integer DOA_REG = 0, parameter integer DOB_REG = 0,
                  parameter RAM_MODE = "TDP", parameter WRITE_MODE_A = "WRITE_FIRST",
                  parameter WRITE_MODE_B = "WRITE_FIRST", parameter SIM_DEVICE = "7SERIES")
                 (output [15:0] DOADO, output [15:0] DOBDO, output [1:0] DOPADOP, output [1:0] DOPBDOP,
                  input [13:0] ADDRARDADDR, input [13:0] ADDRBWRADDR, input CLKARDCLK, input CLKBWRCLK,
                  input [15:0] DIADI, input [15:0] DIBDI, input [1:0] DIPADIP, input [1:0] DIPBDIP,
                  input ENARDEN, input ENBWREN, input REGCEAREGCE, input REGCEB, input RSTRAMARSTRAM,
                  input RSTRAMB, input RSTREGARSTREG, input RSTREGB, input [1:0] WEA, input [3:0] WEBWE);
    reg [17:0] mem_a [0:1023];
    reg [17:0] mem_b [0:1023];
    wire [9:0] aa = ADDRARDADDR[13:4];
    wire [9:0] ab = ADDRBWRADDR[13:4];
    reg [17:0] da, db;
    always @(posedge CLKARDCLK) if (ENARDEN && |WEA) mem_a[aa] <= {DIPADIP, DIADI};
    always @(posedge CLKBWRCLK) if (ENBWREN && |WEBWE) mem_b[ab] <= {DIPBDIP, DIBDI};
    always @(posedge CLKARDCLK)
        if (RSTRAMARSTRAM) da <= 18'd0;
        else if (ENARDEN) da <= mem_a[aa] ^ mem_b[aa];
    always @(posedge CLKBWRCLK)
        if (RSTRAMB) db <= 18'd0;
        else if (ENBWREN) db <= mem_a[ab] ^ mem_b[ab];
    assign {DOPADOP, DOADO} = da;
    assign {DOPBDOP, DOBDO} = db;
endmodule

// UltraScale block RAM: the data ports (cascade, ECC and sleep pins that a
// design connects are accepted and ignored; see FE-PRIMITIVE-PORTS).
module RAMB36E2 #(parameter integer READ_WIDTH_A = 0, parameter integer READ_WIDTH_B = 0,
                  parameter integer WRITE_WIDTH_A = 0, parameter integer WRITE_WIDTH_B = 0,
                  parameter integer DOA_REG = 1, parameter integer DOB_REG = 1,
                  parameter WRITE_MODE_A = "NO_CHANGE", parameter WRITE_MODE_B = "NO_CHANGE")
                 (output [31:0] DOUTADOUT, output [31:0] DOUTBDOUT, output [3:0] DOUTPADOUTP,
                  output [3:0] DOUTPBDOUTP, output DBITERR, output SBITERR, output [7:0] ECCPARITY,
                  output [8:0] RDADDRECC,
                  input [14:0] ADDRARDADDR, input [14:0] ADDRBWRADDR, input ADDRENA, input ADDRENB,
                  input CLKARDCLK, input CLKBWRCLK, input [31:0] DINADIN, input [31:0] DINBDIN,
                  input [3:0] DINPADINP, input [3:0] DINPBDINP, input ENARDEN, input ENBWREN,
                  input REGCEAREGCE, input REGCEB, input RSTRAMARSTRAM, input RSTRAMB, input RSTREGARSTREG,
                  input RSTREGB, input SLEEP, input [3:0] WEA, input [7:0] WEBWE);
    reg [35:0] mem_a [0:1023];
    reg [35:0] mem_b [0:1023];
    wire [9:0] aa = ADDRARDADDR[14:5];
    wire [9:0] ab = ADDRBWRADDR[14:5];
    reg [35:0] da, db;
    always @(posedge CLKARDCLK) if (ENARDEN && |WEA) mem_a[aa] <= {DINPADINP, DINADIN};
    always @(posedge CLKBWRCLK) if (ENBWREN && |WEBWE) mem_b[ab] <= {DINPBDINP, DINBDIN};
    always @(posedge CLKARDCLK)
        if (RSTRAMARSTRAM) da <= 36'd0;
        else if (ENARDEN) da <= mem_a[aa] ^ mem_b[aa];
    always @(posedge CLKBWRCLK)
        if (RSTRAMB) db <= 36'd0;
        else if (ENBWREN) db <= mem_a[ab] ^ mem_b[ab];
    assign {DOUTPADOUTP, DOUTADOUT} = da;
    assign {DOUTPBDOUTP, DOUTBDOUT} = db;
    assign DBITERR = 1'b0;
    assign SBITERR = 1'b0;
    assign ECCPARITY = 8'd0;
    assign RDADDRECC = 9'd0;
endmodule

module RAMB18E2 #(parameter integer READ_WIDTH_A = 0, parameter integer READ_WIDTH_B = 0,
                  parameter integer WRITE_WIDTH_A = 0, parameter integer WRITE_WIDTH_B = 0,
                  parameter integer DOA_REG = 1, parameter integer DOB_REG = 1,
                  parameter WRITE_MODE_A = "NO_CHANGE", parameter WRITE_MODE_B = "NO_CHANGE")
                 (output [15:0] DOUTADOUT, output [15:0] DOUTBDOUT, output [1:0] DOUTPADOUTP,
                  output [1:0] DOUTPBDOUTP,
                  input [13:0] ADDRARDADDR, input [13:0] ADDRBWRADDR, input ADDRENA, input ADDRENB,
                  input CLKARDCLK, input CLKBWRCLK, input [15:0] DINADIN, input [15:0] DINBDIN,
                  input [1:0] DINPADINP, input [1:0] DINPBDINP, input ENARDEN, input ENBWREN,
                  input REGCEAREGCE, input REGCEB, input RSTRAMARSTRAM, input RSTRAMB, input RSTREGARSTREG,
                  input RSTREGB, input SLEEP, input [1:0] WEA, input [3:0] WEBWE);
    reg [17:0] mem_a [0:1023];
    reg [17:0] mem_b [0:1023];
    wire [9:0] aa = ADDRARDADDR[13:4];
    wire [9:0] ab = ADDRBWRADDR[13:4];
    reg [17:0] da, db;
    always @(posedge CLKARDCLK) if (ENARDEN && |WEA) mem_a[aa] <= {DINPADINP, DINADIN};
    always @(posedge CLKBWRCLK) if (ENBWREN && |WEBWE) mem_b[ab] <= {DINPBDINP, DINBDIN};
    always @(posedge CLKARDCLK)
        if (RSTRAMARSTRAM) da <= 18'd0;
        else if (ENARDEN) da <= mem_a[aa] ^ mem_b[aa];
    always @(posedge CLKBWRCLK)
        if (RSTRAMB) db <= 18'd0;
        else if (ENBWREN) db <= mem_a[ab] ^ mem_b[ab];
    assign {DOUTPADOUTP, DOUTADOUT} = da;
    assign {DOUTPBDOUTP, DOUTBDOUT} = db;
endmodule

// ---------------------------------------------------------------- SERDES
// Serial data is shifted in on CLK; the parallel word is registered on CLKDIV
// (CLK and CLKDIV come from one source in every legal configuration).
module ISERDESE2 #(parameter DATA_RATE = "DDR", parameter integer DATA_WIDTH = 4, parameter DYN_CLKDIV_INV_EN = "FALSE",
                   parameter DYN_CLK_INV_EN = "FALSE", parameter [0:0] INIT_Q1 = 1'b0, parameter [0:0] INIT_Q2 = 1'b0,
                   parameter [0:0] INIT_Q3 = 1'b0, parameter [0:0] INIT_Q4 = 1'b0, parameter INTERFACE_TYPE = "MEMORY",
                   parameter IOBDELAY = "NONE", parameter integer NUM_CE = 2, parameter OFB_USED = "FALSE",
                   parameter SERDES_MODE = "MASTER", parameter [0:0] SRVAL_Q1 = 1'b0, parameter [0:0] SRVAL_Q2 = 1'b0,
                   parameter [0:0] SRVAL_Q3 = 1'b0, parameter [0:0] SRVAL_Q4 = 1'b0)
                  (output O, output Q1, output Q2, output Q3, output Q4, output Q5, output Q6, output Q7, output Q8,
                   output SHIFTOUT1, output SHIFTOUT2, input BITSLIP, input CE1, input CE2, input CLK, input CLKB,
                   input CLKDIV, input CLKDIVP, input D, input DDLY, input DYNCLKDIVSEL, input DYNCLKSEL, input OCLK,
                   input OCLKB, input OFB, input RST, input SHIFTIN1, input SHIFTIN2);
    wire din = IOBDELAY == "NONE" ? D : DDLY;
    reg [7:0] sh, q;
    always @(posedge CLK) if (CE1) sh <= {sh[6:0], din};
    always @(posedge CLKDIV) if (RST) q <= 8'd0; else q <= BITSLIP ? {sh[6:0], sh[7]} : sh;
    assign {Q8, Q7, Q6, Q5, Q4, Q3, Q2, Q1} = q;
    assign O = D;
    assign SHIFTOUT1 = sh[7];
    assign SHIFTOUT2 = sh[6];
endmodule

module OSERDESE2 #(parameter DATA_RATE_OQ = "DDR", parameter DATA_RATE_TQ = "DDR", parameter integer DATA_WIDTH = 4,
                   parameter [0:0] INIT_OQ = 1'b0, parameter [0:0] INIT_TQ = 1'b0, parameter SERDES_MODE = "MASTER",
                   parameter [0:0] SRVAL_OQ = 1'b0, parameter [0:0] SRVAL_TQ = 1'b0, parameter TBYTE_CTL = "FALSE",
                   parameter TBYTE_SRC = "FALSE", parameter integer TRISTATE_WIDTH = 4)
                  (output OFB, output OQ, output SHIFTOUT1, output SHIFTOUT2, output TBYTEOUT, output TFB, output TQ,
                   input CLK, input CLKDIV, input D1, input D2, input D3, input D4, input D5, input D6, input D7,
                   input D8, input OCE, input RST, input SHIFTIN1, input SHIFTIN2, input T1, input T2, input T3,
                   input T4, input TBYTEIN, input TCE);
    reg [7:0] word, sh;
    reg [3:0] tword;
    reg tq;
    always @(posedge CLKDIV)
        if (RST) begin word <= 8'd0; tword <= 4'd0; end
        else begin word <= {D8, D7, D6, D5, D4, D3, D2, D1}; tword <= {T4, T3, T2, T1}; end
    always @(posedge CLK)
        if (OCE) sh <= {1'b0, sh[7:1]} | word;
    always @(posedge CLK) if (TCE) tq <= |tword;
    assign OQ = sh[0];
    assign OFB = sh[0];
    assign TQ = tq;
    assign TFB = tq;
    assign SHIFTOUT1 = 1'b0;
    assign SHIFTOUT2 = 1'b0;
    assign TBYTEOUT = TBYTEIN;
endmodule

// UltraScale: with FIFO_ENABLE the parallel word crosses into FIFO_RD_CLK
// through the hard FIFO (a built-in synchronizer); the model hands it over
// in the CLKDIV domain.
module ISERDESE3 #(parameter integer DATA_WIDTH = 8, parameter DDR_CLK_EDGE = "OPPOSITE_EDGE",
                   parameter FIFO_ENABLE = "FALSE", parameter FIFO_SYNC_MODE = "FALSE", parameter IDDR_MODE = "FALSE",
                   parameter [0:0] IS_CLK_B_INVERTED = 1'b0, parameter [0:0] IS_CLK_INVERTED = 1'b0,
                   parameter [0:0] IS_RST_INVERTED = 1'b0, parameter SIM_DEVICE = "ULTRASCALE",
                   parameter real SIM_VERSION = 2.0)
                  (output FIFO_EMPTY, output INTERNAL_DIVCLK, output [7:0] Q, input CLK, input CLKDIV, input CLK_B,
                   input D, input FIFO_RD_CLK, input FIFO_RD_EN, input RST);
    reg [7:0] sh, q;
    always @(posedge CLK) sh <= {sh[6:0], D};
    always @(posedge CLKDIV or posedge RST) if (RST) q <= 8'd0; else q <= sh;
    assign Q = q;
    assign FIFO_EMPTY = 1'b0;
    assign INTERNAL_DIVCLK = CLKDIV;
endmodule

module OSERDESE3 #(parameter integer DATA_WIDTH = 8, parameter [0:0] INIT = 1'b0, parameter [0:0] IS_CLKDIV_INVERTED = 1'b0,
                   parameter [0:0] IS_CLK_INVERTED = 1'b0, parameter [0:0] IS_RST_INVERTED = 1'b0,
                   parameter ODDR_MODE = "FALSE", parameter OSERDES_D_BYPASS = "FALSE",
                   parameter OSERDES_T_BYPASS = "FALSE", parameter SIM_DEVICE = "ULTRASCALE",
                   parameter real SIM_VERSION = 2.0)
                  (output OQ, output T_OUT, input CLK, input CLKDIV, input [7:0] D, input RST, input T);
    reg [7:0] word, sh;
    always @(posedge CLKDIV or posedge RST) if (RST) word <= 8'd0; else word <= D;
    always @(posedge CLK) sh <= {1'b0, sh[7:1]} | word;
    assign OQ = sh[0];
    assign T_OUT = T;
endmodule

// ---------------------------------------------------------------- UltraRAM
// One clock for both ports: no crossing inside; modelled so that designs
// using it elaborate (cascade, ECC and sleep pins are not modelled).
module URAM288 #(parameter integer AUTO_SLEEP_LATENCY = 8, parameter integer AVG_CONS_INACTIVE_CYCLES = 10,
                 parameter BWE_MODE_A = "PARITY_INTERLEAVED", parameter BWE_MODE_B = "PARITY_INTERLEAVED",
                 parameter CASCADE_ORDER_A = "NONE", parameter CASCADE_ORDER_B = "NONE",
                 parameter EN_AUTO_SLEEP_MODE = "FALSE", parameter EN_ECC_RD_A = "FALSE",
                 parameter EN_ECC_RD_B = "FALSE", parameter EN_ECC_WR_A = "FALSE", parameter EN_ECC_WR_B = "FALSE",
                 parameter IREG_PRE_A = "FALSE", parameter IREG_PRE_B = "FALSE", parameter OREG_A = "FALSE",
                 parameter OREG_B = "FALSE", parameter OREG_ECC_A = "FALSE", parameter OREG_ECC_B = "FALSE")
                (output [71:0] DOUT_A, output [71:0] DOUT_B, output DBITERR_A, output DBITERR_B, output SBITERR_A,
                 output SBITERR_B, output RDACCESS_A, output RDACCESS_B,
                 input [22:0] ADDR_A, input [22:0] ADDR_B, input [8:0] BWE_A, input [8:0] BWE_B, input CLK,
                 input [71:0] DIN_A, input [71:0] DIN_B, input EN_A, input EN_B, input RDB_WR_A, input RDB_WR_B,
                 input RST_A, input RST_B, input SLEEP, input OREG_CE_A, input OREG_CE_B, input OREG_ECC_CE_A,
                 input OREG_ECC_CE_B, input INJECT_DBITERR_A, input INJECT_DBITERR_B, input INJECT_SBITERR_A,
                 input INJECT_SBITERR_B);
    reg [71:0] mem [0:4095];
    reg [71:0] da, db;
    always @(posedge CLK) begin
        if (EN_A && RDB_WR_A) mem[ADDR_A[11:0]] <= DIN_A;
        if (EN_B && RDB_WR_B) mem[ADDR_B[11:0]] <= DIN_B;
    end
    always @(posedge CLK) begin
        if (RST_A) da <= 72'd0; else if (EN_A) da <= mem[ADDR_A[11:0]];
        if (RST_B) db <= 72'd0; else if (EN_B) db <= mem[ADDR_B[11:0]];
    end
    assign DOUT_A = da;
    assign DOUT_B = db;
    assign {DBITERR_A, DBITERR_B, SBITERR_A, SBITERR_B} = 4'd0;
    assign RDACCESS_A = EN_A & ~RDB_WR_A;
    assign RDACCESS_B = EN_B & ~RDB_WR_B;
endmodule

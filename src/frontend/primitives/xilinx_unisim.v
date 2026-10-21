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

module BUFR #(parameter BUFR_DIVIDE = "BYPASS", parameter SIM_DEVICE = "7SERIES")
             (output O, input CE, input CLR, input I);
    assign O = I;
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

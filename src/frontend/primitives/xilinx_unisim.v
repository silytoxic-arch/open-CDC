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

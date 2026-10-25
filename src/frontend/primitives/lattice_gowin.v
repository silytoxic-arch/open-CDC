// OpenCDC behavioral models of Lattice and Gowin clocking primitives (the
// clock structure only). Output periods are derived from the divider
// parameters; LOCK has no clock domain.

// Lattice ECP5 PLL with feedback from CLKOP (FEEDBK_PATH = "CLKOP"):
//   f(CLKOP) = f(CLKI) x CLKFB_DIV / CLKI_DIV,  f(CLKOSx) = f(CLKOP) x CLKOP_DIV / CLKOSx_DIV
module EHXPLLL #(parameter CLKI_DIV = 1, parameter CLKFB_DIV = 1, parameter CLKOP_DIV = 8,
                 parameter CLKOS_DIV = 8, parameter CLKOS2_DIV = 8, parameter CLKOS3_DIV = 8,
                 parameter CLKOP_ENABLE = "ENABLED", parameter CLKOS_ENABLE = "DISABLED",
                 parameter CLKOS2_ENABLE = "DISABLED", parameter CLKOS3_ENABLE = "DISABLED",
                 parameter FEEDBK_PATH = "CLKOP", parameter CLKOP_CPHASE = 0, parameter CLKOP_FPHASE = 0,
                 parameter real FREQUENCY_PIN_CLKI = 0.0)
                (input CLKI, input CLKFB, input PHASESEL1, input PHASESEL0, input PHASEDIR, input PHASESTEP,
                 input PHASELOADREG, input STDBY, input PLLWAKESYNC, input RST, input ENCLKOP, input ENCLKOS,
                 input ENCLKOS2, input ENCLKOS3, output CLKOP, output CLKOS, output CLKOS2, output CLKOS3,
                 output LOCK, output INTLOCK, output REFCLK, output CLKINTFB);
    localparam real INP = FREQUENCY_PIN_CLKI > 0.0 ? 1000.0 / FREQUENCY_PIN_CLKI : 0.0;
    localparam real OP = (1.0 * CLKI_DIV) / CLKFB_DIV; // period ratio of CLKOP
    (* OPENCDC_CLOCK_PIN = "CLKOP", OPENCDC_CLOCK_RATIO = OP, OPENCDC_CLOCK_INPUT_PERIOD = INP *) reg op = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS", OPENCDC_CLOCK_RATIO = OP * CLKOS_DIV / CLKOP_DIV,
       OPENCDC_CLOCK_INPUT_PERIOD = INP *) reg os = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS2", OPENCDC_CLOCK_RATIO = OP * CLKOS2_DIV / CLKOP_DIV,
       OPENCDC_CLOCK_INPUT_PERIOD = INP *) reg os2 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS3", OPENCDC_CLOCK_RATIO = OP * CLKOS3_DIV / CLKOP_DIV,
       OPENCDC_CLOCK_INPUT_PERIOD = INP *) reg os3 = 1'b0;
    always @(posedge CLKI) begin op <= ~op; os <= ~os; os2 <= ~os2; os3 <= ~os3; end
    assign CLKOP = op;
    assign CLKOS = os;
    assign CLKOS2 = os2;
    assign CLKOS3 = os3;
    opencdc_async_source u_lock (LOCK);
    assign INTLOCK = LOCK;
    assign REFCLK = CLKI;
    assign CLKINTFB = op;
endmodule

// Lattice iCE40 PLL: f(out) = f(ref) x (DIVF + 1) / ((DIVR + 1) x 2^DIVQ)
module SB_PLL40_CORE #(parameter FEEDBACK_PATH = "SIMPLE", parameter [3:0] DIVR = 4'd0,
                       parameter [6:0] DIVF = 7'd0, parameter [2:0] DIVQ = 3'd0, parameter [2:0] FILTER_RANGE = 3'd0,
                       parameter PLLOUT_SELECT = "GENCLK", parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED",
                       parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED", parameter [3:0] FDA_FEEDBACK = 4'd0,
                       parameter [3:0] FDA_RELATIVE = 4'd0, parameter ENABLE_ICEGATE = 1'b0)
                      (input REFERENCECLK, output PLLOUTCORE, output PLLOUTGLOBAL, input EXTFEEDBACK,
                       input [7:0] DYNAMICDELAY, output LOCK, input BYPASS, input RESETB, input LATCHINPUTVALUE,
                       output SDO, input SDI, input SCLK);
    (* OPENCDC_CLOCK_PIN = "PLLOUTCORE", OPENCDC_CLOCK_RATIO = (1.0 * (DIVR + 1) * (1 << DIVQ)) / (DIVF + 1) *)
    reg o = 1'b0;
    always @(posedge REFERENCECLK) o <= ~o;
    assign PLLOUTCORE = o;
    assign PLLOUTGLOBAL = o;
    opencdc_async_source u_lock (LOCK);
    assign SDO = 1'b0;
endmodule

module SB_PLL40_PAD #(parameter FEEDBACK_PATH = "SIMPLE", parameter [3:0] DIVR = 4'd0,
                      parameter [6:0] DIVF = 7'd0, parameter [2:0] DIVQ = 3'd0, parameter [2:0] FILTER_RANGE = 3'd0,
                      parameter PLLOUT_SELECT = "GENCLK", parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED",
                      parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED", parameter [3:0] FDA_FEEDBACK = 4'd0,
                      parameter [3:0] FDA_RELATIVE = 4'd0, parameter ENABLE_ICEGATE = 1'b0)
                     (input PACKAGEPIN, output PLLOUTCORE, output PLLOUTGLOBAL, input EXTFEEDBACK,
                      input [7:0] DYNAMICDELAY, output LOCK, input BYPASS, input RESETB, input LATCHINPUTVALUE,
                      output SDO, input SDI, input SCLK);
    (* OPENCDC_CLOCK_PIN = "PLLOUTCORE", OPENCDC_CLOCK_RATIO = (1.0 * (DIVR + 1) * (1 << DIVQ)) / (DIVF + 1) *)
    reg o = 1'b0;
    always @(posedge PACKAGEPIN) o <= ~o;
    assign PLLOUTCORE = o;
    assign PLLOUTGLOBAL = o;
    opencdc_async_source u_lock (LOCK);
    assign SDO = 1'b0;
endmodule

module SB_GB (input USER_SIGNAL_TO_GLOBAL_BUFFER, output GLOBAL_BUFFER_OUTPUT);
    assign GLOBAL_BUFFER_OUTPUT = USER_SIGNAL_TO_GLOBAL_BUFFER;
endmodule

module SB_DFF (output reg Q, input C, input D);
    always @(posedge C) Q <= D;
endmodule

module SB_DFFE (output reg Q, input C, input E, input D);
    always @(posedge C) if (E) Q <= D;
endmodule

module SB_DFFR (output reg Q, input C, input R, input D);
    always @(posedge C or posedge R) if (R) Q <= 1'b0; else Q <= D;
endmodule

module SB_DFFS (output reg Q, input C, input S, input D);
    always @(posedge C or posedge S) if (S) Q <= 1'b1; else Q <= D;
endmodule

module SB_DFFER (output reg Q, input C, input E, input R, input D);
    always @(posedge C or posedge R) if (R) Q <= 1'b0; else if (E) Q <= D;
endmodule

// Gowin rPLL: f(CLKOUT) = f(CLKIN) x (FBDIV_SEL + 1) / (IDIV_SEL + 1);
// CLKOUTD = CLKOUT / DYN_SDIV_SEL, CLKOUTD3 = CLKOUT / 3. FCLKIN is the
// input frequency in MHz (a string).
module rPLL #(parameter FCLKIN = "100.0", parameter DYN_IDIV_SEL = "false", parameter integer IDIV_SEL = 0,
              parameter DYN_FBDIV_SEL = "false", parameter integer FBDIV_SEL = 0, parameter DYN_ODIV_SEL = "false",
              parameter integer ODIV_SEL = 8, parameter PSDA_SEL = "0000", parameter DYN_DA_EN = "false",
              parameter DUTYDA_SEL = "1000", parameter CLKOUT_FT_DIR = 1'b1, parameter CLKOUTP_FT_DIR = 1'b1,
              parameter CLKOUT_DLY_STEP = 0, parameter CLKOUTP_DLY_STEP = 0, parameter CLKFB_SEL = "internal",
              parameter CLKOUT_BYPASS = "false", parameter CLKOUTP_BYPASS = "false", parameter CLKOUTD_BYPASS = "false",
              parameter integer DYN_SDIV_SEL = 2, parameter CLKOUTD_SRC = "CLKOUT", parameter CLKOUTD3_SRC = "CLKOUT",
              parameter DEVICE = "GW1N-1")
             (output CLKOUT, output LOCK, output CLKOUTP, output CLKOUTD, output CLKOUTD3, input RESET,
              input RESET_P, input CLKIN, input CLKFB, input [5:0] FBDSEL, input [5:0] IDSEL, input [5:0] ODSEL,
              input [3:0] PSDA, input [3:0] DUTYDA, input [3:0] FDLY);
    localparam real R = (1.0 * (IDIV_SEL + 1)) / (FBDIV_SEL + 1);
    (* OPENCDC_CLOCK_PIN = "CLKOUT", OPENCDC_CLOCK_RATIO = R, OPENCDC_CLOCK_FREQ_IN = FCLKIN *) reg o = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOUTP", OPENCDC_CLOCK_RATIO = R, OPENCDC_CLOCK_FREQ_IN = FCLKIN *) reg p = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOUTD", OPENCDC_CLOCK_RATIO = R * DYN_SDIV_SEL, OPENCDC_CLOCK_FREQ_IN = FCLKIN *)
    reg d = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOUTD3", OPENCDC_CLOCK_RATIO = R * 3, OPENCDC_CLOCK_FREQ_IN = FCLKIN *) reg d3 = 1'b0;
    always @(posedge CLKIN) begin o <= ~o; p <= ~p; d <= ~d; d3 <= ~d3; end
    assign CLKOUT = o;
    assign CLKOUTP = p;
    assign CLKOUTD = d;
    assign CLKOUTD3 = d3;
    opencdc_async_source u_lock (LOCK);
endmodule

module PLLVR #(parameter FCLKIN = "100.0", parameter integer IDIV_SEL = 0, parameter integer FBDIV_SEL = 0,
               parameter integer ODIV_SEL = 8, parameter integer DYN_SDIV_SEL = 2, parameter DEVICE = "GW1NSR-4C")
              (output CLKOUT, output LOCK, output CLKOUTP, output CLKOUTD, output CLKOUTD3, input RESET,
               input RESET_P, input CLKIN, input CLKFB, input [5:0] FBDSEL, input [5:0] IDSEL, input [5:0] ODSEL,
               input [3:0] PSDA, input [3:0] DUTYDA, input [3:0] FDLY, input VREN);
    localparam real R = (1.0 * (IDIV_SEL + 1)) / (FBDIV_SEL + 1);
    (* OPENCDC_CLOCK_PIN = "CLKOUT", OPENCDC_CLOCK_RATIO = R, OPENCDC_CLOCK_FREQ_IN = FCLKIN *) reg o = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOUTD", OPENCDC_CLOCK_RATIO = R * DYN_SDIV_SEL, OPENCDC_CLOCK_FREQ_IN = FCLKIN *)
    reg d = 1'b0;
    always @(posedge CLKIN) begin o <= ~o; d <= ~d; end
    assign CLKOUT = o;
    assign CLKOUTP = o;
    assign CLKOUTD = d;
    assign CLKOUTD3 = d;
    opencdc_async_source u_lock (LOCK);
endmodule

// Generic Gowin names (DFF, DFFE, CLKDIV, ...) are not modelled: they collide
// with common user module names, and a missing user module must stay an error.

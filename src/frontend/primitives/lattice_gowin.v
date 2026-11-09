// OpenCDC behavioral models of Lattice and Gowin primitives: clock generators
// (the clock structure only; output periods are derived from the divider
// parameters; LOCK has no clock domain), registers, DDR cells and block RAMs.
// Gowin IDDR/ODDR are not modelled: their names are taken by the Xilinx cells.

// Lattice ECP5 PLL. The feedback output (FEEDBK_PATH: CLKOP, CLKOS, CLKOS2,
// CLKOS3 or the INT_ variants) closes the loop, so
//   f(VCO) = f(CLKI) x CLKFB_DIV x <feedback output>_DIV / CLKI_DIV,  f(CLKx) = f(VCO) / CLKx_DIV
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
    localparam integer FBD = (FEEDBK_PATH == "CLKOS" || FEEDBK_PATH == "INT_OS")    ? CLKOS_DIV
                           : (FEEDBK_PATH == "CLKOS2" || FEEDBK_PATH == "INT_OS2") ? CLKOS2_DIV
                           : (FEEDBK_PATH == "CLKOS3" || FEEDBK_PATH == "INT_OS3") ? CLKOS3_DIV
                                                                                   : CLKOP_DIV;
    localparam real VCO = (1.0 * CLKI_DIV) / (1.0 * CLKFB_DIV * FBD); // period ratio of the VCO
    (* OPENCDC_CLOCK_PIN = "CLKOP", OPENCDC_CLOCK_RATIO = VCO * CLKOP_DIV, OPENCDC_CLOCK_INPUT_PERIOD = INP *)
    reg op = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS", OPENCDC_CLOCK_RATIO = VCO * CLKOS_DIV, OPENCDC_CLOCK_INPUT_PERIOD = INP *)
    reg os = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS2", OPENCDC_CLOCK_RATIO = VCO * CLKOS2_DIV, OPENCDC_CLOCK_INPUT_PERIOD = INP *)
    reg os2 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLKOS3", OPENCDC_CLOCK_RATIO = VCO * CLKOS3_DIV, OPENCDC_CLOCK_INPUT_PERIOD = INP *)
    reg os3 = 1'b0;
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

// iCE40 SB_PLL40_2F_PAD: two outputs of the PLL; port A/B: GENCLK = f(ref) x (DIVF+1) / ((DIVR+1) x 2^DIVQ),
// GENCLK_HALF = half of it
module SB_PLL40_2F_PAD #(parameter FEEDBACK_PATH = "SIMPLE", parameter [3:0] DIVR = 4'd0, parameter [6:0] DIVF = 7'd0,
                parameter [2:0] DIVQ = 3'd0, parameter [2:0] FILTER_RANGE = 3'd0,
                parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED", parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED",
                parameter [3:0] FDA_FEEDBACK = 4'd0, parameter [3:0] FDA_RELATIVE = 4'd0,
                parameter [1:0] SHIFTREG_DIV_MODE = 2'b00, parameter PLLOUT_SELECT_PORTA = "GENCLK",
                parameter PLLOUT_SELECT_PORTB = "GENCLK", parameter ENABLE_ICEGATE_PORTA = 1'b0,
                parameter ENABLE_ICEGATE_PORTB = 1'b0, parameter TEST_MODE = 1'b0, parameter EXTERNAL_DIVIDE_FACTOR = 1)
               (input PACKAGEPIN, output PLLOUTCOREA, output PLLOUTGLOBALA, output PLLOUTCOREB, output PLLOUTGLOBALB,
                input EXTFEEDBACK, input [7:0] DYNAMICDELAY, output LOCK, input BYPASS, input RESETB,
                input LATCHINPUTVALUE, output SDO, input SDI, input SCLK);
    localparam real R = (1.0 * (DIVR + 1) * (1 << DIVQ)) / (DIVF + 1);
    localparam real RA = PLLOUT_SELECT_PORTA == "GENCLK_HALF" ? 2.0 * R : R;
    localparam real RB = PLLOUT_SELECT_PORTB == "GENCLK_HALF" ? 2.0 * R : R;
    (* OPENCDC_CLOCK_PIN = "PLLOUTGLOBALA", OPENCDC_CLOCK_RATIO = RA *) reg a = 1'b0;
    (* OPENCDC_CLOCK_PIN = "PLLOUTGLOBALB", OPENCDC_CLOCK_RATIO = RB *) reg b = 1'b0;
    always @(posedge PACKAGEPIN) begin a <= ~a; b <= ~b; end
    assign PLLOUTCOREA = a;
    assign PLLOUTGLOBALA = a;
    assign PLLOUTCOREB = b;
    assign PLLOUTGLOBALB = b;
    opencdc_async_source u_lock (LOCK);
    assign SDO = 1'b0;
endmodule

// iCE40 SB_PLL40_2F_CORE: two outputs of the PLL; port A/B: GENCLK = f(ref) x (DIVF+1) / ((DIVR+1) x 2^DIVQ),
// GENCLK_HALF = half of it
module SB_PLL40_2F_CORE #(parameter FEEDBACK_PATH = "SIMPLE", parameter [3:0] DIVR = 4'd0, parameter [6:0] DIVF = 7'd0,
                parameter [2:0] DIVQ = 3'd0, parameter [2:0] FILTER_RANGE = 3'd0,
                parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED", parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED",
                parameter [3:0] FDA_FEEDBACK = 4'd0, parameter [3:0] FDA_RELATIVE = 4'd0,
                parameter [1:0] SHIFTREG_DIV_MODE = 2'b00, parameter PLLOUT_SELECT_PORTA = "GENCLK",
                parameter PLLOUT_SELECT_PORTB = "GENCLK", parameter ENABLE_ICEGATE_PORTA = 1'b0,
                parameter ENABLE_ICEGATE_PORTB = 1'b0, parameter TEST_MODE = 1'b0, parameter EXTERNAL_DIVIDE_FACTOR = 1)
               (input REFERENCECLK, output PLLOUTCOREA, output PLLOUTGLOBALA, output PLLOUTCOREB, output PLLOUTGLOBALB,
                input EXTFEEDBACK, input [7:0] DYNAMICDELAY, output LOCK, input BYPASS, input RESETB,
                input LATCHINPUTVALUE, output SDO, input SDI, input SCLK);
    localparam real R = (1.0 * (DIVR + 1) * (1 << DIVQ)) / (DIVF + 1);
    localparam real RA = PLLOUT_SELECT_PORTA == "GENCLK_HALF" ? 2.0 * R : R;
    localparam real RB = PLLOUT_SELECT_PORTB == "GENCLK_HALF" ? 2.0 * R : R;
    (* OPENCDC_CLOCK_PIN = "PLLOUTGLOBALA", OPENCDC_CLOCK_RATIO = RA *) reg a = 1'b0;
    (* OPENCDC_CLOCK_PIN = "PLLOUTGLOBALB", OPENCDC_CLOCK_RATIO = RB *) reg b = 1'b0;
    always @(posedge REFERENCECLK) begin a <= ~a; b <= ~b; end
    assign PLLOUTCOREA = a;
    assign PLLOUTGLOBALA = a;
    assign PLLOUTCOREB = b;
    assign PLLOUTGLOBALB = b;
    opencdc_async_source u_lock (LOCK);
    assign SDO = 1'b0;
endmodule

// iCE40 SB_PLL40_2_PAD: port A is the input clock itself, port B the PLL output
module SB_PLL40_2_PAD #(parameter FEEDBACK_PATH = "SIMPLE", parameter [3:0] DIVR = 4'd0, parameter [6:0] DIVF = 7'd0,
                parameter [2:0] DIVQ = 3'd0, parameter [2:0] FILTER_RANGE = 3'd0,
                parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED", parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED",
                parameter [3:0] FDA_FEEDBACK = 4'd0, parameter [3:0] FDA_RELATIVE = 4'd0,
                parameter [1:0] SHIFTREG_DIV_MODE = 2'b00, parameter PLLOUT_SELECT_PORTA = "GENCLK",
                parameter PLLOUT_SELECT_PORTB = "GENCLK", parameter ENABLE_ICEGATE_PORTA = 1'b0,
                parameter ENABLE_ICEGATE_PORTB = 1'b0, parameter TEST_MODE = 1'b0, parameter EXTERNAL_DIVIDE_FACTOR = 1)
               (input PACKAGEPIN, output PLLOUTCOREA, output PLLOUTGLOBALA, output PLLOUTCOREB, output PLLOUTGLOBALB,
                input EXTFEEDBACK, input [7:0] DYNAMICDELAY, output LOCK, input BYPASS, input RESETB,
                input LATCHINPUTVALUE, output SDO, input SDI, input SCLK);
    localparam real R = (1.0 * (DIVR + 1) * (1 << DIVQ)) / (DIVF + 1);
    localparam real RB = PLLOUT_SELECT_PORTB == "GENCLK_HALF" ? 2.0 * R : R;
    (* OPENCDC_CLOCK_PIN = "PLLOUTGLOBALB", OPENCDC_CLOCK_RATIO = RB *) reg b = 1'b0;
    always @(posedge PACKAGEPIN) b <= ~b;
    assign PLLOUTCOREA = PACKAGEPIN;
    assign PLLOUTGLOBALA = PACKAGEPIN;
    assign PLLOUTCOREB = b;
    assign PLLOUTGLOBALB = b;
    opencdc_async_source u_lock (LOCK);
    assign SDO = 1'b0;
endmodule

// iCE40 SB_IO: PIN_TYPE[1:0] input (01: direct; otherwise registered on INPUT_CLK, D_IN_1 on its other edge),
// PIN_TYPE[3:2] output (10: direct; 01: registered on OUTPUT_CLK; 11: registered, inverted; 00: DDR),
// PIN_TYPE[5:4] output enable (00: never; 01: always; 10: OUTPUT_ENABLE; 11: OUTPUT_ENABLE registered)
module SB_IO #(parameter [5:0] PIN_TYPE = 6'b000000, parameter [0:0] PULLUP = 1'b0,
               parameter [0:0] NEG_TRIGGER = 1'b0, parameter IO_STANDARD = "SB_LVCMOS")
              (inout PACKAGE_PIN, input LATCH_INPUT_VALUE, input CLOCK_ENABLE, input INPUT_CLK, input OUTPUT_CLK,
               input OUTPUT_ENABLE, input D_OUT_0, input D_OUT_1, output D_IN_0, output D_IN_1);
    wire ice = CLOCK_ENABLE !== 1'b0;
    generate
        if (PIN_TYPE[1:0] == 2'b01) begin : g_in_direct
            assign D_IN_0 = PACKAGE_PIN;
            assign D_IN_1 = 1'b0;
        end else begin : g_in_reg
            reg q0, q1;
            if (NEG_TRIGGER) begin : g_neg
                always @(negedge INPUT_CLK) if (ice) q0 <= PACKAGE_PIN;
                always @(posedge INPUT_CLK) if (ice) q1 <= PACKAGE_PIN;
            end else begin : g_pos
                always @(posedge INPUT_CLK) if (ice) q0 <= PACKAGE_PIN;
                always @(negedge INPUT_CLK) if (ice) q1 <= PACKAGE_PIN;
            end
            assign D_IN_0 = q0;
            assign D_IN_1 = q1;
        end
        wire dout;
        if (PIN_TYPE[5:4] == 2'b00) begin : g_no_out // output never enabled: no output registers
            assign dout = 1'b0;
        end else if (PIN_TYPE[3:2] == 2'b10) begin : g_out_direct
            assign dout = D_OUT_0;
        end else begin : g_out_reg
            reg o0, o1;
            if (NEG_TRIGGER) begin : g_neg
                always @(negedge OUTPUT_CLK) if (ice) o0 <= D_OUT_0;
                always @(posedge OUTPUT_CLK) if (ice) o1 <= D_OUT_1;
            end else begin : g_pos
                always @(posedge OUTPUT_CLK) if (ice) o0 <= D_OUT_0;
                always @(negedge OUTPUT_CLK) if (ice) o1 <= D_OUT_1;
            end
            assign dout = PIN_TYPE[3:2] == 2'b01 ? o0 : PIN_TYPE[3:2] == 2'b11 ? ~o0 : (OUTPUT_CLK ? o0 : o1);
        end
        if (PIN_TYPE[5:4] == 2'b01) begin : g_oe_on
            assign PACKAGE_PIN = dout;
        end else if (PIN_TYPE[5:4] == 2'b10) begin : g_oe
            assign PACKAGE_PIN = OUTPUT_ENABLE ? dout : 1'bz;
        end else if (PIN_TYPE[5:4] == 2'b11) begin : g_oe_reg
            reg oe;
            always @(posedge OUTPUT_CLK) if (ice) oe <= OUTPUT_ENABLE;
            assign PACKAGE_PIN = oe ? dout : 1'bz;
        end
    endgenerate
endmodule

// iCE40 UltraPlus SB_SPRAM256KA: 16k x 16 single-port RAM on CLOCK, registered read
module SB_SPRAM256KA (input [13:0] ADDRESS, input [15:0] DATAIN, input [3:0] MASKWREN, input WREN,
                      input CHIPSELECT, input CLOCK, input STANDBY, input SLEEP, input POWEROFF,
                      output [15:0] DATAOUT);
    reg [15:0] mem [0:16383];
    reg [15:0] q;
    always @(posedge CLOCK) if (CHIPSELECT && WREN) mem[ADDRESS] <= DATAIN;
    always @(posedge CLOCK) if (CHIPSELECT && !WREN) q <= mem[ADDRESS];
    assign DATAOUT = q;
endmodule

// iCE40 UltraPlus SB_SPI / SB_I2C: the system bus and every output in SBCLKI; the serial
// inputs are sampled there
module SB_SPI #(parameter BUS_ADDR74 = "0b0000")
              (input SBCLKI, input SBRWI, input SBSTBI, input SBADRI7, input SBADRI6, input SBADRI5, input SBADRI4, input SBADRI3, input SBADRI2, input SBADRI1, input SBADRI0, input SBDATI7, input SBDATI6, input SBDATI5, input SBDATI4, input SBDATI3, input SBDATI2, input SBDATI1, input SBDATI0,
               input MI, input SI, input SCKI, input SCSNI, output SBDATO7, output SBDATO6, output SBDATO5, output SBDATO4, output SBDATO3, output SBDATO2, output SBDATO1, output SBDATO0, output SBACKO,
               output SPIIRQ, output SPIWKUP, output SO, output SOE, output MO, output MOE, output SCKO,
               output SCKOE, output MCSNO3, output MCSNO2, output MCSNO1, output MCSNO0, output MCSNOE3, output MCSNOE2, output MCSNOE1, output MCSNOE0);
    reg [7:0] q;
    always @(posedge SBCLKI) q <= ^{SBRWI, SBSTBI, {SBADRI7, SBADRI6, SBADRI5, SBADRI4, SBADRI3, SBADRI2, SBADRI1, SBADRI0}, {SBDATI7, SBDATI6, SBDATI5, SBDATI4, SBDATI3, SBDATI2, SBDATI1, SBDATI0}, MI, SI, SCKI, SCSNI} ? q + 8'd1 : q;
    assign {SBDATO7, SBDATO6, SBDATO5, SBDATO4, SBDATO3, SBDATO2, SBDATO1, SBDATO0} = q;
    assign {SBACKO, SPIIRQ, SPIWKUP, SO, SOE, MO, MOE, SCKO, SCKOE} = {9{q[0]}};
    assign {MCSNO3, MCSNO2, MCSNO1, MCSNO0} = q[3:0];
    assign {MCSNOE3, MCSNOE2, MCSNOE1, MCSNOE0} = q[7:4];
endmodule

module SB_I2C #(parameter BUS_ADDR74 = "0b0001", parameter I2C_SLAVE_INIT_ADDR = "0b1111100001")
              (input SBCLKI, input SBRWI, input SBSTBI, input SBADRI7, input SBADRI6, input SBADRI5, input SBADRI4, input SBADRI3, input SBADRI2, input SBADRI1, input SBADRI0, input SBDATI7, input SBDATI6, input SBDATI5, input SBDATI4, input SBDATI3, input SBDATI2, input SBDATI1, input SBDATI0,
               input SCLI, input SDAI, output SBDATO7, output SBDATO6, output SBDATO5, output SBDATO4, output SBDATO3, output SBDATO2, output SBDATO1, output SBDATO0, output SBACKO, output I2CIRQ, output I2CWKUP,
               output SCLO, output SCLOE, output SDAO, output SDAOE);
    reg [7:0] q;
    always @(posedge SBCLKI) q <= ^{SBRWI, SBSTBI, {SBADRI7, SBADRI6, SBADRI5, SBADRI4, SBADRI3, SBADRI2, SBADRI1, SBADRI0}, {SBDATI7, SBDATI6, SBDATI5, SBDATI4, SBDATI3, SBDATI2, SBDATI1, SBDATI0}, SCLI, SDAI} ? q + 8'd1 : q;
    assign {SBDATO7, SBDATO6, SBDATO5, SBDATO4, SBDATO3, SBDATO2, SBDATO1, SBDATO0} = q;
    assign {SBACKO, I2CIRQ, I2CWKUP, SCLO, SCLOE, SDAO, SDAOE} = {7{q[0]}};
endmodule

// iCE40 UltraPlus SB_LEDDA_IP: the LED PWM controller, registers and outputs on LEDDCLK
module SB_LEDDA_IP (input LEDDCS, input LEDDCLK, input LEDDDAT7, input LEDDDAT6, input LEDDDAT5, input LEDDDAT4, input LEDDDAT3, input LEDDDAT2, input LEDDDAT1, input LEDDDAT0, input LEDDADDR3, input LEDDADDR2, input LEDDADDR1, input LEDDADDR0,
                    input LEDDDEN, input LEDDEXE, input LEDDRST, output PWMOUT0, output PWMOUT1, output PWMOUT2,
                    output LEDDON);
    reg [3:0] q;
    always @(posedge LEDDCLK) q <= ^{LEDDCS, {LEDDDAT7, LEDDDAT6, LEDDDAT5, LEDDDAT4, LEDDDAT3, LEDDDAT2, LEDDDAT1, LEDDDAT0}, {LEDDADDR3, LEDDADDR2, LEDDADDR1, LEDDADDR0}, LEDDDEN, LEDDEXE, LEDDRST} ? q + 4'd1 : q;
    assign {LEDDON, PWMOUT2, PWMOUT1, PWMOUT0} = q;
endmodule

// iCE40 UltraPlus SB_RGBA_DRV: the RGB current drivers (combinational)
module SB_RGBA_DRV #(parameter CURRENT_MODE = "0b0", parameter RGB0_CURRENT = "0b000000",
                     parameter RGB1_CURRENT = "0b000000", parameter RGB2_CURRENT = "0b000000")
                    (input CURREN, input RGBLEDEN, input RGB0PWM, input RGB1PWM, input RGB2PWM, output RGB0,
                     output RGB1, output RGB2);
    assign RGB0 = CURREN & RGBLEDEN & RGB0PWM;
    assign RGB1 = CURREN & RGBLEDEN & RGB1PWM;
    assign RGB2 = CURREN & RGBLEDEN & RGB2PWM;
endmodule

// iCE40 SB_WARMBOOT: reconfiguration request, inputs only
module SB_WARMBOOT (input BOOT, input S1, input S0);
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

// Generic Gowin names (CLKDIV, OSER10, ...) are modelled in gowin.v, used only
// in Gowin projects: elsewhere they could hide a missing user module.

// ---- internal oscillators: clocks of their own, with their nominal frequency ----

// MachXO2/XO3 OSCH: NOM_FREQ in MHz ("2.08" .. "133.00")
module OSCH #(parameter NOM_FREQ = "2.08") (input STDBY, output OSC, output SEDSTDBY);
    (* OPENCDC_CLOCK_PIN = "OSC", OPENCDC_CLOCK_FREQ = NOM_FREQ *) wire osc;
    opencdc_async_source u_osc (osc);
    assign OSC = osc;
    assign SEDSTDBY = 1'b0;
endmodule

// ECP5 OSCG: 310 MHz / DIV (DIV 2 .. 128)
module OSCG #(parameter integer DIV = 128) (output OSC);
    localparam real MHZ = 310.0 / DIV;
    (* OPENCDC_CLOCK_PIN = "OSC", OPENCDC_CLOCK_FREQ = MHZ *) wire osc;
    opencdc_async_source u_osc (osc);
    assign OSC = osc;
endmodule

// iCE40 UltraPlus SB_HFOSC: 48 MHz / 1, 2, 4, 8 (CLKHF_DIV "0b00" .. "0b11")
module SB_HFOSC #(parameter CLKHF_DIV = "0b00", parameter TRIM_EN = "0b0")
                 (input CLKHFPU, input CLKHFEN, input [9:0] TRIM, output CLKHF);
    localparam real MHZ = CLKHF_DIV == "0b01" ? 24.0 : CLKHF_DIV == "0b10" ? 12.0 : CLKHF_DIV == "0b11" ? 6.0 : 48.0;
    (* OPENCDC_CLOCK_PIN = "CLKHF", OPENCDC_CLOCK_FREQ = MHZ *) wire osc;
    opencdc_async_source u_osc (osc);
    assign CLKHF = osc;
endmodule

// iCE40 UltraPlus SB_LFOSC: 10 kHz
module SB_LFOSC (input CLKLFPU, input CLKLFEN, output CLKLF);
    (* OPENCDC_CLOCK_PIN = "CLKLF", OPENCDC_CLOCK_FREQ = 0.01 *) wire osc;
    opencdc_async_source u_osc (osc);
    assign CLKLF = osc;
endmodule

// ---- Lattice constants and configuration ports ---------------------------

// VHI / VLO: constant 1 / 0 (the "scuba" constants of IPexpress and Clarity modules)
module VHI (output Z);
    assign Z = 1'b1;
endmodule

module VLO (output Z);
    assign Z = 1'b0;
endmodule

// USRMCLK (ECP5): drives the configuration SPI clock pin (MCLK) from logic; no outputs
module USRMCLK (input USRMCLKI, input USRMCLKTS);
endmodule

// ---- Lattice ECP5 DDR and block RAM -------------------------------------

module IDDRX1F (input D, input SCLK, input RST, output reg Q0, output reg Q1);
    reg neg;
    always @(negedge SCLK or posedge RST) if (RST) neg <= 1'b0; else neg <= D;
    always @(posedge SCLK or posedge RST)
        if (RST) begin Q0 <= 1'b0; Q1 <= 1'b0; end
        else begin Q0 <= D; Q1 <= neg; end
endmodule

module ODDRX1F (input D0, input D1, input SCLK, input RST, output Q);
    reg r0, r1;
    always @(posedge SCLK or posedge RST)
        if (RST) begin r0 <= 1'b0; r1 <= 1'b0; end
        else begin r0 <= D0; r1 <= D1; end
    assign Q = SCLK ? r0 : r1;
endmodule

// MachXO2/XO3 true dual-port 9 kbit RAM (EBR): port A on CLKA, port B on CLKB, per-port write
// arrays as for DP16KD; the word address is the upper address bits (ADx12..ADx3).
module DP8KC #(parameter DATA_WIDTH_A = 9, parameter DATA_WIDTH_B = 9, parameter REGMODE_A = "NOREG",
              parameter REGMODE_B = "NOREG", parameter CSDECODE_A = "0b000", parameter CSDECODE_B = "0b000",
              parameter WRITEMODE_A = "NORMAL", parameter WRITEMODE_B = "NORMAL", parameter GSR = "ENABLED",
              parameter RESETMODE = "SYNC", parameter ASYNC_RESET_RELEASE = "SYNC", parameter INIT_DATA = "STATIC",
    parameter INITVAL_00 = "0x0", parameter INITVAL_01 = "0x0", parameter INITVAL_02 = "0x0", parameter INITVAL_03 = "0x0", parameter INITVAL_04 = "0x0", parameter INITVAL_05 = "0x0", parameter INITVAL_06 = "0x0", parameter INITVAL_07 = "0x0", parameter INITVAL_08 = "0x0", parameter INITVAL_09 = "0x0", parameter INITVAL_0A = "0x0", parameter INITVAL_0B = "0x0", parameter INITVAL_0C = "0x0", parameter INITVAL_0D = "0x0", parameter INITVAL_0E = "0x0", parameter INITVAL_0F = "0x0", parameter INITVAL_10 = "0x0", parameter INITVAL_11 = "0x0", parameter INITVAL_12 = "0x0", parameter INITVAL_13 = "0x0", parameter INITVAL_14 = "0x0", parameter INITVAL_15 = "0x0", parameter INITVAL_16 = "0x0", parameter INITVAL_17 = "0x0", parameter INITVAL_18 = "0x0", parameter INITVAL_19 = "0x0", parameter INITVAL_1A = "0x0", parameter INITVAL_1B = "0x0", parameter INITVAL_1C = "0x0", parameter INITVAL_1D = "0x0", parameter INITVAL_1E = "0x0", parameter INITVAL_1F = "0x0")
             (input DIA0, input DIA1, input DIA2, input DIA3, input DIA4, input DIA5, input DIA6, input DIA7, input DIA8, input ADA0, input ADA1, input ADA2, input ADA3, input ADA4, input ADA5, input ADA6, input ADA7, input ADA8, input ADA9, input ADA10, input ADA11, input ADA12,
              input CEA, input OCEA, input CLKA, input WEA, input CSA0, input CSA1, input CSA2, input RSTA,
              input DIB0, input DIB1, input DIB2, input DIB3, input DIB4, input DIB5, input DIB6, input DIB7, input DIB8, input ADB0, input ADB1, input ADB2, input ADB3, input ADB4, input ADB5, input ADB6, input ADB7, input ADB8, input ADB9, input ADB10, input ADB11, input ADB12,
              input CEB, input OCEB, input CLKB, input WEB, input CSB0, input CSB1, input CSB2, input RSTB,
              output DOA0, output DOA1, output DOA2, output DOA3, output DOA4, output DOA5, output DOA6, output DOA7, output DOA8, output DOB0, output DOB1, output DOB2, output DOB3, output DOB4, output DOB5, output DOB6, output DOB7, output DOB8);
    wire [8:0] dia = {DIA8, DIA7, DIA6, DIA5, DIA4, DIA3, DIA2, DIA1, DIA0};
    wire [8:0] dib = {DIB8, DIB7, DIB6, DIB5, DIB4, DIB3, DIB2, DIB1, DIB0};
    wire [9:0] ada = {ADA12, ADA11, ADA10, ADA9, ADA8, ADA7, ADA6, ADA5, ADA4, ADA3};
    wire [9:0] adb = {ADB12, ADB11, ADB10, ADB9, ADB8, ADB7, ADB6, ADB5, ADB4, ADB3};
    reg [8:0] mem_a [0:1023];
    reg [8:0] mem_b [0:1023];
    reg [8:0] doa, dob;
    always @(posedge CLKA) if (CEA && WEA) mem_a[ada] <= dia;
    always @(posedge CLKB) if (CEB && WEB) mem_b[adb] <= dib;
    always @(posedge CLKA) if (CEA) doa <= mem_a[ada] ^ mem_b[ada];
    always @(posedge CLKB) if (CEB) dob <= mem_a[adb] ^ mem_b[adb];
    assign {DOA8, DOA7, DOA6, DOA5, DOA4, DOA3, DOA2, DOA1, DOA0} = doa;
    assign {DOB8, DOB7, DOB6, DOB5, DOB4, DOB3, DOB2, DOB1, DOB0} = dob;
endmodule

// MachXO2/XO3 pseudo dual-port 9 kbit RAM: write port on CLKW, read port on CLKR.
module PDPW8KC #(parameter DATA_WIDTH_W = 18, parameter DATA_WIDTH_R = 9, parameter REGMODE = "NOREG",
                 parameter CSDECODE_W = "0b000", parameter CSDECODE_R = "0b000", parameter GSR = "ENABLED",
                 parameter RESETMODE = "SYNC", parameter ASYNC_RESET_RELEASE = "SYNC", parameter INIT_DATA = "STATIC",
    parameter INITVAL_00 = "0x0", parameter INITVAL_01 = "0x0", parameter INITVAL_02 = "0x0", parameter INITVAL_03 = "0x0", parameter INITVAL_04 = "0x0", parameter INITVAL_05 = "0x0", parameter INITVAL_06 = "0x0", parameter INITVAL_07 = "0x0", parameter INITVAL_08 = "0x0", parameter INITVAL_09 = "0x0", parameter INITVAL_0A = "0x0", parameter INITVAL_0B = "0x0", parameter INITVAL_0C = "0x0", parameter INITVAL_0D = "0x0", parameter INITVAL_0E = "0x0", parameter INITVAL_0F = "0x0", parameter INITVAL_10 = "0x0", parameter INITVAL_11 = "0x0", parameter INITVAL_12 = "0x0", parameter INITVAL_13 = "0x0", parameter INITVAL_14 = "0x0", parameter INITVAL_15 = "0x0", parameter INITVAL_16 = "0x0", parameter INITVAL_17 = "0x0", parameter INITVAL_18 = "0x0", parameter INITVAL_19 = "0x0", parameter INITVAL_1A = "0x0", parameter INITVAL_1B = "0x0", parameter INITVAL_1C = "0x0", parameter INITVAL_1D = "0x0", parameter INITVAL_1E = "0x0", parameter INITVAL_1F = "0x0")
               (input DI0, input DI1, input DI2, input DI3, input DI4, input DI5, input DI6, input DI7, input DI8, input DI9, input DI10, input DI11, input DI12, input DI13, input DI14, input DI15, input DI16, input DI17, input ADW0, input ADW1, input ADW2, input ADW3, input ADW4, input ADW5, input ADW6, input ADW7, input ADW8, input BE0, input BE1,
                input CEW, input CLKW, input CSW0, input CSW1, input CSW2,
                input ADR0, input ADR1, input ADR2, input ADR3, input ADR4, input ADR5, input ADR6, input ADR7, input ADR8, input ADR9, input ADR10, input ADR11, input ADR12, input CER, input OCER, input CLKR, input CSR0, input CSR1, input CSR2,
                input RST, output DO0, output DO1, output DO2, output DO3, output DO4, output DO5, output DO6, output DO7, output DO8, output DO9, output DO10, output DO11, output DO12, output DO13, output DO14, output DO15, output DO16, output DO17);
    wire [17:0] di = {DI17, DI16, DI15, DI14, DI13, DI12, DI11, DI10, DI9, DI8, DI7, DI6, DI5, DI4, DI3, DI2, DI1, DI0};
    wire [8:0] adw = {ADW8, ADW7, ADW6, ADW5, ADW4, ADW3, ADW2, ADW1, ADW0};
    wire [8:0] adr = {ADR12, ADR11, ADR10, ADR9, ADR8, ADR7, ADR6, ADR5, ADR4};
    reg [17:0] mem [0:511];
    reg [17:0] q;
    always @(posedge CLKW) if (CEW) mem[adw] <= di;
    always @(posedge CLKR) if (CER) q <= mem[adr];
    assign {DO17, DO16, DO15, DO14, DO13, DO12, DO11, DO10, DO9, DO8, DO7, DO6, DO5, DO4, DO3, DO2, DO1, DO0} = q;
endmodule

// MachXO2/XO3 single-port 9 kbit RAM on CLK.
module SP8KC #(parameter DATA_WIDTH = 9, parameter REGMODE = "NOREG", parameter CSDECODE = "0b000",
               parameter WRITEMODE = "NORMAL", parameter GSR = "ENABLED", parameter RESETMODE = "SYNC",
               parameter ASYNC_RESET_RELEASE = "SYNC", parameter INIT_DATA = "STATIC",
    parameter INITVAL_00 = "0x0", parameter INITVAL_01 = "0x0", parameter INITVAL_02 = "0x0", parameter INITVAL_03 = "0x0", parameter INITVAL_04 = "0x0", parameter INITVAL_05 = "0x0", parameter INITVAL_06 = "0x0", parameter INITVAL_07 = "0x0", parameter INITVAL_08 = "0x0", parameter INITVAL_09 = "0x0", parameter INITVAL_0A = "0x0", parameter INITVAL_0B = "0x0", parameter INITVAL_0C = "0x0", parameter INITVAL_0D = "0x0", parameter INITVAL_0E = "0x0", parameter INITVAL_0F = "0x0", parameter INITVAL_10 = "0x0", parameter INITVAL_11 = "0x0", parameter INITVAL_12 = "0x0", parameter INITVAL_13 = "0x0", parameter INITVAL_14 = "0x0", parameter INITVAL_15 = "0x0", parameter INITVAL_16 = "0x0", parameter INITVAL_17 = "0x0", parameter INITVAL_18 = "0x0", parameter INITVAL_19 = "0x0", parameter INITVAL_1A = "0x0", parameter INITVAL_1B = "0x0", parameter INITVAL_1C = "0x0", parameter INITVAL_1D = "0x0", parameter INITVAL_1E = "0x0", parameter INITVAL_1F = "0x0")
             (input DI0, input DI1, input DI2, input DI3, input DI4, input DI5, input DI6, input DI7, input DI8, input AD0, input AD1, input AD2, input AD3, input AD4, input AD5, input AD6, input AD7, input AD8, input AD9, input AD10, input AD11, input AD12, input CE, input OCE, input CLK, input WE, input CS0, input CS1,
              input CS2, input RST, output DO0, output DO1, output DO2, output DO3, output DO4, output DO5, output DO6, output DO7, output DO8);
    wire [8:0] di = {DI8, DI7, DI6, DI5, DI4, DI3, DI2, DI1, DI0};
    wire [9:0] ad = {AD12, AD11, AD10, AD9, AD8, AD7, AD6, AD5, AD4, AD3};
    reg [8:0] mem [0:1023];
    reg [8:0] q;
    always @(posedge CLK) if (CE && WE) mem[ad] <= di;
    always @(posedge CLK) if (CE) q <= mem[ad];
    assign {DO8, DO7, DO6, DO5, DO4, DO3, DO2, DO1, DO0} = q;
endmodule

// True dual-port 16 kbit RAM: port A on CLKA, port B on CLKB. Each port has
// its own write array; a read sees both (the crossing goes through the RAM).
module DP16KD #(parameter DATA_WIDTH_A = 18, parameter DATA_WIDTH_B = 18,
                parameter REGMODE_A = "NOREG", parameter REGMODE_B = "NOREG",
                parameter WRITEMODE_A = "NORMAL", parameter WRITEMODE_B = "NORMAL",
                parameter RESETMODE = "SYNC", parameter ASYNC_RESET_RELEASE = "SYNC",
                parameter CSDECODE_A = "0b000", parameter CSDECODE_B = "0b000", parameter GSR = "ENABLED",
                parameter INITVAL_DATA = "NONE",
    parameter INITVAL_00 = "0x0", parameter INITVAL_01 = "0x0", parameter INITVAL_02 = "0x0", parameter INITVAL_03 = "0x0", parameter INITVAL_04 = "0x0", parameter INITVAL_05 = "0x0", parameter INITVAL_06 = "0x0", parameter INITVAL_07 = "0x0", parameter INITVAL_08 = "0x0", parameter INITVAL_09 = "0x0", parameter INITVAL_0A = "0x0", parameter INITVAL_0B = "0x0", parameter INITVAL_0C = "0x0", parameter INITVAL_0D = "0x0", parameter INITVAL_0E = "0x0", parameter INITVAL_0F = "0x0", parameter INITVAL_10 = "0x0", parameter INITVAL_11 = "0x0", parameter INITVAL_12 = "0x0", parameter INITVAL_13 = "0x0", parameter INITVAL_14 = "0x0", parameter INITVAL_15 = "0x0", parameter INITVAL_16 = "0x0", parameter INITVAL_17 = "0x0", parameter INITVAL_18 = "0x0", parameter INITVAL_19 = "0x0", parameter INITVAL_1A = "0x0", parameter INITVAL_1B = "0x0", parameter INITVAL_1C = "0x0", parameter INITVAL_1D = "0x0", parameter INITVAL_1E = "0x0", parameter INITVAL_1F = "0x0", parameter INITVAL_20 = "0x0", parameter INITVAL_21 = "0x0", parameter INITVAL_22 = "0x0", parameter INITVAL_23 = "0x0", parameter INITVAL_24 = "0x0", parameter INITVAL_25 = "0x0", parameter INITVAL_26 = "0x0", parameter INITVAL_27 = "0x0", parameter INITVAL_28 = "0x0", parameter INITVAL_29 = "0x0", parameter INITVAL_2A = "0x0", parameter INITVAL_2B = "0x0", parameter INITVAL_2C = "0x0", parameter INITVAL_2D = "0x0", parameter INITVAL_2E = "0x0", parameter INITVAL_2F = "0x0", parameter INITVAL_30 = "0x0", parameter INITVAL_31 = "0x0", parameter INITVAL_32 = "0x0", parameter INITVAL_33 = "0x0", parameter INITVAL_34 = "0x0", parameter INITVAL_35 = "0x0", parameter INITVAL_36 = "0x0", parameter INITVAL_37 = "0x0", parameter INITVAL_38 = "0x0", parameter INITVAL_39 = "0x0", parameter INITVAL_3A = "0x0", parameter INITVAL_3B = "0x0", parameter INITVAL_3C = "0x0", parameter INITVAL_3D = "0x0", parameter INITVAL_3E = "0x0", parameter INITVAL_3F = "0x0")
              (input DIA0, input DIA1, input DIA2, input DIA3, input DIA4, input DIA5, input DIA6, input DIA7, input DIA8, input DIA9, input DIA10, input DIA11, input DIA12, input DIA13, input DIA14, input DIA15, input DIA16, input DIA17, input ADA0, input ADA1, input ADA2, input ADA3, input ADA4, input ADA5, input ADA6, input ADA7, input ADA8, input ADA9, input ADA10, input ADA11, input ADA12, input ADA13,
               input CEA, input OCEA, input CLKA, input WEA, input CSA0, input CSA1, input CSA2, input RSTA,
               input DIB0, input DIB1, input DIB2, input DIB3, input DIB4, input DIB5, input DIB6, input DIB7, input DIB8, input DIB9, input DIB10, input DIB11, input DIB12, input DIB13, input DIB14, input DIB15, input DIB16, input DIB17, input ADB0, input ADB1, input ADB2, input ADB3, input ADB4, input ADB5, input ADB6, input ADB7, input ADB8, input ADB9, input ADB10, input ADB11, input ADB12, input ADB13,
               input CEB, input OCEB, input CLKB, input WEB, input CSB0, input CSB1, input CSB2, input RSTB,
               output DOA0, output DOA1, output DOA2, output DOA3, output DOA4, output DOA5, output DOA6, output DOA7, output DOA8, output DOA9, output DOA10, output DOA11, output DOA12, output DOA13, output DOA14, output DOA15, output DOA16, output DOA17, output DOB0, output DOB1, output DOB2, output DOB3, output DOB4, output DOB5, output DOB6, output DOB7, output DOB8, output DOB9, output DOB10, output DOB11, output DOB12, output DOB13, output DOB14, output DOB15, output DOB16, output DOB17);
    wire [17:0] dia = {DIA17, DIA16, DIA15, DIA14, DIA13, DIA12, DIA11, DIA10, DIA9, DIA8, DIA7, DIA6, DIA5, DIA4, DIA3, DIA2, DIA1, DIA0};
    wire [17:0] dib = {DIB17, DIB16, DIB15, DIB14, DIB13, DIB12, DIB11, DIB10, DIB9, DIB8, DIB7, DIB6, DIB5, DIB4, DIB3, DIB2, DIB1, DIB0};
    wire [9:0] ada = {ADA13, ADA12, ADA11, ADA10, ADA9, ADA8, ADA7, ADA6, ADA5, ADA4};
    wire [9:0] adb = {ADB13, ADB12, ADB11, ADB10, ADB9, ADB8, ADB7, ADB6, ADB5, ADB4};
    reg [17:0] mem_a [0:1023];
    reg [17:0] mem_b [0:1023];
    reg [17:0] doa, dob;
    always @(posedge CLKA) if (CEA && WEA) mem_a[ada] <= dia;
    always @(posedge CLKB) if (CEB && WEB) mem_b[adb] <= dib;
    always @(posedge CLKA) if (CEA) doa <= mem_a[ada] ^ mem_b[ada];
    always @(posedge CLKB) if (CEB) dob <= mem_a[adb] ^ mem_b[adb];
    assign {DOA17, DOA16, DOA15, DOA14, DOA13, DOA12, DOA11, DOA10, DOA9, DOA8, DOA7, DOA6, DOA5, DOA4, DOA3, DOA2, DOA1, DOA0} = doa;
    assign {DOB17, DOB16, DOB15, DOB14, DOB13, DOB12, DOB11, DOB10, DOB9, DOB8, DOB7, DOB6, DOB5, DOB4, DOB3, DOB2, DOB1, DOB0} = dob;
endmodule

// ---- Lattice iCE40 block RAM ----------------------------------------------
// Write port on WCLK, read port on RCLK (N variants: falling edge).

module SB_RAM40_4K #(parameter WRITE_MODE = 0, parameter READ_MODE = 0,
                parameter [255:0] INIT_0 = 256'h0,
                parameter [255:0] INIT_1 = 256'h0,
                parameter [255:0] INIT_2 = 256'h0,
                parameter [255:0] INIT_3 = 256'h0,
                parameter [255:0] INIT_4 = 256'h0,
                parameter [255:0] INIT_5 = 256'h0,
                parameter [255:0] INIT_6 = 256'h0,
                parameter [255:0] INIT_7 = 256'h0,
                parameter [255:0] INIT_8 = 256'h0,
                parameter [255:0] INIT_9 = 256'h0,
                parameter [255:0] INIT_A = 256'h0,
                parameter [255:0] INIT_B = 256'h0,
                parameter [255:0] INIT_C = 256'h0,
                parameter [255:0] INIT_D = 256'h0,
                parameter [255:0] INIT_E = 256'h0,
                parameter [255:0] INIT_F = 256'h0,
                parameter INIT_FILE = "")
               (output [15:0] RDATA, input [10:0] RADDR, input RCLK, input RCLKE, input RE,
                input [10:0] WADDR, input WCLK, input WCLKE, input WE, input [15:0] WDATA, input [15:0] MASK);
    reg [15:0] mem [0:2047];
    reg [15:0] q;
    always @(posedge WCLK) if (WCLKE && WE) mem[WADDR] <= WDATA;
    always @(posedge RCLK) if (RCLKE && RE) q <= mem[RADDR];
    assign RDATA = q;
endmodule

module SB_RAM40_4KNR #(parameter WRITE_MODE = 0, parameter READ_MODE = 0,
                parameter [255:0] INIT_0 = 256'h0,
                parameter [255:0] INIT_1 = 256'h0,
                parameter [255:0] INIT_2 = 256'h0,
                parameter [255:0] INIT_3 = 256'h0,
                parameter [255:0] INIT_4 = 256'h0,
                parameter [255:0] INIT_5 = 256'h0,
                parameter [255:0] INIT_6 = 256'h0,
                parameter [255:0] INIT_7 = 256'h0,
                parameter [255:0] INIT_8 = 256'h0,
                parameter [255:0] INIT_9 = 256'h0,
                parameter [255:0] INIT_A = 256'h0,
                parameter [255:0] INIT_B = 256'h0,
                parameter [255:0] INIT_C = 256'h0,
                parameter [255:0] INIT_D = 256'h0,
                parameter [255:0] INIT_E = 256'h0,
                parameter [255:0] INIT_F = 256'h0,
                parameter INIT_FILE = "")
               (output [15:0] RDATA, input [10:0] RADDR, input RCLKN, input RCLKE, input RE,
                input [10:0] WADDR, input WCLK, input WCLKE, input WE, input [15:0] WDATA, input [15:0] MASK);
    reg [15:0] mem [0:2047];
    reg [15:0] q;
    always @(posedge WCLK) if (WCLKE && WE) mem[WADDR] <= WDATA;
    always @(negedge RCLKN) if (RCLKE && RE) q <= mem[RADDR];
    assign RDATA = q;
endmodule

module SB_RAM40_4KNW #(parameter WRITE_MODE = 0, parameter READ_MODE = 0,
                parameter [255:0] INIT_0 = 256'h0,
                parameter [255:0] INIT_1 = 256'h0,
                parameter [255:0] INIT_2 = 256'h0,
                parameter [255:0] INIT_3 = 256'h0,
                parameter [255:0] INIT_4 = 256'h0,
                parameter [255:0] INIT_5 = 256'h0,
                parameter [255:0] INIT_6 = 256'h0,
                parameter [255:0] INIT_7 = 256'h0,
                parameter [255:0] INIT_8 = 256'h0,
                parameter [255:0] INIT_9 = 256'h0,
                parameter [255:0] INIT_A = 256'h0,
                parameter [255:0] INIT_B = 256'h0,
                parameter [255:0] INIT_C = 256'h0,
                parameter [255:0] INIT_D = 256'h0,
                parameter [255:0] INIT_E = 256'h0,
                parameter [255:0] INIT_F = 256'h0,
                parameter INIT_FILE = "")
               (output [15:0] RDATA, input [10:0] RADDR, input RCLK, input RCLKE, input RE,
                input [10:0] WADDR, input WCLKN, input WCLKE, input WE, input [15:0] WDATA, input [15:0] MASK);
    reg [15:0] mem [0:2047];
    reg [15:0] q;
    always @(negedge WCLKN) if (WCLKE && WE) mem[WADDR] <= WDATA;
    always @(posedge RCLK) if (RCLKE && RE) q <= mem[RADDR];
    assign RDATA = q;
endmodule

module SB_RAM40_4KNRNW #(parameter WRITE_MODE = 0, parameter READ_MODE = 0,
                parameter [255:0] INIT_0 = 256'h0,
                parameter [255:0] INIT_1 = 256'h0,
                parameter [255:0] INIT_2 = 256'h0,
                parameter [255:0] INIT_3 = 256'h0,
                parameter [255:0] INIT_4 = 256'h0,
                parameter [255:0] INIT_5 = 256'h0,
                parameter [255:0] INIT_6 = 256'h0,
                parameter [255:0] INIT_7 = 256'h0,
                parameter [255:0] INIT_8 = 256'h0,
                parameter [255:0] INIT_9 = 256'h0,
                parameter [255:0] INIT_A = 256'h0,
                parameter [255:0] INIT_B = 256'h0,
                parameter [255:0] INIT_C = 256'h0,
                parameter [255:0] INIT_D = 256'h0,
                parameter [255:0] INIT_E = 256'h0,
                parameter [255:0] INIT_F = 256'h0,
                parameter INIT_FILE = "")
               (output [15:0] RDATA, input [10:0] RADDR, input RCLKN, input RCLKE, input RE,
                input [10:0] WADDR, input WCLKN, input WCLKE, input WE, input [15:0] WDATA, input [15:0] MASK);
    reg [15:0] mem [0:2047];
    reg [15:0] q;
    always @(negedge WCLKN) if (WCLKE && WE) mem[WADDR] <= WDATA;
    always @(negedge RCLKN) if (RCLKE && RE) q <= mem[RADDR];
    assign RDATA = q;
endmodule

// ---- Gowin block RAM -----------------------------------------------

// Semi dual-port RAM: write port A on CLKA, read port B on CLKB.
module SDPB #(parameter READ_MODE = 1'b0, parameter BIT_WIDTH_0 = 32, parameter BIT_WIDTH_1 = 32,
              parameter BLK_SEL_0 = 3'b000, parameter BLK_SEL_1 = 3'b000, parameter RESET_MODE = "SYNC")
             (output [31:0] DO, input [31:0] DI, input [2:0] BLKSELA, input [2:0] BLKSELB,
              input [13:0] ADA, input [13:0] ADB, input CLKA, input CLKB, input CEA, input CEB, input OCE,
              input RESETA, input RESETB);
    reg [31:0] mem [0:1023];
    reg [31:0] q;
    always @(posedge CLKA) if (CEA) mem[ADA[13:4]] <= DI;
    always @(posedge CLKB) if (CEB) q <= mem[ADB[13:4]];
    assign DO = q;
endmodule

// True dual-port RAM: port A on CLKA, port B on CLKB.
module DPB #(parameter READ_MODE0 = 1'b0, parameter READ_MODE1 = 1'b0, parameter WRITE_MODE0 = 2'b00,
             parameter WRITE_MODE1 = 2'b00, parameter BIT_WIDTH_0 = 16, parameter BIT_WIDTH_1 = 16,
             parameter BLK_SEL_0 = 3'b000, parameter BLK_SEL_1 = 3'b000, parameter RESET_MODE = "SYNC")
            (output [15:0] DOA, output [15:0] DOB, input [15:0] DIA, input [15:0] DIB, input [2:0] BLKSELA,
             input [2:0] BLKSELB, input [13:0] ADA, input [13:0] ADB, input WREA, input WREB, input CLKA,
             input CLKB, input CEA, input CEB, input OCEA, input OCEB, input RESETA, input RESETB);
    reg [15:0] mem_a [0:1023];
    reg [15:0] mem_b [0:1023];
    reg [15:0] qa, qb;
    always @(posedge CLKA) if (CEA && WREA) mem_a[ADA[13:4]] <= DIA;
    always @(posedge CLKB) if (CEB && WREB) mem_b[ADB[13:4]] <= DIB;
    always @(posedge CLKA) if (CEA) qa <= mem_a[ADA[13:4]] ^ mem_b[ADA[13:4]];
    always @(posedge CLKB) if (CEB) qb <= mem_a[ADB[13:4]] ^ mem_b[ADB[13:4]];
    assign DOA = qa;
    assign DOB = qb;
endmodule

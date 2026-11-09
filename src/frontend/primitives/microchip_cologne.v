// OpenCDC behavioral models of Microchip (PolarFire) and Cologne Chip (GateMate)
// primitives: clock generators (the clock structure only; output periods are
// derived from the divider parameters; LOCK has no clock domain) and clock buffers.

// Microchip PolarFire PLL (the PLL of a PF_CCC, as Libero writes it):
//   f(VCO) = f(REF_CLK_0) x (FB_INT_VAL + FB_FRAC_VAL / 2^24) / RFDIV,  f(OUTn) = f(VCO) / (4 x DIVn_VAL)
// The reference selection, bypass, phase and the DRI port are not modelled.
module PLL #(parameter VCOFREQUENCY = 0.0, parameter DELAY_LINE_SIMULATION_MODE = "", parameter DATA_RATE = 0.0,
             parameter FORMAL_NAME = "", parameter INTERFACE_NAME = "", parameter [2:0] INTERFACE_LEVEL = 3'b0,
             parameter SOFTRESET = 1'b0, parameter SOFT_POWERDOWN_N = 1'b1, parameter RFDIV_EN = 1'b1,
             parameter OUT0_DIV_EN = 1'b1, parameter OUT1_DIV_EN = 1'b1, parameter OUT2_DIV_EN = 1'b1,
             parameter OUT3_DIV_EN = 1'b1, parameter SOFT_REF_CLK_SEL = 1'b0, parameter RESET_ON_LOCK = 1'b1,
             parameter [3:0] BYPASS_CLK_SEL = 4'b0, parameter BYPASS_GO_EN_N = 1'b1, parameter [3:0] BYPASS_PLL = 4'b0,
             parameter [3:0] BYPASS_OUT_DIVIDER = 4'b0, parameter FF_REQUIRES_LOCK = 1'b0, parameter FSE_N = 1'b0,
             parameter [1:0] FB_CLK_SEL_0 = 2'b0, parameter FB_CLK_SEL_1 = 1'b0, parameter [5:0] RFDIV = 6'd1,
             parameter FRAC_EN = 1'b0, parameter FRAC_DAC_EN = 1'b0,
             parameter [2:0] DIV0_RST_DELAY = 3'b0, parameter [6:0] DIV0_VAL = 7'd1,
             parameter [2:0] DIV1_RST_DELAY = 3'b0, parameter [6:0] DIV1_VAL = 7'd1,
             parameter [2:0] DIV2_RST_DELAY = 3'b0, parameter [6:0] DIV2_VAL = 7'd1,
             parameter [2:0] DIV3_RST_DELAY = 3'b0, parameter [6:0] DIV3_VAL = 7'd1, parameter DIV3_CLK_SEL = 1'b0,
             parameter [1:0] BW_INT_CTRL = 2'b0, parameter [1:0] BW_PROP_CTRL = 2'b01, parameter IREF_EN = 1'b1,
             parameter IREF_TOGGLE = 1'b0, parameter [3:0] LOCK_CNT = 4'b1000, parameter [2:0] DESKEW_CAL_CNT = 3'b110,
             parameter DESKEW_CAL_EN = 1'b1, parameter DESKEW_CAL_BYPASS = 1'b0, parameter SYNC_REF_DIV_EN = 1'b0,
             parameter SYNC_REF_DIV_EN_2 = 1'b0, parameter [2:0] OUT0_PHASE_SEL = 3'b0,
             parameter [2:0] OUT1_PHASE_SEL = 3'b0, parameter [2:0] OUT2_PHASE_SEL = 3'b0,
             parameter [2:0] OUT3_PHASE_SEL = 3'b0, parameter SOFT_LOAD_PHASE_N = 1'b1,
             parameter [5:0] SSM_DIV_VAL = 6'd1, parameter [23:0] FB_FRAC_VAL = 24'd0, parameter SSM_SPREAD_MODE = 1'b0,
             parameter [4:0] SSM_MODULATION = 5'b00101, parameter [11:0] FB_INT_VAL = 12'd1, parameter SSM_EN_N = 1'b1,
             parameter [1:0] SSM_EXT_WAVE_EN = 2'b0, parameter [7:0] SSM_EXT_WAVE_MAX_ADDR = 8'b0,
             parameter SSM_RANDOM_EN = 1'b0, parameter [2:0] SSM_RANDOM_PATTERN_SEL = 3'b0,
             parameter [1:0] CDMUX0_SEL = 2'b0, parameter CDMUX1_SEL = 1'b1, parameter CDMUX2_SEL = 1'b0,
             parameter [7:0] CDELAY0_SEL = 8'b0, parameter CDELAY0_EN = 1'b0, parameter DRI_EN = 1'b1)
            (input POWERDOWN_N, input OUT0_EN, input OUT1_EN, input OUT2_EN, input OUT3_EN, input REF_CLK_SEL,
             input BYPASS_EN_N, input LOAD_PHASE_N, input [7:0] SSCG_WAVE_TABLE, input PHASE_DIRECTION,
             input PHASE_ROTATE, input PHASE_OUT0_SEL, input PHASE_OUT1_SEL, input PHASE_OUT2_SEL,
             input PHASE_OUT3_SEL, input DELAY_LINE_MOVE, input DELAY_LINE_DIRECTION, input DELAY_LINE_WIDE,
             input DELAY_LINE_LOAD, input REFCLK_SYNC_EN, input REF_CLK_0, input REF_CLK_1, input FB_CLK,
             input DRI_CLK, input [10:0] DRI_CTRL, input [32:0] DRI_WDATA, input DRI_ARST_N,
             output LOCK, output [7:0] SSCG_WAVE_TABLE_ADDR, output DELAY_LINE_OUT_OF_RANGE,
             output OUT0, output OUT1, output OUT2, output OUT3, output [32:0] DRI_RDATA, output DRI_INTERRUPT);
    localparam real FB = FB_INT_VAL + FB_FRAC_VAL / 16777216.0;
    localparam real VCO = FB > 0.0 ? (1.0 * RFDIV) / FB : 0.0; // period ratio of the VCO
    (* OPENCDC_CLOCK_PIN = "OUT0", OPENCDC_CLOCK_RATIO = VCO * 4 * DIV0_VAL *) reg o0 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "OUT1", OPENCDC_CLOCK_RATIO = VCO * 4 * DIV1_VAL *) reg o1 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "OUT2", OPENCDC_CLOCK_RATIO = VCO * 4 * DIV2_VAL *) reg o2 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "OUT3", OPENCDC_CLOCK_RATIO = VCO * 4 * DIV3_VAL *) reg o3 = 1'b0;
    always @(posedge REF_CLK_0) begin o0 <= ~o0; o1 <= ~o1; o2 <= ~o2; o3 <= ~o3; end
    assign OUT0 = o0;
    assign OUT1 = o1;
    assign OUT2 = o2;
    assign OUT3 = o3;
    opencdc_async_source u_lock (LOCK);
    assign SSCG_WAVE_TABLE_ADDR = 8'd0;
    assign DELAY_LINE_OUT_OF_RANGE = 1'b0;
    reg [32:0] rdata = 33'd0;
    always @(posedge DRI_CLK) rdata <= DRI_WDATA; // the DRI port is in the DRI_CLK domain
    assign DRI_RDATA = rdata;
    assign DRI_INTERRUPT = 1'b0;
endmodule

// Microchip global clock buffers
module CLKINT (input A, output Y);
    assign Y = A;
endmodule

module CLKINT_PRESERVE (input A, output Y);
    assign Y = A;
endmodule

module CLKBUF (input PAD, output Y);
    assign Y = PAD;
endmodule

// Cologne Chip GateMate PLL: CLK0/90/180/270 at OUT_CLK (MHz), the input at REF_CLK (MHz);
// CLK_REF_OUT is the reference clock.
module CC_PLL #(parameter REF_CLK = "", parameter OUT_CLK = "", parameter PERF_MD = "", parameter LOCK_REQ = 1,
                parameter CLK270_DOUB = 0, parameter CLK180_DOUB = 0, parameter LOW_JITTER = 1,
                parameter CI_FILTER_CONST = 2, parameter CP_FILTER_CONST = 4)
               (input CLK_REF, input USR_CLK_REF, input CLK_FEEDBACK, input USR_LOCKED_STDY_RST,
                input USR_SEL_A_B, output USR_PLL_LOCKED_STDY, output USR_PLL_LOCKED, output CLK0, output CLK90,
                output CLK180, output CLK270, output CLK_REF_OUT);
    (* OPENCDC_CLOCK_PIN = "CLK0", OPENCDC_CLOCK_FREQ_IN = REF_CLK, OPENCDC_CLOCK_FREQ_OUT = OUT_CLK *) reg c0 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLK90", OPENCDC_CLOCK_FREQ_IN = REF_CLK, OPENCDC_CLOCK_FREQ_OUT = OUT_CLK *) reg c90 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLK180", OPENCDC_CLOCK_FREQ_IN = REF_CLK, OPENCDC_CLOCK_FREQ_OUT = OUT_CLK *) reg c180 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "CLK270", OPENCDC_CLOCK_FREQ_IN = REF_CLK, OPENCDC_CLOCK_FREQ_OUT = OUT_CLK *) reg c270 = 1'b0;
    always @(posedge CLK_REF) begin c0 <= ~c0; c90 <= ~c90; c180 <= ~c180; c270 <= ~c270; end
    assign CLK0 = c0;
    assign CLK90 = c90;
    assign CLK180 = c180;
    assign CLK270 = c270;
    assign CLK_REF_OUT = CLK_REF;
    opencdc_async_source u_lock (USR_PLL_LOCKED);
    assign USR_PLL_LOCKED_STDY = USR_PLL_LOCKED;
endmodule

// GateMate user reset (from the configuration logic): no clock domain
module CC_USR_RSTN (output USR_RSTN);
    opencdc_async_source u_rstn (USR_RSTN);
endmodule

module CC_BUFG (input I, output O);
    assign O = I;
endmodule

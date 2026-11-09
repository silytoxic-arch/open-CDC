// OpenCDC: models of Gowin cells whose names are generic (CLKDIV, OSER10, ...).
// They are used only in Gowin projects (a .gprj, or the project option
// "gowinPrimitives"): elsewhere a module of such a name that the design does
// not define stays unknown, as a missing user module must.
// The register structure follows the Gowin primitive user guide (UG289).

// CLKDIV: HCLKIN divided by DIV_MODE ("2", "3.5", "4", "5", "8"); a clock
// derived from HCLKIN.
module CLKDIV #(parameter DIV_MODE = "2", parameter GSREN = "false")
               (output CLKOUT, input HCLKIN, input RESETN, input CALIB);
    localparam real DIV = DIV_MODE == "3.5" ? 3.5 : DIV_MODE == "4" ? 4.0 : DIV_MODE == "5" ? 5.0 :
                          DIV_MODE == "8" ? 8.0 : 2.0;
    (* OPENCDC_CLOCK_PIN = "CLKOUT", OPENCDC_CLOCK_RATIO = DIV *) reg o = 1'b0;
    always @(posedge HCLKIN) o <= ~o;
    assign CLKOUT = o;
endmodule

// CLKDIV2: HCLKIN divided by 2.
module CLKDIV2 #(parameter GSREN = "false") (output CLKOUT, input HCLKIN, input RESETN);
    (* OPENCDC_CLOCK_PIN = "CLKOUT", OPENCDC_CLOCK_RATIO = 2.0 *) reg o = 1'b0;
    always @(posedge HCLKIN) o <= ~o;
    assign CLKOUT = o;
endmodule

// DHCEN: a clock enable on a high-speed clock (the clock passes through).
module DHCEN (output CLKOUT, input CLKIN, input CE);
    assign CLKOUT = CLKIN;
endmodule

// Serializers: the parallel word is loaded on PCLK and shifted out on FCLK
// (PCLK is FCLK divided, from the same clock source: a CLKDIV).
module OSER4 #(parameter GSREN = "false", parameter LSREN = "true", parameter HWL = "false",
               parameter TXCLK_POL = 1'b0)
              (output Q0, output Q1, input D0, input D1, input D2, input D3, input TX0, input TX1, input PCLK,
               input FCLK, input RESET);
    reg [3:0] word, sh;
    reg [1:0] tx;
    always @(posedge PCLK or posedge RESET)
        if (RESET) begin word <= 4'd0; tx <= 2'd0; end
        else begin word <= {D3, D2, D1, D0}; tx <= {TX1, TX0}; end
    always @(posedge FCLK) sh <= {1'b0, sh[3:1]} | word;
    assign Q0 = sh[0];
    assign Q1 = |tx;
endmodule

module OSER8 #(parameter GSREN = "false", parameter LSREN = "true", parameter HWL = "false",
               parameter TXCLK_POL = 1'b0)
              (output Q0, output Q1, input D0, input D1, input D2, input D3, input D4, input D5, input D6, input D7,
               input TX0, input TX1, input TX2, input TX3, input PCLK, input FCLK, input RESET);
    reg [7:0] word, sh;
    reg [3:0] tx;
    always @(posedge PCLK or posedge RESET)
        if (RESET) begin word <= 8'd0; tx <= 4'd0; end
        else begin word <= {D7, D6, D5, D4, D3, D2, D1, D0}; tx <= {TX3, TX2, TX1, TX0}; end
    always @(posedge FCLK) sh <= {1'b0, sh[7:1]} | word;
    assign Q0 = sh[0];
    assign Q1 = |tx;
endmodule

module OSER10 #(parameter GSREN = "false", parameter LSREN = "true")
               (output Q, input D0, input D1, input D2, input D3, input D4, input D5, input D6, input D7, input D8,
                input D9, input PCLK, input FCLK, input RESET);
    reg [9:0] word, sh;
    always @(posedge PCLK or posedge RESET)
        if (RESET) word <= 10'd0;
        else word <= {D9, D8, D7, D6, D5, D4, D3, D2, D1, D0};
    always @(posedge FCLK) sh <= {1'b0, sh[9:1]} | word;
    assign Q = sh[0];
endmodule

// Deserializers: shifted in on FCLK, the word is output on PCLK.
module IDES4 #(parameter GSREN = "false", parameter LSREN = "true")
              (output Q0, output Q1, output Q2, output Q3, input D, input FCLK, input PCLK, input CALIB,
               input RESET);
    reg [3:0] sh, word;
    always @(posedge FCLK) sh <= {D, sh[3:1]};
    always @(posedge PCLK or posedge RESET)
        if (RESET) word <= 4'd0;
        else word <= sh;
    assign {Q3, Q2, Q1, Q0} = word;
endmodule

module IDES8 #(parameter GSREN = "false", parameter LSREN = "true")
              (output Q0, output Q1, output Q2, output Q3, output Q4, output Q5, output Q6, output Q7, input D,
               input FCLK, input PCLK, input CALIB, input RESET);
    reg [7:0] sh, word;
    always @(posedge FCLK) sh <= {D, sh[7:1]};
    always @(posedge PCLK or posedge RESET)
        if (RESET) word <= 8'd0;
        else word <= sh;
    assign {Q7, Q6, Q5, Q4, Q3, Q2, Q1, Q0} = word;
endmodule

module IDES10 #(parameter GSREN = "false", parameter LSREN = "true")
               (output Q0, output Q1, output Q2, output Q3, output Q4, output Q5, output Q6, output Q7, output Q8,
                output Q9, input D, input FCLK, input PCLK, input CALIB, input RESET);
    reg [9:0] sh, word;
    always @(posedge FCLK) sh <= {D, sh[9:1]};
    always @(posedge PCLK or posedge RESET)
        if (RESET) word <= 10'd0;
        else word <= sh;
    assign {Q9, Q8, Q7, Q6, Q5, Q4, Q3, Q2, Q1, Q0} = word;
endmodule

// Differential I/O buffers (emulated and true LVDS).
module ELVDS_OBUF (output O, output OB, input I);
    assign O = I;
    assign OB = ~I;
endmodule

module TLVDS_OBUF (output O, output OB, input I);
    assign O = I;
    assign OB = ~I;
endmodule

module ELVDS_IBUF (output O, input I, input IB);
    assign O = I;
endmodule

module TLVDS_IBUF (output O, input I, input IB);
    assign O = I;
endmodule

// OSC: the internal oscillator, 250 MHz / FREQ_DIV (GW1N-1/GW1NZ-1: 240 MHz)
module OSC #(parameter integer FREQ_DIV = 100, parameter DEVICE = "GW1N-4") (output OSCOUT);
    localparam real MHZ = (DEVICE == "GW1N-1" || DEVICE == "GW1NZ-1" ? 240.0 : 250.0) / FREQ_DIV;
    (* OPENCDC_CLOCK_PIN = "OSCOUT", OPENCDC_CLOCK_FREQ = MHZ *) wire osc;
    opencdc_async_source u_osc (osc);
    assign OSCOUT = osc;
endmodule

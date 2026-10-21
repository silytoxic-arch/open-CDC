// Vendor primitives example: an MMCM derives two clocks from clk_in; data
// crosses from clk_in's domain family to an unrelated clk_ext through an
// XPM Gray-code synchronizer, a block RAM written in one domain and read in another,
// and one unsynchronized register (reported by CDC-002).
module top (
    input  wire        clk_in,
    input  wire        clk_ext,
    input  wire        rst,
    input  wire [7:0]  din,
    output wire [7:0]  dout_sync,
    output wire [31:0] ram_q,
    output reg         flag_ext
);
    wire clk_fb, c0_u, c1_u, clk_fast, clk_slow, locked;
    MMCME2_ADV #(.CLKIN1_PERIOD(10.0), .CLKFBOUT_MULT_F(10.0), .DIVCLK_DIVIDE(1),
                 .CLKOUT0_DIVIDE_F(5.0), .CLKOUT1_DIVIDE(20), .BANDWIDTH("OPTIMIZED"))
        u_mmcm (.CLKIN1(clk_in), .CLKFBIN(clk_fb), .CLKFBOUT(clk_fb), .CLKOUT0(c0_u), .CLKOUT1(c1_u),
                .LOCKED(locked), .RST(rst), .PWRDWN(1'b0), .CLKIN2(1'b0), .CLKINSEL(1'b1));
    BUFG u_bufg0 (.I(c0_u), .O(clk_fast));
    BUFG u_bufg1 (.I(c1_u), .O(clk_slow));

    // reset synchronizer released by LOCKED (no clock domain)
    (* ASYNC_REG = "TRUE" *) reg [1:0] rst_fast_n;
    always @(posedge clk_fast or negedge locked)
        if (!locked) rst_fast_n <= 2'b00;
        else         rst_fast_n <= {rst_fast_n[0], 1'b1};

    reg [7:0] data_fast;
    reg       flag_fast;
    always @(posedge clk_fast or negedge rst_fast_n[1])
        if (!rst_fast_n[1]) begin data_fast <= 8'd0; flag_fast <= 1'b0; end
        else begin data_fast <= din; flag_fast <= ^din; end

    // clk_fast -> clk_slow: same MMCM, synchronous, not a crossing
    reg [7:0] data_slow, cnt_slow;
    always @(posedge clk_slow) begin
        data_slow <= data_fast;
        cnt_slow  <= cnt_slow + {7'd0, data_slow[0]};
    end

    // clk_slow -> clk_ext: a counter through the XPM Gray synchronizer
    xpm_cdc_gray #(.DEST_SYNC_FF(2), .WIDTH(8))
        u_sync (.src_clk(clk_slow), .src_in_bin(cnt_slow), .dest_clk(clk_ext), .dest_out_bin(dout_sync));

    // block RAM: written on clk_fast (port B), read on clk_ext (port A)
    reg [9:0] waddr, raddr;
    always @(posedge clk_fast) waddr <= waddr + 10'd1;
    always @(posedge clk_ext)  raddr <= raddr + 10'd1;
    RAMB36E1 #(.READ_WIDTH_A(36), .WRITE_WIDTH_B(36), .INIT_00(256'h0))
        u_ram (.CLKARDCLK(clk_ext), .ENARDEN(1'b1), .ADDRARDADDR({1'b0, raddr, 5'd0}), .DOADO(ram_q), .WEA(4'd0),
               .CLKBWRCLK(clk_fast), .ENBWREN(1'b1), .ADDRBWRADDR({1'b0, waddr, 5'd0}),
               .DIBDI({24'd0, data_fast}), .WEBWE(8'hff), .DIADI(32'd0), .DIPADIP(4'd0), .DIPBDIP(4'd0),
               .REGCEAREGCE(1'b1), .REGCEB(1'b1), .RSTRAMARSTRAM(1'b0), .RSTRAMB(1'b0),
               .RSTREGARSTREG(1'b0), .RSTREGB(1'b0), .CASCADEINA(1'b0), .CASCADEINB(1'b0),
               .INJECTDBITERR(1'b0), .INJECTSBITERR(1'b0));

    // unsynchronized: one flop in clk_ext
    always @(posedge clk_ext) flag_ext <= flag_fast;
endmodule

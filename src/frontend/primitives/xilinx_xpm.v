// OpenCDC behavioral models of the Xilinx XPM clock-domain-crossing macros
// (UG953). They have the register structure of the real macros, so the
// synchronizer recognition sees what Vivado builds: ASYNC_REG flop chains,
// Gray-coded counters, handshakes, toggle pulses and reset bridges.

module xpm_cdc_single #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT_SYNC_FF = 0,
                        parameter integer SIM_ASSERT_CHK = 0, parameter integer SRC_INPUT_REG = 1)
                       (input src_clk, input src_in, input dest_clk, output dest_out);
    reg src_ff;
    always @(posedge src_clk) src_ff <= src_in;
    wire src_sel = SRC_INPUT_REG ? src_ff : src_in;
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] syncstages_ff;
    always @(posedge dest_clk) syncstages_ff <= {syncstages_ff[DEST_SYNC_FF-2:0], src_sel};
    assign dest_out = syncstages_ff[DEST_SYNC_FF-1];
endmodule

module xpm_cdc_array_single #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT_SYNC_FF = 0,
                              parameter integer SIM_ASSERT_CHK = 0, parameter integer SRC_INPUT_REG = 1,
                              parameter integer WIDTH = 2)
                             (input src_clk, input [WIDTH-1:0] src_in, input dest_clk,
                              output [WIDTH-1:0] dest_out);
    reg [WIDTH-1:0] src_ff;
    always @(posedge src_clk) src_ff <= src_in;
    wire [WIDTH-1:0] src_sel = SRC_INPUT_REG ? src_ff : src_in;
    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : bit_sync
            (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] syncstages_ff;
            always @(posedge dest_clk) syncstages_ff <= {syncstages_ff[DEST_SYNC_FF-2:0], src_sel[i]};
            assign dest_out[i] = syncstages_ff[DEST_SYNC_FF-1];
        end
    endgenerate
endmodule

module xpm_cdc_gray #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT_SYNC_FF = 0,
                      parameter integer REG_OUTPUT = 0, parameter integer SIM_ASSERT_CHK = 0,
                      parameter integer SIM_LOSSLESS_GRAY_CHK = 0, parameter integer WIDTH = 2)
                     (input src_clk, input [WIDTH-1:0] src_in_bin, input dest_clk,
                      output [WIDTH-1:0] dest_out_bin);
    // source: binary -> Gray, registered
    reg [WIDTH-1:0] src_gray_ff;
    always @(posedge src_clk) src_gray_ff <= src_in_bin ^ (src_in_bin >> 1);
    // destination: synchronizer chain per bit, Gray -> binary
    wire [WIDTH-1:0] dest_gray;
    wire [WIDTH-1:0] dest_bin;
    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : bit_sync
            (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] syncstages_ff;
            always @(posedge dest_clk) syncstages_ff <= {syncstages_ff[DEST_SYNC_FF-2:0], src_gray_ff[i]};
            assign dest_gray[i] = syncstages_ff[DEST_SYNC_FF-1];
            assign dest_bin[i] = ^(dest_gray >> i);
        end
    endgenerate
    reg [WIDTH-1:0] dest_out_bin_ff;
    always @(posedge dest_clk) dest_out_bin_ff <= dest_bin;
    assign dest_out_bin = REG_OUTPUT ? dest_out_bin_ff : dest_bin;
endmodule

module xpm_cdc_handshake #(parameter integer DEST_EXT_HSK = 1, parameter integer DEST_SYNC_FF = 4,
                           parameter integer INIT_SYNC_FF = 0, parameter integer SIM_ASSERT_CHK = 0,
                           parameter integer SRC_SYNC_FF = 4, parameter integer WIDTH = 1)
                          (input src_clk, input [WIDTH-1:0] src_in, input src_send, output src_rcv,
                           input dest_clk, output [WIDTH-1:0] dest_out, output dest_req, input dest_ack);
    // source: capture the data with the request, hold both until acknowledged
    reg [WIDTH-1:0] src_hsdata_ff;
    reg src_sendd_ff;
    wire src_rcv_s;
    always @(posedge src_clk) begin
        src_sendd_ff <= src_send;
        if (src_send && !src_sendd_ff) src_hsdata_ff <= src_in;
    end
    // request into the destination domain
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] dest_req_ff;
    always @(posedge dest_clk) dest_req_ff <= {dest_req_ff[DEST_SYNC_FF-2:0], src_sendd_ff};
    wire dest_req_s = dest_req_ff[DEST_SYNC_FF-1];
    reg dest_req_d;
    reg [WIDTH-1:0] dest_hsdata_ff;
    always @(posedge dest_clk) begin
        dest_req_d <= dest_req_s;
        if (dest_req_s && !dest_req_d) dest_hsdata_ff <= src_hsdata_ff;
    end
    assign dest_out = dest_hsdata_ff;
    assign dest_req = dest_req_s & ~dest_req_d;
    // acknowledge back into the source domain
    reg dest_ack_ff;
    always @(posedge dest_clk) dest_ack_ff <= DEST_EXT_HSK ? dest_ack : dest_req_s;
    (* ASYNC_REG = "TRUE" *) reg [SRC_SYNC_FF-1:0] src_ack_ff;
    always @(posedge src_clk) src_ack_ff <= {src_ack_ff[SRC_SYNC_FF-2:0], dest_ack_ff};
    assign src_rcv = src_ack_ff[SRC_SYNC_FF-1];
endmodule

module xpm_cdc_pulse #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT_SYNC_FF = 0,
                       parameter integer REG_OUTPUT = 0, parameter integer RST_USED = 1,
                       parameter integer SIM_ASSERT_CHK = 0)
                      (input src_clk, input src_pulse, input src_rst, input dest_clk, input dest_rst,
                       output dest_pulse);
    // source: toggle on every pulse
    reg src_level_ff;
    reg src_pulse_d;
    always @(posedge src_clk)
        if (RST_USED && src_rst) begin
            src_level_ff <= 1'b0;
            src_pulse_d <= 1'b0;
        end else begin
            src_pulse_d <= src_pulse;
            src_level_ff <= src_level_ff ^ (src_pulse & ~src_pulse_d);
        end
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] syncstages_ff;
    always @(posedge dest_clk) syncstages_ff <= {syncstages_ff[DEST_SYNC_FF-2:0], src_level_ff};
    // destination: edge detection of the synchronized level
    reg dest_level_d;
    always @(posedge dest_clk)
        if (RST_USED && dest_rst) dest_level_d <= 1'b0;
        else dest_level_d <= syncstages_ff[DEST_SYNC_FF-1];
    wire dest_p = syncstages_ff[DEST_SYNC_FF-1] ^ dest_level_d;
    reg dest_pulse_ff;
    always @(posedge dest_clk) dest_pulse_ff <= dest_p;
    assign dest_pulse = REG_OUTPUT ? dest_pulse_ff : dest_p;
endmodule

module xpm_cdc_async_rst #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT_SYNC_FF = 0,
                           parameter integer RST_ACTIVE_HIGH = 0)
                          (input src_arst, input dest_clk, output dest_arst);
    // asynchronous assertion, synchronous release
    wire arst = RST_ACTIVE_HIGH ? src_arst : ~src_arst;
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] arststages_ff;
    always @(posedge dest_clk or posedge arst)
        if (arst) arststages_ff <= {DEST_SYNC_FF{RST_ACTIVE_HIGH ? 1'b1 : 1'b0}};
        else arststages_ff <= {arststages_ff[DEST_SYNC_FF-2:0], RST_ACTIVE_HIGH ? 1'b0 : 1'b1};
    assign dest_arst = arststages_ff[DEST_SYNC_FF-1];
endmodule

module xpm_cdc_sync_rst #(parameter integer DEST_SYNC_FF = 4, parameter integer INIT = 1,
                          parameter integer INIT_SYNC_FF = 0, parameter integer SIM_ASSERT_CHK = 0)
                         (input src_rst, input dest_clk, output dest_rst);
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] syncstages_ff;
    always @(posedge dest_clk) syncstages_ff <= {syncstages_ff[DEST_SYNC_FF-2:0], src_rst};
    assign dest_rst = syncstages_ff[DEST_SYNC_FF-1];
endmodule

module xpm_fifo_async #(parameter integer CASCADE_HEIGHT = 0, parameter integer CDC_SYNC_STAGES = 2,
                        parameter DOUT_RESET_VALUE = "0", parameter ECC_MODE = "no_ecc",
                        parameter FIFO_MEMORY_TYPE = "auto", parameter integer FIFO_READ_LATENCY = 1,
                        parameter integer FIFO_WRITE_DEPTH = 2048, parameter integer FULL_RESET_VALUE = 0,
                        parameter integer PROG_EMPTY_THRESH = 10, parameter integer PROG_FULL_THRESH = 10,
                        parameter integer RD_DATA_COUNT_WIDTH = 1, parameter integer READ_DATA_WIDTH = 32,
                        parameter READ_MODE = "std", parameter integer RELATED_CLOCKS = 0,
                        parameter integer SIM_ASSERT_CHK = 0, parameter USE_ADV_FEATURES = "0707",
                        parameter integer WAKEUP_TIME = 0, parameter integer WRITE_DATA_WIDTH = 32,
                        parameter integer WR_DATA_COUNT_WIDTH = 1)
                       (output almost_empty, output almost_full, output data_valid, output dbiterr,
                        output [READ_DATA_WIDTH-1:0] dout, output empty, output full, output overflow,
                        output prog_empty, output prog_full, output [RD_DATA_COUNT_WIDTH-1:0] rd_data_count,
                        output rd_rst_busy, output sbiterr, output underflow, output wr_ack,
                        output [WR_DATA_COUNT_WIDTH-1:0] wr_data_count, output wr_rst_busy,
                        input [WRITE_DATA_WIDTH-1:0] din, input injectdbiterr, input injectsbiterr,
                        input rd_clk, input rd_en, input rst, input sleep, input wr_clk, input wr_en);
    localparam integer AW = $clog2(FIFO_WRITE_DEPTH);
    reg [WRITE_DATA_WIDTH-1:0] mem [0:FIFO_WRITE_DEPTH-1];

    // write side (rst is synchronous to wr_clk)
    reg  [AW:0] wbin, wptr;
    wire [AW:0] rptr_w;
    wire        full_i = wptr == {~rptr_w[AW:AW-1], rptr_w[AW-2:0]};
    wire [AW:0] wbin_next = wbin + {{AW{1'b0}}, wr_en & ~full_i};
    always @(posedge wr_clk)
        if (rst) begin
            wbin <= {(AW+1){1'b0}};
            wptr <= {(AW+1){1'b0}};
        end else begin
            wbin <= wbin_next;
            wptr <= (wbin_next >> 1) ^ wbin_next;
        end
    always @(posedge wr_clk)
        if (wr_en && !full_i) mem[wbin[AW-1:0]] <= din;

    // reset bridge into the read domain
    (* ASYNC_REG = "TRUE" *) reg [CDC_SYNC_STAGES-1:0] rd_rst_ff;
    always @(posedge rd_clk) rd_rst_ff <= {rd_rst_ff[CDC_SYNC_STAGES-2:0], rst};
    wire rd_rst = rd_rst_ff[CDC_SYNC_STAGES-1];

    // read side
    reg  [AW:0] rbin, rptr;
    wire [AW:0] wptr_r;
    wire        empty_i = rptr == wptr_r;
    wire [AW:0] rbin_next = rbin + {{AW{1'b0}}, rd_en & ~empty_i};
    reg  [WRITE_DATA_WIDTH-1:0] dout_ff;
    always @(posedge rd_clk)
        if (rd_rst) begin
            rbin <= {(AW+1){1'b0}};
            rptr <= {(AW+1){1'b0}};
        end else begin
            rbin <= rbin_next;
            rptr <= (rbin_next >> 1) ^ rbin_next;
            if (rd_en && !empty_i) dout_ff <= mem[rbin[AW-1:0]];
        end

    // Gray pointers across, CDC_SYNC_STAGES flops per bit
    genvar i;
    generate
        for (i = 0; i <= AW; i = i + 1) begin : ptr_sync
            (* ASYNC_REG = "TRUE" *) reg [CDC_SYNC_STAGES-1:0] w2r;
            (* ASYNC_REG = "TRUE" *) reg [CDC_SYNC_STAGES-1:0] r2w;
            always @(posedge rd_clk) w2r <= {w2r[CDC_SYNC_STAGES-2:0], wptr[i]};
            always @(posedge wr_clk) r2w <= {r2w[CDC_SYNC_STAGES-2:0], rptr[i]};
            assign wptr_r[i] = w2r[CDC_SYNC_STAGES-1];
            assign rptr_w[i] = r2w[CDC_SYNC_STAGES-1];
        end
    endgenerate

    assign dout = dout_ff;
    assign full = full_i;
    assign empty = empty_i;
    assign almost_empty = 1'b0;
    assign almost_full = 1'b0;
    assign data_valid = 1'b0;
    assign dbiterr = 1'b0;
    assign sbiterr = 1'b0;
    assign overflow = 1'b0;
    assign underflow = 1'b0;
    assign prog_empty = empty_i;
    assign prog_full = full_i;
    assign rd_data_count = {RD_DATA_COUNT_WIDTH{1'b0}};
    assign wr_data_count = {WR_DATA_COUNT_WIDTH{1'b0}};
    assign rd_rst_busy = rd_rst;
    assign wr_rst_busy = rst;
    assign wr_ack = 1'b0;
endmodule

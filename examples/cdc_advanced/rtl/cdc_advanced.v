// Stage-5 example: correct protocol synchronizers next to one deliberate
// bug per stage-5 rule. See README.md for the expected result of each case.
module cdc_advanced (
    input  wire       clk_a,      // 100 MHz
    input  wire       clk_b,      // 125 MHz
    input  wire       clk_c,      //  83 MHz
    input  wire       rst_n,      // external asynchronous reset
    // clk_a side
    input  wire       a_wr,
    input  wire [7:0] a_wdata,
    input  wire       a_event,
    input  wire       a_send,
    input  wire [7:0] a_word,
    input  wire       a_start_set,
    input  wire       a_mode_set,
    input  wire       a_cfg_we,
    input  wire [3:0] a_cfg_in,
    input  wire       a_x_in,
    output wire       a_full,
    output wire       a_busy,
    // clk_b side
    input  wire       b_rd,
    output wire [7:0] b_rdata,
    output wire       b_empty,
    output reg  [7:0] b_events,
    output reg        b_go,
    output reg  [3:0] b_kicks,
    output reg        b_cfg_hit,
    output wire       b_mix,
    // clk_c side
    input  wire       c_y_in,
    output wire [7:0] c_word,
    output wire       c_valid
);
    // ---------------- resets ----------------
    // (1) one reset synchronizer per clock for clk_a and clk_b: safe
    wire rst_a_n, rst_b_n;
    rst_sync u_rst_a (.clk(clk_a), .rst_in_n(rst_n), .rst_out_n(rst_a_n));
    rst_sync u_rst_b (.clk(clk_b), .rst_in_n(rst_n), .rst_out_n(rst_b_n));
    // (2) BUG CDC-006: clk_c logic uses the reset synchronized for clk_a
    wire rst_c_n = rst_a_n;

    // ---------------- (3) asynchronous FIFO clk_a -> clk_b: safe ----------------
    gray_fifo #(.DW(8), .AW(3)) u_fifo (
        .wclk(clk_a), .wrst_n(rst_a_n), .winc(a_wr), .wdata(a_wdata), .wfull(a_full),
        .rclk(clk_b), .rrst_n(rst_b_n), .rinc(b_rd), .rdata(b_rdata), .rempty(b_empty));

    // ---------------- (4) toggle pulse synchronizer clk_a -> clk_b: safe ----------------
    wire b_event;
    pulse_sync u_pulse (.src_clk(clk_a), .src_rst_n(rst_a_n), .src_pulse(a_event),
                        .dst_clk(clk_b), .dst_rst_n(rst_b_n), .dst_pulse(b_event));
    always @(posedge clk_b or negedge rst_b_n)
        if (!rst_b_n)     b_events <= 8'd0;
        else if (b_event) b_events <= b_events + 8'd1;

    // ---------------- (5) handshake clk_a -> clk_c: safe protocol ----------------
    handshake #(.DW(8)) u_hs (
        .src_clk(clk_a), .src_rst_n(rst_a_n), .send(a_send), .din(a_word), .busy(a_busy),
        .dst_clk(clk_c), .dst_rst_n(rst_c_n), .dout(c_word), .dvalid(c_valid));

    // ---------------- (6) BUG CDC-005: two synchronized flags reconverge ----------------
    reg a_start, a_mode;
    always @(posedge clk_a or negedge rst_a_n)
        if (!rst_a_n) begin
            a_start <= 1'b0;
            a_mode  <= 1'b0;
        end else begin
            if (a_start_set) a_start <= a_word[0];
            if (a_mode_set)  a_mode  <= a_word[1];
        end
    wire start_s, mode_s;
    sync_2ff u_start (.clk(clk_b), .rst_n(rst_b_n), .d(a_start), .q(start_s));
    sync_2ff u_mode  (.clk(clk_b), .rst_n(rst_b_n), .d(a_mode),  .q(mode_s));
    always @(posedge clk_b or negedge rst_b_n)
        if (!rst_b_n) b_go <= 1'b0;
        else          b_go <= start_s & mode_s;

    // ---------------- (7) BUG CDC-007: one-cycle pulse into a level synchronizer ----------------
    reg [3:0] a_cnt;
    reg       a_kick;
    always @(posedge clk_a or negedge rst_a_n)
        if (!rst_a_n) begin
            a_cnt  <= 4'd0;
            a_kick <= 1'b0;
        end else begin
            a_cnt  <= a_cnt + 4'd1;
            a_kick <= a_cnt == 4'd7;        // high for one clk_a cycle out of 16
        end
    wire kick_s;
    sync_2ff u_kick (.clk(clk_b), .rst_n(rst_b_n), .d(a_kick), .q(kick_s));
    always @(posedge clk_b or negedge rst_b_n)
        if (!rst_b_n)    b_kicks <= 4'd0;
        else if (kick_s) b_kicks <= b_kicks + 4'd1;

    // ---------------- (8) BUG CDC-008: configuration word synchronized bit by bit ----------------
    reg [3:0] a_cfg;
    always @(posedge clk_a or negedge rst_a_n)
        if (!rst_a_n)      a_cfg <= 4'd0;
        else if (a_cfg_we) a_cfg <= a_cfg_in;
    wire [3:0] cfg_s;
    sync_vec #(.W(4)) u_cfg (.clk(clk_b), .rst_n(rst_b_n), .d(a_cfg), .q(cfg_s));
    always @(posedge clk_b or negedge rst_b_n)
        if (!rst_b_n) b_cfg_hit <= 1'b0;
        else          b_cfg_hit <= cfg_s == 4'hA;

    // ---------------- (9) BUG CDC-004 + CDC-009: two foreign clocks ORed before a synchronizer ----------------
    reg a_x, c_y;
    always @(posedge clk_a or negedge rst_a_n)
        if (!rst_a_n) a_x <= 1'b0;
        else          a_x <= a_x_in;
    always @(posedge clk_c or negedge rst_c_n)
        if (!rst_c_n) c_y <= 1'b0;
        else          c_y <= c_y_in;
    sync_2ff u_mix (.clk(clk_b), .rst_n(rst_b_n), .d(a_x | c_y), .q(b_mix));
endmodule

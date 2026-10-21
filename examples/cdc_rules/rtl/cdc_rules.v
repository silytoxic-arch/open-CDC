// CDC rule examples: every crossing below is placed on purpose.
// clk_a and clk_b are asynchronous (see constraints/cdc_rules.sdc);
// clk_c has no constraint, so its relation to clk_a is unknown.
// The expected result of each case is listed in README.md.

module sync_2ff (
    input  wire clk,
    input  wire d,
    output wire q
);
    (* ASYNC_REG = "TRUE" *) reg meta;
    (* ASYNC_REG = "TRUE" *) reg sync;
    always @(posedge clk) begin
        meta <= d;
        sync <= meta;
    end
    assign q = sync;
endmodule

module cdc_rules (
    input  wire       clk_a,
    input  wire       clk_b,
    input  wire       clk_c,
    input  wire [7:0] din,
    input  wire [7:0] bin,
    output reg  [7:0] dout,
    output wire [7:0] status
);
    // ---------------- source domain (clk_a) ----------------
    reg       a_flag, a_go, a_lvl, a_tap, a_req, a_x, a_p, a_q, a_valid, a_tog;
    reg [7:0] a_bus, a_data;
    always @(posedge clk_a) begin
        a_flag  <= din[0];
        a_go    <= din[1];
        a_lvl   <= din[2];
        a_tap   <= din[3];
        a_req   <= din[4];
        a_x     <= din[5];
        a_p     <= din[6];
        a_q     <= din[7];
        a_valid <= ~a_valid;
        a_tog   <= a_tog ^ din[0];
        a_bus   <= din;
        a_data  <= din + 8'd1;
    end

    // ---------------- destination domain (clk_b) ----------------
    // 1. CDC-001: single bit through logic, no synchronizer
    reg b_flag_and;
    always @(posedge clk_b) b_flag_and <= a_flag & bin[0];

    // 2. CDC-001: crossing into a clock enable
    reg [3:0] b_cnt;
    always @(posedge clk_b) if (a_go) b_cnt <= b_cnt + 4'd1;

    // 3. CDC-002: a single flop whose output is used directly
    reg b_lvl_s1, b_lvl_use;
    always @(posedge clk_b) begin
        b_lvl_s1  <= a_lvl;
        b_lvl_use <= b_lvl_s1 ^ bin[1];
    end

    // 4. CDC-002: two-flop chain whose first stage is tapped
    reg b_tap_s1, b_tap_s2, b_tap_bad;
    always @(posedge clk_b) begin
        b_tap_s1  <= a_tap;
        b_tap_s2  <= b_tap_s1;
        b_tap_bad <= b_tap_s1 & bin[2];
    end

    // 5. CDC-003: multi-bit bus used directly in arithmetic
    reg [7:0] b_bus_sum;
    always @(posedge clk_b) b_bus_sum <= a_bus + bin;

    // 6. Safe: two-flop synchronizer module
    wire b_req;
    sync_2ff u_req_sync (.clk(clk_b), .d(a_req), .q(b_req));

    // 7. Safe: three-flop chain
    reg b_x_s1, b_x_s2, b_x_s3;
    always @(posedge clk_b) begin
        b_x_s1 <= a_x;
        b_x_s2 <= b_x_s1;
        b_x_s3 <= b_x_s2;
    end

    // 8. Safe (protocol unverified): data qualified by a synchronized valid
    wire      b_valid;
    reg [7:0] b_data;
    sync_2ff u_valid_sync (.clk(clk_b), .d(a_valid), .q(b_valid));
    always @(posedge clk_b) if (b_valid) b_data <= a_data;

    // 9. Needs review: combinational logic in front of the first stage (CDC-004 in stage 5)
    reg b_or_s1, b_or_s2;
    always @(posedge clk_b) begin
        b_or_s1 <= a_p | a_q;
        b_or_s2 <= b_or_s1;
    end

    always @(posedge clk_b)
        dout <= b_data ^ b_bus_sum ^ {b_cnt, b_flag_and, b_lvl_use, b_tap_s2, b_tap_bad} ^
                {b_req, b_x_s3, b_or_s2, 5'd0};

    // ---------------- clk_c: relation to clk_a unknown ----------------
    // 10. CDC-010: synchronized, but the clock relation is not constrained
    reg c_s1, c_s2;
    always @(posedge clk_c) begin
        c_s1 <= a_tog;
        c_s2 <= c_s1;
    end

    assign status = {7'd0, c_s2};
endmodule

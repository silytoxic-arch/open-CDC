// OpenCDC behavioral models of Intel (Altera) clock-domain-crossing cells.

module altera_std_synchronizer #(parameter depth = 3)
                                (input clk, input reset_n, input din, output dout);
    (* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS" *)
    reg [depth-1:0] din_s;
    always @(posedge clk or negedge reset_n)
        if (!reset_n) din_s <= {depth{1'b0}};
        else din_s <= {din_s[depth-2:0], din};
    assign dout = din_s[depth-1];
endmodule

module altera_std_synchronizer_bundle #(parameter depth = 3, parameter width = 1)
                                       (input clk, input reset_n, input [width-1:0] din,
                                        output [width-1:0] dout);
    genvar i;
    generate
        for (i = 0; i < width; i = i + 1) begin : sync
            altera_std_synchronizer #(.depth(depth)) u (.clk(clk), .reset_n(reset_n), .din(din[i]),
                                                        .dout(dout[i]));
        end
    endgenerate
endmodule

module altera_reset_synchronizer #(parameter ASYNC_RESET = 1, parameter DEPTH = 2)
                                  (input reset_in, input clk, output reset_out);
    reg [DEPTH-1:0] sync;
    generate
        if (ASYNC_RESET) begin : g_async
            always @(posedge clk or posedge reset_in)
                if (reset_in) sync <= {DEPTH{1'b1}};
                else sync <= {sync[DEPTH-2:0], 1'b0};
        end else begin : g_sync
            always @(posedge clk) sync <= {sync[DEPTH-2:0], reset_in};
        end
    endgenerate
    assign reset_out = sync[DEPTH-1];
endmodule

module dcfifo #(parameter add_usedw_msb_bit = "OFF", parameter clocks_are_synchronized = "FALSE",
                parameter delay_rdusedw = 1, parameter delay_wrusedw = 1, parameter intended_device_family = "",
                parameter enable_ecc = "FALSE", parameter lpm_numwords = 16, parameter lpm_showahead = "OFF",
                parameter lpm_type = "dcfifo", parameter lpm_width = 8, parameter lpm_widthu = 4,
                parameter overflow_checking = "ON", parameter rdsync_delaypipe = 4,
                parameter read_aclr_synch = "OFF", parameter underflow_checking = "ON",
                parameter use_eab = "ON", parameter write_aclr_synch = "OFF", parameter wrsync_delaypipe = 4)
               (input aclr, input [lpm_width-1:0] data, input rdclk, input rdreq, input wrclk, input wrreq,
                output [lpm_width-1:0] q, output rdempty, output rdfull, output [lpm_widthu-1:0] rdusedw,
                output wrempty, output wrfull, output [lpm_widthu-1:0] wrusedw, output eccstatus);
    localparam integer AW = lpm_widthu;
    localparam integer SYNC = (rdsync_delaypipe > 2) ? rdsync_delaypipe - 2 : 2;
    reg [lpm_width-1:0] mem [0:(1 << AW)-1];
    reg  [AW:0] wbin, wptr, rbin, rptr;
    wire [AW:0] rptr_w, wptr_r;
    wire full_i = wptr == {~rptr_w[AW:AW-1], rptr_w[AW-2:0]};
    wire empty_i = rptr == wptr_r;
    wire [AW:0] wbin_next = wbin + {{AW{1'b0}}, wrreq & ~full_i};
    wire [AW:0] rbin_next = rbin + {{AW{1'b0}}, rdreq & ~empty_i};
    reg [lpm_width-1:0] q_ff;
    always @(posedge wrclk or posedge aclr)
        if (aclr) begin
            wbin <= {(AW+1){1'b0}};
            wptr <= {(AW+1){1'b0}};
        end else begin
            wbin <= wbin_next;
            wptr <= (wbin_next >> 1) ^ wbin_next;
        end
    always @(posedge wrclk)
        if (wrreq && !full_i) mem[wbin[AW-1:0]] <= data;
    always @(posedge rdclk or posedge aclr)
        if (aclr) begin
            rbin <= {(AW+1){1'b0}};
            rptr <= {(AW+1){1'b0}};
        end else begin
            rbin <= rbin_next;
            rptr <= (rbin_next >> 1) ^ rbin_next;
            if (rdreq && !empty_i) q_ff <= mem[rbin[AW-1:0]];
        end
    genvar i;
    generate
        for (i = 0; i <= AW; i = i + 1) begin : ptr_sync
            reg [SYNC-1:0] w2r;
            reg [SYNC-1:0] r2w;
            always @(posedge rdclk) w2r <= {w2r[SYNC-2:0], wptr[i]};
            always @(posedge wrclk) r2w <= {r2w[SYNC-2:0], rptr[i]};
            assign wptr_r[i] = w2r[SYNC-1];
            assign rptr_w[i] = r2w[SYNC-1];
        end
    endgenerate
    assign q = q_ff;
    assign rdempty = empty_i;
    assign wrfull = full_i;
    assign rdfull = 1'b0;
    assign wrempty = 1'b0;
    assign rdusedw = {lpm_widthu{1'b0}};
    assign wrusedw = {lpm_widthu{1'b0}};
    assign eccstatus = 1'b0;
endmodule

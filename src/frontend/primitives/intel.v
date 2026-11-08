// OpenCDC behavioral models of Intel (Altera) clock-domain-crossing cells.

module altera_std_synchronizer #(parameter depth = 3)
                                (input clk, input reset_n, input din, output dout);
    // register names as in Intel's source, which its SDC refers to
    // (set_false_path -to [get_keepers {*altera_std_synchronizer:*|din_s1}])
    (* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS" *)
    reg din_s1;
    (* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS" *)
    reg [depth-2:0] dreg;
    always @(posedge clk or negedge reset_n)
        if (!reset_n) din_s1 <= 1'b0;
        else din_s1 <= din;
    generate
        if (depth < 3) begin : g_short
            always @(posedge clk or negedge reset_n)
                if (!reset_n) dreg <= {(depth-1){1'b0}};
                else dreg <= din_s1;
        end else begin : g_long
            always @(posedge clk or negedge reset_n)
                if (!reset_n) dreg <= {(depth-1){1'b0}};
                else dreg <= {dreg[depth-3:0], din_s1};
        end
    endgenerate
    assign dout = dreg[depth-2];
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
    // structure and register names as in Intel's source: a DEPTH-stage chain
    // shifting towards bit 0, then one output register
    (* preserve *) reg [DEPTH-1:0] altera_reset_synchronizer_int_chain;
    reg altera_reset_synchronizer_int_chain_out;
    generate
        if (ASYNC_RESET) begin : g_async
            always @(posedge clk or posedge reset_in)
                if (reset_in) begin
                    altera_reset_synchronizer_int_chain <= {DEPTH{1'b1}};
                    altera_reset_synchronizer_int_chain_out <= 1'b1;
                end else begin
                    altera_reset_synchronizer_int_chain <= {1'b0, altera_reset_synchronizer_int_chain[DEPTH-1:1]};
                    altera_reset_synchronizer_int_chain_out <= altera_reset_synchronizer_int_chain[0];
                end
        end else begin : g_sync
            always @(posedge clk) begin
                altera_reset_synchronizer_int_chain <= {reset_in, altera_reset_synchronizer_int_chain[DEPTH-1:1]};
                altera_reset_synchronizer_int_chain_out <= altera_reset_synchronizer_int_chain[0];
            end
        end
    endgenerate
    assign reset_out = altera_reset_synchronizer_int_chain_out;
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

// ---------------------------------------------------------------- scfifo
// Single-clock FIFO: memory and pointers on one clock (no crossing inside);
// lpm_showahead = "ON" reads the head combinationally, otherwise q is registered.
module scfifo #(parameter add_ram_output_register = "OFF", parameter allow_rwcycle_when_full = "OFF",
                parameter almost_empty_value = 0, parameter almost_full_value = 0,
                parameter enable_ecc = "FALSE", parameter intended_device_family = "",
                parameter lpm_hint = "UNUSED", parameter lpm_numwords = 16, parameter lpm_showahead = "OFF",
                parameter lpm_type = "scfifo", parameter lpm_width = 8, parameter lpm_widthu = 4,
                parameter maximum_depth = 0, parameter overflow_checking = "ON",
                parameter underflow_checking = "ON", parameter use_eab = "ON")
               (input aclr, output almost_empty, output almost_full, input clock, input [lpm_width-1:0] data,
                output eccstatus, output empty, output full, output [lpm_width-1:0] q, input rdreq, input sclr,
                output [lpm_widthu-1:0] usedw, input wrreq);
    localparam integer AW = lpm_widthu;
    reg [lpm_width-1:0] mem [0:(1 << AW)-1];
    reg [AW:0] wp, rp;
    wire empty_i = wp == rp;
    wire full_i = (wp[AW-1:0] == rp[AW-1:0]) && (wp[AW] != rp[AW]);
    always @(posedge clock or posedge aclr)
        if (aclr) begin
            wp <= {(AW+1){1'b0}};
            rp <= {(AW+1){1'b0}};
        end else if (sclr) begin
            wp <= {(AW+1){1'b0}};
            rp <= {(AW+1){1'b0}};
        end else begin
            if (wrreq && !full_i) wp <= wp + 1'b1;
            if (rdreq && !empty_i) rp <= rp + 1'b1;
        end
    always @(posedge clock)
        if (wrreq && !full_i) mem[wp[AW-1:0]] <= data;
    generate
        if (lpm_showahead == "ON") begin : g_ahead
            assign q = mem[rp[AW-1:0]];
        end else begin : g_reg
            reg [lpm_width-1:0] q_ff;
            always @(posedge clock)
                if (rdreq && !empty_i) q_ff <= mem[rp[AW-1:0]];
            assign q = q_ff;
        end
    endgenerate
    wire [AW:0] used = wp - rp;
    assign usedw = used[AW-1:0];
    assign empty = empty_i;
    assign full = full_i;
    assign almost_empty = used < almost_empty_value;
    assign almost_full = used >= almost_full_value;
    assign eccstatus = 1'b0;
endmodule

// ---------------------------------------------------------------- LPM arithmetic
// lpm_mult / lpm_divide: the result combinationally (lpm_pipeline = 0) or after
// lpm_pipeline registers on clock (enabled by clken).
module lpm_mult #(parameter lpm_widtha = 1, parameter lpm_widthb = 1, parameter lpm_widthp = 2,
                  parameter lpm_widths = 1, parameter lpm_representation = "UNSIGNED",
                  parameter lpm_pipeline = 0, parameter lpm_type = "LPM_MULT", parameter lpm_hint = "UNUSED")
                 (input [lpm_widtha-1:0] dataa, input [lpm_widthb-1:0] datab, input [lpm_widths-1:0] sum,
                  input clock, input clken, input aclr, input sclr, output [lpm_widthp-1:0] result);
    localparam integer W = lpm_widtha + lpm_widthb;
    wire [W-1:0] p = lpm_representation == "SIGNED" ? $signed(dataa) * $signed(datab) : dataa * datab;
    wire [lpm_widthp-1:0] r = p;
    genvar i;
    generate
        if (lpm_pipeline == 0) begin : g_comb
            assign result = r;
        end else begin : g_pipe
            reg [lpm_widthp-1:0] stage [0:lpm_pipeline-1];
            always @(posedge clock) if (clken !== 1'b0) stage[0] <= r;
            for (i = 1; i < lpm_pipeline; i = i + 1) begin : g_stage
                always @(posedge clock) if (clken !== 1'b0) stage[i] <= stage[i-1];
            end
            assign result = stage[lpm_pipeline-1];
        end
    endgenerate
endmodule

module lpm_divide #(parameter lpm_widthn = 1, parameter lpm_widthd = 1,
                    parameter lpm_nrepresentation = "UNSIGNED", parameter lpm_drepresentation = "UNSIGNED",
                    parameter lpm_pipeline = 0, parameter lpm_remainderpositive = "TRUE",
                    parameter maximize_speed = 5, parameter lpm_type = "LPM_DIVIDE", parameter lpm_hint = "UNUSED")
                   (input [lpm_widthn-1:0] numer, input [lpm_widthd-1:0] denom, input clock, input clken,
                    input aclr, output [lpm_widthn-1:0] quotient, output [lpm_widthd-1:0] remain);
    wire [lpm_widthn-1:0] q = lpm_nrepresentation == "SIGNED" ? $signed(numer) / $signed(denom) : numer / denom;
    wire [lpm_widthd-1:0] r = lpm_nrepresentation == "SIGNED" ? $signed(numer) % $signed(denom) : numer % denom;
    genvar i;
    generate
        if (lpm_pipeline == 0) begin : g_comb
            assign quotient = q;
            assign remain = r;
        end else begin : g_pipe
            reg [lpm_widthn-1:0] sq [0:lpm_pipeline-1];
            reg [lpm_widthd-1:0] sr [0:lpm_pipeline-1];
            always @(posedge clock) if (clken !== 1'b0) begin sq[0] <= q; sr[0] <= r; end
            for (i = 1; i < lpm_pipeline; i = i + 1) begin : g_stage
                always @(posedge clock) if (clken !== 1'b0) begin sq[i] <= sq[i-1]; sr[i] <= sr[i-1]; end
            end
            assign quotient = sq[lpm_pipeline-1];
            assign remain = sr[lpm_pipeline-1];
        end
    endgenerate
endmodule

// ---------------------------------------------------------------- altsyncram
// Port A on clock0; port B on clock1 when its address/output registers say
// so (address_reg_b / outdata_reg_b = "CLOCK1"), otherwise on clock0. Each
// write port writes its own array; both are read (see the XPM memory note).
module altsyncram #(parameter address_aclr_a = "NONE", parameter address_aclr_b = "NONE",
                    parameter address_reg_b = "CLOCK1", parameter byte_size = 8,
                    parameter clock_enable_input_a = "BYPASS", parameter clock_enable_input_b = "BYPASS",
                    parameter clock_enable_output_a = "BYPASS", parameter clock_enable_output_b = "BYPASS",
                    parameter indata_reg_b = "CLOCK1", parameter init_file = "UNUSED",
                    parameter intended_device_family = "", parameter lpm_type = "altsyncram",
                    parameter numwords_a = 256, parameter numwords_b = 256, parameter operation_mode = "DUAL_PORT",
                    parameter outdata_aclr_a = "NONE", parameter outdata_aclr_b = "NONE",
                    parameter outdata_reg_a = "UNREGISTERED", parameter outdata_reg_b = "UNREGISTERED",
                    parameter power_up_uninitialized = "FALSE", parameter ram_block_type = "AUTO",
                    parameter rdcontrol_reg_b = "CLOCK1", parameter read_during_write_mode_mixed_ports = "DONT_CARE",
                    parameter read_during_write_mode_port_a = "NEW_DATA_NO_NBE_READ",
                    parameter read_during_write_mode_port_b = "NEW_DATA_NO_NBE_READ",
                    parameter width_a = 8, parameter width_b = 8, parameter width_byteena_a = 1,
                    parameter width_byteena_b = 1, parameter widthad_a = 8, parameter widthad_b = 8,
                    parameter wrcontrol_wraddress_reg_b = "CLOCK1")
                   (input aclr0, input aclr1, input [widthad_a-1:0] address_a, input [widthad_b-1:0] address_b,
                    input addressstall_a, input addressstall_b, input [width_byteena_a-1:0] byteena_a,
                    input [width_byteena_b-1:0] byteena_b, input clock0, input clock1, input clocken0,
                    input clocken1, input clocken2, input clocken3, input [width_a-1:0] data_a,
                    input [width_b-1:0] data_b, output [7:0] eccstatus, output [width_a-1:0] q_a,
                    output [width_b-1:0] q_b, input rden_a, input rden_b, input wren_a, input wren_b);
    localparam integer W = width_a > width_b ? width_a : width_b;
    reg [W-1:0] mem_a [0:numwords_a-1];
    reg [W-1:0] mem_b [0:numwords_a-1];
    wire cb;
    generate
        if (address_reg_b == "CLOCK1" || outdata_reg_b == "CLOCK1") begin : g_c1
            assign cb = clock1;
        end else begin : g_c0
            assign cb = clock0;
        end
    endgenerate
    always @(posedge clock0) if (wren_a) mem_a[address_a] <= data_a;
    always @(posedge cb) if (wren_b) mem_b[address_b] <= data_b;
    reg [width_a-1:0] qa;
    reg [width_b-1:0] qb;
    always @(posedge clock0) qa <= mem_a[address_a] ^ mem_b[address_a];
    always @(posedge cb) qb <= mem_a[address_b] ^ mem_b[address_b];
    assign q_a = qa;
    assign q_b = qb;
    assign eccstatus = 8'd0;
endmodule

// ---------------------------------------------------------------- altdpram
// Dual-port RAM (MLAB or LUT RAM): written on inclock; the read address is
// registered on inclock or outclock (rdaddress_reg), or not; the output is
// registered on outclock or inclock (outdata_reg), or read asynchronously.
module altdpram #(parameter width = 1, parameter widthad = 1, parameter numwords = 0,
                  parameter byte_size = 8, parameter indata_aclr = "ON", parameter indata_reg = "INCLOCK",
                  parameter intended_device_family = "", parameter lpm_file = "UNUSED",
                  parameter lpm_hint = "UNUSED", parameter lpm_type = "altdpram", parameter maximum_depth = 0,
                  parameter outdata_aclr = "ON", parameter outdata_reg = "UNREGISTERED",
                  parameter outdata_sclr = "OFF", parameter power_up_uninitialized = "FALSE",
                  parameter ram_block_type = "AUTO", parameter rdaddress_aclr = "ON",
                  parameter rdaddress_reg = "OUTCLOCK", parameter rdcontrol_aclr = "ON",
                  parameter rdcontrol_reg = "OUTCLOCK", parameter read_during_write_mode_mixed_ports = "DONT_CARE",
                  parameter use_eab = "ON", parameter width_byteena = 1, parameter wraddress_aclr = "ON",
                  parameter wraddress_reg = "INCLOCK", parameter wrcontrol_aclr = "ON",
                  parameter wrcontrol_reg = "INCLOCK")
                 (input aclr, input [width_byteena-1:0] byteena, input [width-1:0] data, input inclock,
                  input inclocken, input outclock, input outclocken, output [width-1:0] q,
                  input [widthad-1:0] rdaddress, input rdaddressstall, input rden, input sclr,
                  input [widthad-1:0] wraddress, input wraddressstall, input wren);
    reg [width-1:0] mem [0:(1 << widthad)-1];
    always @(posedge inclock) if (wren) mem[wraddress] <= data;
    // read address
    wire [widthad-1:0] ra;
    generate
        if (rdaddress_reg == "INCLOCK") begin : g_ra_in
            reg [widthad-1:0] r;
            always @(posedge inclock) r <= rdaddress;
            assign ra = r;
        end else if (rdaddress_reg == "OUTCLOCK") begin : g_ra_out
            reg [widthad-1:0] r;
            always @(posedge outclock) r <= rdaddress;
            assign ra = r;
        end else begin : g_ra_async
            assign ra = rdaddress;
        end
    endgenerate
    // output
    generate
        if (outdata_reg == "OUTCLOCK") begin : g_q_out
            reg [width-1:0] r;
            always @(posedge outclock) r <= mem[ra];
            assign q = r;
        end else if (outdata_reg == "INCLOCK") begin : g_q_in
            reg [width-1:0] r;
            always @(posedge inclock) r <= mem[ra];
            assign q = r;
        end else begin : g_q_async
            assign q = mem[ra];
        end
    endgenerate
endmodule

// ---------------------------------------------------------------- DDR I/O
module altddio_in #(parameter intended_device_family = "", parameter invert_input_clocks = "OFF",
                    parameter lpm_hint = "UNUSED", parameter lpm_type = "altddio_in",
                    parameter power_up_high = "OFF", parameter width = 1)
                   (input aclr, input aset, input [width-1:0] datain, output reg [width-1:0] dataout_h,
                    output reg [width-1:0] dataout_l, input inclock, input inclocken, input sclr, input sset);
    reg [width-1:0] neg;
    always @(negedge inclock) if (inclocken) neg <= datain;
    always @(posedge inclock or posedge aclr)
        if (aclr) begin dataout_h <= {width{1'b0}}; dataout_l <= {width{1'b0}}; end
        else if (inclocken) begin dataout_h <= datain; dataout_l <= neg; end
endmodule

module altddio_out #(parameter extend_oe_disable = "OFF", parameter intended_device_family = "",
                     parameter invert_output = "OFF", parameter lpm_hint = "UNUSED", parameter lpm_type = "altddio_out",
                     parameter oe_reg = "UNREGISTERED", parameter power_up_high = "OFF", parameter width = 1)
                    (input aclr, input aset, input [width-1:0] datain_h, input [width-1:0] datain_l,
                     output [width-1:0] dataout, input oe, output oe_out, input outclock, input outclocken,
                     input sclr, input sset);
    reg [width-1:0] h, l;
    always @(posedge outclock or posedge aclr)
        if (aclr) begin h <= {width{1'b0}}; l <= {width{1'b0}}; end
        else if (outclocken) begin h <= datain_h; l <= datain_l; end
    assign dataout = h | l;
    assign oe_out = oe;
endmodule


// Quartus Prime successor of altsyncram: same ports and behavior.
module altera_syncram #(parameter address_aclr_a = "NONE", parameter address_aclr_b = "NONE",
                    parameter address_reg_b = "CLOCK1", parameter byte_size = 8,
                    parameter clock_enable_input_a = "BYPASS", parameter clock_enable_input_b = "BYPASS",
                    parameter clock_enable_output_a = "BYPASS", parameter clock_enable_output_b = "BYPASS",
                    parameter indata_reg_b = "CLOCK1", parameter init_file = "UNUSED",
                    parameter intended_device_family = "", parameter lpm_type = "altera_syncram",
                    parameter numwords_a = 256, parameter numwords_b = 256, parameter operation_mode = "DUAL_PORT",
                    parameter outdata_aclr_a = "NONE", parameter outdata_aclr_b = "NONE",
                    parameter outdata_reg_a = "UNREGISTERED", parameter outdata_reg_b = "UNREGISTERED",
                    parameter power_up_uninitialized = "FALSE", parameter ram_block_type = "AUTO",
                    parameter rdcontrol_reg_b = "CLOCK1", parameter read_during_write_mode_mixed_ports = "DONT_CARE",
                    parameter read_during_write_mode_port_a = "NEW_DATA_NO_NBE_READ",
                    parameter read_during_write_mode_port_b = "NEW_DATA_NO_NBE_READ",
                    parameter width_a = 8, parameter width_b = 8, parameter width_byteena_a = 1,
                    parameter width_byteena_b = 1, parameter widthad_a = 8, parameter widthad_b = 8,
                    parameter wrcontrol_wraddress_reg_b = "CLOCK1")
                   (input aclr0, input aclr1, input [widthad_a-1:0] address_a, input [widthad_b-1:0] address_b,
                    input addressstall_a, input addressstall_b, input [width_byteena_a-1:0] byteena_a,
                    input [width_byteena_b-1:0] byteena_b, input clock0, input clock1, input clocken0,
                    input clocken1, input clocken2, input clocken3, input [width_a-1:0] data_a,
                    input [width_b-1:0] data_b, output [7:0] eccstatus, output [width_a-1:0] q_a,
                    output [width_b-1:0] q_b, input rden_a, input rden_b, input wren_a, input wren_b);
    localparam integer W = width_a > width_b ? width_a : width_b;
    reg [W-1:0] mem_a [0:numwords_a-1];
    reg [W-1:0] mem_b [0:numwords_a-1];
    wire cb;
    generate
        if (address_reg_b == "CLOCK1" || outdata_reg_b == "CLOCK1") begin : g_c1
            assign cb = clock1;
        end else begin : g_c0
            assign cb = clock0;
        end
    endgenerate
    always @(posedge clock0) if (wren_a) mem_a[address_a] <= data_a;
    always @(posedge cb) if (wren_b) mem_b[address_b] <= data_b;
    reg [width_a-1:0] qa;
    reg [width_b-1:0] qb;
    always @(posedge clock0) qa <= mem_a[address_a] ^ mem_b[address_a];
    always @(posedge cb) qb <= mem_a[address_b] ^ mem_b[address_b];
    assign q_a = qa;
    assign q_b = qb;
    assign eccstatus = 8'd0;
endmodule

// ---------------------------------------------------------------- PLLs
// altpll: clkN period = input period x clkN_divide_by / clkN_multiply_by
// (inclk0_input_frequency is the input period in ps). locked has no clock
// domain.
module altpll #(parameter bandwidth_type = "AUTO", parameter compensate_clock = "CLK0",
                parameter integer inclk0_input_frequency = 10000, parameter intended_device_family = "",
                parameter lpm_type = "altpll", parameter operation_mode = "NORMAL",
                parameter pll_type = "AUTO", parameter port_locked = "PORT_USED", parameter width_clock = 5,
                parameter integer clk0_multiply_by = 1, parameter integer clk0_divide_by = 1,
                parameter integer clk1_multiply_by = 1, parameter integer clk1_divide_by = 1,
                parameter integer clk2_multiply_by = 1, parameter integer clk2_divide_by = 1,
                parameter integer clk3_multiply_by = 1, parameter integer clk3_divide_by = 1,
                parameter integer clk4_multiply_by = 1, parameter integer clk4_divide_by = 1,
                parameter clk0_duty_cycle = 50, parameter clk0_phase_shift = "0",
                parameter clk1_duty_cycle = 50, parameter clk1_phase_shift = "0",
                parameter clk2_duty_cycle = 50, parameter clk2_phase_shift = "0",
                parameter clk3_duty_cycle = 50, parameter clk3_phase_shift = "0",
                parameter clk4_duty_cycle = 50, parameter clk4_phase_shift = "0")
               (input [1:0] inclk, input areset, input pfdena, input clkswitch, input fbin,
                output [width_clock-1:0] clk, output locked, output fbout, output activeclock);
    wire ref = inclk[0];
    localparam real INP = inclk0_input_frequency / 1000.0;
    (* OPENCDC_CLOCK_PIN = "clk[0]", OPENCDC_CLOCK_RATIO = (1.0 * clk0_divide_by) / clk0_multiply_by,
       OPENCDC_CLOCK_INPUT_PERIOD = INP, OPENCDC_CLOCK_QUARTUS_SUFFIX = "auto_generated|pll1|clk[0]" *) reg c0 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "clk[1]", OPENCDC_CLOCK_RATIO = (1.0 * clk1_divide_by) / clk1_multiply_by,
       OPENCDC_CLOCK_INPUT_PERIOD = INP, OPENCDC_CLOCK_QUARTUS_SUFFIX = "auto_generated|pll1|clk[1]" *) reg c1 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "clk[2]", OPENCDC_CLOCK_RATIO = (1.0 * clk2_divide_by) / clk2_multiply_by,
       OPENCDC_CLOCK_INPUT_PERIOD = INP, OPENCDC_CLOCK_QUARTUS_SUFFIX = "auto_generated|pll1|clk[2]" *) reg c2 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "clk[3]", OPENCDC_CLOCK_RATIO = (1.0 * clk3_divide_by) / clk3_multiply_by,
       OPENCDC_CLOCK_INPUT_PERIOD = INP, OPENCDC_CLOCK_QUARTUS_SUFFIX = "auto_generated|pll1|clk[3]" *) reg c3 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "clk[4]", OPENCDC_CLOCK_RATIO = (1.0 * clk4_divide_by) / clk4_multiply_by,
       OPENCDC_CLOCK_INPUT_PERIOD = INP, OPENCDC_CLOCK_QUARTUS_SUFFIX = "auto_generated|pll1|clk[4]" *) reg c4 = 1'b0;
    always @(posedge ref) begin c0 <= ~c0; c1 <= ~c1; c2 <= ~c2; c3 <= ~c3; c4 <= ~c4; end
    wire [4:0] all = {c4, c3, c2, c1, c0};
    assign clk = all[width_clock-1:0];
    opencdc_async_source u_locked (locked);
    assign fbout = ref;
    assign activeclock = 1'b0;
endmodule

// altera_pll (Quartus Prime): frequencies are strings such as "200.000000 MHz";
// the clock analysis derives each output period from them.
module altera_pll #(parameter fractional_vco_multiplier = "false",
                    parameter reference_clock_frequency = "100.0 MHz", parameter operation_mode = "direct",
                    parameter integer number_of_clocks = 1,
                    parameter output_clock_frequency0 = "100.0 MHz", parameter output_clock_frequency1 = "100.0 MHz",
                    parameter output_clock_frequency2 = "100.0 MHz", parameter output_clock_frequency3 = "100.0 MHz",
                    parameter output_clock_frequency4 = "100.0 MHz", parameter output_clock_frequency5 = "100.0 MHz",
                    parameter output_clock_frequency6 = "100.0 MHz", parameter output_clock_frequency7 = "100.0 MHz",
                    parameter phase_shift0 = "0 ps", parameter duty_cycle0 = 50, parameter pll_type = "General",
                    parameter pll_subtype = "General")
                   (input refclk, input rst, input fbclk, output [number_of_clocks-1:0] outclk, output locked,
                    output fboutclk);
    (* OPENCDC_CLOCK_PIN = "outclk[0]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency0,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[0].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o0 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[1]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency1,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[1].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o1 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[2]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency2,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[2].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o2 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[3]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency3,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[3].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o3 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[4]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency4,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[4].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o4 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[5]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency5,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[5].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o5 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[6]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency6,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[6].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o6 = 1'b0;
    (* OPENCDC_CLOCK_PIN = "outclk[7]", OPENCDC_CLOCK_FREQ_IN = reference_clock_frequency,
       OPENCDC_CLOCK_FREQ_OUT = output_clock_frequency7,
       OPENCDC_CLOCK_QUARTUS_SUFFIX = "general[7].gpll~PLL_OUTPUT_COUNTER|divclk" *) reg o7 = 1'b0;
    always @(posedge refclk) begin
        o0 <= ~o0; o1 <= ~o1; o2 <= ~o2; o3 <= ~o3; o4 <= ~o4; o5 <= ~o5; o6 <= ~o6; o7 <= ~o7;
    end
    wire [7:0] all = {o7, o6, o5, o4, o3, o2, o1, o0};
    assign outclk = all[number_of_clocks-1:0];
    opencdc_async_source u_locked (locked);
    assign fboutclk = refclk;
endmodule

// ---- Cyclone V atoms ----

// clock multiplexer: the output carries every input clock
module cyclonev_clkselect #(parameter lpm_type = "cyclonev_clkselect", parameter test_cff = "low")
                          (input [3:0] inclk, input [1:0] clkselect, output outclk);
    assign outclk = clkselect[1] ? (clkselect[0] ? inclk[3] : inclk[2]) : (clkselect[0] ? inclk[1] : inclk[0]);
endmodule

// LUT: combinational
module cyclonev_lcell_comb #(parameter lut_mask = 64'h0, parameter shared_arith = "off",
                             parameter extended_lut = "off", parameter dont_touch = "off",
                             parameter lpm_type = "cyclonev_lcell_comb")
                           (input dataa, input datab, input datac, input datad, input datae, input dataf,
                            input datag, input cin, input sharein, output combout, output sumout, output cout,
                            output shareout);
    wire x = ^{dataa, datab, datac, datad, datae, dataf, datag};
    assign combout = x;
    assign sumout = x ^ cin ^ sharein;
    assign cout = x & cin;
    assign shareout = x;
endmodule

// ---- Cyclone V hard processor system (HPS) atoms: the FPGA side of the HPS ----
// Signals of HPS peripherals (registers written by the processor, interrupts,
// UART, SPI, I2C, debug, boot) have no fabric clock domain.

// the HPS's clocks to the FPGA (h2f_user0..2_clk: their frequencies are set in
// Platform Designer, constrain them with create_clock) and its reset
module cyclonev_hps_interface_clocks_resets (output h2f_user0_clk, output h2f_user1_clk, output h2f_user2_clk,
                                             output h2f_rst_n, output h2f_cold_rst_n, output h2f_pending_rst_req_n,
                                             input f2h_cold_rst_req_n, input f2h_warm_rst_req_n,
                                             input f2h_dbg_rst_req_n, input f2h_pending_rst_ack,
                                             input f2h_periph_ref_clk, input f2h_sdram_ref_clk);
    (* OPENCDC_CLOCK_PIN = "h2f_user0_clk" *) wire user0_src;
    opencdc_async_source u_user0 (user0_src);
    (* OPENCDC_CLOCK_PIN = "h2f_user1_clk" *) wire user1_src;
    opencdc_async_source u_user1 (user1_src);
    (* OPENCDC_CLOCK_PIN = "h2f_user2_clk" *) wire user2_src;
    opencdc_async_source u_user2 (user2_src);
    assign h2f_user0_clk = user0_src;
    assign h2f_user1_clk = user1_src;
    assign h2f_user2_clk = user2_src;
    opencdc_async_source u_rst (h2f_rst_n);
    opencdc_async_source u_cold (h2f_cold_rst_n);
    opencdc_async_source u_pending (h2f_pending_rst_req_n);
endmodule

// general-purpose registers between the processor and the FPGA
module cyclonev_hps_interface_mpu_general_purpose (input [31:0] gp_in, output [31:0] gp_out);
    wire s;
    opencdc_async_source u_gp (s);
    assign gp_out = {32{s}};
endmodule

// FPGA-to-HPS interrupts: read by the processor
module cyclonev_hps_interface_interrupts (input [63:0] irq, output h2f_gic_cnt);
    opencdc_async_source u_cnt (h2f_gic_cnt);
endmodule

module cyclonev_hps_interface_peripheral_uart (input ri, input dsr, input dcd, input cts, input rxd,
                                               output dtr, output rts, output txd, output out1_n, output out2_n);
    opencdc_async_source u_dtr (dtr);
    opencdc_async_source u_rts (rts);
    opencdc_async_source u_txd (txd);
    opencdc_async_source u_out1 (out1_n);
    opencdc_async_source u_out2 (out2_n);
endmodule

// SPI master: sclk_out is a clock (the HPS's SPI clock)
module cyclonev_hps_interface_peripheral_spi_master (input rxd, input ss_in_n, output sclk_out, output txd,
                                                     output ss_0_n, output ss_1_n, output ss_2_n, output ss_3_n,
                                                     output ssi_oe_n);
    (* OPENCDC_CLOCK_PIN = "sclk_out" *) wire sclk_src;
    opencdc_async_source u_sclk (sclk_src);
    assign sclk_out = sclk_src;
    opencdc_async_source u_txd (txd);
    opencdc_async_source u_ss0 (ss_0_n);
    opencdc_async_source u_ss1 (ss_1_n);
    opencdc_async_source u_ss2 (ss_2_n);
    opencdc_async_source u_ss3 (ss_3_n);
    opencdc_async_source u_oe (ssi_oe_n);
endmodule

// I2C: out_clk drives SCL (a clock), out_data drives SDA
module cyclonev_hps_interface_peripheral_i2c (input scl, input sda, output out_clk, output out_data);
    (* OPENCDC_CLOCK_PIN = "out_clk" *) wire scl_src;
    opencdc_async_source u_scl (scl_src);
    assign out_clk = scl_src;
    opencdc_async_source u_sda (out_data);
endmodule

module cyclonev_hps_interface_dbg_apb (input DBG_APB_DISABLE, input P_CLK_EN, input P_CLK, input P_RESET_N,
                                       input [31:0] P_RDATA, input P_READY, input P_SLV_ERR, output [17:0] P_ADDR,
                                       output P_ADDR_31, output P_SEL, output P_ENABLE, output P_WRITE,
                                       output [31:0] P_WDATA);
    wire s;
    opencdc_async_source u_s (s);
    assign P_ADDR = {18{s}};
    assign P_ADDR_31 = s;
    assign P_SEL = s;
    assign P_ENABLE = s;
    assign P_WRITE = s;
    assign P_WDATA = {32{s}};
endmodule

module cyclonev_hps_interface_tpiu_trace (input traceclk_ctl, input traceclkin, output [31:0] trace_data);
    wire s;
    opencdc_async_source u_s (s);
    assign trace_data = {32{s}};
endmodule

module cyclonev_hps_interface_boot_from_fpga (input boot_from_fpga_ready, input boot_from_fpga_on_failure,
                                              input bsel_en, input csel_en, input [1:0] csel, input [2:0] bsel);
endmodule

// HPS-to-FPGA bridge (the HPS is the AXI master): every port in clk
module cyclonev_hps_interface_hps2fpga (
    input clk,
    output [11:0] awid,
    output [29:0] awaddr,
    output [3:0] awlen,
    output [2:0] awsize,
    output [1:0] awburst,
    output [1:0] awlock,
    output [3:0] awcache,
    output [2:0] awprot,
    output awvalid,
    input awready,
    output [11:0] arid,
    output [29:0] araddr,
    output [3:0] arlen,
    output [2:0] arsize,
    output [1:0] arburst,
    output [1:0] arlock,
    output [3:0] arcache,
    output [2:0] arprot,
    output arvalid,
    input arready,
    output [11:0] wid,
    output [127:0] wdata,
    output [15:0] wstrb,
    output wlast,
    output wvalid,
    input wready,
    input [11:0] bid,
    input [1:0] bresp,
    input bvalid,
    output bready,
    input [11:0] rid,
    input [127:0] rdata,
    input [1:0] rresp,
    input rlast,
    input rvalid,
    output rready,
    input [1:0] port_size_config);
    reg d0;
    always @(posedge clk) d0 <= ^{awready, arready, wready, bid, bresp, bvalid, rid, rdata, rresp, rlast, rvalid};
    reg [11:0] awid_q;
    always @(posedge clk) awid_q <= {12{d0}};
    assign awid = awid_q;
    reg [29:0] awaddr_q;
    always @(posedge clk) awaddr_q <= {30{d0}};
    assign awaddr = awaddr_q;
    reg [3:0] awlen_q;
    always @(posedge clk) awlen_q <= {4{d0}};
    assign awlen = awlen_q;
    reg [2:0] awsize_q;
    always @(posedge clk) awsize_q <= {3{d0}};
    assign awsize = awsize_q;
    reg [1:0] awburst_q;
    always @(posedge clk) awburst_q <= {2{d0}};
    assign awburst = awburst_q;
    reg [1:0] awlock_q;
    always @(posedge clk) awlock_q <= {2{d0}};
    assign awlock = awlock_q;
    reg [3:0] awcache_q;
    always @(posedge clk) awcache_q <= {4{d0}};
    assign awcache = awcache_q;
    reg [2:0] awprot_q;
    always @(posedge clk) awprot_q <= {3{d0}};
    assign awprot = awprot_q;
    reg [0:0] awvalid_q;
    always @(posedge clk) awvalid_q <= {1{d0}};
    assign awvalid = awvalid_q;
    reg [11:0] arid_q;
    always @(posedge clk) arid_q <= {12{d0}};
    assign arid = arid_q;
    reg [29:0] araddr_q;
    always @(posedge clk) araddr_q <= {30{d0}};
    assign araddr = araddr_q;
    reg [3:0] arlen_q;
    always @(posedge clk) arlen_q <= {4{d0}};
    assign arlen = arlen_q;
    reg [2:0] arsize_q;
    always @(posedge clk) arsize_q <= {3{d0}};
    assign arsize = arsize_q;
    reg [1:0] arburst_q;
    always @(posedge clk) arburst_q <= {2{d0}};
    assign arburst = arburst_q;
    reg [1:0] arlock_q;
    always @(posedge clk) arlock_q <= {2{d0}};
    assign arlock = arlock_q;
    reg [3:0] arcache_q;
    always @(posedge clk) arcache_q <= {4{d0}};
    assign arcache = arcache_q;
    reg [2:0] arprot_q;
    always @(posedge clk) arprot_q <= {3{d0}};
    assign arprot = arprot_q;
    reg [0:0] arvalid_q;
    always @(posedge clk) arvalid_q <= {1{d0}};
    assign arvalid = arvalid_q;
    reg [11:0] wid_q;
    always @(posedge clk) wid_q <= {12{d0}};
    assign wid = wid_q;
    reg [127:0] wdata_q;
    always @(posedge clk) wdata_q <= {128{d0}};
    assign wdata = wdata_q;
    reg [15:0] wstrb_q;
    always @(posedge clk) wstrb_q <= {16{d0}};
    assign wstrb = wstrb_q;
    reg [0:0] wlast_q;
    always @(posedge clk) wlast_q <= {1{d0}};
    assign wlast = wlast_q;
    reg [0:0] wvalid_q;
    always @(posedge clk) wvalid_q <= {1{d0}};
    assign wvalid = wvalid_q;
    reg [0:0] bready_q;
    always @(posedge clk) bready_q <= {1{d0}};
    assign bready = bready_q;
    reg [0:0] rready_q;
    always @(posedge clk) rready_q <= {1{d0}};
    assign rready = rready_q;
endmodule

// lightweight HPS-to-FPGA bridge: every port in clk
module cyclonev_hps_interface_hps2fpga_light_weight (
    input clk,
    output [11:0] awid,
    output [20:0] awaddr,
    output [3:0] awlen,
    output [2:0] awsize,
    output [1:0] awburst,
    output [1:0] awlock,
    output [3:0] awcache,
    output [2:0] awprot,
    output awvalid,
    input awready,
    output [11:0] arid,
    output [20:0] araddr,
    output [3:0] arlen,
    output [2:0] arsize,
    output [1:0] arburst,
    output [1:0] arlock,
    output [3:0] arcache,
    output [2:0] arprot,
    output arvalid,
    input arready,
    output [11:0] wid,
    output [31:0] wdata,
    output [3:0] wstrb,
    output wlast,
    output wvalid,
    input wready,
    input [11:0] bid,
    input [1:0] bresp,
    input bvalid,
    output bready,
    input [11:0] rid,
    input [31:0] rdata,
    input [1:0] rresp,
    input rlast,
    input rvalid,
    output rready);
    reg d0;
    always @(posedge clk) d0 <= ^{awready, arready, wready, bid, bresp, bvalid, rid, rdata, rresp, rlast, rvalid};
    reg [11:0] awid_q;
    always @(posedge clk) awid_q <= {12{d0}};
    assign awid = awid_q;
    reg [20:0] awaddr_q;
    always @(posedge clk) awaddr_q <= {21{d0}};
    assign awaddr = awaddr_q;
    reg [3:0] awlen_q;
    always @(posedge clk) awlen_q <= {4{d0}};
    assign awlen = awlen_q;
    reg [2:0] awsize_q;
    always @(posedge clk) awsize_q <= {3{d0}};
    assign awsize = awsize_q;
    reg [1:0] awburst_q;
    always @(posedge clk) awburst_q <= {2{d0}};
    assign awburst = awburst_q;
    reg [1:0] awlock_q;
    always @(posedge clk) awlock_q <= {2{d0}};
    assign awlock = awlock_q;
    reg [3:0] awcache_q;
    always @(posedge clk) awcache_q <= {4{d0}};
    assign awcache = awcache_q;
    reg [2:0] awprot_q;
    always @(posedge clk) awprot_q <= {3{d0}};
    assign awprot = awprot_q;
    reg [0:0] awvalid_q;
    always @(posedge clk) awvalid_q <= {1{d0}};
    assign awvalid = awvalid_q;
    reg [11:0] arid_q;
    always @(posedge clk) arid_q <= {12{d0}};
    assign arid = arid_q;
    reg [20:0] araddr_q;
    always @(posedge clk) araddr_q <= {21{d0}};
    assign araddr = araddr_q;
    reg [3:0] arlen_q;
    always @(posedge clk) arlen_q <= {4{d0}};
    assign arlen = arlen_q;
    reg [2:0] arsize_q;
    always @(posedge clk) arsize_q <= {3{d0}};
    assign arsize = arsize_q;
    reg [1:0] arburst_q;
    always @(posedge clk) arburst_q <= {2{d0}};
    assign arburst = arburst_q;
    reg [1:0] arlock_q;
    always @(posedge clk) arlock_q <= {2{d0}};
    assign arlock = arlock_q;
    reg [3:0] arcache_q;
    always @(posedge clk) arcache_q <= {4{d0}};
    assign arcache = arcache_q;
    reg [2:0] arprot_q;
    always @(posedge clk) arprot_q <= {3{d0}};
    assign arprot = arprot_q;
    reg [0:0] arvalid_q;
    always @(posedge clk) arvalid_q <= {1{d0}};
    assign arvalid = arvalid_q;
    reg [11:0] wid_q;
    always @(posedge clk) wid_q <= {12{d0}};
    assign wid = wid_q;
    reg [31:0] wdata_q;
    always @(posedge clk) wdata_q <= {32{d0}};
    assign wdata = wdata_q;
    reg [3:0] wstrb_q;
    always @(posedge clk) wstrb_q <= {4{d0}};
    assign wstrb = wstrb_q;
    reg [0:0] wlast_q;
    always @(posedge clk) wlast_q <= {1{d0}};
    assign wlast = wlast_q;
    reg [0:0] wvalid_q;
    always @(posedge clk) wvalid_q <= {1{d0}};
    assign wvalid = wvalid_q;
    reg [0:0] bready_q;
    always @(posedge clk) bready_q <= {1{d0}};
    assign bready = bready_q;
    reg [0:0] rready_q;
    always @(posedge clk) rready_q <= {1{d0}};
    assign rready = rready_q;
endmodule

// FPGA-to-HPS bridge (the FPGA is the AXI master): every port in clk
module cyclonev_hps_interface_fpga2hps (
    input clk,
    input [7:0] awid,
    input [31:0] awaddr,
    input [3:0] awlen,
    input [2:0] awsize,
    input [1:0] awburst,
    input [1:0] awlock,
    input [3:0] awcache,
    input [2:0] awprot,
    input awvalid,
    output awready,
    input [4:0] awuser,
    input [7:0] arid,
    input [31:0] araddr,
    input [3:0] arlen,
    input [2:0] arsize,
    input [1:0] arburst,
    input [1:0] arlock,
    input [3:0] arcache,
    input [2:0] arprot,
    input arvalid,
    output arready,
    input [4:0] aruser,
    input [7:0] wid,
    input [127:0] wdata,
    input [15:0] wstrb,
    input wlast,
    input wvalid,
    output wready,
    output [7:0] bid,
    output [1:0] bresp,
    output bvalid,
    input bready,
    output [7:0] rid,
    output [127:0] rdata,
    output [1:0] rresp,
    output rlast,
    output rvalid,
    input rready,
    input [1:0] port_size_config);
    reg d0;
    always @(posedge clk) d0 <= ^{awid, awaddr, awlen, awsize, awburst, awlock, awcache, awprot, awvalid, awuser, arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arvalid, aruser, wid, wdata, wstrb, wlast, wvalid, bready, rready};
    reg [0:0] awready_q;
    always @(posedge clk) awready_q <= {1{d0}};
    assign awready = awready_q;
    reg [0:0] arready_q;
    always @(posedge clk) arready_q <= {1{d0}};
    assign arready = arready_q;
    reg [0:0] wready_q;
    always @(posedge clk) wready_q <= {1{d0}};
    assign wready = wready_q;
    reg [7:0] bid_q;
    always @(posedge clk) bid_q <= {8{d0}};
    assign bid = bid_q;
    reg [1:0] bresp_q;
    always @(posedge clk) bresp_q <= {2{d0}};
    assign bresp = bresp_q;
    reg [0:0] bvalid_q;
    always @(posedge clk) bvalid_q <= {1{d0}};
    assign bvalid = bvalid_q;
    reg [7:0] rid_q;
    always @(posedge clk) rid_q <= {8{d0}};
    assign rid = rid_q;
    reg [127:0] rdata_q;
    always @(posedge clk) rdata_q <= {128{d0}};
    assign rdata = rdata_q;
    reg [1:0] rresp_q;
    always @(posedge clk) rresp_q <= {2{d0}};
    assign rresp = rresp_q;
    reg [0:0] rlast_q;
    always @(posedge clk) rlast_q <= {1{d0}};
    assign rlast = rlast_q;
    reg [0:0] rvalid_q;
    always @(posedge clk) rvalid_q <= {1{d0}};
    assign rvalid = rvalid_q;
endmodule

// FPGA-to-SDRAM ports: command ports in cmd_port_clk_N, read data in rd_clk_N, write data in wr_clk_N
module cyclonev_hps_interface_fpga2sdram (
    input cmd_port_clk_0,
    input cmd_port_clk_1,
    input cmd_port_clk_2,
    input cmd_port_clk_3,
    input cmd_port_clk_4,
    input cmd_port_clk_5,
    input rd_clk_0,
    input wr_clk_0,
    input rd_clk_1,
    input wr_clk_1,
    input rd_clk_2,
    input wr_clk_2,
    input rd_clk_3,
    input wr_clk_3,
    input [59:0] cmd_data_0,
    input cmd_valid_0,
    output cmd_ready_0,
    input wrack_ready_0,
    output wrack_valid_0,
    output [7:0] wrack_data_0,
    input [59:0] cmd_data_1,
    input cmd_valid_1,
    output cmd_ready_1,
    input wrack_ready_1,
    output wrack_valid_1,
    output [7:0] wrack_data_1,
    input [59:0] cmd_data_2,
    input cmd_valid_2,
    output cmd_ready_2,
    input wrack_ready_2,
    output wrack_valid_2,
    output [7:0] wrack_data_2,
    input [59:0] cmd_data_3,
    input cmd_valid_3,
    output cmd_ready_3,
    input wrack_ready_3,
    output wrack_valid_3,
    output [7:0] wrack_data_3,
    input [59:0] cmd_data_4,
    input cmd_valid_4,
    output cmd_ready_4,
    input wrack_ready_4,
    output wrack_valid_4,
    output [7:0] wrack_data_4,
    input [59:0] cmd_data_5,
    input cmd_valid_5,
    output cmd_ready_5,
    input wrack_ready_5,
    output wrack_valid_5,
    output [7:0] wrack_data_5,
    input rd_ready_0,
    output [63:0] rd_data_0,
    output rd_valid_0,
    input [89:0] wr_data_0,
    input wr_valid_0,
    output wr_ready_0,
    input rd_ready_1,
    output [63:0] rd_data_1,
    output rd_valid_1,
    input [89:0] wr_data_1,
    input wr_valid_1,
    output wr_ready_1,
    input rd_ready_2,
    output [63:0] rd_data_2,
    output rd_valid_2,
    input [89:0] wr_data_2,
    input wr_valid_2,
    output wr_ready_2,
    input rd_ready_3,
    output [63:0] rd_data_3,
    output rd_valid_3,
    input [89:0] wr_data_3,
    input wr_valid_3,
    output wr_ready_3,
    input [15:0] cfg_rfifo_cport_map,
    input [15:0] cfg_wfifo_cport_map,
    input [11:0] cfg_port_width,
    input [5:0] cfg_axi_mm_select,
    input [17:0] cfg_cport_rfifo_map,
    input [11:0] cfg_cport_type,
    input [17:0] cfg_cport_wfifo_map);
    reg d0;
    always @(posedge cmd_port_clk_0) d0 <= ^{cmd_data_0, cmd_valid_0, wrack_ready_0};
    reg [0:0] cmd_ready_0_q;
    always @(posedge cmd_port_clk_0) cmd_ready_0_q <= {1{d0}};
    assign cmd_ready_0 = cmd_ready_0_q;
    reg [0:0] wrack_valid_0_q;
    always @(posedge cmd_port_clk_0) wrack_valid_0_q <= {1{d0}};
    assign wrack_valid_0 = wrack_valid_0_q;
    reg [7:0] wrack_data_0_q;
    always @(posedge cmd_port_clk_0) wrack_data_0_q <= {8{d0}};
    assign wrack_data_0 = wrack_data_0_q;
    reg d1;
    always @(posedge cmd_port_clk_1) d1 <= ^{cmd_data_1, cmd_valid_1, wrack_ready_1};
    reg [0:0] cmd_ready_1_q;
    always @(posedge cmd_port_clk_1) cmd_ready_1_q <= {1{d1}};
    assign cmd_ready_1 = cmd_ready_1_q;
    reg [0:0] wrack_valid_1_q;
    always @(posedge cmd_port_clk_1) wrack_valid_1_q <= {1{d1}};
    assign wrack_valid_1 = wrack_valid_1_q;
    reg [7:0] wrack_data_1_q;
    always @(posedge cmd_port_clk_1) wrack_data_1_q <= {8{d1}};
    assign wrack_data_1 = wrack_data_1_q;
    reg d2;
    always @(posedge cmd_port_clk_2) d2 <= ^{cmd_data_2, cmd_valid_2, wrack_ready_2};
    reg [0:0] cmd_ready_2_q;
    always @(posedge cmd_port_clk_2) cmd_ready_2_q <= {1{d2}};
    assign cmd_ready_2 = cmd_ready_2_q;
    reg [0:0] wrack_valid_2_q;
    always @(posedge cmd_port_clk_2) wrack_valid_2_q <= {1{d2}};
    assign wrack_valid_2 = wrack_valid_2_q;
    reg [7:0] wrack_data_2_q;
    always @(posedge cmd_port_clk_2) wrack_data_2_q <= {8{d2}};
    assign wrack_data_2 = wrack_data_2_q;
    reg d3;
    always @(posedge cmd_port_clk_3) d3 <= ^{cmd_data_3, cmd_valid_3, wrack_ready_3};
    reg [0:0] cmd_ready_3_q;
    always @(posedge cmd_port_clk_3) cmd_ready_3_q <= {1{d3}};
    assign cmd_ready_3 = cmd_ready_3_q;
    reg [0:0] wrack_valid_3_q;
    always @(posedge cmd_port_clk_3) wrack_valid_3_q <= {1{d3}};
    assign wrack_valid_3 = wrack_valid_3_q;
    reg [7:0] wrack_data_3_q;
    always @(posedge cmd_port_clk_3) wrack_data_3_q <= {8{d3}};
    assign wrack_data_3 = wrack_data_3_q;
    reg d4;
    always @(posedge cmd_port_clk_4) d4 <= ^{cmd_data_4, cmd_valid_4, wrack_ready_4};
    reg [0:0] cmd_ready_4_q;
    always @(posedge cmd_port_clk_4) cmd_ready_4_q <= {1{d4}};
    assign cmd_ready_4 = cmd_ready_4_q;
    reg [0:0] wrack_valid_4_q;
    always @(posedge cmd_port_clk_4) wrack_valid_4_q <= {1{d4}};
    assign wrack_valid_4 = wrack_valid_4_q;
    reg [7:0] wrack_data_4_q;
    always @(posedge cmd_port_clk_4) wrack_data_4_q <= {8{d4}};
    assign wrack_data_4 = wrack_data_4_q;
    reg d5;
    always @(posedge cmd_port_clk_5) d5 <= ^{cmd_data_5, cmd_valid_5, wrack_ready_5};
    reg [0:0] cmd_ready_5_q;
    always @(posedge cmd_port_clk_5) cmd_ready_5_q <= {1{d5}};
    assign cmd_ready_5 = cmd_ready_5_q;
    reg [0:0] wrack_valid_5_q;
    always @(posedge cmd_port_clk_5) wrack_valid_5_q <= {1{d5}};
    assign wrack_valid_5 = wrack_valid_5_q;
    reg [7:0] wrack_data_5_q;
    always @(posedge cmd_port_clk_5) wrack_data_5_q <= {8{d5}};
    assign wrack_data_5 = wrack_data_5_q;
    reg d6;
    always @(posedge rd_clk_0) d6 <= ^{rd_ready_0};
    reg [63:0] rd_data_0_q;
    always @(posedge rd_clk_0) rd_data_0_q <= {64{d6}};
    assign rd_data_0 = rd_data_0_q;
    reg [0:0] rd_valid_0_q;
    always @(posedge rd_clk_0) rd_valid_0_q <= {1{d6}};
    assign rd_valid_0 = rd_valid_0_q;
    reg d7;
    always @(posedge wr_clk_0) d7 <= ^{wr_data_0, wr_valid_0};
    reg [0:0] wr_ready_0_q;
    always @(posedge wr_clk_0) wr_ready_0_q <= {1{d7}};
    assign wr_ready_0 = wr_ready_0_q;
    reg d8;
    always @(posedge rd_clk_1) d8 <= ^{rd_ready_1};
    reg [63:0] rd_data_1_q;
    always @(posedge rd_clk_1) rd_data_1_q <= {64{d8}};
    assign rd_data_1 = rd_data_1_q;
    reg [0:0] rd_valid_1_q;
    always @(posedge rd_clk_1) rd_valid_1_q <= {1{d8}};
    assign rd_valid_1 = rd_valid_1_q;
    reg d9;
    always @(posedge wr_clk_1) d9 <= ^{wr_data_1, wr_valid_1};
    reg [0:0] wr_ready_1_q;
    always @(posedge wr_clk_1) wr_ready_1_q <= {1{d9}};
    assign wr_ready_1 = wr_ready_1_q;
    reg d10;
    always @(posedge rd_clk_2) d10 <= ^{rd_ready_2};
    reg [63:0] rd_data_2_q;
    always @(posedge rd_clk_2) rd_data_2_q <= {64{d10}};
    assign rd_data_2 = rd_data_2_q;
    reg [0:0] rd_valid_2_q;
    always @(posedge rd_clk_2) rd_valid_2_q <= {1{d10}};
    assign rd_valid_2 = rd_valid_2_q;
    reg d11;
    always @(posedge wr_clk_2) d11 <= ^{wr_data_2, wr_valid_2};
    reg [0:0] wr_ready_2_q;
    always @(posedge wr_clk_2) wr_ready_2_q <= {1{d11}};
    assign wr_ready_2 = wr_ready_2_q;
    reg d12;
    always @(posedge rd_clk_3) d12 <= ^{rd_ready_3};
    reg [63:0] rd_data_3_q;
    always @(posedge rd_clk_3) rd_data_3_q <= {64{d12}};
    assign rd_data_3 = rd_data_3_q;
    reg [0:0] rd_valid_3_q;
    always @(posedge rd_clk_3) rd_valid_3_q <= {1{d12}};
    assign rd_valid_3 = rd_valid_3_q;
    reg d13;
    always @(posedge wr_clk_3) d13 <= ^{wr_data_3, wr_valid_3};
    reg [0:0] wr_ready_3_q;
    always @(posedge wr_clk_3) wr_ready_3_q <= {1{d13}};
    assign wr_ready_3 = wr_ready_3_q;
endmodule

// ---- Quartus primitives (buffers and simple storage, as in Quartus' primitive library) ----

module lcell (input in, output out);
    assign out = in;
endmodule


module soft (input in, output out);
    assign out = in;
endmodule


module carry (input in, output out);
    assign out = in;
endmodule

module carry_sum (input sin, input cin, output sout, output cout);
    assign sout = sin;
    assign cout = cin;
endmodule

module cascade (input in, output out);
    assign out = in;
endmodule

module opndrn (input in, output out);
    assign out = in ? 1'bz : 1'b0;
endmodule


module alt_inbuf (input i, output o);
    assign o = i;
endmodule

module alt_outbuf (input i, output o);
    assign o = i;
endmodule

module alt_outbuf_tri (input i, input oe, output o);
    assign o = oe ? i : 1'bz;
endmodule

module alt_iobuf (input i, input oe, output o, inout io);
    assign io = oe ? i : 1'bz;
    assign o = io;
endmodule

// flip-flops with active-low asynchronous clear and preset
module dff (input d, input clk, input clrn, input prn, output reg q);
    always @(posedge clk or negedge clrn or negedge prn)
        if (!clrn) q <= 1'b0;
        else if (!prn) q <= 1'b1;
        else q <= d;
endmodule

module dffe (input d, input clk, input clrn, input prn, input ena, output reg q);
    always @(posedge clk or negedge clrn or negedge prn)
        if (!clrn) q <= 1'b0;
        else if (!prn) q <= 1'b1;
        else if (ena) q <= d;
endmodule

module dffea (input d, input clk, input clrn, input prn, input ena, input adata, input aload, output reg q);
    always @(posedge clk or negedge clrn or negedge prn)
        if (!clrn) q <= 1'b0;
        else if (!prn) q <= 1'b1;
        else if (ena) q <= d;
endmodule

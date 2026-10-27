// Output ports and the clock their set_output_delay names.
//  io_data : registered on clk_io, sampled by the receiver with clk_io  -> fine
//  io_req  : clk_sys request synchronized into clk_io, then output     -> fine
//  sys_flag: registered on clk_sys, but sampled with clk_io            -> reported
//  dbg_led : registered on clk_sys, sampled with clk_io                -> waived (an LED)
module sync_2ff (input clk, input d, output q);
    (* ASYNC_REG = "TRUE" *) reg s1, s2;
    always @(posedge clk) begin
        s1 <= d;
        s2 <= s1;
    end
    assign q = s2;
endmodule

module output_ports (
    input            clk_sys,
    input            clk_io,
    input      [7:0] d,
    input      [7:0] d_io,
    input            req,
    output reg [7:0] io_data,
    output reg       io_req,
    output reg       sys_flag,
    output reg       dbg_led
);
    reg req_sys;
    wire req_io;
    always @(posedge clk_sys) begin
        req_sys  <= req;
        sys_flag <= ^d;
        dbg_led  <= d[0];
    end
    sync_2ff u_req_sync (.clk(clk_io), .d(req_sys), .q(req_io));
    always @(posedge clk_io) begin
        io_data <= d_io;
        io_req  <= req_io;
    end
endmodule

create_clock -name clk_sys -period 10 [get_ports clk_sys]
create_clock -name clk_io -period 8 [get_ports clk_io]
set_clock_groups -asynchronous -group clk_sys -group clk_io
set_input_delay -clock clk_sys 1 [get_ports {d[*] req}]
set_input_delay -clock clk_io 1 [get_ports {d_io[*]}]
# the receiver samples every output with clk_io
set_output_delay -clock clk_io 2 [get_ports {io_data[*] io_req}]
set_output_delay -clock clk_io 2 [get_ports sys_flag]
set_output_delay -clock clk_io 2 [get_ports dbg_led]

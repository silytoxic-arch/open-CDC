create_clock -name clk_in  -period 10 [get_ports clk_in]
create_clock -name clk_ext -period 8  [get_ports clk_ext]
set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks clk_in] -group [get_clocks clk_ext]

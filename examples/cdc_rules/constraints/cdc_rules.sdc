create_clock -name clk_a -period 10.0 [get_ports clk_a]
create_clock -name clk_b -period 7.5  [get_ports clk_b]
create_clock -name clk_c -period 4.0  [get_ports clk_c]

# clk_a and clk_b are independent. clk_c is deliberately left unrelated
# so that crossings into it are reported by CDC-010.
set_clock_groups -asynchronous -group [get_clocks clk_a] -group [get_clocks clk_b]

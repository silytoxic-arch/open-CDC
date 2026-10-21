# Smoke example clocks (SDC).
create_clock -name clk_a -period 10.000 [get_ports clk_a]
create_clock -name clk_b -period 6.400 -waveform {0 3.2} [get_ports clk_b]

# clk_a and clk_b come from independent oscillators.
set_clock_groups -asynchronous -group [get_clocks clk_a] -group [get_clocks clk_b]

# Timing-only constraints are recorded but not used by CDC analysis.
set_input_delay -clock clk_a 2.0 [get_ports rst_a_n]
set_false_path -from [get_ports rst_b_n]

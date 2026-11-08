create_clock -period "50.0 MHz" [get_ports clk_50]
create_clock -period "33.0 MHz" [get_ports clk_ext]
derive_pll_clocks
# Quartus' names of the PLL clocks
set_clock_groups -exclusive \
   -group [get_clocks {pll|altera_pll_i|general[*].gpll~PLL_OUTPUT_COUNTER|divclk clk_50}] \
   -group [get_clocks {clk_ext}]
# the configuration is static while the counter runs (Quartus: a plain name is
# a collection of keepers)
set_false_path -from {cfg_mode[*]}

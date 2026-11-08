// PLL variation (as the IP parameter editor writes it): 50 MHz in, 100 MHz and 25 MHz out
module demo_pll (input refclk, input rst, output outclk_0, output outclk_1, output locked);
    altera_pll #(
        .fractional_vco_multiplier("false"),
        .reference_clock_frequency("50.0 MHz"),
        .operation_mode("direct"),
        .number_of_clocks(2),
        .output_clock_frequency0("100.000000 MHz"),
        .output_clock_frequency1("25.000000 MHz"),
        .pll_type("General"),
        .pll_subtype("General")
    ) altera_pll_i (
        .rst(rst),
        .outclk({outclk_1, outclk_0}),
        .locked(locked),
        .fboutclk(),
        .fbclk(1'b0),
        .refclk(refclk)
    );
endmodule

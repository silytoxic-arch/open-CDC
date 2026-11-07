// simulation only
module demo_tb;
    reg clk_a = 0, clk_b = 0;
    always #5 clk_a = ~clk_a;
    always #7 clk_b = ~clk_b;
    demo_top dut (.clk_a(clk_a), .clk_b(clk_b), .rst_n(1'b1), .start(1'b1), .busy_b(), .count_b());
endmodule

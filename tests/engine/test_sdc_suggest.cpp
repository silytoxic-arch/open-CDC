// Suggested constraints (stage 13): missing clocks, generated clocks for
// fabric dividers, clock groups for unknown relations; the suggestion read
// back resolves every unknown relation.

#include "../TestUtil.h"
#include "clock/ClockModel.h"
#include "engine/AnalysisSession.h"
#include "engine/SdcSuggest.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace opencdc;

namespace {

std::shared_ptr<const engine::AnalysisResult> analyze(test::TempDir& dir, const std::string& verilog,
                                                      const std::vector<std::string>& sdcs) {
    engine::ProjectConfig cfg;
    cfg.name = "sug";
    cfg.top = "top";
    cfg.sources = {dir.write("t.v", verilog)};
    cfg.sourceLibraries = {""};
    for (std::size_t i = 0; i < sdcs.size(); ++i)
        cfg.constraints.push_back(dir.write("c" + std::to_string(i) + ".sdc", sdcs[i]));
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

std::size_t unknownPairs(const engine::AnalysisResult& r) {
    std::size_t n = 0;
    const auto& cm = *r.clocks;
    for (std::size_t i = 0; i < cm.clocks.size(); ++i)
        for (std::size_t j = i + 1; j < cm.clocks.size(); ++j)
            n += cm.relation(clock::ClockId{static_cast<std::uint32_t>(i)}, clock::ClockId{static_cast<std::uint32_t>(j)}) ==
                         clock::ClockRelation::Unknown
                     ? 1U
                     : 0U;
    return n;
}

constexpr const char* kDividers =
    "module sub(input clk, input d, output reg q); reg [3:0] cnt; always @(posedge clk) cnt <= cnt + 1'b1;\n"
    "  always @(posedge cnt[2]) q <= d; endmodule\n"
    "module top(input clk_a, input clk_b, input d, output reg q, output q2);\n"
    "  reg div = 1'b0; reg a;\n"
    "  always @(posedge clk_a) div <= ~div;\n"
    "  always @(posedge div) a <= d;\n"
    "  always @(posedge clk_b) q <= a;\n"
    "  sub u (.clk(clk_b), .d(a), .q(q2));\n"
    "endmodule\n";

} // namespace

TEST_CASE("Suggested constraints: input clocks, fabric dividers and groups", "[engine][sdc-suggest]") {
    test::TempDir dir;
    auto r = analyze(dir, kDividers, {});
    REQUIRE(r->clocks);
    REQUIRE(unknownPairs(*r) > 0);
    const auto sug = engine::suggestConstraints(*r);
    UNSCOPED_INFO(sug.text);
    CHECK(sug.clocks == 4);
    CHECK(sug.groups == 1);
    CHECK(sug.todos == 2);
    CHECK(sug.text.find("create_clock -name clk_a -period 10.000 [get_ports {clk_a}]") != std::string::npos);
    CHECK(sug.text.find("-source [get_pins {div_reg/C}] -divide_by 2 [get_pins {div_reg/Q}]") != std::string::npos);
    CHECK(sug.text.find("-source [get_pins {u/cnt_reg[2]/C}] -divide_by 8") != std::string::npos);
    CHECK(sug.text.find("set_clock_groups -asynchronous -group [get_clocks -include_generated_clocks {clk_a}] "
                        "-group [get_clocks -include_generated_clocks {clk_b}]") != std::string::npos);

    // read back: every clock is constrained and no relation is unknown
    auto again = analyze(dir, kDividers, {sug.text});
    REQUIRE(again->clocks);
    CHECK(unknownPairs(*again) == 0);
    CHECK(std::ranges::count_if(again->diagnostics, [](const Diagnostic& d) { return d.code.starts_with("CON-"); }) == 0);
    for (const auto& c : again->clocks->clocks) {
        INFO(c.name);
        CHECK((c.kind == clock::ClockKind::Constraint || c.kind == clock::ClockKind::Generated));
        CHECK(c.periodNs.has_value());
    }
    CHECK(engine::suggestConstraints(*again).clocks == 0); // nothing left to suggest
}

TEST_CASE("Suggested constraints: GT clock pins and vendor-derived clocks", "[engine][sdc-suggest]") {
    test::TempDir dir;
    const std::string v =
        "module top(input refclk, input rxp, input rxn, input clk_a, input d, input [31:0] w, output reg q,\n"
        "           output reg [31:0] rx);\n"
        "  wire rxoutclk, c0; wire [63:0] rxdata;\n"
        "  GTXE2_CHANNEL ch (.GTREFCLK0(refclk), .GTXRXP(rxp), .GTXRXN(rxn), .RXOUTCLK(rxoutclk),\n"
        "    .RXUSRCLK2(rxoutclk), .RXDATA(rxdata));\n"
        "  MMCME2_BASE #(.CLKFBOUT_MULT_F(10.0), .CLKOUT0_DIVIDE_F(5.0)) m (.CLKIN1(clk_a), .CLKOUT0(c0),\n"
        "    .RST(1'b0), .PWRDWN(1'b0));\n"
        "  reg a; always @(posedge c0) a <= d;\n"
        "  always @(posedge rxoutclk) begin q <= a; rx <= rxdata[31:0]; end\n"
        "endmodule\n";
    auto r = analyze(dir, v, {"create_clock -name clk_a -period 10 [get_ports clk_a]\n"});
    REQUIRE(r->clocks);
    const auto sug = engine::suggestConstraints(*r);
    UNSCOPED_INFO(sug.text);
    CHECK(sug.text.find("create_clock -name ch_RXOUTCLK -period 10.000 [get_pins {ch/RXOUTCLK}]") != std::string::npos);
    CHECK(sug.text.find("# derived automatically by the vendor tool: m.CLKOUT0 = 5.000 ns (clk_a x 0.5)") !=
          std::string::npos);
    CHECK(sug.text.find("# already constrained: clk_a (10.000 ns)") != std::string::npos);
    CHECK(sug.text.find("-group [get_clocks -include_generated_clocks {clk_a}]") != std::string::npos);
    auto again = analyze(dir, v, {"create_clock -name clk_a -period 10 [get_ports clk_a]\n", sug.text});
    REQUIRE(again->clocks);
    CHECK(unknownPairs(*again) == 0);
}

TEST_CASE("Suggested constraints negative: a fully constrained design needs nothing", "[engine][sdc-suggest]") {
    test::TempDir dir;
    auto r = analyze(dir,
                     "module top(input clk_a, input clk_b, input d, output reg q);\n"
                     "  reg a; always @(posedge clk_a) a <= d; always @(posedge clk_b) q <= a;\n"
                     "endmodule\n",
                     {"create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                      "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                      "set_clock_groups -asynchronous -group clk_a -group clk_b\n"});
    const auto sug = engine::suggestConstraints(*r);
    CHECK(sug.clocks == 0);
    CHECK(sug.groups == 0);
    CHECK(sug.unknownPairs.empty());
    CHECK(sug.text.find("no clock groups needed") != std::string::npos);
}

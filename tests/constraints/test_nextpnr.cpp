#include "constraints/Nextpnr.h"
#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

std::shared_ptr<const engine::AnalysisResult> run(const engine::ProjectConfig& cfg) {
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

std::size_t count(const engine::AnalysisResult& r, std::string_view code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
}

const clock::Clock* clockNamed(const engine::AnalysisResult& r, std::string_view name) {
    for (const auto& c : r.clocks->clocks)
        if (c.name == name)
            return &c;
    return nullptr;
}

} // namespace

TEST_CASE("nextpnr files: the readers recognize .pcf and .py", "[constraints][nextpnr]") {
    using namespace constraints;
    CHECK(isPcfFile("icecam.pcf"));
    CHECK(isPcfFile("BOARD.PCF"));
    CHECK_FALSE(isPcfFile("board.lpf"));
    CHECK(isNextpnrScript("constraints.py"));
    CHECK_FALSE(isNextpnrScript("constraints.pyc"));
}

TEST_CASE("nextpnr files: set_frequency and ctx.addClock become clocks", "[constraints][nextpnr]") {
    using namespace constraints;
    CollectingDiagnosticSink sink;
    const auto pcf = pcfToSdc("set_io clk 35 # board clock\nset_frequency clk 12\nset_frequency u_pll.clk_o 48.5\n"
                              "set_frequency bad\n",
                              "b.pcf", sink);
    CHECK(pcf.find("create_clock -opencdc_physical_net -name {clk} -period 83.3333333333 [get_nets {clk}]") !=
          std::string::npos);
    CHECK(pcf.find("[get_nets {u_pll/clk_o}]") != std::string::npos); // Yosys' '.' between levels
    CHECK(pcf.find("set_io") == std::string::npos);
    // one SDC line per constraint line: diagnostics keep their line numbers
    CHECK(std::ranges::count(pcf, '\n') == 5);
    auto d = sink.take();
    REQUIRE(d.size() == 1); // negative: set_frequency without a frequency
    CHECK(d[0].code == "CON-NEXTPNR-PARSE");
    CHECK(d[0].location.line == 4);

    const auto py = nextpnrScriptToSdc("ctx.addClock(\"csi_rx_i.dphy_clk\", 96)\nctx.addClock('video_clk', 24)  # px\n"
                                       "# a comment\nctx.addClock(name, 12)\nprint(ctx)\n",
                                       "c.py", sink);
    CHECK(py.find("-name {csi_rx_i.dphy_clk} -period 10.4166666667 [get_nets {csi_rx_i/dphy_clk}]") != std::string::npos);
    CHECK(py.find("-name {video_clk} -period 41.6666666667") != std::string::npos);
    d = sink.take();
    // negative: a call with a variable is reported, other statements are counted
    REQUIRE(d.size() == 2);
    CHECK(d[0].code == "CON-NEXTPNR-PARSE");
    CHECK(d[0].location.line == 4);
    CHECK(d[1].code == "CON-NEXTPNR-IGNORED");
    CHECK(d[1].message.find("1 statement(s)") != std::string::npos);
}

TEST_CASE("nextpnr files: clocks on the physical nets", "[constraints][nextpnr]") {
    // the CSI-2 receiver's iCEBreaker test: ctx.addClock("uart_i.sys_clk_i", 12) names the board clock
    // inside the UART, ctx.addClock("video_clk", 24) the output of a divider
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "ice";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", "module uart(input sys_clk_i, input d, output reg q); always @(posedge sys_clk_i) q <= d;"
                                      " endmodule\n"
                                      "module top(input clk12, input dphy_clk, input d, output q, output reg v, output reg w);\n"
                                      "  reg [1:0] div = 0; always @(posedge dphy_clk) div <= div + 1'b1;\n"
                                      "  wire video_clk = div[1];\n"
                                      "  always @(posedge video_clk) v <= d;\n"
                                      "  reg a; always @(posedge clk12) a <= d;\n"
                                      "  uart uart_i(.sys_clk_i(clk12), .d(a), .q(q));\n"
                                      "  always @(posedge dphy_clk) w <= v;\n"
                                      "endmodule\n")};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.py", "ctx.addClock(\"dphy_clk\", 96)\nctx.addClock(\"video_clk\", 24)\n"
                                         "ctx.addClock(\"uart_i.sys_clk_i\", 12)\n"),
                       dir.write("p.pcf", "set_io clk12 35\nset_io dphy_clk 32\n")};
    auto r = run(cfg);
    REQUIRE(r->clocks);
    REQUIRE(r->cdc);
    // the UART's clock is the board clock: one clock for clk12 and the UART, no crossing between them
    const auto* uart = clockNamed(*r, "uart_i.sys_clk_i");
    REQUIRE(uart);
    CHECK(uart->registers() == 2);
    CHECK(clockNamed(*r, "clk12") == nullptr);
    // the divider's output stays derived from dphy_clk (related: no crossing to w)
    CHECK(clockNamed(*r, "video_clk") == nullptr);
    CHECK(count(*r, "CON-NEXTPNR-DERIVED") == 1);
    CHECK(r->cdc->stats.paths == 0);
    // set_io is placement: nothing to report
    CHECK(count(*r, "CON-UNKNOWN-COMMAND") == 0);
    // negative: a net that does not exist
    engine::ProjectConfig bad = cfg;
    bad.constraints = {dir.write("b.py", "ctx.addClock(\"no_such_net\", 50)\n")};
    auto b = run(bad);
    CHECK(count(*b, "CON-UNMATCHED") == 1);
}

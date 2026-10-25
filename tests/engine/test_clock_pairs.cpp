// Crossings summarized per clock pair (stage 16): counts in the CLI, the
// HTML report and the JSON result.

#include "../TestUtil.h"
#include "engine/AnalysisSession.h"
#include "engine/ClockPairs.h"
#include "engine/Report.h"
#include "engine/ResultJson.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace opencdc;

TEST_CASE("Clock pair summary of the CDC paths", "[engine][pairs]") {
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "pairs";
    cfg.top = "top";
    cfg.sources = {dir.write("t.v",
                             "module top(input clk_a, input clk_b, input clk_c, input d, output reg q1, output reg q2,\n"
                             "           output reg q3);\n"
                             "  reg a, b1, b2, c1;\n"
                             "  always @(posedge clk_a) a <= d;\n"
                             "  always @(posedge clk_b) begin b1 <= a; b2 <= b1; q1 <= b2; q2 <= a; end\n"
                             "  always @(posedge clk_c) begin c1 <= a; q3 <= c1; end\n"
                             "endmodule\n")};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                                          "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                                          "create_clock -name clk_c -period 5 [get_ports clk_c]\n"
                                          "set_clock_groups -asynchronous -group clk_a -group clk_b\n")};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto r = session.run(cfg, progress, cancel);
    REQUIRE(r->cdc);
    const auto pairs = engine::summarizeClockPairs(*r);
    REQUIRE(pairs.size() == 2);
    // busiest pair first: clk_a -> clk_b has the synchronizer and the unsynchronized capture
    CHECK(pairs[0].srcClock == "clk_a");
    CHECK(pairs[0].dstClock == "clk_b");
    CHECK(pairs[0].relation == "asynchronous");
    CHECK(pairs[0].paths == 2);
    CHECK(pairs[0].synchronized == 1);
    CHECK(pairs[0].violations + pairs[0].potentialRisk == 1);
    CHECK(pairs[1].dstClock == "clk_c");
    CHECK(pairs[1].relation == "unknown");
    CHECK(pairs[1].needsReview == 1);

    const std::string html = engine::toHtmlReport(*r);
    CHECK(html.find("Crossings by clock pair") != std::string::npos);
    const std::string json = engine::toJson(*r);
    CHECK(json.find("\"clockPairs\"") != std::string::npos);
    CHECK(json.find("\"needsReview\": 1") != std::string::npos);

    // negative: no CDC analysis, no pairs
    engine::AnalysisResult empty;
    CHECK(engine::summarizeClockPairs(empty).empty());
}

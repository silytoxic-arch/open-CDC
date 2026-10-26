// Coding patterns of the Analog Devices HDL library, end to end.

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

// up_xfer_status style toggle synchronizer: the destination flops have a
// synchronous reset written as `if (rstn == 1'b0)`.
std::string toggleSync(std::string_view resetCond) {
    return "module top(input a_clk, input b_clk, input b_rstn, input d, output reg q);\n"
           "  reg a_toggle = 1'b0;\n"
           "  reg b_m1, b_m2, b_m3;\n"
           "  always @(posedge a_clk) if (d) a_toggle <= ~a_toggle;\n"
           "  always @(posedge b_clk) begin\n"
           "    if (" + std::string(resetCond) + ") begin\n"
           "      b_m1 <= 1'b0; b_m2 <= 1'b0; b_m3 <= 1'b0; q <= 1'b0;\n"
           "    end else begin\n"
           "      b_m1 <= a_toggle; b_m2 <= b_m1; b_m3 <= b_m2; q <= b_m3 ^ b_m2;\n"
           "    end\n"
           "  end\n"
           "endmodule\n";
}

constexpr const char* kSdc = "create_clock -name a -period 10 [get_ports a_clk]\n"
                             "create_clock -name b -period 7 [get_ports b_clk]\n"
                             "set_input_delay -clock a 1 [get_ports d]\n"
                             "set_input_delay -clock b 1 [get_ports b_rstn]\n";

} // namespace

TEST_CASE("ADI: toggle synchronizer with an `== 1'b0` synchronous reset is synchronized", "[engine][adi]") {
    for (const char* cond : {"b_rstn == 1'b0", "!b_rstn", "b_rstn != 1'b1"}) {
        INFO(cond);
        test::TempDir dir;
        engine::ProjectConfig cfg;
        cfg.name = "adi";
        cfg.top = "top";
        cfg.sources = {dir.write("top.v", toggleSync(cond))};
        cfg.sourceLibraries = {""};
        cfg.constraints = {dir.write("c.sdc", kSdc)};
        auto r = run(cfg);
        REQUIRE(r->cdc);
        CHECK(r->cdc->stats.paths == 1);
        CHECK(r->cdc->stats.synchronized == 1);
        CHECK(r->cdc->stats.unsynchronized == 0);
    }
}

TEST_CASE("ADI: an `== 1'b0` synchronous reset does not hide an unsynchronized crossing", "[engine][adi]") {
    // negative: same reset style, but the toggle is also read directly by
    // logic in the destination domain
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "adi_neg";
    cfg.top = "top";
    std::string src = toggleSync("b_rstn == 1'b0");
    const std::string from = "q <= b_m3 ^ b_m2;";
    src.replace(src.find(from), from.size(), "q <= b_m3 ^ a_toggle;");
    cfg.sources = {dir.write("top.v", src)};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", kSdc)};
    auto r = run(cfg);
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.unsynchronized >= 1);
}

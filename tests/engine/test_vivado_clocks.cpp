// Cross-check with Vivado's report_clocks (stage 14): parsing of both report
// formats, matching by source and name, period mismatches, clocks only one
// side knows about.

#include "../TestUtil.h"
#include "engine/AnalysisSession.h"
#include "engine/VivadoClocks.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>

using namespace opencdc;

namespace {

constexpr const char* kDesign =
    "module top(input clk_in, input d, output reg q0, output reg q1, output reg q2);\n"
    "  wire c0, c1, fb;\n"
    "  MMCME2_ADV #(.CLKIN1_PERIOD(10.0), .CLKFBOUT_MULT_F(10.0), .DIVCLK_DIVIDE(1),\n"
    "               .CLKOUT0_DIVIDE_F(5.0), .CLKOUT1_DIVIDE(10))\n"
    "    u_mmcm (.CLKIN1(clk_in), .CLKFBIN(fb), .CLKFBOUT(fb), .CLKOUT0(c0), .CLKOUT1(c1), .RST(1'b0));\n"
    "  reg div = 1'b0;\n"
    "  always @(posedge clk_in) div <= ~div;\n"
    "  always @(posedge c0) q0 <= d;\n"
    "  always @(posedge c1) q1 <= d;\n"
    "  always @(posedge div) q2 <= d;\n"
    "endmodule\n";

constexpr const char* kSdc = "create_clock -name clk_in -period 10.000 [get_ports clk_in]\n";

std::shared_ptr<const engine::AnalysisResult> analyze(test::TempDir& dir, const std::vector<std::string>& reports,
                                                      const char* sdc = kSdc) {
    engine::ProjectConfig cfg;
    cfg.name = "viv";
    cfg.top = "top";
    cfg.sources = {dir.write("t.v", kDesign)};
    cfg.sourceLibraries = {""};
    if (sdc && *sdc)
        cfg.constraints = {dir.write("c.sdc", sdc)};
    for (std::size_t i = 0; i < reports.size(); ++i)
        cfg.vivadoClockReports.push_back(dir.write("clocks" + std::to_string(i) + ".rpt", reports[i]));
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

std::size_t count(const engine::AnalysisResult& r, const std::string& code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
}

const Diagnostic* find(const engine::AnalysisResult& r, const std::string& code) {
    const auto it = std::ranges::find_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; });
    return it == r.diagnostics.end() ? nullptr : &*it;
}

const engine::StageReport* stage(const engine::AnalysisResult& r) {
    const auto it =
        std::ranges::find_if(r.stages, [](const engine::StageReport& s) { return s.name == "Vivado clock report check"; });
    return it == r.stages.end() ? nullptr : &*it;
}

constexpr const char* kHeader = "Copyright 1986-2022 Xilinx, Inc. All Rights Reserved.\n"
                                "| Command      : report_clocks -file clocks.rpt\n\n"
                                "Clock Report\n\n"
                                "Clock     Period(ns)  Waveform(ns)    Attributes  Sources\n";

} // namespace

TEST_CASE("Vivado report_clocks: current table format", "[engine][vivado]") {
    const std::string text = std::string(kHeader) +
                             "clk_in    10.000      {0.000 5.000}   P           {clk_in}\n"
                             "clkout0   5.000       {0.000 2.500}   P,G,A       {u_mmcm/CLKOUT0}\n"
                             "virt      8.000       {0.000 4.000}   V           \n"
                             "two_src   4.000       {0.000 2.000}   P           {a/O b/O}\n"
                             "\n====================================================\n"
                             "Generated Clocks\n";
    std::string error;
    const auto clocks = engine::parseVivadoClockReport(text, error);
    REQUIRE(clocks);
    REQUIRE(clocks->size() == 4);
    CHECK((*clocks)[0].name == "clk_in");
    CHECK_THAT((*clocks)[0].periodNs, Catch::Matchers::WithinAbs(10.0, 1e-9));
    CHECK((*clocks)[0].sources == std::vector<std::string>{"clk_in"});
    CHECK((*clocks)[1].attributes == "P,G,A");
    CHECK((*clocks)[1].line == 8);
    CHECK((*clocks)[2].sources.empty());
    CHECK((*clocks)[3].sources == std::vector<std::string>{"a/O", "b/O"});
}

TEST_CASE("Vivado report_clocks: older table format with generated clock details", "[engine][vivado]") {
    const std::string text = "Clock Report\n\n"
                             "Clock       Waveform(ns)       Period(ns)      Frequency(MHz)\n"
                             "-----       ------------       ----------      --------------\n"
                             "clk_in      {0.000 5.000}      10.000          100.000\n"
                             "clkout0     {0.000 2.500}      5.000           200.000\n"
                             "\n"
                             "Generated Clock   : clkout0\n"
                             "Master Source     : u_mmcm/CLKIN1\n"
                             "Master Clock      : clk_in\n"
                             "Generated Sources : {u_mmcm/CLKOUT0}\n";
    std::string error;
    const auto clocks = engine::parseVivadoClockReport(text, error);
    REQUIRE(clocks);
    REQUIRE(clocks->size() == 2);
    CHECK_THAT((*clocks)[1].periodNs, Catch::Matchers::WithinAbs(5.0, 1e-9));
    CHECK((*clocks)[1].sources == std::vector<std::string>{"u_mmcm/CLKOUT0"});
    CHECK((*clocks)[0].sources.empty());
}

TEST_CASE("Vivado report_timing_summary: clock summary with indented generated clocks", "[engine][vivado]") {
    // Clock Summary section as printed by Vivado (published example, 01signal.com)
    const std::string text = "------------------------------------------------------------------------\n"
                             "| Clock Summary\n| -------------\n"
                             "------------------------------------------------------------------------\n\n"
                             "Clock                  Waveform(ns)         Period(ns)      Frequency(MHz)\n"
                             "-----                  ------------         ----------      --------------\n"
                             "clk_fpga_1             {0.000 5.000}        10.000          100.000\n"
                             "gclk                   {0.000 4.000}        8.000           125.000\n"
                             "  audio_mclk_OBUF      {0.000 41.667}       83.333          12.000\n"
                             "  clk_fb               {0.000 20.000}       40.000          25.000\n"
                             "  vga_clk_ins/clk_fb   {0.000 20.000}       40.000          25.000\n"
                             "  vga_clk_ins/clkout0  {0.000 1.538}        3.077           325.000\n"
                             "  vga_clk_ins/clkout1  {0.000 7.692}        15.385          65.000\n"
                             "  vga_clk_ins/clkout2  {0.000 7.692}        15.385          65.000\n\n\n"
                             "Intra Clock Table\n-----------------\n\n"
                             "Clock     WNS(ns)  TNS(ns)  TNS Failing Endpoints\n"
                             "-----     -------  -------  ---------------------\n"
                             "clk_fpga_1  3.791  0.000  0\n";
    std::string error;
    const auto clocks = engine::parseVivadoClockReport(text, error);
    REQUIRE(clocks);
    REQUIRE(clocks->size() == 8);
    CHECK((*clocks)[5].name == "vga_clk_ins/clkout0");
    CHECK_THAT((*clocks)[5].periodNs, Catch::Matchers::WithinAbs(3.077, 1e-9));
    CHECK_THAT((*clocks)[2].periodNs, Catch::Matchers::WithinAbs(83.333, 1e-9));
}

TEST_CASE("Vivado report_clocks: text without a clock table is rejected", "[engine][vivado]") {
    std::string error;
    CHECK_FALSE(engine::parseVivadoClockReport("Timing summary\nWNS 0.1\n", error));
    CHECK(error.find("no clock table") != std::string::npos);

    test::TempDir dir;
    const auto r = analyze(dir, {"just some text\n"});
    CHECK(count(*r, "VIV-REPORT-READ") == 1);
    REQUIRE(stage(*r));
    CHECK(stage(*r)->status == engine::StageStatus::Failed);
}

TEST_CASE("Vivado clock check: consistent report", "[engine][vivado]") {
    test::TempDir dir;
    const auto r = analyze(dir, {std::string(kHeader) +
                                 "clk_in    10.000      {0.000 5.000}   P           {clk_in}\n"
                                 "clkout0   5.000       {0.000 2.500}   P,G,A       {u_mmcm/CLKOUT0}\n"
                                 "clkout1   10.000      {0.000 5.000}   P,G,A       {u_mmcm/CLKOUT1}\n"
                                 "div_clk   20.000      {0.000 10.000}  P,G         {div_reg/Q}\n"
                                 "virt      8.000       {0.000 4.000}   V           \n"});
    CHECK(count(*r, "VIV-PERIOD-MISMATCH") == 0);
    CHECK(count(*r, "VIV-CLOCK-UNMATCHED") == 0);
    // the fabric divider has no period in OpenCDC without a generated clock constraint
    REQUIRE(count(*r, "VIV-PERIOD-UNKNOWN") == 1);
    CHECK(find(*r, "VIV-PERIOD-UNKNOWN")->message.find("create_generated_clock") != std::string::npos);
    REQUIRE(stage(*r));
    CHECK(stage(*r)->status == engine::StageStatus::Completed);
    CHECK(stage(*r)->detail.find("5 Vivado clock(s), 4 matched, 0 period mismatch(es), 0 unmatched") !=
          std::string::npos);
    // every OpenCDC clock is in the report (the divider by its register pin)
    CHECK(count(*r, "VIV-CLOCK-MISSING") == 0);
}

TEST_CASE("Vivado clock check: mismatch, unmatched and missing clocks", "[engine][vivado]") {
    test::TempDir dir;
    const auto r = analyze(dir, {std::string(kHeader) +
                                 "clk_in    10.000      {0.000 5.000}   P           {clk_in}\n"
                                 "clkout0   5.000       {0.000 2.500}   P,G,A       {U_MMCM/CLKOUT0}\n"
                                 "clkout1   8.000       {0.000 4.000}   P,G,A       {u_mmcm/CLKOUT1}\n"
                                 "gt_clk    3.200       {0.000 1.600}   P           {u_gt/RXOUTCLK}\n"});
    REQUIRE(count(*r, "VIV-PERIOD-MISMATCH") == 1);
    const auto* mm = find(*r, "VIV-PERIOD-MISMATCH");
    CHECK(mm->severity == Severity::Warning);
    CHECK(mm->message.find("10.000 ns but Vivado reports 8.000 ns for 'clkout1'") != std::string::npos);
    CHECK(mm->location.line == 9);
    CHECK_FALSE(mm->notes.empty());
    CHECK(mm->notes.front().find("x 1.000") != std::string::npos);

    REQUIRE(count(*r, "VIV-CLOCK-UNMATCHED") == 1);
    CHECK(find(*r, "VIV-CLOCK-UNMATCHED")->message.find("gt_clk (u_gt/RXOUTCLK)") != std::string::npos);
    REQUIRE(count(*r, "VIV-CLOCK-MISSING") == 1);
    CHECK(find(*r, "VIV-CLOCK-MISSING")->message.find("(fabric divider)") != std::string::npos);
    REQUIRE(stage(*r));
    CHECK(stage(*r)->detail.find("3 matched, 1 period mismatch(es), 1 unmatched") != std::string::npos);
}

TEST_CASE("Vivado clock check: a period only Vivado knows", "[engine][vivado]") {
    test::TempDir dir;
    // no create_clock: OpenCDC has the input clock but no period for it or the MMCM outputs' basis
    const auto r = analyze(dir, {std::string(kHeader) + "clk_in    8.000       {0.000 4.000}   P           {clk_in}\n"},
                           "");
    CHECK(count(*r, "VIV-PERIOD-UNKNOWN") == 1);
    CHECK(count(*r, "VIV-PERIOD-MISMATCH") == 0);
}

TEST_CASE("Vivado clock check: matching by clock name", "[engine][vivado]") {
    test::TempDir dir;
    // sources renamed by synthesis: the constrained clock still matches by its name
    const auto r = analyze(dir, {std::string(kHeader) +
                                 "clk_in    12.000      {0.000 6.000}   P           {clk_in_IBUF_inst/O}\n"});
    CHECK(count(*r, "VIV-PERIOD-MISMATCH") == 1);
    CHECK(count(*r, "VIV-CLOCK-UNMATCHED") == 0);
}

TEST_CASE("Vivado clock check: no report configured adds no stage", "[engine][vivado]") {
    test::TempDir dir;
    const auto r = analyze(dir, {});
    CHECK_FALSE(stage(*r));
    CHECK(count(*r, "VIV-CLOCK-MISSING") == 0);
}

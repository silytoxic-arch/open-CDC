// Lattice Diamond projects (.ldf, stage 78): the default implementation, IPexpress
// modules, preference files, and the analysis with the preferences' clocks.

#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/DiamondProject.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;
using namespace opencdc::engine;

namespace {

bool hasNote(const ProjectConfig& cfg, const std::string& code, const std::string& text = "") {
    return std::ranges::any_of(cfg.importNotes, [&](const Diagnostic& d) {
        if (d.code != code)
            return false;
        if (text.empty() || d.message.find(text) != std::string::npos)
            return true;
        return std::ranges::any_of(d.notes, [&](const std::string& n) { return n.find(text) != std::string::npos; });
    });
}

std::vector<std::string> names(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::string> out;
    for (const auto& p : paths)
        out.push_back(p.filename().string());
    return out;
}

} // namespace

TEST_CASE("Diamond project: the default implementation, IP modules, preferences", "[engine][diamond]") {
    test::TempDir dir;
    dir.write("rtl/top.v", "module top; endmodule\n");
    dir.write("rtl/pkg.vhd", "package pkg is end package;\n");
    dir.write("rtl/core.vhd", "entity core is end; architecture a of core is begin end;\n");
    dir.write("rtl/old.v", "module old; endmodule\n");
    dir.write("proj/top.lpf", "FREQUENCY PORT \"clk\" 25 MHZ;\n");
    dir.write("proj/syn.sdc", "");
    dir.write("proj/rom.v", "module rom; endmodule\n");
    dir.write("proj/rom.ipx", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                              "<DiamondModule name=\"rom\" module=\"rom\" generator=\"IPexpress\" type=\"Module\">\n"
                              "  <Package>\n"
                              "    <File name=\"rom.lpc\" type=\"lpc\"/>\n"
                              "    <File name=\"rom.v\" type=\"top_level_verilog\"/>\n"
                              "    <File name=\"rom_tmpl.v\" type=\"template_verilog\"/>\n"
                              "  </Package>\n"
                              "</DiamondModule>\n");
    dir.write("proj/p.ldf",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<BaliProject version=\"3.2\" title=\"demo\" device=\"LFE5U-12F-8BG381C\" default_implementation=\"impl2\">\n"
              "  <Options/>\n"
              "  <Implementation title=\"impl1\" dir=\"impl1\" synthesis=\"lse\">\n"
              "    <Options def_top=\"other\"/>\n"
              "    <Source name=\"../rtl/old.v\" type=\"Verilog\" type_short=\"Verilog\"><Options/></Source>\n"
              "  </Implementation>\n"
              "  <Implementation title=\"impl2\" dir=\"impl2\" synthesis=\"synplify\">\n"
              "    <Options def_top=\"core\" top=\"top\"/>\n"
              "    <Source name=\"../rtl/top.v\" type=\"Verilog\" type_short=\"Verilog\"><Options top_module=\"top\"/></Source>\n"
              "    <Source name=\"..\\rtl\\pkg.vhd\" type=\"VHDL\" type_short=\"VHDL\"><Options lib=\"mylib\"/></Source>\n"
              "    <Source name=\"../rtl/core.vhd\" type=\"VHDL\" type_short=\"VHDL\"><Options/></Source>\n"
              "    <Source name=\"../rtl/old.v\" type=\"Verilog\" type_short=\"Verilog\" excluded=\"TRUE\"><Options/></Source>\n"
              "    <Source name=\"../rtl/missing.v\" type=\"Verilog\" type_short=\"Verilog\"><Options/></Source>\n"
              "    <Source name=\"rom.ipx\" type=\"IPX_Module\" type_short=\"IPX\"><Options/></Source>\n"
              "    <Source name=\"top.lpf\" type=\"Logic Preference\" type_short=\"LPF\"><Options/></Source>\n"
              "    <Source name=\"syn.sdc\" type=\"Synplify Design Constraints File\" type_short=\"SDC\"><Options/></Source>\n"
              "    <Source name=\"impl2/p.xcf\" type=\"Programming Project File\" type_short=\"Programming\"><Options/></Source>\n"
              "  </Implementation>\n"
              "</BaliProject>\n");
    REQUIRE(isDiamondProjectFile(dir.path() / "proj/p.ldf"));
    REQUIRE(isVendorProjectFile(dir.path() / "proj/p.LDF"));
    auto r = loadProjectFile(dir.path() / "proj/p.ldf");
    REQUIRE(r);
    const auto& cfg = r.value();
    CHECK(cfg.name == "demo");
    CHECK(cfg.top == "top"); // the implementation's top before its default top
    CHECK(names(cfg.sources) == std::vector<std::string>{"top.v", "pkg.vhd", "core.vhd", "rom.v"});
    REQUIRE(cfg.sourceLibraries.size() == 4);
    CHECK(cfg.sourceLibraries[1] == "mylib");
    CHECK(cfg.sourceLibraries[2].empty());
    CHECK(names(cfg.constraints) == std::vector<std::string>{"top.lpf", "syn.sdc"});
    CHECK(hasNote(cfg, "PRJ-LDF-MISSING", "../rtl/missing.v"));
    CHECK(hasNote(cfg, "PRJ-LDF-SKIPPED", "1 file(s) are excluded"));
    CHECK(hasNote(cfg, "PRJ-LDF-SKIPPED", "impl2/p.xcf (Programming Project File)"));
    CHECK(hasNote(cfg, "PRJ-LDF-IMPLEMENTATION", "impl1"));
    CHECK(hasNote(cfg, "PRJ-LDF-IMPORT", "implementation 'impl2', device LFE5U-12F-8BG381C"));
}

TEST_CASE("Diamond project negative: malformed projects and IP modules", "[engine][diamond]") {
    test::TempDir dir;
    dir.write("a.v", "module a; endmodule\n");
    auto notProject = projectFromLdfText("<Project/>", dir.path() / "x.ldf");
    REQUIRE_FALSE(notProject);
    CHECK(notProject.error().code == "PRJ-LDF-PARSE");
    auto noImpl = projectFromLdfText("<BaliProject title=\"x\"/>", dir.path() / "x.ldf");
    REQUIRE_FALSE(noImpl);
    CHECK(noImpl.error().code == "PRJ-LDF-PARSE");
    auto noFiles = projectFromLdfText("<BaliProject><Implementation title=\"i\">"
                                      "<Source name=\"b.v\" type=\"Verilog\" type_short=\"Verilog\"/>"
                                      "</Implementation></BaliProject>",
                                      dir.path() / "x.ldf");
    REQUIRE_FALSE(noFiles);
    CHECK(noFiles.error().code == "PRJ-LDF-PARSE");
    // an IP module whose HDL was not generated, and one that is not an IPexpress file
    dir.write("ip1.ipx", "<DiamondModule name=\"ip1\"><Package><File name=\"ip1.v\" type=\"top_level_verilog\"/>"
                         "</Package></DiamondModule>");
    dir.write("ip2.ipx", "not xml <");
    auto ip = projectFromLdfText("<BaliProject><Implementation title=\"i\">"
                                 "<Source name=\"a.v\" type=\"Verilog\" type_short=\"Verilog\"/>"
                                 "<Source name=\"ip1.ipx\" type=\"IPX_Module\" type_short=\"IPX\"/>"
                                 "<Source name=\"ip2.ipx\" type=\"IPX_Module\" type_short=\"IPX\"/>"
                                 "</Implementation></BaliProject>",
                                 dir.path() / "x.ldf");
    REQUIRE(ip);
    CHECK(hasNote(ip.value(), "PRJ-LDF-IPX", "generated file not found: ip1.v"));
    CHECK(hasNote(ip.value(), "PRJ-LDF-IPX", "ip2.ipx could not be read"));
    CHECK(ip.value().sources.size() == 1);
    CHECK(ip.value().top.empty());
    auto missing = projectFromLdfFile(dir.path() / "nope.ldf");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "PRJ-OPEN");
}

TEST_CASE("Diamond project: the analysis with the preference file's clocks", "[engine][diamond]") {
    test::TempDir dir;
    // two clocks from the same divider chain; the preferences block the paths between them
    dir.write("top.v", "module top(input clk, input d, output reg q);\n"
                       "  reg c2 = 0, c4 = 0, a, s1, s2;\n"
                       "  always @(posedge clk) c2 <= ~c2;\n"
                       "  always @(posedge c2) c4 <= ~c4;\n"
                       "  always @(posedge c2) a <= d;\n"
                       "  always @(posedge c4) begin s1 <= a; s2 <= s1; q <= s2; end\n"
                       "endmodule\n");
    dir.write("top.lpf", "BLOCK RESETPATHS;\nFREQUENCY PORT \"clk\" 100 MHZ;\n"
                         "BLOCK PATH FROM CLKNET \"c2\" TO CLKNET \"c4\";\n");
    dir.write("p.ldf", "<BaliProject title=\"p\" default_implementation=\"i\"><Implementation title=\"i\">"
                       "<Options def_top=\"top\"/>"
                       "<Source name=\"top.v\" type=\"Verilog\" type_short=\"Verilog\"/>"
                       "<Source name=\"top.lpf\" type=\"Logic Preference\" type_short=\"LPF\"/>"
                       "</Implementation></BaliProject>");
    auto cfg = loadProjectFile(dir.path() / "p.ldf");
    REQUIRE(cfg);
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto r = session.run(cfg.value(), progress, cancel);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    REQUIRE(r->clocks);
    REQUIRE(r->cdc);
    const auto clk = std::ranges::find_if(r->clocks->clocks, [](const auto& c) { return c.name == "clk"; });
    REQUIRE(clk != r->clocks->clocks.end());
    REQUIRE(clk->periodNs);
    CHECK(*clk->periodNs == 10.0);
    // the blocked pair is checked as a crossing, synchronized by the two flops
    CHECK(r->cdc->stats.synchronized == 1);
    CHECK(r->cdc->stats.unsynchronized == 0);
}

// Gowin EDA projects (.gprj, stage 77): the import, the settings of the impl
// directory, the top chosen without a top module, the Gowin cell models and
// the Verilog that Gowin EDA accepts (SystemVerilog in .v files, an input
// port driven by its module).

#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/GowinProject.h"
#include "engine/ProjectConfig.h"
#include "frontend/Primitives.h"

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

std::string gprj(const std::string& files) {
    return "<?xml version=\"1\" encoding=\"UTF-8\"?>\n<!DOCTYPE gowin-fpga-project>\n<Project>\n"
           "    <Template>FPGA</Template>\n    <Version>5</Version>\n"
           "    <Device name=\"GW2AR-18C\" pn=\"GW2AR-LV18QN88C8/I7\">gw2ar18c-000</Device>\n"
           "    <FileList>\n" +
           files + "    </FileList>\n</Project>\n";
}

std::shared_ptr<const AnalysisResult> run(const ProjectConfig& cfg) {
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

std::size_t count(const AnalysisResult& r, std::string_view code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
}

} // namespace

TEST_CASE("Gowin project: files, settings and what is not read", "[engine][gowin]") {
    test::TempDir dir;
    dir.write("src/top.v", "module top; endmodule\n");
    dir.write("src/hdmi.sv", "module hdmi; endmodule\n");
    dir.write("src/pkg.vhd", "package pkg is end package;\n");
    dir.write("src/top.sdc", "");
    dir.write("src/top.cst", "");
    dir.write("src/old.v", "module old; endmodule\n");
    dir.write("src/inc/defs.vh", "`define W 8\n");
    dir.write("p.gprj", gprj("        <File path=\"src/top.v\" type=\"file.verilog\" enable=\"1\"/>\n"
                             "        <File path=\"src/hdmi.sv\" type=\"file.verilog\" enable=\"1\"/>\n"
                             "        <File path=\"src/pkg.vhd\" type=\"file.vhdl\" enable=\"1\"/>\n"
                             "        <File path=\"src/old.v\" type=\"file.verilog\" enable=\"0\"/>\n"
                             "        <File path=\"src/missing.v\" type=\"file.verilog\" enable=\"1\"/>\n"
                             "        <File path=\"src/top.sdc\" type=\"file.sdc\" enable=\"1\"/>\n"
                             "        <File path=\"src/top.cst\" type=\"file.cst\" enable=\"1\"/>\n"
                             "        <File path=\"src/top.gao\" type=\"file.gao\" enable=\"1\"/>\n"));
    dir.write("impl/p_process_config.json", "{\n \"TopModule\" : \"top\",\n \"Verilog_Standard\" : \"Vlg_Std_V2001\",\n"
                                            " \"IncludePath\" : [ \"src/inc\" ],\n \"VHDL_Standard\" : \"VHDL_Std_1993\"\n}\n");
    REQUIRE(isGowinProjectFile(dir.path() / "p.gprj"));
    REQUIRE(isVendorProjectFile(dir.path() / "p.GPRJ"));
    auto r = loadProjectFile(dir.path() / "p.gprj");
    REQUIRE(r);
    const auto& cfg = r.value();
    CHECK(cfg.name == "p");
    CHECK(cfg.top == "top");
    CHECK(names(cfg.sources) == std::vector<std::string>{"top.v", "hdmi.sv", "pkg.vhd"});
    CHECK(names(cfg.constraints) == std::vector<std::string>{"top.sdc"});
    REQUIRE(cfg.includeDirs.size() == 1);
    CHECK(cfg.includeDirs[0].filename() == "inc");
    // a .sv source: SystemVerilog, but the .v files keep the Verilog keywords (Vlg_Std_V2001)
    CHECK(cfg.language == frontend::LanguageStd::SystemVerilog2017);
    CHECK_FALSE(cfg.verilogFilesSystemVerilog);
    CHECK(cfg.options.gowinPrimitives);
    CHECK_FALSE(cfg.options.deepestRootTop); // the top is set
    CHECK(hasNote(cfg, "PRJ-GPRJ-MISSING", "src/missing.v"));
    CHECK(hasNote(cfg, "PRJ-GPRJ-SKIPPED", "src/top.cst (physical constraints)"));
    CHECK(hasNote(cfg, "PRJ-GPRJ-SKIPPED", "src/top.gao (analyzer configuration)"));
    CHECK(hasNote(cfg, "PRJ-GPRJ-SKIPPED", "1 design or constraint file(s) are disabled"));
    CHECK(hasNote(cfg, "PRJ-GPRJ-IMPORT", "device GW2AR-LV18QN88C8/I7"));
    CHECK(hasNote(cfg, "PRJ-GPRJ-IMPORT", "settings impl/p_process_config.json"));

    // the JSON written by 'opencdc import' keeps the Gowin options
    const auto json = projectToJson(cfg, dir.path());
    CHECK(json.find("\"gowinPrimitives\": true") != std::string::npos);
    auto back = parseProjectJson(json, dir.path());
    REQUIRE(back);
    CHECK(back.value().options.gowinPrimitives);
}

TEST_CASE("Gowin project: SystemVerilog 2017 setting, no top module, older settings file", "[engine][gowin]") {
    test::TempDir dir;
    dir.write("src/a.v", "module a; endmodule\n");
    dir.write("nes.gprj", gprj("        <File path=\"src/a.v\" type=\"file.verilog\" enable=\"1\"/>\n"));
    // the older Gowin EDA name of the settings file
    dir.write("impl/project_process_config.json",
              "{\"TopModule\" : \"\", \"Verilog_Standard\" : \"Vlg_Std_Sysv2017\", \"IncludePath\" : []}");
    auto r = projectFromGprjFile(dir.path() / "nes.gprj");
    REQUIRE(r);
    CHECK(r.value().top.empty());
    CHECK(r.value().options.deepestRootTop);
    CHECK(r.value().language == frontend::LanguageStd::SystemVerilog2017);
    CHECK(r.value().verilogFilesSystemVerilog); // Gowin: every Verilog file
    CHECK(hasNote(r.value(), "PRJ-GPRJ-IMPORT", "top not set"));
    const auto json = projectToJson(r.value(), dir.path());
    CHECK(json.find("\"verilogFilesSystemVerilog\": true") != std::string::npos);
    CHECK(json.find("\"topSelection\": \"deepest\"") != std::string::npos);
    auto back = parseProjectJson(json, dir.path());
    REQUIRE(back);
    CHECK(back.value().verilogFilesSystemVerilog);
    CHECK(back.value().options.deepestRootTop);

    // no settings file: Verilog-2001, the deepest root
    std::filesystem::remove(dir.path() / "impl/project_process_config.json");
    auto bare = projectFromGprjFile(dir.path() / "nes.gprj");
    REQUIRE(bare);
    CHECK(bare.value().language == frontend::LanguageStd::Verilog2001);
    CHECK(bare.value().options.deepestRootTop);
    CHECK(hasNote(bare.value(), "PRJ-GPRJ-IMPORT", "no synthesis settings file"));
}

TEST_CASE("Gowin project negative: malformed projects and settings", "[engine][gowin]") {
    test::TempDir dir;
    dir.write("src/a.v", "module a; endmodule\n");
    // not a Gowin project
    auto notProject = projectFromGprjText("<Other/>", "", dir.path() / "x.gprj");
    REQUIRE_FALSE(notProject);
    CHECK(notProject.error().code == "PRJ-GPRJ-PARSE");
    // no file list
    auto noList = projectFromGprjText("<Project><Version>5</Version></Project>", "", dir.path() / "x.gprj");
    REQUIRE_FALSE(noList);
    CHECK(noList.error().code == "PRJ-GPRJ-PARSE");
    // only missing or disabled design files
    auto none = projectFromGprjText(gprj("<File path=\"src/a.v\" type=\"file.verilog\" enable=\"0\"/>\n"
                                         "<File path=\"src/b.v\" type=\"file.verilog\" enable=\"1\"/>\n"),
                                    "", dir.path() / "x.gprj");
    REQUIRE_FALSE(none);
    CHECK(none.error().code == "PRJ-GPRJ-PARSE");
    // settings that are not JSON: a warning, the files are still imported
    auto badJson = projectFromGprjText(gprj("<File path=\"src/a.v\" type=\"file.verilog\" enable=\"1\"/>\n"),
                                       "{ not json", dir.path() / "x.gprj");
    REQUIRE(badJson);
    CHECK(hasNote(badJson.value(), "PRJ-GPRJ-CONFIG", "could not be read"));
    CHECK(badJson.value().sources.size() == 1);
    // an unknown file can not be opened
    auto missing = projectFromGprjFile(dir.path() / "nope.gprj");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "PRJ-OPEN");
    // the project option is checked
    auto badOpt = parseProjectJson(
        R"({"format":"opencdc-project","version":1,"name":"x","sources":["src/a.v"],"options":{"topSelection":"first"}})",
        dir.path());
    REQUIRE_FALSE(badOpt);
    auto badFlag = parseProjectJson(
        R"({"format":"opencdc-project","version":1,"name":"x","sources":["src/a.v"],"verilogFilesSystemVerilog":1})",
        dir.path());
    REQUIRE_FALSE(badFlag);
}

TEST_CASE("Gowin project: the analysis with the Gowin cells, the deepest root as the top", "[engine][gowin]") {
    test::TempDir dir;
    // nestang: a PLL output divided by CLKDIV, a serializer, an LVDS buffer; an unused root
    // module instantiates a Xilinx cell (dsp.v: FirFilter -> Mac -> DSP48A1)
    dir.write("src/top.v",
              "module top(input clk27, input [9:0] d, output p, output n);\n"
              "  wire fast, pix, ser;\n"
              "  rPLL #(.FCLKIN(\"27\"), .IDIV_SEL(3), .FBDIV_SEL(54)) pll (.CLKIN(clk27), .CLKOUT(fast));\n"
              "  CLKDIV #(.DIV_MODE(\"5\")) div (.HCLKIN(fast), .RESETN(1'b1), .CALIB(1'b0), .CLKOUT(pix));\n"
              "  reg [9:0] word;\n"
              "  always @(posedge pix) word <= d;\n"
              "  OSER10 s (.Q(ser), .D0(word[0]), .D1(word[1]), .D2(word[2]), .D3(word[3]), .D4(word[4]),\n"
              "            .D5(word[5]), .D6(word[6]), .D7(word[7]), .D8(word[8]), .D9(word[9]), .PCLK(pix),\n"
              "            .FCLK(fast), .RESET(1'b0));\n"
              "  ELVDS_OBUF b (.I(ser), .O(p), .OB(n));\n"
              "  sub u1 (); sub u2 ();\n"
              "endmodule\n"
              "module sub; leaf l (); endmodule\n"
              "module leaf; endmodule\n"
              "module unused_root(input clk); mac m (.clk(clk)); endmodule\n"
              "module mac(input clk); DSP48A1 dsp (.CLK(clk)); endmodule\n");
    dir.write("p.gprj", gprj("<File path=\"src/top.v\" type=\"file.verilog\" enable=\"1\"/>\n"));
    auto cfg = loadProjectFile(dir.path() / "p.gprj");
    REQUIRE(cfg);
    const auto r = run(cfg.value());
    REQUIRE(r->cdc);
    CHECK(count(*r, "FE-AUTO-TOP") == 1);
    CHECK(count(*r, "FE-UNKNOWN-MODULE") == 0); // DSP48A1 is only in the root not analyzed
    const auto auto_top = std::ranges::find(r->diagnostics, "FE-AUTO-TOP", &Diagnostic::code);
    REQUIRE(auto_top != r->diagnostics.end());
    CHECK(auto_top->message.find("'top' is the top") != std::string::npos);
    CHECK(std::ranges::any_of(auto_top->notes, [](const std::string& n) { return n == "not analyzed: unused_root"; }));
    // pix = fast / 5, both derived from clk27
    REQUIRE(r->clocks);
    const auto pix = std::ranges::find_if(r->clocks->clocks, [](const auto& c) { return c.name == "top.div.CLKOUT"; });
    REQUIRE(pix != r->clocks->clocks.end());
    const auto fast = std::ranges::find_if(r->clocks->clocks, [](const auto& c) { return c.name == "top.pll.CLKOUT"; });
    REQUIRE(fast != r->clocks->clocks.end());
    REQUIRE(pix->periodNs);
    REQUIRE(fast->periodNs);
    CHECK(*pix->periodNs > 4.9 * *fast->periodNs);
    CHECK(*pix->periodNs < 5.1 * *fast->periodNs);
    CHECK(r->cdc->stats.unsynchronized == 0); // the serializer's load is a related-clock transfer

    // negative: outside a Gowin project the generic names stay unknown modules, and
    // without a top every root is analyzed
    ProjectConfig plain;
    plain.name = "plain";
    plain.sources = {dir.path() / "src/top.v"};
    plain.sourceLibraries = {""};
    const auto p = run(plain);
    CHECK(count(*p, "FE-AUTO-TOP") == 0);
    CHECK(std::ranges::any_of(p->diagnostics, [](const Diagnostic& d) {
        return d.isError() && d.message.find("'CLKDIV'") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(p->diagnostics, [](const Diagnostic& d) {
        return d.isError() && d.message.find("'DSP48A1'") != std::string::npos;
    }));
    CHECK(frontend::PrimitiveLibrary::instance().gowinOnly("OSER10"));
    CHECK_FALSE(frontend::PrimitiveLibrary::instance().gowinOnly("rPLL"));
}

TEST_CASE("Gowin project: SystemVerilog in .v files, an input port driven by its module", "[engine][gowin]") {
    test::TempDir dir;
    // nestang hw_uart.v: a typedef in a .v file; autofire.v: 'input reg out' assigned in the module
    dir.write("src/top.v", "typedef logic [7:0] BYTE;\n"
                           "module top(input clk, input btn, output o, output BYTE q);\n"
                           "  fire f (.clk(clk), .btn(btn), .out(o));\n"
                           "  always_ff @(posedge clk) q <= 8'd1;\n"
                           "endmodule\n"
                           "module fire(input clk, input btn, input reg out);\n"
                           "  always @(posedge clk) out <= btn ? ~out : 1'b0;\n"
                           "endmodule\n");
    ProjectConfig cfg;
    cfg.name = "kw";
    cfg.top = "top";
    cfg.sources = {dir.path() / "src/top.v"};
    cfg.sourceLibraries = {""};
    cfg.language = frontend::LanguageStd::SystemVerilog2017;
    cfg.verilogFilesSystemVerilog = true;
    const auto r = run(cfg);
    CHECK(std::ranges::none_of(r->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    REQUIRE(r->cdc);
    CHECK(count(*r, "FE-NET-PROCEDURAL-ASSIGN") == 1);
    const auto w = std::ranges::find(r->diagnostics, "FE-NET-PROCEDURAL-ASSIGN", &Diagnostic::code);
    CHECK(w->message.find("input port 'out'") != std::string::npos);

    // negative: as in Vivado, a .v file of a SystemVerilog project keeps the Verilog keywords
    cfg.verilogFilesSystemVerilog = false;
    const auto v = run(cfg);
    CHECK(std::ranges::any_of(v->diagnostics, [](const Diagnostic& d) { return d.isError(); }));

    // negative: an input that is not declared 'reg' and is assigned stays an error
    dir.write("src/bad.sv", "module bad(input clk, input x); always @(posedge clk) x <= 1'b0; endmodule\n");
    ProjectConfig bad;
    bad.name = "bad";
    bad.top = "bad";
    bad.sources = {dir.path() / "src/bad.sv"};
    bad.sourceLibraries = {""};
    bad.language = frontend::LanguageStd::SystemVerilog2017;
    const auto b = run(bad);
    CHECK(std::ranges::any_of(b->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    CHECK(count(*b, "FE-NET-PROCEDURAL-ASSIGN") == 0);
}

TEST_CASE("Gowin project: the internal oscillator OSC", "[engine][gowin]") {
    test::TempDir dir;
    dir.write("src/top.v", "module top(input clk, input d, output reg q);\n"
                           "  wire o; reg a;\n"
                           "  OSC #(.FREQ_DIV(10), .DEVICE(\"GW2AR-18C\")) osc (.OSCOUT(o));\n"
                           "  always @(posedge clk) a <= d;\n"
                           "  always @(posedge o) q <= a;\n"
                           "endmodule\n");
    dir.write("p.gprj", gprj("<File path=\"src/top.v\" type=\"file.verilog\" enable=\"1\"/>\n"));
    auto cfg = loadProjectFile(dir.path() / "p.gprj");
    REQUIRE(cfg);
    const auto r = run(cfg.value());
    REQUIRE(r->clocks);
    const auto osc = std::ranges::find_if(r->clocks->clocks, [](const auto& c) { return c.name == "top.osc.OSCOUT"; });
    REQUIRE(osc != r->clocks->clocks.end());
    REQUIRE(osc->periodNs);
    CHECK(*osc->periodNs == 40.0); // 250 MHz / 10
    REQUIRE(r->cdc);
    CHECK(r->cdc->stats.unsynchronized == 1);
}

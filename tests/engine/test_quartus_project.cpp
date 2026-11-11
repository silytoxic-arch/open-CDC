#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "engine/QuartusProject.h"

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

std::string notesOf(const ProjectConfig& cfg) {
    std::string s;
    for (const auto& d : cfg.importNotes)
        s += format(d) + "\n";
    return s;
}

} // namespace

TEST_CASE("Quartus project: the revision's assignments, IP file lists and systems", "[engine][quartus]") {
    test::TempDir dir;
    dir.write("rtl/top.sv", "module top; endmodule\n");
    dir.write("rtl/core.v", "module core; endmodule\n");
    dir.write("rtl/pkg.vhd", "package pkg is end package;\n");
    dir.write("rtl/inc/defs.vh", "`define W 8\n");
    dir.write("rtl/rom.mif", "");
    dir.write("rtl/old.tdf", "");
    dir.write("top.sdc", "");
    // an IP: its files through $::quartus(qip_path) (with Quartus' trailing '/')
    dir.write("ip/pll/pll.v", "module pll; endmodule\n");
    dir.write("ip/pll/pll/pll_0002.v", "module pll_0002; endmodule\n");
    dir.write("ip/pll/pll.qip",
              "set_global_assignment -entity \"pll\" -library \"pll\" -name IP_TOOL_NAME \"altera_pll\"\n"
              "set_global_assignment -library \"pll\" -name VERILOG_FILE [file join $::quartus(qip_path) \"pll.v\"]\n"
              "set_global_assignment -library \"pll\" -name VERILOG_FILE "
              "[file join $::quartus(qip_path) \"pll/pll_0002.v\"]\n");
    // a version-dependent list (MiSTer: pll_q17.qip for Quartus 17)
    dir.write("sys/pll_q17.qip", "set_global_assignment -name QIP_FILE ip/pll/pll.qip\n");
    dir.write("sys/pll_q13.qip", "set_global_assignment -name QIP_FILE nowhere.qip\n");
    dir.write("sys/sys.qip",
              "set_global_assignment -name QIP_FILE [join [list $::quartus(qip_path) pll_q [regexp -inline "
              "{[0-9]+} $quartus(version)] .qip] {}]\n"
              "set_global_assignment -name SDC_FILE [file join $::quartus(qip_path) sys.sdc]\n");
    dir.write("sys/sys.sdc", "");
    dir.write("sys/sys.tcl",
              "set_global_assignment -name FAMILY \"Cyclone V\"\n"
              "set_location_assignment PIN_V11 -to FPGA_CLK1_50\n"
              "set_location_assignment PIN_W8 -to SDRAM_A[0]\n"
              "set_instance_assignment -name IO_STANDARD \"3.3-V LVTTL\" -to SDRAM_DQ[*]\n");
    // a Platform Designer system with its generated synthesis files, and one without
    dir.write("soc/soc.qsys", "<system/>");
    dir.write("soc/soc/synthesis/soc.v", "module soc; endmodule\n");
    dir.write("soc/soc/synthesis/submodules/soc_mm.v", "module soc_mm; endmodule\n");
    dir.write("soc/soc/synthesis/soc.qip",
              "set_global_assignment -library \"soc\" -name SOURCE_FILE [file join $::quartus(qip_path) "
              "\"soc.qsys\"]\n"
              "set_global_assignment -library \"soc\" -name VERILOG_FILE [file join $::quartus(qip_path) "
              "\"soc.v\"]\n"
              "set_global_assignment -library \"soc\" -name VERILOG_FILE [file join $::quartus(qip_path) "
              "\"submodules/soc_mm.v\"]\n");
    dir.write("soc/other.qsys", "<system/>");
    dir.write("p.qpf", "QUARTUS_VERSION = \"17.0\"\nDATE = \"x\"\nPROJECT_REVISION = \"p\"\n"
                       "PROJECT_REVISION = \"p_alt\"\n");
    dir.write("p.qsf",
              "# Quartus settings\n"
              "set_global_assignment -name TOP_LEVEL_ENTITY top\n"
              "set_global_assignment -name LAST_QUARTUS_VERSION \"17.0.2 Standard Edition\"\n"
              "set_global_assignment -name PRE_FLOW_SCRIPT_FILE \"quartus_sh:sys/build_id.tcl\"\n"
              "source sys/sys.tcl\n"
              "set_global_assignment -name QIP_FILE sys/sys.qip\n"
              "set_global_assignment -name SYSTEMVERILOG_FILE rtl/core.v\n"
              "set_global_assignment -name VHDL_FILE rtl/pkg.vhd -library my_lib\n"
              "set_global_assignment -name SYSTEMVERILOG_FILE rtl/top.sv\n"
              "set_global_assignment -name SYSTEMVERILOG_FILE rtl/top.sv\n"
              "set_global_assignment -name VERILOG_FILE rtl/gone.v\n"
              "set_global_assignment -name VERILOG_FILE rtl/disabled.v -remove\n"
              "set_global_assignment -name SEARCH_PATH rtl/inc\n"
              "set_global_assignment -name VERILOG_MACRO \"MISTER_FB=1\"\n"
              "set_global_assignment -name VERILOG_MACRO DEBUG\n"
              "set_parameter -name DEPTH 32\n"
              "set_parameter -name WIDTH 8 -to \"core:u_core\"\n"
              "set_global_assignment -name QSYS_FILE soc/soc.qsys\n"
              "set_global_assignment -name QSYS_FILE soc/other.qsys\n"
              "set_global_assignment -name MIF_FILE rtl/rom.mif\n"
              "set_global_assignment -name AHDL_FILE rtl/old.tdf\n"
              "set_global_assignment -name SDC_FILE top.sdc\n"
              "post_message \"done\"\n"
              "set_unknown_thing a b\n");
    auto r = loadProjectFile(dir.path() / "p.qpf");
    REQUIRE(r);
    const auto& cfg = r.value();
    INFO(notesOf(cfg));
    CHECK(cfg.name == "p");
    CHECK(cfg.top == "top");
    CHECK(cfg.projectFile.empty());
    CHECK(cfg.importedFrom.filename() == "p.qsf");
    CHECK(names(cfg.sources) ==
          std::vector<std::string>{"pll.v", "pll_0002.v", "core.v", "pkg.vhd", "top.sv", "soc.v", "soc_mm.v"});
    CHECK(cfg.sourceLibraries == std::vector<std::string>{"", "", "", "my_lib", "", "", ""});
    CHECK(cfg.language == frontend::LanguageStd::SystemVerilog2017);
    CHECK(names(cfg.includeDirs) == std::vector<std::string>{"inc"});
    REQUIRE(cfg.defines.size() == 2);
    CHECK(cfg.defines[0].name == "MISTER_FB");
    CHECK(cfg.defines[0].value == "1");
    CHECK_FALSE(cfg.defines[1].value);
    CHECK(cfg.parameters == std::vector<std::pair<std::string, std::string>>{{"DEPTH", "32"}});
    CHECK(names(cfg.constraints) == std::vector<std::string>{"sys.sdc", "top.sdc"});
    CHECK(hasNote(cfg, "PRJ-QSF-IMPORT", "7 source(s), 2 constraint file(s), top 'top' (revision 'p', 4 IP file"));
    CHECK(hasNote(cfg, "PRJ-QSF-REVISION", "p_alt"));
    CHECK(hasNote(cfg, "PRJ-QSF-PRE-FLOW", "build_id.tcl"));
    // negative: what is not read is said
    CHECK(hasNote(cfg, "PRJ-QSF-MISSING", "rtl/gone.v"));
    CHECK(hasNote(cfg, "PRJ-QSF-NOT-GENERATED", "other.qsys"));
    CHECK(hasNote(cfg, "PRJ-QSF-UNSUPPORTED-FILE", "old.tdf"));
    CHECK(hasNote(cfg, "PRJ-QSF-SKIPPED", "rom.mif"));
    CHECK(hasNote(cfg, "PRJ-QSF-TCL", "set_unknown_thing"));
    CHECK_FALSE(hasNote(cfg, "PRJ-QSF-TCL", "post_message"));
    CHECK_FALSE(hasNote(cfg, "PRJ-QSF-TCL", "0")); // SDRAM_A[0] is a bus index, not a command
    CHECK_FALSE(hasNote(cfg, "PRJ-QSF-MISSING", "nowhere.qip")); // the list of the other version is not read
    CHECK_FALSE(hasNote(cfg, "PRJ-QSF-MISSING", "disabled.v"));
    // the other revision through its .qsf
    dir.write("p_alt.qsf", "set_global_assignment -name TOP_LEVEL_ENTITY core\n"
                           "set_global_assignment -name VERILOG_FILE rtl/core.v\n");
    auto alt = loadProjectFile(dir.path() / "p_alt.qsf");
    REQUIRE(alt);
    CHECK(alt.value().top == "core");
    CHECK(names(alt.value().sources) == std::vector<std::string>{"core.v"});
    // a Verilog file read as Verilog keeps the Verilog mode
    CHECK(alt.value().language == frontend::LanguageStd::Verilog2001);
}

TEST_CASE("Quartus project negative: not a project, broken scripts, no revision", "[engine][quartus]") {
    test::TempDir dir;
    auto empty = projectFromQsfText("# nothing here\nset_global_assignment -name FAMILY \"Cyclone V\"\n",
                                    dir.path() / "e.qsf");
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "PRJ-QSF-PARSE");
    auto broken = projectFromQsfText("set_global_assignment -name TOP_LEVEL_ENTITY {top\n", dir.path() / "b.qsf");
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("unbalanced") != std::string::npos);
    auto undefined = projectFromQsfText("set_global_assignment -name VERILOG_FILE $nosuch/x.v\n", dir.path() / "u.qsf");
    REQUIRE_FALSE(undefined);
    CHECK(undefined.error().message.find("no such variable") != std::string::npos);
    dir.write("n.qpf", "QUARTUS_VERSION = \"17.0\"\n");
    auto noRevision = loadProjectFile(dir.path() / "n.qpf");
    REQUIRE_FALSE(noRevision);
    CHECK(noRevision.error().code == "PRJ-QSF-PARSE");
    auto missing = loadProjectFile(dir.path() / "none.qpf");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "PRJ-OPEN");
    CHECK(isQuartusProjectFile("x.QPF"));
    CHECK(isQuartusProjectFile("x.qsf"));
    CHECK_FALSE(isQuartusProjectFile("x.qip"));
    CHECK(isVendorProjectFile("x.qsf"));
    CHECK(isVendorProjectFile("x.xpr"));
    CHECK_FALSE(isVendorProjectFile("x.ocdc.json"));
}

TEST_CASE("Quartus project: analyzed directly, with its constraints", "[engine][quartus]") {
    test::TempDir dir;
    dir.write("top.v", "module top(input clk_a, input clk_b, input d, output reg q);\n"
                       "  reg a; always @(posedge clk_a) a <= d;\n"
                       "  always @(posedge clk_b) q <= a;\nendmodule\n");
    dir.write("top.sdc", "create_clock -period 10 [get_ports clk_a]\ncreate_clock -period 7 [get_ports clk_b]\n"
                         "set_clock_groups -exclusive -group [get_clocks clk_a] -group [get_clocks clk_b]\n");
    dir.write("t.qsf", "set_global_assignment -name TOP_LEVEL_ENTITY top\n"
                       "set_global_assignment -name VERILOG_FILE top.v\n"
                       "set_global_assignment -name SDC_FILE top.sdc\n");
    auto cfg = loadProjectFile(dir.path() / "t.qsf");
    REQUIRE(cfg);
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto result = session.run(cfg.value(), progress, cancel);
    REQUIRE(result->cdc);
    // -exclusive cuts timing but the crossing is still checked
    CHECK(result->cdc->stats.unsynchronized == 1);
    CHECK(std::ranges::any_of(result->diagnostics, [](const Diagnostic& d) { return d.code == "PRJ-QSF-IMPORT"; }));
}

TEST_CASE("Quartus project: the Transceiver Native PHY's core is the built-in model", "[engine][quartus][xcvr]") {
    // MiSTVGA (Arria V GX): a DVI transmitter on the transceivers; tmds_tx_phy.qip lists the generated
    // core (altera_xcvr_native_av, atoms arriav_hssi_* that Quartus' library defines) below tmds_tx_phy/
    test::TempDir dir;
    dir.write("ip/tmds_tx_phy/altera_xcvr_native_av.sv", "module altera_xcvr_native_av; arriav_hssi_8g_tx_pcs wys(); endmodule\n");
    dir.write("ip/tmds_tx_phy/av_pcs.sv", "module av_pcs; endmodule\n");
    dir.write("ip/tmds_tx_phy.v",
              "module tmds_tx_phy(input [1:0] tx_pll_refclk, input [3:0] tx_std_coreclkin, output [3:0] tx_std_clkout,\n"
              "                   input [39:0] tx_parallel_data, input [3:0] tx_digitalreset, output [0:0] pll_locked);\n"
              "  altera_xcvr_native_av #(.channels(4), .data_rate(\"1250 Mbps\"), .pll_refclk_cnt(2),\n"
              "    .pll_refclk_select(\"0\"), .pll_refclk_freq(\"125.0 MHz,unused\"), .std_pcs_pma_width(10))\n"
              "    tmds_tx_phy_inst (.tx_pll_refclk(tx_pll_refclk), .tx_std_coreclkin(tx_std_coreclkin),\n"
              "    .tx_std_clkout(tx_std_clkout), .tx_parallel_data({136'd0, tx_parallel_data}),\n"
              "    .tx_digitalreset(tx_digitalreset), .pll_locked(pll_locked));\n"
              "endmodule\n");
    dir.write("ip/tmds_tx_phy.qip",
              "set_global_assignment -entity \"tmds_tx_phy\" -library \"tmds_tx_phy\" -name IP_TOOL_NAME "
              "\"altera_xcvr_native_av\"\n"
              "set_global_assignment -library \"tmds_tx_phy\" -name VERILOG_FILE [file join $::quartus(qip_path) "
              "\"tmds_tx_phy.v\"]\n"
              "set_global_assignment -library \"tmds_tx_phy\" -name SYSTEMVERILOG_FILE [file join $::quartus(qip_path) "
              "\"tmds_tx_phy/altera_xcvr_native_av.sv\"]\n"
              "set_global_assignment -library \"tmds_tx_phy\" -name SYSTEMVERILOG_FILE [file join $::quartus(qip_path) "
              "\"tmds_tx_phy/av_pcs.sv\"]\n");
    dir.write("top.v", "module top(input refclk, input clk_50, input [9:0] d, output reg [9:0] q, output reg [9:0] r);\n"
                       "  wire [3:0] clkout; wire [0:0] locked;\n"
                       "  reg [9:0] fab; always @(posedge clk_50) fab <= d;\n"
                       "  reg [9:0] px; always @(posedge clkout[0]) px <= d;\n"
                       "  tmds_tx_phy phy(.tx_pll_refclk({1'b0, refclk}), .tx_std_coreclkin({4{clkout[0]}}),\n"
                       "                  .tx_std_clkout(clkout), .tx_parallel_data({30'd0, px}), .tx_digitalreset(4'd0),\n"
                       "                  .pll_locked(locked));\n"
                       "  always @(posedge clkout[0]) q <= fab;\n"
                       "  always @(posedge clk_50) r <= {9'd0, locked[0]};\n"
                       "endmodule\n");
    dir.write("top.sdc", "create_clock -name refclk -period 8 [get_ports refclk]\n"
                         "create_clock -name clk_50 -period 20 [get_ports clk_50]\n"
                         "set_clock_groups -asynchronous -group [get_clocks clk_50] -group [get_clocks {*txpmalocalclk*}]\n");
    dir.write("t.qsf", "set_global_assignment -name TOP_LEVEL_ENTITY top\n"
                       "set_global_assignment -name VERILOG_FILE top.v\n"
                       "set_global_assignment -name QIP_FILE ip/tmds_tx_phy.qip\n"
                       "set_global_assignment -name SDC_FILE top.sdc\n");
    auto cfg = loadProjectFile(dir.path() / "t.qsf");
    REQUIRE(cfg);
    INFO(notesOf(cfg.value()));
    // the core's generated files are not read; the wrapper is
    CHECK(names(cfg.value().sources) == std::vector<std::string>{"top.v", "tmds_tx_phy.v"});
    CHECK(hasNote(cfg.value(), "PRJ-IP-MODEL", "2 generated file(s) not read"));
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto result = session.run(cfg.value(), progress, cancel);
    for (const auto& d : result->diagnostics)
        UNSCOPED_INFO(format(d));
    REQUIRE(result->clocks);
    REQUIRE(result->cdc);
    CHECK(std::ranges::none_of(result->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    // tx_std_clkout: 1250 Mbps / 10 = 125 MHz, from the selected reference clock; named txpmalocalclk for the SDC
    const clock::Clock* tx = nullptr;
    for (const auto& c : result->clocks->clocks)
        if (std::ranges::any_of(c.aliases, [](const std::string& a) { return a.find("txpmalocalclk") != std::string::npos; }))
            tx = &c;
    REQUIRE(tx);
    REQUIRE(tx->periodNs);
    CHECK(*tx->periodNs == 8.0);
    // fab (clk_50) -> q (the transceiver clock): asynchronous by the clock groups, an unsynchronized crossing;
    // the PLL lock is an asynchronous status
    CHECK(result->cdc->stats.unsynchronized >= 1);
    // negative: without the .qip's IP_TOOL_NAME the generated files are read (and their atoms are unknown)
    dir.write("ip/tmds_tx_phy.qip",
              "set_global_assignment -library \"tmds_tx_phy\" -name VERILOG_FILE [file join $::quartus(qip_path) "
              "\"tmds_tx_phy.v\"]\n"
              "set_global_assignment -library \"tmds_tx_phy\" -name SYSTEMVERILOG_FILE [file join $::quartus(qip_path) "
              "\"tmds_tx_phy/altera_xcvr_native_av.sv\"]\n");
    auto plain = loadProjectFile(dir.path() / "t.qsf");
    REQUIRE(plain);
    CHECK(names(plain.value().sources) == std::vector<std::string>{"top.v", "tmds_tx_phy.v", "altera_xcvr_native_av.sv"});
    CHECK_FALSE(hasNote(plain.value(), "PRJ-IP-MODEL"));
}

// FuseSoC cores (CAPI2 .core, stage 82): targets, conditional filesets, dependency
// order, parameters, generators (lowRISC primgen written by OpenCDC) and the analysis.

#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/FuseSocProject.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

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

std::string define(const ProjectConfig& cfg, const std::string& name) {
    for (const auto& d : cfg.defines)
        if (d.name == name)
            return d.value ? *d.value : std::string("(no value)");
    return "(undefined)";
}

std::string param(const ProjectConfig& cfg, const std::string& name) {
    for (const auto& [n, v] : cfg.parameters)
        if (n == name)
            return v;
    return "(none)";
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream t;
    t << in.rdbuf();
    return t.str();
}

void writeDemo(test::TempDir& dir) {
    dir.write("lib/fifo/fifo.core", "CAPI=2:\n"
                                    "name: acme:ip:fifo:1.0\n"
                                    "filesets:\n"
                                    "  rtl:\n"
                                    "    files: [rtl/fifo_pkg.sv, rtl/fifo.sv]\n"
                                    "    file_type: systemVerilogSource\n"
                                    "targets:\n"
                                    "  default:\n"
                                    "    filesets: [rtl]\n");
    // an older version of the same core: not used
    dir.write("old/fifo.core", "CAPI=2:\nname: acme:ip:fifo:0.9\nfilesets:\n  rtl:\n    files: [x.sv]\n"
                               "    file_type: systemVerilogSource\ntargets:\n  default:\n    filesets: [rtl]\n");
    dir.write("lib/fifo/rtl/fifo_pkg.sv", "package fifo_pkg; endpackage\n");
    dir.write("lib/fifo/rtl/fifo.sv", "module fifo; endmodule\n");
    dir.write("lib/vh/vh.core", "CAPI=2:\nname: acme:ip:vh\nfilesets:\n  rtl:\n"
                                "    files:\n      - core.vhd: {logical_name: acme}\n"
                                "    file_type: vhdlSource-2008\ntargets:\n  default:\n    filesets: [rtl]\n");
    dir.write("lib/vh/core.vhd", "entity core is end; architecture a of core is begin end;\n");
    dir.write("rtl/top.sv", "module top; endmodule\n");
    dir.write("rtl/defs.svh", "`define W 8\n");
    dir.write("rtl/xilinx.sv", "module x; endmodule\n");
    dir.write("rtl/generic.sv", "module g; endmodule\n");
    dir.write("rtl/sim.sv", "module s; endmodule\n");
    dir.write("data/pins.xdc", "create_clock -period 10 [get_ports clk]\n");
    dir.write("data/mem.vmem", "00\n");
    dir.write("top.core", "CAPI=2:\n"
                          "name: acme:demo:top:0.1\n"
                          "filesets:\n"
                          "  files_rtl:\n"
                          "    depend:\n"
                          "      - \">=acme:ip:fifo:0.5\"\n"
                          "      - acme:ip:vh\n"
                          "      - acme:ip:missing\n"
                          "    files:\n"
                          "      - rtl/defs.svh: {is_include_file: true}\n"
                          "      - rtl/top.sv\n"
                          "      - \"tool_vivado ? (rtl/xilinx.sv)\"\n"
                          "      - \"!tool_vivado ? (rtl/generic.sv)\"\n"
                          "    file_type: systemVerilogSource\n"
                          "  files_sim:\n"
                          "    files: [rtl/sim.sv]\n"
                          "    file_type: systemVerilogSource\n"
                          "  files_constraints:\n"
                          "    files:\n"
                          "      - data/pins.xdc\n"
                          "      - data/mem.vmem: {file_type: user}\n"
                          "    file_type: xdc\n"
                          "parameters:\n"
                          "  SRAMInitFile:\n"
                          "    datatype: str\n"
                          "    default: mem.vmem\n"
                          "    paramtype: vlogparam\n"
                          "  UseFast:\n"
                          "    datatype: bool\n"
                          "    paramtype: vlogparam\n"
                          "  FPGA:\n"
                          "    datatype: bool\n"
                          "    paramtype: vlogdefine\n"
                          "targets:\n"
                          "  default: &default_target\n"
                          "    filesets:\n"
                          "      - files_rtl\n"
                          "      - target_sim ? (files_sim)\n"
                          "    toplevel: top\n"
                          "  sim:\n"
                          "    <<: *default_target\n"
                          "    default_tool: verilator\n"
                          "  synth:\n"
                          "    <<: *default_target\n"
                          "    default_tool: vivado\n"
                          "    filesets_append: [files_constraints]\n"
                          "    parameters: [SRAMInitFile, UseFast=true, FPGA=1]\n");
}

} // namespace

TEST_CASE("FuseSoC core: the synth target, its dependencies and parameters", "[engine][fusesoc]") {
    test::TempDir dir;
    writeDemo(dir);
    REQUIRE(isFuseSocCoreFile(dir.path() / "top.CORE"));
    REQUIRE(isVendorProjectFile(dir.path() / "top.core"));
    auto r = loadProjectFile(dir.path() / "top.core");
    REQUIRE(r);
    const auto& cfg = r.value();
    CHECK(cfg.name == "top");
    CHECK(cfg.top == "top");
    // dependencies first (fifo at its highest version), then the core's own files;
    // tool_vivado selects xilinx.sv, target_sim is false
    CHECK(names(cfg.sources) == std::vector<std::string>{"fifo_pkg.sv", "fifo.sv", "core.vhd", "top.sv", "xilinx.sv"});
    REQUIRE(cfg.sourceLibraries.size() == 5);
    CHECK(cfg.sourceLibraries[2] == "acme");
    CHECK(cfg.language == frontend::LanguageStd::SystemVerilog2017);
    REQUIRE(cfg.includeDirs.size() == 1);
    CHECK(cfg.includeDirs[0].filename() == "rtl");
    CHECK(names(cfg.constraints) == std::vector<std::string>{"pins.xdc"});
    CHECK(param(cfg, "SRAMInitFile") == "\"mem.vmem\"");
    CHECK(param(cfg, "UseFast") == "1");
    CHECK(define(cfg, "FPGA") == "1");
    CHECK(define(cfg, "SYNTHESIS") == "(no value)"); // Vivado defines it
    CHECK(hasNote(cfg, "PRJ-CORE-MISSING-DEP", "acme:ip:missing"));
    CHECK(hasNote(cfg, "PRJ-CORE-SKIPPED", "user (x1)"));
    CHECK(hasNote(cfg, "PRJ-CORE-IMPORT", "target 'synth', tool vivado, 3 core(s)"));
}

TEST_CASE("FuseSoC core: another target and its conditions", "[engine][fusesoc]") {
    test::TempDir dir;
    writeDemo(dir);
    auto r = projectFromCoreFile(dir.path() / "top.core", "sim");
    REQUIRE(r);
    const auto& cfg = r.value();
    // verilator: not tool_vivado, target_sim adds the simulation files
    CHECK(names(cfg.sources) ==
          std::vector<std::string>{"fifo_pkg.sv", "fifo.sv", "core.vhd", "top.sv", "generic.sv", "sim.sv"});
    CHECK(cfg.constraints.empty());
    CHECK(cfg.parameters.empty());
    CHECK(define(cfg, "SYNTHESIS") == "(undefined)"); // a simulator does not define it
}

TEST_CASE("FuseSoC core negative: no target, not a core, generators", "[engine][fusesoc]") {
    test::TempDir dir;
    writeDemo(dir);
    auto noTarget = projectFromCoreFile(dir.path() / "top.core", "lint");
    REQUIRE_FALSE(noTarget);
    CHECK(noTarget.error().code == "PRJ-CORE-TARGET");
    dir.write("bad.core", "just text\n");
    auto bad = projectFromCoreFile(dir.path() / "bad.core");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "PRJ-CORE-PARSE");
    // the unreadable core is reported when another core is imported
    auto r = projectFromCoreFile(dir.path() / "top.core");
    REQUIRE(r);
    CHECK(hasNote(r.value(), "PRJ-CORE-UNREADABLE", "bad.core"));
    // a generator other than primgen is not run
    test::TempDir g;
    g.write("a.sv", "module a; endmodule\n");
    g.write("g.core", "CAPI=2:\nname: x:y:g\nfilesets:\n  rtl:\n    files: [a.sv]\n    file_type: systemVerilogSource\n"
                      "generate:\n  regs:\n    generator: reggen\n    parameters: {ip_hjson: a.hjson}\n"
                      "targets:\n  default:\n    filesets: [rtl]\n    generate: [regs]\n");
    auto gr = projectFromCoreFile(g.path() / "g.core");
    REQUIRE(gr);
    CHECK(hasNote(gr.value(), "PRJ-CORE-GENERATOR", "generator 'reggen'"));
    // a target without sources
    g.write("e.core", "CAPI=2:\nname: x:y:e\ntargets:\n  default: {}\n");
    auto e = projectFromCoreFile(g.path() / "e.core");
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "PRJ-CORE-PARSE");
}

TEST_CASE("FuseSoC core: lowRISC primgen output written by OpenCDC", "[engine][fusesoc][primgen]") {
    test::TempDir dir;
    auto primCore = [&](const std::string& lib, const std::string& file) {
        dir.write("prim/" + lib + "/" + lib + "_flop.core",
                  "CAPI=2:\nname: lowrisc:prim_" + lib + ":flop\nfilesets:\n  rtl:\n    files: [" + file +
                      "]\n    file_type: systemVerilogSource\ntargets:\n  default:\n    filesets: [rtl]\n");
    };
    primCore("generic", "prim_generic_flop.sv");
    primCore("xilinx", "prim_xilinx_flop.sv");
    dir.write("prim/generic/prim_generic_flop.sv",
              "// header comment with module fake;\n"
              "module prim_generic_flop #(\n"
              "  parameter int Width = 1,\n"
              "  parameter logic [Width-1:0] ResetValue = 0 // reset\n"
              ") (\n"
              "  input clk_i, input rst_ni,\n"
              "  input [Width-1:0] d_i, output logic [Width-1:0] q_o\n"
              ");\n"
              "  always_ff @(posedge clk_i or negedge rst_ni) if (!rst_ni) q_o <= ResetValue; else q_o <= d_i;\n"
              "endmodule\n");
    dir.write("prim/xilinx/prim_xilinx_flop.sv",
              "module prim_xilinx_flop #(parameter int Width = 1, parameter logic [Width-1:0] ResetValue = 0)\n"
              "  (input clk_i, input rst_ni, input [Width-1:0] d_i, output logic [Width-1:0] q_o);\n"
              "  always_ff @(posedge clk_i or negedge rst_ni) if (!rst_ni) q_o <= ResetValue; else q_o <= d_i;\n"
              "endmodule\n");
    dir.write("prim/prim_pkg.core", "CAPI=2:\nname: lowrisc:prim:prim_pkg\n"
                                    "generate:\n  impl:\n    generator: primgen\n"
                                    "    parameters: {action: generate_prim_pkg}\n"
                                    "targets:\n  default:\n    generate: [impl]\n");
    dir.write("prim/flop.core", "CAPI=2:\nname: lowrisc:prim:flop\n"
                                "generate:\n  impl:\n    generator: primgen\n    parameters: {prim_name: flop}\n"
                                "targets:\n  default:\n    generate: [impl]\n");
    dir.write("top.sv", "module top(input clk, input rst_n, input [3:0] d, output [3:0] q);\n"
                        "  prim_flop #(.Width(4), .ResetValue('0)) u_flop (.clk_i(clk), .rst_ni(rst_n), .d_i(d), .q_o(q));\n"
                        "endmodule\n");
    dir.write("top.core", "CAPI=2:\nname: x:y:top\nfilesets:\n  rtl:\n    depend: [lowrisc:prim:flop]\n"
                          "    files: [top.sv]\n    file_type: systemVerilogSource\n"
                          "targets:\n  synth:\n    default_tool: vivado\n    filesets: [rtl]\n    toplevel: top\n");
    auto r = projectFromCoreFile(dir.path() / "top.core");
    REQUIRE(r);
    const auto& cfg = r.value();
    CHECK(names(cfg.sources) == std::vector<std::string>{"prim_pkg.sv", "prim_generic_flop.sv", "prim_xilinx_flop.sv",
                                                         "prim_flop.sv", "top.sv"});
    CHECK(hasNote(cfg, "PRJ-CORE-GENERATED", "prim_flop.sv (primgen)"));
    const auto pkg = slurp(cfg.sources[0]);
    CHECK(pkg.find("ImplGeneric,\n    ImplXilinx") != std::string::npos);
    const auto abstract = slurp(cfg.sources[3]);
    CHECK(abstract.find("module prim_flop") != std::string::npos);
    CHECK(abstract.find("if (Impl == prim_pkg::ImplXilinx) begin : gen_xilinx") != std::string::npos);
    CHECK(abstract.find(".ResetValue(ResetValue)") != std::string::npos);
    CHECK(abstract.find("fake") == std::string::npos); // the comment is not read as a header
    // the generated design elaborates and selects the generic implementation by default
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto res = session.run(cfg, progress, cancel);
    for (const auto& d : res->diagnostics)
        UNSCOPED_INFO(format(d));
    CHECK(std::ranges::none_of(res->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    REQUIRE(res->design);
    CHECK(res->design->stats().registers == 1); // the generic flop, 4 bits in one register
    // negative: a primitive without a generic implementation is reported
    dir.write("prim/bad.core", "CAPI=2:\nname: lowrisc:prim:bad\n"
                               "generate:\n  impl:\n    generator: primgen\n    parameters: {prim_name: bad}\n"
                               "targets:\n  default:\n    generate: [impl]\n");
    dir.write("top.core", "CAPI=2:\nname: x:y:top\nfilesets:\n  rtl:\n    depend: [lowrisc:prim:flop, lowrisc:prim:bad]\n"
                          "    files: [top.sv]\n    file_type: systemVerilogSource\n"
                          "targets:\n  synth:\n    filesets: [rtl]\n    toplevel: top\n");
    auto r2 = projectFromCoreFile(dir.path() / "top.core");
    REQUIRE(r2);
    CHECK(hasNote(r2.value(), "PRJ-CORE-GENERATOR", "no generic implementation of primitive 'bad'"));
}

TEST_CASE("FuseSoC core: SERV-style targets (flows, conditional names, icepll)", "[engine][fusesoc][icepll]") {
    test::TempDir dir;
    dir.write("rtl/top.v", "module top(input clk, output o); wire c;\n"
                           "  SB_PLL40_PAD #(`include \"pll.vh\") pll (.PACKAGEPIN(clk), .PLLOUTCORE(c), .RESETB(1'b1), "
                           ".BYPASS(1'b0));\n"
                           "  reg r; always @(posedge c) r <= ~r; assign o = r;\nendmodule\n");
    dir.write("rtl/ram_quartus.sv", "module ram; endmodule\n");
    dir.write("rtl/ram.v", "module ram; endmodule\n");
    dir.write("data/b.lpf", "FREQUENCY PORT \"clk\" 12 MHZ;\n");
    dir.write("data/b.pcf", "set_io clk 35\n");
    dir.write("data/b.cst", "IO_LOC \"clk\" 52;\n");
    dir.write("t.core", "CAPI=2:\n"
                        "name : x:y:t:1.0\n"
                        "filesets:\n"
                        "  soc:\n"
                        "    files:\n"
                        "      - rtl/top.v\n"
                        "      -  \"tool_quartus? (rtl/ram_quartus.sv)\" : {file_type : systemVerilogSource}\n"
                        "      - \"!tool_quartus? (rtl/ram.v)\"\n"
                        "    file_type : verilogSource\n"
                        "    depend : [\"fusesoc:utils:generators\"]\n"
                        "  ice: {files: [data/b.pcf : {file_type : PCF}]}\n"
                        "  ecp: {files: [data/b.lpf : {file_type : LPF}]}\n"
                        "  gw: {files: [data/b.cst : {file_type : CST}]}\n"
                        "parameters:\n"
                        "  memfile: {datatype: file, paramtype: vlogparam}\n"
                        "generate:\n"
                        "  ice40pll:\n"
                        "    generator: icepll\n"
                        "    parameters: {freq_out: 60}\n"
                        "targets:\n"
                        "  icebreaker:\n"
                        "    default_tool : icestorm\n"
                        "    filesets : [soc, ice]\n"
                        "    generate: [ice40pll : {freq_out : 16}]\n"
                        "    parameters : [memfile=blinky.hex]\n"
                        "    toplevel : top\n"
                        "  ecp:\n"
                        "    default_tool : diamond\n"
                        "    filesets : [soc, ecp]\n"
                        "    toplevel : top\n"
                        "  tang:\n"
                        "    flow: gowin\n"
                        "    filesets : [soc, gw]\n"
                        "    toplevel : top\n"
                        "  de0:\n"
                        "    default_tool : quartus\n"
                        "    filesets : [soc]\n"
                        "    toplevel : top\n");
    auto ice = projectFromCoreFile(dir.path() / "t.core", "icebreaker");
    REQUIRE(ice);
    const auto& cfg = ice.value();
    CHECK(names(cfg.sources) == std::vector<std::string>{"top.v", "ram.v"});
    CHECK(cfg.constraints.empty()); // a PCF has no timing
    CHECK(hasNote(cfg, "PRJ-CORE-SKIPPED", "PCF (x1)"));
    CHECK_FALSE(hasNote(cfg, "PRJ-CORE-MISSING-DEP")); // the generators' core: built in
    CHECK(param(cfg, "memfile") == "\"blinky.hex\"");
    CHECK(define(cfg, "SYNTHESIS") == "(no value)"); // icestorm runs Yosys
    // icepll for 12 -> 16 MHz (the target's freq_out replaces the generator's 60): VCO 12 x 85 / 64
    CHECK(hasNote(cfg, "PRJ-CORE-GENERATED", "pll.vh (icepll, 12 -> 15.9375 MHz)"));
    REQUIRE_FALSE(cfg.includeDirs.empty());
    const auto vh = slurp(cfg.includeDirs.back() / "pll.vh");
    CHECK(vh.find(".DIVR(4'b0000)") != std::string::npos);
    CHECK(vh.find(".DIVF(7'b1010100)") != std::string::npos); // 84
    CHECK(vh.find(".DIVQ(3'b110)") != std::string::npos);     // 6
    CHECK(vh.find(".FILTER_RANGE(3'b001)") != std::string::npos);
    // the analysis: the PLL output is a clock of 12 MHz x 85 / 64 when the input has 12 MHz
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto res = session.run(cfg, progress, cancel);
    for (const auto& d : res->diagnostics)
        UNSCOPED_INFO(format(d));
    CHECK(std::ranges::none_of(res->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    REQUIRE(res->clocks);
    CHECK(std::ranges::any_of(res->clocks->clocks, [](const auto& c) { return c.name == "top.pll.PLLOUTCORE"; }));

    auto ecp = projectFromCoreFile(dir.path() / "t.core", "ecp");
    REQUIRE(ecp);
    CHECK(names(ecp.value().constraints) == std::vector<std::string>{"b.lpf"}); // read for its clocks
    CHECK(define(ecp.value(), "SYNTHESIS") == "(undefined)");
    auto tang = projectFromCoreFile(dir.path() / "t.core", "tang");
    REQUIRE(tang);
    CHECK(hasNote(tang.value(), "PRJ-CORE-IMPORT", "tool gowin"));
    CHECK(hasNote(tang.value(), "PRJ-CORE-SKIPPED", "CST (x1)"));
    // negative: with Quartus the condition selects the other RAM
    auto de0 = projectFromCoreFile(dir.path() / "t.core", "de0");
    REQUIRE(de0);
    CHECK(names(de0.value().sources) == std::vector<std::string>{"top.v", "ram_quartus.sv"});
    // negative: an input frequency icepll cannot use
    dir.write("bad.core", "CAPI=2:\nname: x:y:bad\nfilesets:\n  s: {files: [rtl/ram.v], file_type: verilogSource}\n"
                          "generate:\n  p: {generator: icepll, parameters: {freq_in: 5}}\n"
                          "targets:\n  default: {filesets: [s], generate: [p]}\n");
    auto bad = projectFromCoreFile(dir.path() / "bad.core");
    REQUIRE(bad);
    CHECK(hasNote(bad.value(), "PRJ-CORE-GENERATOR", "icepll: no PLL setting for 5 MHz"));
}

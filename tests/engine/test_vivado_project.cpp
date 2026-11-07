#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "engine/VivadoProject.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;
using namespace opencdc::engine;

namespace {

/// A <File> element with its FileInfo attributes.
std::string file(const std::string& path, const std::vector<std::pair<std::string, std::string>>& attrs = {
                                              {"UsedIn", "synthesis"}, {"UsedIn", "simulation"}}) {
    std::string s = "      <File Path=\"" + path + "\">\n        <FileInfo>\n";
    for (const auto& [k, v] : attrs)
        s += "          <Attr Name=\"" + k + "\" Val=\"" + v + "\"/>\n";
    return s + "        </FileInfo>\n      </File>\n";
}

std::string fileSet(const std::string& name, const std::string& type, const std::string& files,
                    const std::string& options = "") {
    return "    <FileSet Name=\"" + name + "\" Type=\"" + type + "\" RelSrcDir=\"$PSRCDIR/" + name + "\">\n" + files +
           "      <Config>\n" + options + "      </Config>\n    </FileSet>\n";
}

std::string xpr(const std::string& sets, const std::string& runs, const std::string& projectOptions = "") {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- Product Version: Vivado v2023.2 -->\n"
           "<Project Product=\"Vivado\" Version=\"7\" Minor=\"67\" Path=\"C:/elsewhere/p.xpr\">\n"
           "  <Configuration>\n    <Option Name=\"DefaultLib\" Val=\"xil_defaultlib\"/>\n" +
           projectOptions + "  </Configuration>\n  <FileSets Version=\"1\" Minor=\"32\">\n" + sets +
           "  </FileSets>\n  <Runs Version=\"1\" Minor=\"22\">\n" + runs + "  </Runs>\n</Project>\n";
}

std::string opt(const std::string& name, const std::string& val) {
    return "        <Option Name=\"" + name + "\" Val=\"" + val + "\"/>\n";
}

const std::string kSynthRun = "    <Run Id=\"synth_1\" Type=\"Ft3:Synth\" SrcSet=\"sources_1\" ConstrsSet=\"constrs_1\" "
                              "State=\"current\"/>\n";

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

TEST_CASE("Vivado project: the sources, IP and constraints of the current synthesis run", "[engine][xpr]") {
    test::TempDir dir;
    dir.write("p/p.srcs/sources_1/new/top.v", "module top; endmodule\n");
    dir.write("p/p.srcs/sources_1/new/pkg.vhd", "package pkg is end package;\n");
    dir.write("p/p.srcs/sources_1/new/defs.vh", "`define W 8\n");
    dir.write("rtl/inc/global.v", "`define G 1\n");
    dir.write("rtl/core.vhd", "entity core is end entity;\n");
    dir.write("rtl/old.v", "module old; endmodule\n");
    dir.write("rtl/model.v", "module model; endmodule\n");
    dir.write("rtl/rom.coe", "memory_initialization_radix=16;\n");
    dir.write("rtl/core.dcp", "PK");
    dir.write("p/p.srcs/sources_1/ip/fifo/fifo.xci", "<xci/>");
    dir.write("p/p.srcs/sources_1/ip/fifo/fifo_ooc.xdc", "");
    dir.write("p/p.gen/sources_1/bd/sys/hdl/sys_wrapper.v", "module sys_wrapper; endmodule\n");
    dir.write("p/p.srcs/sources_1/bd/sys/sys.bd", "{}");
    dir.write("p/p.srcs/sources_1/bd/sys/ip/sys_xbar_0/sys_xbar_0.xci", "<xci/>");
    dir.write("ip/cc.xcix", "PK");
    dir.write("ip/cc/cc.xci", "<xci/>");
    dir.write("p/p.srcs/constrs_1/new/top.xdc", "");
    dir.write("p/p.srcs/constrs_1/new/sync.xdc", "");
    dir.write("p/p.srcs/constrs_1/new/cell.xdc", "");
    dir.write("p/p.srcs/constrs_1/new/ooc.xdc", "");
    dir.write("p/p.srcs/constrs_1/new/impl.tcl", "");
    dir.write("other/c.xdc", "");
    std::filesystem::create_directories(dir.path() / "repo");

    const std::string sources =
        file("$PSRCDIR/sources_1/new/defs.vh") +
        file("$PSRCDIR/sources_1/new/pkg.vhd", {{"Library", "my_lib"}, {"UsedIn", "synthesis"}}) +
        file("$PPRDIR/../rtl/core.vhd") +                                      // default library
        file("$PPRDIR/../rtl/inc/global.v", {{"IsGlobalInclude", "1"}, {"UsedIn", "synthesis"}}) +
        file("$PSRCDIR/sources_1/new/top.v") +
        file("$PPRDIR/../rtl/old.v", {{"AutoDisabled", "1"}, {"UsedIn", "synthesis"}}) +
        file("$PPRDIR/../rtl/model.v", {{"UsedIn", "simulation"}}) +           // simulation only
        file("$PPRDIR/../rtl/rom.coe") +
        file("$PPRDIR/../rtl/core.dcp") +
        file("$PSRCDIR/sources_1/bd/sys/sys.bd") +
        file("$PGENDIR/sources_1/bd/sys/hdl/sys_wrapper.v") +
        file("$PPRDIR/../ip/cc.xcix") + file("$PPRDIR/../ip/cc/cc.xci") +   // the container brings its .xci
        file("$PPRDIR/../rtl/missing.v");
    const std::string sets =
        fileSet("sources_1", "DesignSrcs", sources,
                opt("TopModule", "top") + opt("VerilogDefine", "SYNTH WIDTH=16") + opt("VerilogDefine", "MODE=2") +
                    opt("VerilogDir", "$PPRDIR/../rtl/inc2") + opt("Generic", "DEPTH=32")) +
        // a top-level IP in its own file set (its output-product constraint is not read); a block design's IP
        fileSet("fifo", "BlockSrcs",
                file("$PSRCDIR/sources_1/ip/fifo/fifo.xci") + file("$PSRCDIR/sources_1/ip/fifo/fifo_ooc.xdc")) +
        fileSet("sys_xbar_0", "BlockSrcs", file("$PSRCDIR/sources_1/bd/sys/ip/sys_xbar_0/sys_xbar_0.xci")) +
        fileSet("constrs_1", "Constrs",
                file("$PSRCDIR/constrs_1/new/top.xdc", {{"UsedIn", "synthesis"}, {"UsedIn", "implementation"}}) +
                    file("$PSRCDIR/constrs_1/new/sync.xdc",
                         {{"ScopedToRef", "sync"}, {"UsedIn", "synthesis"}, {"UsedIn", "implementation"}}) +
                    file("$PSRCDIR/constrs_1/new/cell.xdc", {{"ScopedToCell", "u_core"}, {"UsedIn", "synthesis"}}) +
                    file("$PSRCDIR/constrs_1/new/ooc.xdc", {{"UsedIn", "out_of_context"}}) +
                    file("$PSRCDIR/constrs_1/new/impl.tcl", {{"UsedIn", "implementation"}})) +
        fileSet("constrs_2", "Constrs", file("$PPRDIR/../other/c.xdc")) +
        fileSet("sim_1", "SimulationSrcs", file("$PPRDIR/../rtl/model.v"), opt("TopModule", "tb"));
    const auto x = dir.write("p/p.xpr", xpr(sets, kSynthRun,
                                             "    <Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../repo\"/>\n"
                                             "    <Option Name=\"IPRepoPath\" Val=\"$PPRDIR/../nowhere\"/>\n"));

    auto r = loadProjectFile(x);
    REQUIRE(r);
    const auto& cfg = r.value();
    INFO([&] {
        std::string s;
        for (const auto& d : cfg.importNotes)
            s += format(d) + "\n";
        return s;
    }());
    CHECK(cfg.name == "p");
    CHECK(cfg.top == "top");
    CHECK(cfg.projectFile.empty()); // never saved over the .xpr
    CHECK(cfg.importedFrom == std::filesystem::absolute(x).lexically_normal());
    CHECK(names(cfg.sources) == std::vector<std::string>{"pkg.vhd", "core.vhd", "top.v", "sys.bd", "sys_wrapper.v",
                                                         "cc.xcix", "fifo.xci"});
    CHECK(cfg.sourceLibraries == std::vector<std::string>{"my_lib", "xil_defaultlib", "", "", "", "", ""});
    // headers are no sources: their directories are include directories (and VerilogDir)
    CHECK(names(cfg.includeDirs) == std::vector<std::string>{"new", "inc", "inc2"});
    REQUIRE(cfg.defines.size() == 3);
    CHECK(cfg.defines[0].name == "SYNTH");
    CHECK_FALSE(cfg.defines[0].value);
    CHECK(cfg.defines[1].name == "WIDTH");
    CHECK(cfg.defines[1].value == "16");
    CHECK(cfg.defines[2].value == "2");
    CHECK(cfg.parameters == std::vector<std::pair<std::string, std::string>>{{"DEPTH", "32"}});
    CHECK(names(cfg.constraints) == std::vector<std::string>{"top.xdc", "sync.xdc", "impl.tcl"});
    CHECK(cfg.constraintScopes == std::vector<std::string>{"", "sync", ""});
    REQUIRE(cfg.ipRepositories.size() == 1);
    CHECK(cfg.ipRepositories[0].filename() == "repo");
    // negative: what is not read is said
    CHECK(hasNote(cfg, "PRJ-XPR-DISABLED", "1 disabled source"));
    CHECK(hasNote(cfg, "PRJ-XPR-SKIPPED", "rom.coe"));
    CHECK(hasNote(cfg, "PRJ-XPR-SKIPPED", "model.v (not used in synthesis)"));
    CHECK(hasNote(cfg, "PRJ-XPR-NETLIST", "core.dcp"));
    CHECK(hasNote(cfg, "PRJ-XPR-MISSING", "rtl/missing.v"));
    CHECK(hasNote(cfg, "PRJ-XPR-MISSING", "IP repository not found: $PPRDIR/../nowhere"));
    CHECK(hasNote(cfg, "PRJ-XPR-SCOPED", "cell 'u_core'"));
    CHECK(hasNote(cfg, "PRJ-XPR-IMPORT", "7 source(s), 3 constraint file(s), top 'top'"));
    for (const auto& d : cfg.importNotes)
        CHECK(d.severity != Severity::Error);
}

TEST_CASE("Vivado project: the file sets of the current run, the IP user files directory", "[engine][xpr]") {
    test::TempDir dir;
    dir.write("a.v", "module a; endmodule\n");
    dir.write("b.v", "module b; endmodule\n");
    dir.write("a.xdc", "");
    dir.write("b.xdc", "");
    dir.write("shared/ipu/ipstatic/x.v", "");
    const std::string sets = fileSet("sources_1", "DesignSrcs", file("$PPRDIR/a.v"), opt("TopModule", "a")) +
                             fileSet("sources_2", "DesignSrcs", file("$PPRDIR/b.v"), opt("TopModule", "b")) +
                             fileSet("constrs_1", "Constrs", file("$PPRDIR/a.xdc")) +
                             fileSet("constrs_2", "Constrs", file("$PPRDIR/b.xdc"));
    // synth_2 is the current run
    const std::string runs =
        "    <Run Id=\"synth_1\" Type=\"Ft3:Synth\" SrcSet=\"sources_1\" ConstrsSet=\"constrs_1\"/>\n"
        "    <Run Id=\"synth_2\" Type=\"Ft3:Synth\" SrcSet=\"sources_2\" ConstrsSet=\"constrs_2\" State=\"current\"/>\n"
        "    <Run Id=\"impl_2\" Type=\"Ft2:EntireDesign\" ConstrsSet=\"constrs_1\" State=\"current\"/>\n";
    auto r = projectFromXprText(xpr(sets, runs), dir.path() / "p.xpr");
    REQUIRE(r);
    CHECK(r.value().top == "b");
    CHECK(names(r.value().sources) == std::vector<std::string>{"b.v"});
    CHECK(names(r.value().constraints) == std::vector<std::string>{"b.xdc"});
    // negative: without runs, sources_1 and constrs_1
    auto r1 = projectFromXprText(xpr(sets, ""), dir.path() / "p.xpr");
    REQUIRE(r1);
    CHECK(r1.value().top == "a");
    CHECK(names(r1.value().constraints) == std::vector<std::string>{"a.xdc"});
    // $PIPUSERFILESDIR from the project's IPUserFilesDir
    const std::string ipu = fileSet("sources_1", "DesignSrcs", file("$PIPUSERFILESDIR/ipstatic/x.v"));
    auto r2 = projectFromXprText(
        xpr(ipu, "", "    <Option Name=\"IPUserFilesDir\" Val=\"$PPRDIR/shared/ipu\"/>\n"), dir.path() / "p.xpr");
    REQUIRE(r2);
    REQUIRE(r2.value().sources.size() == 1);
    CHECK(r2.value().sources[0] == (dir.path() / "shared/ipu/ipstatic/x.v").lexically_normal());
}

TEST_CASE("Vivado project negative: not a project, no design sources, broken XML", "[engine][xpr]") {
    test::TempDir dir;
    const auto p = dir.path() / "p.xpr";
    auto notProject = projectFromXprText("<?xml version=\"1.0\"?>\n<bd:repository/>\n", p);
    REQUIRE_FALSE(notProject);
    CHECK(notProject.error().code == "PRJ-XPR-PARSE");
    auto noSources = projectFromXprText(xpr(fileSet("constrs_1", "Constrs", ""), ""), p);
    REQUIRE_FALSE(noSources);
    CHECK(noSources.error().message.find("no design sources") != std::string::npos);
    auto broken = projectFromXprText("<Project><FileSets><FileSet Name=\"x\"></Project>", p);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().code == "PRJ-XPR-PARSE");
    auto missing = loadProjectFile(dir.path() / "none.xpr");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "PRJ-OPEN");
    CHECK_FALSE(isVivadoProjectFile("p.ocdc.json"));
    CHECK(isVivadoProjectFile("P.XPR"));
}

TEST_CASE("Vivado project: analyzed directly, the import notes with the diagnostics", "[engine][xpr]") {
    test::TempDir dir;
    dir.write("rtl/top.v", "module top(input clk_a, input clk_b, input d, output reg q);\n"
                           "  reg a; always @(posedge clk_a) a <= d;\n"
                           "  always @(posedge clk_b) q <= a;\nendmodule\n");
    dir.write("c.xdc", "create_clock -period 10 [get_ports clk_a]\ncreate_clock -period 7 [get_ports clk_b]\n");
    const auto x = dir.write(
        "p.xpr", xpr(fileSet("sources_1", "DesignSrcs", file("$PPRDIR/rtl/top.v") + file("$PPRDIR/rtl/gone.v"),
                             opt("TopModule", "top")) +
                         fileSet("constrs_1", "Constrs", file("$PPRDIR/c.xdc")),
                     kSynthRun));
    auto cfg = loadProjectFile(x);
    REQUIRE(cfg);
    AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    const auto result = session.run(cfg.value(), progress, cancel);
    REQUIRE(result->cdc);
    CHECK(result->cdc->stats.unsynchronized == 1);
    const auto has = [&](const std::string& code) {
        return std::ranges::any_of(result->diagnostics, [&](const Diagnostic& d) { return d.code == code; });
    };
    CHECK(has("PRJ-XPR-IMPORT"));
    CHECK(has("PRJ-XPR-MISSING"));
    // saved as an OpenCDC project, it loads back with the same sources and no import notes
    const auto saved = dir.path() / "p.ocdc.json";
    REQUIRE_FALSE(saveProjectFile(cfg.value(), saved));
    auto back = loadProjectFile(saved);
    REQUIRE(back);
    CHECK(back.value().sources == cfg.value().sources);
    CHECK(back.value().constraints == cfg.value().constraints);
    CHECK(back.value().top == "top");
    CHECK(back.value().importNotes.empty());
    CHECK(back.value().importedFrom.empty());
}

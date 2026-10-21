#include "frontend/Frontend.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace opencdc;
using namespace opencdc::frontend;
namespace fs = std::filesystem;

namespace {

struct Run {
    Result<FrontendOutput> summary;
    std::vector<Diagnostic> diags;

    [[nodiscard]] bool hasErrors() const {
        return std::ranges::any_of(diags, [](const Diagnostic& d) { return d.isError(); });
    }
    [[nodiscard]] const Diagnostic* findCode(std::string_view code) const {
        auto it = std::ranges::find(diags, std::string(code), &Diagnostic::code);
        return it == diags.end() ? nullptr : &*it;
    }
    [[nodiscard]] std::vector<std::string> definitionNames() const {
        std::vector<std::string> names;
        for (const auto& d : summary.value().summary.definitions)
            names.push_back(d.name);
        return names;
    }
};

Run elaborate(const FrontendInput& input) {
    CollectingDiagnosticSink sink;
    auto frontend = makeSlangFrontend();
    auto summary = frontend->elaborate(input, sink);
    return {std::move(summary), sink.take()};
}

FrontendInput singleFile(const fs::path& file, LanguageStd lang = LanguageStd::Verilog2001) {
    FrontendInput in;
    in.sourceFiles = {file};
    in.language = lang;
    return in;
}

const InstanceInfo* child(const InstanceInfo& parent, std::string_view name) {
    auto it = std::ranges::find(parent.children, std::string(name), &InstanceInfo::name);
    return it == parent.children.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("LanguageStd string conversion round-trips", "[frontend]") {
    for (auto lang : {LanguageStd::Verilog2001, LanguageStd::Verilog2005, LanguageStd::SystemVerilog2017})
        CHECK(parseLanguageStd(toString(lang)) == lang);
    CHECK_FALSE(parseLanguageStd("vhdl-2008").has_value());
}

TEST_CASE("Smoke example: definitions, include files and hierarchy", "[frontend][smoke]") {
    const auto dir = test::examplesDir() / "smoke";
    FrontendInput in;
    for (const char* f : {"rtl/sync_2ff.v", "rtl/tx_domain.v", "rtl/rx_domain.v", "rtl/top.v"})
        in.sourceFiles.push_back(dir / f);
    in.includeDirs = {dir / "rtl" / "include"};
    in.top = "top";

    auto run = elaborate(in);
    REQUIRE(run.summary.ok());
    for (const auto& d : run.diags)
        UNSCOPED_INFO(format(d));
    CHECK_FALSE(run.hasErrors());

    const auto& s = run.summary.value().summary;
    CHECK(run.definitionNames() == std::vector<std::string>{"rx_domain", "sync_2ff", "top", "tx_domain"});
    for (const auto& d : s.definitions) {
        CHECK(d.kind == "module");
        CHECK(d.location.valid());
    }
    // `include "defs.vh" must have been resolved through the include directory.
    CHECK(std::ranges::any_of(s.parsedFiles, [](const fs::path& p) { return p.filename() == "defs.vh"; }));

    REQUIRE(s.topInstances.size() == 1);
    const auto& top = s.topInstances[0];
    CHECK(top.definition == "top");
    CHECK(top.hierPath == "top");

    const auto* rx = child(top, "u_rx");
    REQUIRE(rx);
    CHECK(rx->definition == "rx_domain");
    CHECK(rx->hierPath == "top.u_rx");
    CHECK(rx->location.line == 24); // instance name line in top.v
    REQUIRE(child(top, "u_tx"));

    // generate-for: 4 synchronizer instances plus u_valid_sync
    CHECK(rx->children.size() == 5);
    const auto* g2 = child(*rx, "g_status[2].u_sync");
    REQUIRE(g2);
    CHECK(g2->definition == "sync_2ff");
    CHECK(g2->hierPath == "top.u_rx.g_status[2].u_sync");
    REQUIRE(child(*rx, "u_valid_sync"));

    CHECK(s.instanceCount == 8);
}

TEST_CASE("Syntax errors are reported with file and line", "[frontend][diag]") {
    test::TempDir dir;
    auto file = dir.write("bad.v",
                          "module bad (input wire a);\n"
                          "  wire b;\n"
                          "  assign b = a\n"      // missing semicolon
                          "endmodule\n");
    auto run = elaborate(singleFile(file));
    REQUIRE(run.summary.ok()); // a summary is still produced
    REQUIRE(run.hasErrors());
    const auto& err = *std::ranges::find_if(run.diags, [](const Diagnostic& d) { return d.isError(); });
    CHECK(err.code.starts_with("FE-SLANG-"));
    CHECK(fs::path(err.location.file).filename() == "bad.v");
    CHECK(err.location.line >= 3);
    CHECK(err.location.line <= 4);
}

TEST_CASE("Unknown module instantiation is an error", "[frontend][diag]") {
    test::TempDir dir;
    auto file = dir.write("top.v", "module top; missing_mod u_x (); endmodule\n");
    auto run = elaborate(singleFile(file));
    REQUIRE(run.summary.ok());
    CHECK(run.findCode("FE-SLANG-UnknownModule") != nullptr);
}

TEST_CASE("Missing source file yields a frontend error", "[frontend][diag]") {
    test::TempDir dir;
    auto run = elaborate(singleFile(dir.path() / "nope.v"));
    REQUIRE_FALSE(run.summary.ok());
    CHECK(run.summary.error().code == "FE-FILE-NOT-FOUND");
}

TEST_CASE("No sources yields a frontend error", "[frontend][diag]") {
    auto run = elaborate(FrontendInput{});
    REQUIRE_FALSE(run.summary.ok());
    CHECK(run.summary.error().code == "FE-NO-SOURCES");
}

TEST_CASE("Missing include directory is a warning", "[frontend][diag]") {
    test::TempDir dir;
    auto in = singleFile(dir.write("m.v", "module m; endmodule\n"));
    in.includeDirs = {dir.path() / "no_such_dir"};
    auto run = elaborate(in);
    REQUIRE(run.summary.ok());
    const auto* d = run.findCode("FE-INCDIR-NOT-FOUND");
    REQUIRE(d);
    CHECK(d->severity == Severity::Warning);
}

TEST_CASE("Project defines drive conditional compilation", "[frontend][preprocessor]") {
    test::TempDir dir;
    auto file = dir.write("cond.v",
                          "`ifdef USE_B\n"
                          "module chosen_b; endmodule\n"
                          "`else\n"
                          "module chosen_a; endmodule\n"
                          "`endif\n");
    auto in = singleFile(file);

    auto runA = elaborate(in);
    REQUIRE(runA.summary.ok());
    CHECK(runA.definitionNames() == std::vector<std::string>{"chosen_a"});

    in.defines = {{"USE_B", std::nullopt}};
    auto runB = elaborate(in);
    REQUIRE(runB.summary.ok());
    CHECK(runB.definitionNames() == std::vector<std::string>{"chosen_b"});
}

TEST_CASE("Define values and parameters are evaluated", "[frontend][preprocessor]") {
    test::TempDir dir;
    auto file = dir.write("w.v",
                          "module w #(parameter W = `WIDTH) (output wire [W-1:0] o);\n"
                          "  assign o = {W{1'b1}};\n"
                          "endmodule\n");
    auto in = singleFile(file);
    in.defines = {{"WIDTH", "8"}};
    auto ok = elaborate(in);
    REQUIRE(ok.summary.ok());
    CHECK_FALSE(ok.hasErrors());

    in.defines.clear(); // `WIDTH undefined -> error
    auto bad = elaborate(in);
    REQUIRE(bad.summary.ok());
    CHECK(bad.hasErrors());
}

TEST_CASE("Verilog-2001 mode accepts SystemVerilog keywords as identifiers", "[frontend][language]") {
    test::TempDir dir;
    auto file = dir.write("kw.v",
                          "module kw (input wire clk, output wire o);\n"
                          "  reg logic;\n"            // 'logic' is a keyword only in SystemVerilog
                          "  wire bit;\n"
                          "  assign bit = logic;\n"
                          "  always @(posedge clk) logic <= ~logic;\n"
                          "  assign o = bit;\n"
                          "endmodule\n");

    auto v2001 = elaborate(singleFile(file, LanguageStd::Verilog2001));
    REQUIRE(v2001.summary.ok());
    for (const auto& d : v2001.diags)
        UNSCOPED_INFO(format(d));
    CHECK_FALSE(v2001.hasErrors());

    auto sv = elaborate(singleFile(file, LanguageStd::SystemVerilog2017));
    REQUIRE(sv.summary.ok());
    CHECK(sv.hasErrors());
}

TEST_CASE("Explicit top selection and invalid top", "[frontend][top]") {
    test::TempDir dir;
    auto file = dir.write("two.v",
                          "module leaf; endmodule\n"
                          "module a; leaf u_l (); endmodule\n"
                          "module b; endmodule\n");
    auto in = singleFile(file);

    auto inferred = elaborate(in);
    REQUIRE(inferred.summary.ok());
    CHECK(inferred.summary.value().summary.topInstances.size() == 2); // a and b are unreferenced

    in.top = "a";
    auto chosen = elaborate(in);
    REQUIRE(chosen.summary.ok());
    REQUIRE(chosen.summary.value().summary.topInstances.size() == 1);
    CHECK(chosen.summary.value().summary.topInstances[0].definition == "a");
    CHECK(chosen.summary.value().summary.topInstances[0].children.size() == 1);

    in.top = "does_not_exist";
    auto invalid = elaborate(in);
    REQUIRE(invalid.summary.ok());
    CHECK(invalid.hasErrors());
}

TEST_CASE("Uninstantiated generate branches are not in the hierarchy", "[frontend][generate]") {
    test::TempDir dir;
    auto file = dir.write("gen.v",
                          "module leaf; endmodule\n"
                          "module top #(parameter USE_X = 0);\n"
                          "  generate\n"
                          "    if (USE_X) begin : g_x\n"
                          "      leaf u_x ();\n"
                          "    end else begin : g_y\n"
                          "      leaf u_y ();\n"
                          "    end\n"
                          "  endgenerate\n"
                          "endmodule\n");
    auto in = singleFile(file);
    in.top = "top";
    auto run = elaborate(in);
    REQUIRE(run.summary.ok());
    CHECK_FALSE(run.hasErrors());
    const auto& top = run.summary.value().summary.topInstances.at(0);
    REQUIRE(top.children.size() == 1);
    CHECK(top.children[0].name == "g_y.u_y");
}

TEST_CASE("Instance arrays are expanded", "[frontend][hierarchy]") {
    test::TempDir dir;
    auto file = dir.write("arr.v",
                          "module leaf (input wire i); endmodule\n"
                          "module top (input wire [2:0] v);\n"
                          "  leaf u_l [2:0] (.i(v));\n"
                          "endmodule\n");
    auto in = singleFile(file);
    in.top = "top";
    auto run = elaborate(in);
    REQUIRE(run.summary.ok());
    CHECK_FALSE(run.hasErrors());
    const auto& top = run.summary.value().summary.topInstances.at(0);
    CHECK(top.children.size() == 3);
    CHECK(run.summary.value().summary.instanceCount == 4);
}

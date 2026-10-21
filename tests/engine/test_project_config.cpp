#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;
using namespace opencdc::engine;
namespace fs = std::filesystem;

namespace {
const fs::path kBase = fs::path("/proj").lexically_normal();
}

TEST_CASE("parseProjectJson accepts a complete project", "[engine][project]") {
    auto r = parseProjectJson(R"({
        "format": "opencdc-project", "version": 1, "name": "demo", "top": "top",
        "language": "verilog-2005",
        "sources": ["rtl/a.v", "rtl/b.v"],
        "includeDirs": ["inc"],
        "defines": {"WIDTH": "8", "FLAG": null, "N": 4},
        "constraints": ["c/clk.sdc"]
    })", kBase);
    REQUIRE(r.ok());
    const auto& c = r.value();
    CHECK(c.name == "demo");
    CHECK(c.top == "top");
    CHECK(c.language == frontend::LanguageStd::Verilog2005);
    REQUIRE(c.sources.size() == 2);
    CHECK(c.sources[0] == (kBase / "rtl/a.v").lexically_normal());
    CHECK(c.includeDirs.at(0) == (kBase / "inc").lexically_normal());
    CHECK(c.constraints.at(0) == (kBase / "c/clk.sdc").lexically_normal());
    REQUIRE(c.defines.size() == 3);
    auto find = [&](const std::string& n) {
        return *std::ranges::find(c.defines, n, &frontend::MacroDefine::name);
    };
    CHECK(find("WIDTH").value == std::optional<std::string>("8"));
    CHECK_FALSE(find("FLAG").value.has_value());
    CHECK(find("N").value == std::optional<std::string>("4"));
}

TEST_CASE("parseProjectJson applies defaults", "[engine][project]") {
    auto r = parseProjectJson(R"({"format":"opencdc-project","version":1,"sources":["a.v"]})", kBase);
    REQUIRE(r.ok());
    CHECK(r.value().name == "unnamed");
    CHECK(r.value().top.empty());
    CHECK(r.value().language == frontend::LanguageStd::Verilog2001);
}

TEST_CASE("parseProjectJson reads analysis options", "[engine][project]") {
    auto def = parseProjectJson(R"({"format":"opencdc-project","version":1,"sources":["a.v"]})", kBase);
    REQUIRE(def.ok());
    CHECK(def.value().options.unknownClockRelation == UnknownClockPolicy::Report);
    auto r = parseProjectJson(
        R"({"format":"opencdc-project","version":1,"sources":["a.v"],"options":{"unknownClockRelation":"assume_async"}})",
        kBase);
    REQUIRE(r.ok());
    CHECK(r.value().options.unknownClockRelation == UnknownClockPolicy::AssumeAsync);
}

TEST_CASE("parseProjectJson keeps absolute paths", "[engine][project]") {
    const auto abs = (fs::temp_directory_path() / "x.v").lexically_normal();
    auto json = std::string(R"({"format":"opencdc-project","version":1,"sources":[")") +
                abs.generic_string() + "\"]}";
    auto r = parseProjectJson(json, kBase);
    REQUIRE(r.ok());
    CHECK(r.value().sources.at(0) == abs);
}

TEST_CASE("parseProjectJson rejects invalid input with specific codes", "[engine][project]") {
    struct Case {
        const char* json;
        const char* code;
    };
    const Case cases[] = {
        {"{not json", "PRJ-PARSE"},
        {"[1,2]", "PRJ-SCHEMA"},
        {R"({"version":1,"sources":["a.v"]})", "PRJ-SCHEMA"},
        {R"({"format":"other","version":1,"sources":["a.v"]})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":2,"sources":["a.v"]})", "PRJ-VERSION"},
        {R"({"format":"opencdc-project","version":1})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":[]})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":"a.v"})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":[1]})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"language":"vhdl"})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"defines":{"A":[1]}})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"top":3})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"options":{"unknownClockRelation":"maybe"}})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"options":{"bogus":1}})", "PRJ-SCHEMA"},
        {R"({"format":"opencdc-project","version":1,"sources":["a.v"],"options":[]})", "PRJ-SCHEMA"},
    };
    for (const auto& c : cases) {
        INFO(c.json);
        auto r = parseProjectJson(c.json, kBase);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == c.code);
        CHECK(r.error().severity == Severity::Error);
    }
}

TEST_CASE("loadProjectFile resolves paths relative to the project file", "[engine][project]") {
    test::TempDir dir;
    auto file = dir.write("sub/p.ocdc.json",
                          R"({"format":"opencdc-project","version":1,"name":"t","sources":["../rtl/x.v"]})");
    auto r = loadProjectFile(file);
    REQUIRE(r.ok());
    CHECK(r.value().projectFile == fs::absolute(file).lexically_normal());
    CHECK(r.value().sources.at(0) == (fs::absolute(dir.path()) / "rtl/x.v").lexically_normal());
}

TEST_CASE("loadProjectFile reports a missing file", "[engine][project]") {
    test::TempDir dir;
    auto r = loadProjectFile(dir.path() / "missing.ocdc.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "PRJ-OPEN");
    CHECK_FALSE(r.error().location.file.empty());
}

TEST_CASE("loadProjectFile reports schema errors with the file location", "[engine][project]") {
    test::TempDir dir;
    auto file = dir.write("bad.ocdc.json", R"({"format":"opencdc-project","version":1})");
    auto r = loadProjectFile(file);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "PRJ-SCHEMA");
    CHECK(r.error().location.file.find("bad.ocdc.json") != std::string::npos);
}

TEST_CASE("The smoke example project loads", "[engine][project]") {
    auto r = loadProjectFile(test::examplesDir() / "smoke" / "smoke.ocdc.json");
    REQUIRE(r.ok());
    CHECK(r.value().name == "smoke");
    CHECK(r.value().top == "top");
    CHECK(r.value().sources.size() == 4);
    for (const auto& s : r.value().sources)
        CHECK(fs::is_regular_file(s));
}

TEST_CASE("parseProjectJson selects SystemVerilog for .sv sources unless a language is given", "[engine][project]") {
    auto sv = parseProjectJson(R"({"format": "opencdc-project", "version": 1, "sources": ["a.v", "b.SV"]})", kBase);
    REQUIRE(sv.ok());
    CHECK(sv.value().language == frontend::LanguageStd::SystemVerilog2017);
    auto v = parseProjectJson(R"({"format": "opencdc-project", "version": 1, "sources": ["a.v", "x.vhd"]})", kBase);
    REQUIRE(v.ok());
    CHECK(v.value().language == frontend::LanguageStd::Verilog2001);
    auto given = parseProjectJson(
        R"({"format": "opencdc-project", "version": 1, "language": "verilog-2005", "sources": ["b.sv"]})", kBase);
    REQUIRE(given.ok());
    CHECK(given.value().language == frontend::LanguageStd::Verilog2005);
}

TEST_CASE("projectToJson round-trips through parseProjectJson", "[engine][project]") {
    auto r = parseProjectJson(R"({
        "format": "opencdc-project", "version": 1, "name": "demo", "top": "top",
        "sources": ["rtl/a.v", {"file": "rtl/b.vhd", "library": "olo"}, "/abs/c.sv"],
        "includeDirs": ["inc"],
        "defines": {"WIDTH": "8", "FLAG": null},
        "parameters": {"DEPTH": 16, "MODE": "\"FAST\""},
        "constraints": ["c/clk.sdc"],
        "waivers": ["../w.json"],
        "options": {"unknownClockRelation": "assume_async", "minSyncStages": 3}
    })", kBase);
    REQUIRE(r.ok());
    const auto& a = r.value();
    const std::string text = projectToJson(a, kBase);
    CHECK(text.find("\"rtl/a.v\"") != std::string::npos);  // relative, '/' separators
    CHECK(text.find("\"../w.json\"") != std::string::npos);
    CHECK(text.find("\"library\": \"olo\"") != std::string::npos);
    auto r2 = parseProjectJson(text, kBase);
    REQUIRE(r2.ok());
    const auto& b = r2.value();
    CHECK(b.name == a.name);
    CHECK(b.top == a.top);
    CHECK(b.language == a.language);
    CHECK(b.sources == a.sources);
    CHECK(b.sourceLibraries == a.sourceLibraries);
    CHECK(b.includeDirs == a.includeDirs);
    CHECK(b.constraints == a.constraints);
    CHECK(b.waivers == a.waivers);
    CHECK(b.parameters == a.parameters);
    REQUIRE(b.defines.size() == a.defines.size());
    for (std::size_t i = 0; i < a.defines.size(); ++i) {
        CHECK(b.defines[i].name == a.defines[i].name);
        CHECK(b.defines[i].value == a.defines[i].value);
    }
    CHECK(b.options.unknownClockRelation == UnknownClockPolicy::AssumeAsync);
    CHECK(b.options.minSyncStages == 3);
    CHECK(b.options.reconvergenceDepth == 2);
}

TEST_CASE("saveProjectFile writes a loadable file; unwritable location is PRJ-WRITE", "[engine][project]") {
    test::TempDir dir;
    ProjectConfig cfg;
    cfg.name = "saved";
    cfg.sources = {dir.write("rtl/top.v", "module top; endmodule\n")};
    cfg.sourceLibraries = {""};
    const fs::path file = dir.path() / "p.ocdc.json";
    CHECK_FALSE(saveProjectFile(cfg, file).has_value());
    auto loaded = loadProjectFile(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().name == "saved");
    CHECK(loaded.value().sources == cfg.sources);
    CHECK_FALSE(fs::exists(dir.path() / "p.ocdc.json.tmp"));

    auto err = saveProjectFile(cfg, dir.path() / "no_such_dir" / "p.ocdc.json");
    REQUIRE(err.has_value());
    CHECK(err->code == "PRJ-WRITE");
}

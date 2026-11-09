#include "engine/FileList.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;
using namespace opencdc::engine;
namespace fs = std::filesystem;

namespace {

bool hasSource(const std::vector<fs::path>& sources, const fs::path& p) {
    return std::ranges::find(sources, p.lexically_normal()) != sources.end();
}

const Diagnostic* findNote(const ProjectConfig& cfg, std::string_view code) {
    for (const auto& d : cfg.importNotes)
        if (d.code == code)
            return &d;
    return nullptr;
}

} // namespace

TEST_CASE("isFileList recognizes .f lists only", "[engine][filelist]") {
    CHECK(isFileList("rtl/lib.f"));
    CHECK(isFileList("rtl/LIB.F"));
    CHECK_FALSE(isFileList("rtl/lib.v"));
    CHECK_FALSE(isFileList("rtl/lib.fl"));
    CHECK_FALSE(isFileList("rtl/f"));
}

TEST_CASE("readFileList reads sources, nested lists, include dirs and defines", "[engine][filelist]") {
    test::TempDir dir;
    dir.write("lib/sync/sync.f", "sync_reset.sv\nsync_signal.sv\n");
    dir.write("lib/mac/mac.f", "// MAC with its libraries\n"
                               "mac.sv  mac_rx.sv   # two on one line\n"
                               "../sync/sync.f\n"
                               "-f ../sync/sync.f\n"
                               "+incdir+inc+../common\n"
                               "+define+WIDTH=8+FAST\n"
                               "-v cells.v\n"
                               "+libext+.v\n");
    std::set<fs::path> seen;
    const auto r = readFileList(dir.path() / "lib/mac/mac.f", seen);
    CHECK(r.error.empty());
    // relative to the directory of the list that names them; the nested list read once
    REQUIRE(r.sources.size() == 4);
    CHECK(r.sources[0] == (dir.path() / "lib/mac/mac.sv").lexically_normal());
    CHECK(r.sources[1] == (dir.path() / "lib/mac/mac_rx.sv").lexically_normal());
    CHECK(r.sources[2] == (dir.path() / "lib/sync/sync_reset.sv").lexically_normal());
    CHECK(r.sources[3] == (dir.path() / "lib/sync/sync_signal.sv").lexically_normal());
    REQUIRE(r.includeDirs.size() == 2);
    CHECK(r.includeDirs[0] == (dir.path() / "lib/mac/inc").lexically_normal());
    CHECK(r.includeDirs[1] == (dir.path() / "lib/common").lexically_normal());
    REQUIRE(r.defines.size() == 2);
    CHECK(r.defines[0].name == "WIDTH");
    CHECK(r.defines[0].value == std::optional<std::string>("8"));
    CHECK(r.defines[1].name == "FAST");
    CHECK_FALSE(r.defines[1].value.has_value());
    // options that are not read are reported, not taken as files
    REQUIRE(r.ignored.size() == 2);
    CHECK(r.ignored[0] == "mac.f:7: -v cells.v");
    CHECK(r.ignored[1] == "mac.f:8: +libext+.v");
}

TEST_CASE("readFileList skips files already seen and survives a cycle", "[engine][filelist]") {
    test::TempDir dir;
    dir.write("a.f", "x.sv\nb.f\n");
    dir.write("b.f", "y.sv\na.f\nx.sv\n");
    std::set<fs::path> seen;
    const auto r = readFileList(dir.path() / "a.f", seen);
    CHECK(r.error.empty());
    REQUIRE(r.sources.size() == 2);
    CHECK(r.sources[0].filename() == "x.sv");
    CHECK(r.sources[1].filename() == "y.sv");
    // a second list naming the same files adds nothing
    const auto again = readFileList(dir.path() / "b.f", seen);
    CHECK(again.sources.empty());
}

TEST_CASE("readFileList reports a list that cannot be read", "[engine][filelist]") {
    test::TempDir dir;
    std::set<fs::path> seen;
    CHECK_FALSE(readFileList(dir.path() / "missing.f", seen).error.empty());
    // a nested list that is missing is an error too, not an empty list
    dir.write("top.f", "a.sv\n-f gone.f\n");
    const auto r = readFileList(dir.path() / "top.f", seen);
    CHECK(r.error.find("gone.f") != std::string::npos);
}

TEST_CASE("A project source ending in .f is expanded", "[engine][filelist][project]") {
    test::TempDir dir;
    dir.write("lib/lib.f", "common.sv\nfifo.sv\n+incdir+include\n+define+SYNTH\n-y cells\n");
    const auto file = dir.write("p.ocdc.json", R"({"format":"opencdc-project","version":1,"top":"top",
        "sources":["rtl/top.sv","lib/lib.f","lib/common.sv"]})");
    auto r = loadProjectFile(file);
    REQUIRE(r.ok());
    const auto& cfg = r.value();
    // a file named directly and by a list is read once, where it is first named
    REQUIRE(cfg.sources.size() == 3);
    CHECK(cfg.sources[0] == (dir.path() / "rtl/top.sv").lexically_normal());
    CHECK(cfg.sources[1] == (dir.path() / "lib/common.sv").lexically_normal());
    CHECK(cfg.sources[2] == (dir.path() / "lib/fifo.sv").lexically_normal());
    CHECK(cfg.sourceLibraries.size() == cfg.sources.size());
    CHECK(std::ranges::find(cfg.includeDirs, (dir.path() / "lib/include").lexically_normal()) != cfg.includeDirs.end());
    CHECK(std::ranges::find(cfg.defines, "SYNTH", &frontend::MacroDefine::name) != cfg.defines.end());
    const auto* note = findNote(cfg, "PRJ-FILELIST");
    REQUIRE(note != nullptr);
    CHECK(note->severity == Severity::Info);
    CHECK(note->message.find("2 source(s) read from 1 file list(s)") != std::string::npos);
    CHECK(note->message.find("1 option(s) not read") != std::string::npos);
    REQUIRE(note->notes.size() == 1);
    CHECK(note->notes[0] == "lib.f:5: -y cells");
}

TEST_CASE("A project without file lists is unchanged; a missing list is PRJ-FILELIST", "[engine][filelist][project]") {
    test::TempDir dir;
    // without a list, a file named twice stays (and no note is added)
    const auto plain = dir.write("plain.ocdc.json", R"({"format":"opencdc-project","version":1,
        "sources":["a.sv","a.sv"]})");
    auto p = loadProjectFile(plain);
    REQUIRE(p.ok());
    CHECK(p.value().sources.size() == 2);
    CHECK(findNote(p.value(), "PRJ-FILELIST") == nullptr);
    CHECK(hasSource(p.value().sources, dir.path() / "a.sv"));

    const auto bad = dir.write("bad.ocdc.json", R"({"format":"opencdc-project","version":1,
        "sources":["a.sv","lists/none.f"]})");
    auto b = loadProjectFile(bad);
    REQUIRE_FALSE(b.ok());
    CHECK(b.error().code == "PRJ-FILELIST");
    CHECK(b.error().message.find("none.f") != std::string::npos);
}

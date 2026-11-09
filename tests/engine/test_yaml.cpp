// The YAML subset of FuseSoC core files (stage 82).

#include "engine/Yaml.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc::engine;

namespace {

std::shared_ptr<const YamlNode> parse(std::string_view text) {
    std::string err;
    auto r = parseYaml(text, err);
    INFO(err);
    REQUIRE(r);
    return *r;
}

} // namespace

TEST_CASE("YAML: mappings, sequences, scalars and comments", "[engine][yaml]") {
    const auto d = parse("CAPI=2:\n"
                         "# a comment\n"
                         "name: \"lowrisc:ibex:top:0.1\" # trailing comment\n"
                         "description: 'it''s #1'\n"
                         "filesets:\n"
                         "  rtl:\n"
                         "    files:\n"
                         "    - a.sv\n"
                         "    - b.svh: {is_include_file: true, file_type: systemVerilogSource}\n"
                         "    - c.vhd:\n"
                         "        logical_name: lib\n"
                         "    depend: [ \"lowrisc:prim:all\", 'x:y:z' ]\n"
                         "empty:\n"
                         "flow: {a: [1, 2],\n"
                         "       b: c}\n");
    REQUIRE(d->isMap());
    CHECK(d->str("name") == "lowrisc:ibex:top:0.1");
    CHECK(d->str("description") == "it's #1");
    const auto* fs = d->get("filesets")->get("rtl");
    REQUIRE(fs);
    const auto* files = fs->get("files");
    REQUIRE(files->isSeq()); // a sequence at the indentation of its key
    REQUIRE(files->items.size() == 3);
    CHECK(files->items[0]->scalar == "a.sv");
    REQUIRE(files->items[1]->isMap());
    CHECK(files->items[1]->entries.front().first == "b.svh");
    CHECK(files->items[1]->entries.front().second->str("is_include_file") == "true");
    CHECK(files->items[2]->entries.front().second->str("logical_name") == "lib");
    const auto* dep = fs->get("depend");
    REQUIRE(dep->items.size() == 2);
    CHECK(dep->items[1]->scalar == "x:y:z");
    CHECK(d->get("empty")->kind == YamlNode::Kind::Null);
    CHECK(d->get("flow")->get("a")->items.size() == 2); // a flow mapping over two lines
    CHECK(d->get("flow")->str("b") == "c");
    CHECK(d->get("missing") == nullptr);
    CHECK(d->str("missing", "def") == "def");
}

TEST_CASE("YAML: anchors, aliases and merge keys", "[engine][yaml]") {
    const auto d = parse("base: &base\n"
                         "  default_tool: vivado\n"
                         "  toplevel: top\n"
                         "list: &l [a, b]\n"
                         "synth:\n"
                         "  <<: *base\n"
                         "  toplevel: top_artya7\n"
                         "copy: *l\n");
    const auto* s = d->get("synth");
    CHECK(s->str("default_tool") == "vivado"); // merged
    CHECK(s->str("toplevel") == "top_artya7"); // the mapping's own key wins
    CHECK(d->get("copy")->items.size() == 2);
}

TEST_CASE("YAML: implicit pairs in flow sequences, spaces before colons", "[engine][yaml]") {
    // SERV's servant.core
    const auto d = parse("name : award-winning:serv:servant:1.4.0\n"
                         "tinyfpga_bx: {files: [data/tinyfpga_bx.pcf : {file_type : PCF}, x.v]}\n"
                         "generate: [ice40pll: {freq_in : 12, freq_out : 32}]\n");
    CHECK(d->str("name") == "award-winning:serv:servant:1.4.0");
    const auto* files = d->get("tinyfpga_bx")->get("files");
    REQUIRE(files->items.size() == 2);
    REQUIRE(files->items[0]->isMap());
    CHECK(files->items[0]->entries.front().first == "data/tinyfpga_bx.pcf");
    CHECK(files->items[0]->entries.front().second->str("file_type") == "PCF");
    CHECK(files->items[1]->scalar == "x.v");
    const auto* g = d->get("generate");
    REQUIRE(g->items.size() == 1);
    CHECK(g->items[0]->entries.front().second->str("freq_out") == "32");
}

TEST_CASE("YAML negative: unsupported or malformed documents", "[engine][yaml]") {
    std::string err;
    CHECK_FALSE(parseYaml("a: *nope\n", err)); // unknown alias
    CHECK(err.find("line 1") != std::string::npos);
    err.clear();
    CHECK_FALSE(parseYaml("a: [1, 2\n", err)); // unclosed flow sequence
    err.clear();
    CHECK_FALSE(parseYaml("a: \"open\n", err)); // unclosed quote
    err.clear();
    CHECK_FALSE(parseYaml("a: |\n  text\n", err)); // block scalars are not supported
    err.clear();
    CHECK_FALSE(parseYaml("a: 1\n   b: 2\n", err)); // bad indentation
}

#include "constraints/Sdc.h"

#include "../DesignUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;
using namespace opencdc::constraints;

namespace {

/// Resolver with a fixed list of names (no design): records queries.
class FakeResolver final : public IObjectResolver {
public:
    std::vector<std::string> portNames{"clk", "clk_b", "rst_n", "data[0]", "data[1]"};
    std::vector<std::string> queries;

    std::vector<Object> ports(std::string_view p, bool nocase) override {
        queries.push_back("ports:" + std::string(p));
        std::vector<Object> out;
        for (const auto& n : portNames)
            if (globMatch(p, n, nocase))
                out.push_back({ObjectKind::Port, n, {graph::BitRef{ir::InstanceId{0}, ir::NetId{0}, 0}}});
        return out;
    }
    std::vector<Object> pins(std::string_view p, bool, bool) override {
        queries.push_back("pins:" + std::string(p));
        return {};
    }
    std::vector<Object> nets(std::string_view p, bool hier, bool) override {
        queries.push_back(std::string(hier ? "hnets:" : "nets:") + std::string(p));
        return {};
    }
};

struct Parsed {
    ConstraintSet set;
    std::vector<Diagnostic> diags;
    [[nodiscard]] std::size_t countCode(std::string_view c) const {
        return static_cast<std::size_t>(std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == c; }));
    }
};

Parsed parse(std::string_view text, IObjectResolver& r) {
    Parsed p;
    CollectingDiagnosticSink sink;
    parseSdc(text, "t.sdc", r, p.set, sink);
    p.diags = sink.take();
    for (const auto& d : p.diags)
        UNSCOPED_INFO(format(d));
    return p;
}

} // namespace

TEST_CASE("Glob matching", "[constraints]") {
    CHECK(globMatch("clk*", "clk_a"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("c?k", "clk"));
    CHECK_FALSE(globMatch("clk", "clk_a"));
    CHECK(globMatch("u_*/q_reg", "u_rx/q_reg"));
    CHECK(globMatch("data\\[0\\]", "data[0]"));
    CHECK(globMatch("CLK", "clk", true));
    CHECK_FALSE(globMatch("CLK", "clk", false));
    CHECK(globMatch("*_reg*", "a_reg[3]"));
}

TEST_CASE("Tcl syntax: braces, quotes, continuation, comments, variables, nesting", "[constraints]") {
    FakeResolver r;
    auto p = parse("# comment line\n"
                   "set P 4.0 ; set name \"clk_main\"\n"
                   "create_clock -period $P -name ${name} \\\n"
                   "    -waveform {0 2.0} [get_ports {clk}]\n"
                   "create_clock -name vclk -period 10   ;# virtual clock\n"
                   "create_clock -period 8 [get_ports [list clk_b]]\n",
                   r);
    CHECK(p.countCode("CON-PARSE") == 0);
    REQUIRE(p.set.clocks.size() == 3);
    const auto& c = p.set.clocks[0];
    CHECK(c.name == "clk_main");
    CHECK(c.periodNs == 4.0);
    CHECK(c.waveform == std::vector<double>{0.0, 2.0});
    CHECK(c.sources.size() == 1);
    CHECK(c.loc.line == 3);
    CHECK(p.set.clocks[1].name == "vclk");
    CHECK(p.set.clocks[1].sources.empty());
    CHECK(p.set.clocks[2].name == "clk_b"); // default name = object name
}

TEST_CASE("CRLF line endings (files written on Windows) parse like LF", "[constraints]") {
    const std::string lf = "# comment \\\n"
                           "  continued comment\n"
                           "create_clock -period 4 -name a [get_ports clk]\n"
                           "create_clock -period 5 -name b [get_ports clk_b]\n"
                           "set_clock_groups -asynchronous \\\n"
                           "    -group [get_clocks a] \\\n"
                           "    -group [get_clocks b]\n"
                           "create_clock -period 6 -name c [get_ports rst_n]\n";
    std::string crlf;
    for (const char ch : lf)
        crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
    for (const std::string* text : {&lf, static_cast<const std::string*>(&crlf)}) {
        FakeResolver r;
        auto p = parse(*text, r);
        CHECK(p.countCode("CON-PARSE") == 0);
        CHECK(p.diags.empty());
        REQUIRE(p.set.clocks.size() == 3);
        REQUIRE(p.set.groups.size() == 1);
        CHECK(p.set.groups[0].groups == std::vector<std::vector<std::string>>{{"a"}, {"b"}});
        CHECK(p.set.clocks[2].name == "c");
        CHECK(p.set.clocks[2].loc.line == 8); // line numbers are unchanged
    }
}

TEST_CASE("Bus bit selects are not command substitutions", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_clock -period 5 [get_ports data[0]]\n"
                   "create_clock -period 5 -name b1 [get_ports {data[1]}]\n",
                   r);
    REQUIRE(p.set.clocks.size() == 2);
    CHECK(p.set.clocks[0].name == "data[0]");
    CHECK(p.set.clocks[1].sources.size() == 1);
}

TEST_CASE("Clock groups with get_clocks and plain lists", "[constraints]") {
    FakeResolver r;
    auto p = parse("set_clock_groups -asynchronous -group [get_clocks {a b}] -group {c}\n"
                   "set_clock_groups -name ex -physically_exclusive -group a -group [get_clocks d*]\n",
                   r);
    REQUIRE(p.set.groups.size() == 2);
    CHECK(p.set.groups[0].kind == GroupKind::Asynchronous);
    CHECK(p.set.groups[0].groups == std::vector<std::vector<std::string>>{{"a", "b"}, {"c"}});
    CHECK(p.set.groups[1].kind == GroupKind::PhysicallyExclusive);
    CHECK(p.set.groups[1].name == "ex");
    CHECK(p.set.groups[1].groups[1] == std::vector<std::string>{"d*"});
}

TEST_CASE("Generated clocks", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_generated_clock -name div2 -source [get_ports clk] -divide_by 2 [get_ports clk_b]\n"
                   "create_generated_clock -name g2 -master_clock [get_clocks clk] -multiply_by 3 -invert [get_ports "
                   "rst_n]\n",
                   r);
    REQUIRE(p.set.clocks.size() == 2);
    CHECK(p.set.clocks[0].generated);
    CHECK(p.set.clocks[0].divideBy == 2);
    CHECK(p.set.clocks[0].masterSource.size() == 1);
    CHECK(p.set.clocks[1].masterClock == "clk");
    CHECK(p.set.clocks[1].multiplyBy == 3);
    CHECK(p.set.clocks[1].invert);
}

TEST_CASE("Errors, unsupported and ignored commands are reported", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_clock -period 10 [get_ports nosuch]\n"         // unmatched
                   "create_clock -period 10 -bogus 3 -name x\n"           // unknown option
                   "create_clock -period abc -name y\n"                   // bad period
                   "set_clock_groups -group a -group b\n"                 // missing kind
                   "set_false_path -from [get_ports clk]\n"               // ignored (args not evaluated)
                   "set_output_delay 1 -clock x [get_ports rst_n]\n"
                   "set_output_delay 2 -clock x [get_ports rst_n]\n"
                   "foreach p {a b} { puts $p }\n"                        // control flow
                   "frobnicate 1 2\n"                                     // unknown
                   "get_ports -filter {NAME =~ clk*} clk\n"              // unsupported option
                   "create_clock -period 1 {clk\n",                       // unterminated
                   r);
    CHECK(p.countCode("CON-UNMATCHED") == 1);
    CHECK(p.countCode("CON-UNKNOWN-OPTION") == 1);
    CHECK(p.countCode("CON-PARSE") >= 3);
    CHECK(p.countCode("CON-UNSUPPORTED-COMMAND") == 1);
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 1);
    CHECK(p.countCode("CON-UNSUPPORTED-OPTION") == 1);
    CHECK(p.countCode("CON-IGNORED") == 1);
    CHECK(p.set.ignoredCommands.at("set_output_delay") == 2);
    CHECK(p.set.ignoredCommands.at("set_false_path") == 1);
    // Arguments of ignored commands are not evaluated (no get_ports query for them).
    CHECK(std::ranges::count(r.queries, std::string("ports:clk")) == 1);
}

TEST_CASE("set_input_delay -clock assigns ports to a clock", "[constraints]") {
    FakeResolver r;
    auto p = parse("set_input_delay -clock [get_clocks clk] -max 2.5 [get_ports {data[0] data[1]}]\n"
                   "set_input_delay 1.0 -clock_fall -clock clk_b [get_ports rst_n]\n"
                   "set_input_delay 1.0 [get_ports clk]\n", // no clock: no domain
                   r);
    REQUIRE(p.set.inputDelays.size() == 2);
    CHECK(p.set.inputDelays[0].clock == "clk");
    CHECK(p.set.inputDelays[0].bits.size() == 2);
    CHECK(p.set.inputDelays[1].clock == "clk_b");
    CHECK(p.set.inputDelays[1].clockFall);
    CHECK(p.set.ignoredCommands.at("set_input_delay (without -clock)") == 1);
}

TEST_CASE("Redefinition replaces a clock unless -add", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_clock -name c -period 10 [get_ports clk]\n"
                   "create_clock -name c -period 5 [get_ports clk]\n"
                   "create_clock -name c2 -period 7 -add [get_ports clk]\n",
                   r);
    CHECK(p.countCode("CON-DUPLICATE-CLOCK") == 1);
    REQUIRE(p.set.clocks.size() == 2);
    CHECK(p.set.clocks[0].periodNs == 5.0);
}

TEST_CASE("Design resolver: ports, bits, hierarchical nets, pins and register pins", "[constraints]") {
    auto b = test::buildDesign("module leaf(input ck, input [3:0] d, output reg [3:0] q);\n"
                               "  always @(posedge ck) q <= d;\n"
                               "endmodule\n"
                               "module top(input clk, input [3:0] din, output [3:0] y);\n"
                               "  wire gclk = clk;\n"
                               "  genvar i;\n"
                               "  generate for (i = 0; i < 1; i = i + 1) begin : g\n"
                               "    leaf u_leaf (.ck(gclk), .d(din), .q(y));\n"
                               "  end endgenerate\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);

    auto ports = r.ports("clk", false);
    REQUIRE(ports.size() == 1);
    CHECK(view.name(ports[0].bits.at(0)) == "top.clk");
    CHECK(r.ports("din[2]", false).at(0).bits.size() == 1);
    CHECK(r.ports("din[*]", false).at(0).bits.size() == 4);
    CHECK(r.ports("din", false).at(0).bits.size() == 4);
    CHECK(r.ports("c*", false).size() == 1);

    CHECK(r.nets("gclk", false, false).size() == 1);
    CHECK(r.nets("q", false, false).empty());
    CHECK(r.nets("q", true, false).size() == 1); // -hierarchical matches below any level
    auto qnet = r.nets("g[0].u_leaf/q", false, false);
    REQUIRE(qnet.size() == 1);
    CHECK(view.name(qnet[0].bits.at(3)) == "top.g[0].u_leaf.q[3]");

    auto pin = r.pins("g[0].u_leaf/ck", false, false);
    REQUIRE(pin.size() == 1);
    CHECK(view.name(pin[0].bits.at(0)) == "top.g[0].u_leaf.ck");

    auto regq = r.pins("g[0].u_leaf/q_reg[1]/Q", false, false);
    REQUIRE(regq.size() == 1);
    CHECK(view.name(regq[0].bits.at(0)) == "top.g[0].u_leaf.q[1]");
    auto regc = r.pins("*/q_reg[0]/C", true, false);
    REQUIRE(regc.size() == 1);
    CHECK(view.name(regc[0].bits.at(0)) == "top.g[0].u_leaf.ck");
}

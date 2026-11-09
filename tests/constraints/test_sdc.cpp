#include "constraints/Lpf.h"
#include "constraints/Sdc.h"
#include "core/PathUtil.h"

#include "../DesignUtil.h"
#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

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
        for (std::size_t i = 0; i < portNames.size(); ++i) // each port its own bit
            if (globMatch(p, portNames[i], nocase))
                out.push_back({ObjectKind::Port, portNames[i],
                               {graph::BitRef{ir::InstanceId{0}, ir::NetId{static_cast<std::uint32_t>(i)}, 0}}});
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

TEST_CASE("Tcl: a backslash-newline inside braces or quotes is one space", "[constraints]") {
    // SNES_MiSTer SNES.sdc lists register names inside braces over several lines,
    // each ending in a backslash
    FakeResolver r;
    auto p = parse("set_false_path -from [get_ports { clk \\\n"
                   "\t\t\t\t   clk_b }] \\\n"
                   "  -to [get_ports \"data\\[0\\] \\\n    data\\[1\\]\"]\n"
                   // negative: an escaped character that is not a newline stays part of the name
                   "set_false_path -from [get_ports {data\\[0\\]}]\n"
                   "create_clock -period 4 -name late [get_ports rst_n]\n",
                   r);
    CHECK(p.countCode("CON-PARSE") == 0);
    CHECK(p.countCode("CON-UNMATCHED") == 0);
    CHECK(std::ranges::find(r.queries, "ports:\\") == r.queries.end());
    CHECK(std::ranges::find(r.queries, "ports:clk") != r.queries.end());
    CHECK(std::ranges::find(r.queries, "ports:clk_b") != r.queries.end());
    CHECK(std::ranges::find(r.queries, "ports:data[1]") != r.queries.end()); // quotes substitute \\[
    CHECK(std::ranges::find(r.queries, "ports:data\\[0\\]") != r.queries.end()); // braces keep it
    REQUIRE(p.set.pathExceptions.size() == 2);
    CHECK(p.set.pathExceptions[0].from.size() == 2);
    CHECK(p.set.pathExceptions[0].to.size() == 2);
    CHECK(p.set.pathExceptions[1].from.size() == 1);
    // line numbers still count the continued lines
    REQUIRE(p.set.clocks.size() == 1);
    CHECK(p.set.clocks[0].loc.line == 6);
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
                   "set_false_path -through [get_ports clk]\n"            // ignored (args not evaluated)
                   "set_multicycle_path 2 -to [get_ports rst_n]\n"
                   "set_multicycle_path 3 -to [get_ports rst_n]\n"
                   "namespace eval ns { puts x }\n"                       // unsupported Tcl
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
    CHECK(p.set.ignoredCommands.at("set_multicycle_path") == 2);
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

TEST_CASE("set_output_delay -clock names the clock outputs are sampled with", "[constraints]") {
    FakeResolver r;
    auto p = parse("set_output_delay -clock [get_clocks clk] -max 1 [get_ports {data[0] data[1]}]\n"
                   "set_output_delay -add_delay -clock clk_b -clock_fall -min -1 [get_ports rst_n]\n"
                   "set_output_delay 0 [get_ports clk]\n", // no clock: nothing to check
                   r);
    REQUIRE(p.set.outputDelays.size() == 2);
    CHECK(p.set.outputDelays[0].clock == "clk");
    CHECK(p.set.outputDelays[0].bits.size() == 2);
    CHECK(p.set.outputDelays[1].clock == "clk_b");
    CHECK(p.set.outputDelays[1].clockFall);
    CHECK(p.set.inputDelays.empty());
    CHECK(p.set.ignoredCommands.at("set_output_delay (without -clock)") == 1);
}

TEST_CASE("Vivado clock overrides: the same name (also with -add), the same source without -add", "[constraints]") {
    FakeResolver r;
    // the Digilent master XDC: two clocks named sys_clk_pin, and the 100 MHz one twice
    auto p = parse("create_clock -add -name sys_clk_pin -period 83.333 [get_ports clk]\n"
                   "create_clock -add -name sys_clk_pin -period 10.000 [get_ports clk_b]\n"
                   "create_clock -period 10.000 -name sys_clk_pin -add [get_ports clk_b]\n",
                   r);
    CHECK(p.countCode("CON-DUPLICATE-CLOCK") == 2);
    REQUIRE(p.set.clocks.size() == 1);
    CHECK(p.set.clocks[0].periodNs == 10.0);
    CHECK(p.set.clocks[0].loc.line == 3);
    // a second clock on the same port without -add replaces the first
    auto q = parse("create_clock -name a -period 10 [get_ports clk]\n"
                   "create_clock -name b -period 5 [get_ports clk]\n",
                   r);
    REQUIRE(q.set.clocks.size() == 1);
    CHECK(q.set.clocks[0].name == "b");
    // negative: with -add (another name) both stay; clocks on other ports stay
    auto k = parse("create_clock -name a -period 10 [get_ports clk]\n"
                   "create_clock -name b -period 5 -add [get_ports clk]\n"
                   "create_clock -name c -period 4 [get_ports clk_b]\n",
                   r);
    CHECK(k.countCode("CON-DUPLICATE-CLOCK") == 0);
    CHECK(k.set.clocks.size() == 3);
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

TEST_CASE("Design resolver: ports of unpacked arrays are named per element, as in Vivado", "[constraints][array-port]") {
    // taxi: `output wire sfp_led[2]` constrained with get_ports {sfp_led[*]},
    // `output wire [1:0] sfp_rs[4]` with get_ports {sfp_rs[*][*]}
    auto b = test::buildDesign("module leaf(input wire ck, input wire [1:0] v[0:2]);\n"
                               "endmodule\n"
                               "module top(input wire clk, input wire d[2], output wire led[2],\n"
                               "           output wire [1:0] rs[4], input wire [3:0] bus);\n"
                               "  assign led = d;\n"
                               "  assign rs[0] = 2'b01; assign rs[1] = 2'b10; assign rs[2] = bus[1:0]; assign rs[3] = bus[3:2];\n"
                               "  leaf u(.ck(clk), .v(rs[0:2]));\n"
                               "endmodule\n",
                               "top", frontend::LanguageStd::SystemVerilog2017);
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    // one-bit elements: led[0], led[1]
    const auto leds = r.ports("led[*]", false);
    REQUIRE(leds.size() == 2);
    for (const auto& o : leds)
        CHECK(o.bits.size() == 1);
    const auto led0 = r.ports("led[0]", false);
    REQUIRE(led0.size() == 1);
    REQUIRE(led0[0].bits.size() == 1);
    // [2] is [0:1]: element 0 is the left one, at the higher offset
    CHECK(led0[0].bits[0].bit == 1);
    CHECK(r.ports("led[1]", false).at(0).bits.at(0).bit == 0);
    // vector elements: rs[3] is two bits, rs[*][*] every bit (one object per element)
    CHECK(r.ports("rs[3]", false).at(0).bits.size() == 2);
    const auto rsBit = r.ports("rs[2][1]", false);
    REQUIRE(rsBit.size() == 1);
    REQUIRE(rsBit[0].bits.size() == 1);
    CHECK(rsBit[0].bits[0].bit == 1 * 2 + 1); // [4] = [0:3]: element 2 at offset 1
    std::size_t bits = 0;
    for (const auto& o : r.ports("rs[*][*]", false))
        bits += o.bits.size();
    CHECK(bits == 8);
    // instance pins of an array port: u/v[2]
    CHECK(r.pins("u/v[2]", false, false).size() == 1);
    // negative: an index outside the array, and a vector port keeps its bit selects only
    CHECK(r.ports("led[2]", false).empty());
    CHECK(r.ports("rs[4]", false).empty());
    CHECK(r.ports("bus[3]", false).at(0).bits.size() == 1);
    CHECK(r.ports("bus[3][0]", false).empty());
}

TEST_CASE("Options can be abbreviated to a unique prefix", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_clock -name c -per 10 [get_ports clk]\n"
                   "set_clock_groups -async -group c -group d\n",
                   r);
    REQUIRE(p.set.clocks.size() == 1);
    CHECK(p.set.clocks[0].periodNs == 10.0);
    REQUIRE(p.set.groups.size() == 1);
    CHECK(p.set.groups[0].kind == GroupKind::Asynchronous);
    CHECK(p.countCode("CON-UNKNOWN-OPTION") == 0);
    // negative: an ambiguous prefix is still unknown (-p: -period or nothing else? -physically_exclusive)
    auto q = parse("set_clock_groups -a -group c -group d\n", r); // -asynchronous or -allow_paths
    CHECK(q.countCode("CON-UNKNOWN-OPTION") == 1);
}

TEST_CASE("Tcl subset: expr, if/elseif/else, foreach, for, while, break, continue, list commands", "[constraints]") {
    FakeResolver r;
    auto p = parse("set n 0\n"
                   "foreach {name per} {clk 10 clk_b 4} {\n"
                   "    incr n\n"
                   "    create_clock -name $name -period [expr {$per * 2}] [get_ports $name]\n"
                   "}\n"
                   "set fast [expr {min(10.0, 4)}]\n"
                   "if {$fast < 5 && $n == 2} then { create_clock -name ok1 -period 1 } else { create_clock -name bad1 -period 1 }\n"
                   "if {$n > 5} { create_clock -name bad2 -period 1 } elseif {[llength {a b c}] == 3} {\n"
                   "    create_clock -name ok2 -period 1\n"
                   "}\n"
                   "set v [if {0} {expr 1} {expr 2}]\n"
                   "create_clock -name val$v -period 1\n"
                   "set acc {}\n"
                   "for {set i 0} {$i < 10} {incr i} {\n"
                   "    if {$i == 1} continue\n"
                   "    if {$i == 3} { break }\n"
                   "    lappend acc $i\n"
                   "}\n"
                   "create_clock -name acc[llength $acc] -period [lindex $acc end]\n"
                   "set k 0\n"
                   "while {$k < 3} { incr k 2 }\n"
                   "create_clock -name k$k -period 1\n"
                   "create_clock -name e[expr {7 / 2}]_[expr {7 / 2.0}]_[expr {\"ab\" eq \"ab\"}] -period 1\n"
                   "puts \"done $n\"\n",
                   r);
    CHECK(p.diags.empty());
    std::vector<std::string> names;
    for (const auto& c : p.set.clocks)
        names.push_back(c.name);
    CHECK(names == std::vector<std::string>{"clk", "clk_b", "ok1", "ok2", "val2", "acc2", "k4", "e3_3.5_1"});
    CHECK(p.set.clocks[1].periodNs == 8.0);
    CHECK(p.set.clocks[5].periodNs == 2.0); // acc = {0 2}: lindex end
    CHECK(p.set.clocks[1].loc.line == 4);   // commands in loop bodies keep their line
}

TEST_CASE("Tcl subset: errors in expressions and loops are reported and skip the command", "[constraints]") {
    FakeResolver r;
    auto p = parse("set x [expr {1 +}]\n"                                   // syntax error
                   "if {$nosuch} { create_clock -name bad -period 1 }\n"    // undefined variable
                   "if {\"abc\"} { create_clock -name bad2 -period 1 }\n"    // not a boolean
                   "expr {\"a\" + 1}\n"                                     // non-numeric operand
                   "while {1} { }\n"                                        // runaway loop
                   "foreach a {1 2} {3 4} { }\n",                           // several lists
                   r);
    CHECK(p.set.clocks.empty());
    CHECK(p.countCode("CON-PARSE") == 5);
    CHECK(p.countCode("CON-UNSUPPORTED-COMMAND") == 1);
}

TEST_CASE("Design resolver: cells, instances, filters, regexp, clocks of cells and periods", "[constraints]") {
    auto b = test::buildDesign("module sync2(input clk, input d, output q, output [1:0] c);\n"
                               "  (* ASYNC_REG = \"TRUE\" *) reg s1_reg;\n"
                               "  (* ASYNC_REG = \"TRUE\" *) reg s2_reg;\n"
                               "  reg [1:0] cnt_reg;\n"
                               "  always @(posedge clk) begin s1_reg <= d; s2_reg <= s1_reg; cnt_reg <= cnt_reg + 2'd1; end\n"
                               "  assign q = s2_reg;\n"
                               "  assign c = cnt_reg;\n"
                               "endmodule\n"
                               "module top(input clk_a, input clk_b, input d, output q0, output q1, output [3:0] c);\n"
                               "  reg a_reg;\n"
                               "  always @(posedge clk_a) a_reg <= d;\n"
                               "  sync2 u_s0 (.clk(clk_b), .d(a_reg), .q(q0), .c(c[1:0]));\n"
                               "  sync2 u_s1 (.clk(clk_b), .d(a_reg), .q(q1), .c(c[3:2]));\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);

    auto p = parse("create_clock -name ca -period 10 [get_ports clk_a]\n"
                   "create_generated_clock -name cd -source [get_ports clk_a] -divide_by 4 [get_ports clk_b]\n"
                   "set n 0\n"
                   "foreach inst [get_cells -hier -filter {REF_NAME == sync2 || ORIG_REF_NAME == sync2}] {\n"
                   "    puts $inst\n"
                   "    set ffs [get_cells -quiet -hier -regexp {.*/s[12]_reg_reg} -filter \"PARENT == $inst\"]\n"
                   "    set bits [get_cells -quiet -hier -regexp {.*/cnt_reg_reg\\[\\d+\\]} -filter \"PARENT == $inst\"]\n"
                   "    set async [get_cells -quiet -hier -filter \"PARENT == $inst && ASYNC_REG == TRUE\"]\n"
                   "    if {[llength $ffs] == 2 && [llength $bits] == 2 && [llength $async] == 2} { incr n }\n"
                   "    set clk [get_clocks -of_objects [get_cells \"$inst/s1_reg_reg $inst/s2_reg_reg\"]]\n"
                   "    if {[llength $clk] == 1} { incr n }\n"
                   "}\n"
                   "set p [get_property -min PERIOD [get_clocks {ca cd}]]\n"
                   "create_clock -name n${n}_p$p -period 1\n"
                   "set unknown [get_property PERIOD [get_clocks -of_objects [get_cells u_s0/s1_reg_reg]]]\n"
                   "create_clock -name u_$unknown -period 1\n"
                   "set top [get_cells -filter {IS_SEQUENTIAL}]\n"
                   "create_clock -name top[llength $top]_[get_property REF_NAME [get_cells u_s1]] -period 1\n",
                   r);
    CHECK(p.diags.empty());
    REQUIRE(p.set.clocks.size() == 5);
    CHECK(p.set.clocks[2].name == "n4_p10.0"); // two instances x two checks; min(10, 40)
    CHECK(p.set.clocks[3].name == "u_nan");    // clock of a cell: period known only after clock analysis
    CHECK(p.set.clocks[4].name == "top1_sync2");

    auto s1 = r.cells("u_s0/s1_reg_reg", false, false);
    REQUIRE(s1.size() == 1);
    REQUIRE(r.cellClocks(s1[0]).size() == 1);
    CHECK(view.name(r.cellClocks(s1[0])[0]) == "top.u_s0.clk");
    auto cells = r.cells("u_*", false, false);
    CHECK(cells.size() == 2); // instances only (registers are below them)
    CHECK(std::ranges::all_of(cells, [](const Object& o) { return o.instance && o.bits.empty(); }));
    auto regs = r.cellsRegexp(".*/cnt_reg_reg\\[1\\]", true, false);
    REQUIRE(regs.size() == 2);
    CHECK(regs[0].name == "u_s0/cnt_reg_reg[1]");
    CHECK(regs[0].bits.size() == 1);
    CHECK(r.cellProperty(regs[0], "PARENT") == "u_s0");
    CHECK_FALSE(r.cellProperty(regs[0], "REF_NAME").has_value());
    CHECK(r.cellProperty(cells[0], "REF_NAME") == "sync2");

    // negative: unknown filter properties and bad expressions return nothing, with a warning
    auto q = parse("set a [get_cells -hier -filter {ars_ff1 == TRUE}]\n"
                   "set b [get_cells -hier -regexp {(}]\n"
                   "set c [get_cells -hier -filter {REF_NAME ==}]\n"
                   "set d [get_cells -hier -filter {REF_NAME == nosuch}]\n"
                   "create_clock -name x[llength $a][llength $b][llength $d] -period 1\n",
                   r);
    CHECK(q.countCode("CON-UNSUPPORTED-OPTION") == 1);
    CHECK(q.countCode("CON-PARSE") == 1);
    CHECK(q.countCode("CON-UNMATCHED") == 2); // empty REF_NAME value matches nothing; nosuch matches nothing
    REQUIRE(q.set.clocks.size() == 1);
    CHECK(q.set.clocks[0].name == "x000");
}

TEST_CASE("Design resolver: netlist traversal of flip-flop pins, nets and cells; arrays of flip-flops", "[constraints]") {
    auto b = test::buildDesign("module top(input clk_a, input clk_b, input rst, input d, input [3:0] w, output q, output [3:0] y);\n"
                               "  reg s1, s2;\n"
                               "  reg [0:0] v_reg;\n"
                               "  reg [3:0] pipe [0:1];\n"
                               "  reg [3:0] mem [0:15];\n"
                               "  reg [3:0] rd;\n"
                               "  always @(posedge clk_b or posedge rst)\n"
                               "    if (rst) begin s1 <= 1'b1; s2 <= 1'b1; end else begin s1 <= 1'b0; s2 <= s1; end\n"
                               "  always @(posedge clk_a) begin v_reg <= d; pipe[0] <= w; pipe[1] <= pipe[0]; end\n"
                               "  always @(posedge clk_a) mem[w] <= pipe[1];\n"
                               "  always @(posedge clk_b) rd <= mem[w];\n"
                               "  assign q = s2 ^ v_reg[0];\n"
                               "  assign y = rd;\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);

    auto p = parse("set dest [get_cells s2_reg]\n"
                   "set pins [get_pins -of_objects $dest]\n"
                   "set dpin [get_pins -of_objects $dest -filter {REF_PIN_NAME == D}]\n"
                   "set net [get_nets -segments -of_objects $dpin]\n"
                   "set src_pins [get_pins -of_objects $net -filter {IS_LEAF && DIRECTION == OUT}]\n"
                   "set src [get_cells -of_objects $src_pins]\n"
                   "set pre [get_pins -of_objects [get_cells {s1_reg s2_reg}] -filter {IS_PRESET || IS_RESET}]\n"
                   "set loads [get_pins -of_objects [get_nets -of_objects [get_pins s1_reg/Q]] -filter {DIRECTION == IN}]\n"
                   "create_clock -name n[llength $pins]_[llength $src]_[llength $pre]_[llength $loads] -period 1\n"
                   "create_clock -name src_[lindex $src 0] -period 1\n"
                   "set v [get_cells v_reg_reg[0]]\n"
                   "set pp [get_cells pipe_reg[1][*]]\n"
                   "set mm [get_cells -quiet mem_reg*]\n"
                   "create_clock -name a[llength $v]_[llength $pp]_[llength $mm] -period 1\n",
                   r);
    CHECK(p.diags.empty());
    REQUIRE(p.set.clocks.size() == 3);
    CHECK(p.set.clocks[0].name == "n4_1_2_1"); // C D Q PRE; the source; two PRE pins; s2's D
    CHECK(p.set.clocks[1].name == "src_s1_reg");
    CHECK(p.set.clocks[2].name == "a1_1_0"); // [0:0] vector bit; pipeline element; a memory has no flip-flops

    auto pipe = r.cells("pipe_reg[1]", false, false);
    REQUIRE(pipe.size() == 1);
    CHECK(pipe[0].bits.size() == 4);
    auto dp = r.cellPins(pipe[0]);
    CHECK(std::ranges::count_if(dp, [](const Object& o) { return o.pin == "D"; }) == 1);

    // negative: properties of instance ports that are not recorded match nothing
    auto q = parse("set x [get_pins -of_objects [get_cells s2_reg] -filter {IS_FOO}]\n"
                   "set y [get_nets -of_objects [get_cells s2_reg]]\n"
                   "create_clock -name x[llength $x][llength $y] -period 1\n",
                   r);
    CHECK(q.countCode("CON-UNSUPPORTED-OPTION") == 2);
    REQUIRE(q.set.clocks.size() == 1);
    CHECK(q.set.clocks[0].name == "x00");
}

TEST_CASE("get_cells -regexp: the literal pre-check keeps every match", "[constraints]") {
    // corundum's scripts call get_cells -hier -regexp per instance: names without the
    // pattern's literal part are skipped before std::regex_match; the result is the same
    auto b = test::buildDesign("module m(input clk, input [3:0] d, output [3:0] q);\n"
                               "  reg rb_addr_reg, rb_di_reg, rb_we_reg, other_reg;\n"
                               "  reg [1:0] Sync_reg;\n"
                               "  always @(posedge clk) begin rb_addr_reg <= d[0]; rb_di_reg <= d[1]; rb_we_reg <= d[2];\n"
                               "    other_reg <= d[3]; Sync_reg <= d[1:0]; end\n"
                               "  assign q = {rb_addr_reg ^ rb_di_reg ^ rb_we_reg, other_reg, Sync_reg};\n"
                               "endmodule\n"
                               "module top(input clk, input [3:0] d, output [3:0] q0, output [3:0] q1);\n"
                               "  m u0 (.clk(clk), .d(d), .q(q0));\n"
                               "  m u1 (.clk(clk), .d(d), .q(q1));\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    // an alternative inside a group: the literal outside it is still required
    CHECK(r.cellsRegexp(".*/rb_(addr|di|we)_reg_reg", true, false).size() == 6);
    // an alternative at the top level: no literal is certain
    CHECK(r.cellsRegexp(".*/rb_di_reg_reg|u1/other_reg_reg", true, false).size() == 3);
    // an optional part (quantified group, quantified character)
    CHECK(r.cellsRegexp(".*/rb_we(_x)?_reg_reg", true, false).size() == 2);
    CHECK(r.cellsRegexp(".*/othe?r_reg_reg", true, false).size() == 2);
    // bits: the literal stops at the escaped bracket
    CHECK(r.cellsRegexp(".*/Sync_reg_reg\\[\\d+\\]", true, false).size() == 4);
    // -nocase: the literal is compared without case too
    CHECK(r.cellsRegexp(".*/sync_reg_reg\\[1\\]", true, true).size() == 2);
    // negative: nothing that lacks the literal
    CHECK(r.cellsRegexp(".*/rb_(addr|di)_xyz_reg", true, false).empty());
    CHECK(r.cellsRegexp(".*/sync_reg_reg\\[1\\]", true, false).empty()); // case matters without -nocase
}

TEST_CASE("Design resolver: an array written through a multiplexer is a memory, not flip-flops", "[constraints]") {
    // corundum queue_manager: several writes to queue_ram in one block (a mux of dynamic inserts)
    auto b = test::buildDesign("module top(input clk, input [1:0] a, input [1:0] b, input [7:0] d, input we, input wb,\n"
                               "           output reg [7:0] q, output reg [7:0] p);\n"
                               "  reg [7:0] mem [0:3];\n"
                               "  always @(posedge clk) begin\n"
                               "    if (we) mem[a] <= d;\n"
                               "    if (wb) mem[b] <= ~d;\n"
                               "    q <= mem[a];\n"
                               "  end\n"
                               // negative: written at constant indices only (a pipeline): flip-flops per element
                               "  reg [7:0] pipe [0:1];\n"
                               "  always @(posedge clk) begin pipe[0] <= d; pipe[1] <= pipe[0]; p <= pipe[1]; end\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    CHECK(r.cells("mem_reg*", false, false).empty());
    auto bits = [&](std::string_view pattern) {
        std::size_t n = 0;
        for (const auto& o : r.cells(pattern, false, false))
            n += o.bits.size();
        return n;
    };
    CHECK(bits("pipe_reg[1][*]") == 8);
    CHECK(bits("q_reg[*]") == 8);
}

TEST_CASE("get_cells -regexp: filters compare with regular expressions", "[constraints]") {
    auto b = test::buildDesign("module mii_phy_if(input clk, input d, output reg q);\n"
                               "  always @(posedge clk) q <= d;\n"
                               "endmodule\n"
                               "module top(input clk, input d, output q0);\n"
                               "  mii_phy_if u_phy (.clk(clk), .d(d), .q(q0));\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    auto p = parse("set a [get_cells -hier -regexp -filter {ORIG_REF_NAME =~ \"mii_phy_if(__\\w+__\\d+)?\"}]\n"
                   "set b [get_cells -hier -filter {REF_NAME =~ mii_*}]\n"
                   "set c [get_cells -quiet -hier -regexp -filter {REF_NAME =~ \"mii_*\"}]\n"
                   "create_clock -name r[llength $a][llength $b][llength $c] -period 1\n",
                   r);
    CHECK(p.diags.empty());
    REQUIRE(p.set.clocks.size() == 1);
    CHECK(p.set.clocks[0].name == "r110"); // glob without -regexp; "mii_*" as a regex needs "mii" + underscores
    // negative: a malformed regular expression in a filter
    auto q = parse("set a [get_cells -hier -regexp -filter {REF_NAME =~ \"(\"}]\n", r);
    CHECK(q.countCode("CON-PARSE") == 1);
}

TEST_CASE("Tcl procedures: arguments, defaults, args, return, global, local scope", "[constraints]") {
    FakeResolver r;
    auto p = parse("set base 2\n"
                   "proc mk {name {period 10} args} {\n"
                   "    global base\n"
                   "    set local [expr {$period * $base}]\n"
                   "    create_clock -name $name -period $local\n"
                   "    return [llength $args]\n"
                   "}\n"
                   "proc twice {x} { return [expr {$x * 2}]; create_clock -name never -period 1 }\n"
                   "proc first {l} { foreach e $l { if {$e > 1} { return $e } }; return none }\n"
                   "set n [mk a]\n"
                   "set m [mk b 3 x y z]\n"
                   "create_clock -name n${n}_m${m}_t[twice 4]_f[first {1 5 7}]_g[first {0}] -period 1\n"
                   "create_clock -name local_[info_exists_local] -period 1\n",
                   r);
    // 'info_exists_local' is unknown: reported, the rest runs
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 0); // [x] with an unknown command stays literal text
    std::vector<std::string> names;
    for (const auto& c : p.set.clocks)
        names.push_back(c.name);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "a");
    CHECK(p.set.clocks[0].periodNs == 20.0);
    CHECK(p.set.clocks[1].periodNs == 6.0);
    CHECK(names[2] == "n0_m3_t8_f5_gnone");
    CHECK(p.set.clocks[0].loc.line == 5); // diagnostics and locations inside the body keep their line

    // negative: wrong argument counts, runaway recursion, a local variable does not leak
    auto q = parse("proc one {x} { set inner 1 }\n"
                   "one\n"
                   "one 1 2\n"
                   "one 1\n"
                   "proc rec {} { rec }\n"
                   "rec\n"
                   "set y $inner\n",
                   r);
    CHECK(q.countCode("CON-PARSE") == 4); // two argument errors, nesting limit, undefined variable
}

TEST_CASE("Tcl source: relative to the sourcing file, then the configured directories", "[constraints]") {
    test::TempDir dir;
    dir.write("lib/syn/clocks.sdc", "proc constrain_clk {name} { create_clock -name $name -period 8 }\n"
                                    "return\n"
                                    "create_clock -name after_return -period 1\n");
    dir.write("lib/syn/more.sdc", "create_clock -name more -period 4\n");
    const auto top = dir.write("proj/fpga.sdc", "source ../lib/syn/clocks.sdc\n"
                                                "source syn/more.sdc\n" // only via the configured directory
                                                "source missing.sdc\n"
                                                "constrain_clk core\n");
    FakeResolver r;
    ConstraintSet set;
    CollectingDiagnosticSink sink;
    {
        SdcReader reader(r, set, sink, {{dir.path() / "lib"}});
        CHECK(reader.readFile(top));
        // a later file sees the procedures of an earlier one
        reader.parse("constrain_clk second\n", "second.sdc");
    }
    const auto diags = sink.take();
    for (const auto& d : diags)
        UNSCOPED_INFO(format(d));
    std::vector<std::string> names;
    for (const auto& c : set.clocks)
        names.push_back(c.name);
    CHECK(names == std::vector<std::string>{"more", "core", "second"});
    CHECK(std::ranges::count_if(diags, [](const Diagnostic& d) {
              return d.code == "CON-OPEN" && d.message.find("tclSourceDirs") != std::string::npos;
          }) == 1);
    CHECK(set.files.size() == 3); // the top file and the two sourced ones
    // negative: a separate reader does not know the procedure
    auto p = parse("constrain_clk x\n", r);
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 1);
}

TEST_CASE("Clock periods with units, Quartus commands that need no action", "[constraints]") {
    FakeResolver r;
    auto p = parse("create_clock -name a -period \"40.000 ns\"\n"
                   "create_clock -name b -period {125 MHz}\n"
                   "create_clock -name c -period \"800 ps\"\n"
                   "derive_pll_clocks\n"
                   "derive_clock_uncertainty\n"
                   "set_time_format -unit ns -decimal_places 3\n"
                   "create_clock -period 40 -name {altera_reserved_tck} {altera_reserved_tck}\n",
                   r);
    REQUIRE(p.set.clocks.size() == 4);
    CHECK(p.set.clocks[0].periodNs == 40.0);
    CHECK(p.set.clocks[1].periodNs == 8.0);
    CHECK(p.set.clocks[2].periodNs == 0.8);
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 0);
    // Quartus's reserved JTAG port is not in the RTL: noted, not warned
    CHECK(std::ranges::all_of(p.diags, [](const Diagnostic& d) { return d.severity != Severity::Warning; }));
    // negative: an unknown unit
    auto q = parse("create_clock -name d -period \"8 furlongs\"\n", r);
    CHECK(q.countCode("CON-PARSE") == 1);
}

TEST_CASE("Quartus collections: '|' names, registers by signal name, keepers, collection commands", "[constraints]") {
    auto b = test::buildDesign("module sync2(input clk, input d, output q);\n"
                               "  reg s1_reg, s2_reg;\n"
                               "  always @(posedge clk) begin s1_reg <= d; s2_reg <= s1_reg; end\n"
                               "  assign q = s2_reg;\n"
                               "endmodule\n"
                               "module pll(input inclk, output [1:0] clk);\n"
                               "  assign clk = {inclk, inclk};\n"
                               "endmodule\n"
                               "module top(input clk_a, input clk_b, input d, output q, output [1:0] pc);\n"
                               "  reg [1:0] a_reg;\n"
                               "  always @(posedge clk_a) a_reg <= {a_reg[0], d};\n"
                               "  sync2 u_s (.clk(clk_b), .d(a_reg[1]), .q(q));\n"
                               "  pll altpll_component (.inclk(clk_a), .clk(pc));\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    auto p = parse("set regs [get_registers \"u_s|s1_reg u_s|s2_reg\"]\n"
                   "set bits [get_registers {a_reg[*]}]\n"
                   "set all [get_registers *]\n"            // '*' crosses '|'
                   "set keep [get_keepers *_a]\n"           // a port
                   "set none [get_registers -nowarn nosuch*]\n"
                   "set n 0\n"
                   "foreach_in_collection reg $regs { if {[string_first]} {} ; incr n }\n"
                   "set name [get_node_info -name [get_registers u_s|s1_reg]]\n"
                   "set pin [get_pins altpll_component|auto_generated|pll1|clk[0]]\n" // inside the IP: its port
                   "set rpin [get_pins u_s|s1_reg|clk]\n"
                   "set cells [get_cells u_s|*]\n"
                   "create_clock -name r[get_collection_size $regs]_b[get_collection_size $bits]_a[get_collection_size $all]_k[get_collection_size $keep]_n[get_collection_size $none]_f$n -period 1\n"
                   "create_clock -name x_$name -period 1\n"
                   "create_clock -name p[get_collection_size $pin]_rp[get_collection_size $rpin]_c[get_collection_size $cells] -period 1\n",
                   r);
    REQUIRE(p.set.clocks.size() == 3);
    CHECK(p.set.clocks[0].name == "r2_b1_a3_k1_n0_f2"); // a_reg as one vector cell with two bits
    CHECK(p.set.clocks[1].name == "x_u_s|s1_reg");
    CHECK(p.set.clocks[2].name == "p1_rp1_c2");
    CHECK(p.countCode("CON-UNMATCHED") == 0);
    // negative: a path that does not leave the modelled hierarchy is an ordinary miss
    auto q = parse("set x [get_pins u_s|nosuch]\n"
                   "set y [get_registers nosuch]\n",
                   r);
    CHECK(q.countCode("CON-UNMATCHED") == 2);
    // stars after the index (SNES_MiSTer: emu|sdram|st_num[*]*): the bits, any name suffix
    auto t = parse("set all [get_registers {a_reg[*]*}]\n"
                   "set one [get_registers {a_reg[1]*}]\n"
                   "set none [get_registers {a_reg[2]*}]\n" // negative: no such bit
                   "create_clock -name a[get_collection_size $all]_o[get_collection_size $one] -period 1\n",
                   r);
    REQUIRE(t.set.clocks.size() == 1);
    CHECK(t.set.clocks[0].name == "a1_o1");
    CHECK(t.countCode("CON-UNMATCHED") == 1);
    CHECK(r.quartus("a_reg[*]*", QuartusKind::Registers, false).at(0).bits.size() == 2);
    CHECK(r.quartus("a_reg[1]*", QuartusKind::Registers, false).at(0).bits.size() == 1);
}

TEST_CASE("Vivado names leave unnamed generate blocks out (genblk)", "[constraints]") {
    auto b = test::buildDesign("module leaf(input clk, input d, output reg q);\n"
                               "  always @(posedge clk) q <= d;\n"
                               "endmodule\n"
                               "module top(input clk, input [1:0] d, output [1:0] q);\n"
                               "  genvar n;\n"
                               "  generate if (1) begin\n"
                               "    for (n = 0; n < 2; n = n + 1) begin : g\n"
                               "      if (1) begin\n"
                               "        leaf u (.clk(clk), .d(d[n]), .q(q[n]));\n"
                               "      end\n"
                               "    end\n"
                               "  end endgenerate\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    CHECK(r.cells("g[0].u", false, false).size() == 1);                // as Vivado names it
    CHECK(r.cells("genblk1.g[0].genblk1.u", false, false).size() == 1); // as elaborated
    CHECK(r.cells("g[*].u/q_reg", false, false).size() == 2);
    CHECK(r.cellsRegexp("g\\[1\\]\\.u/q_reg", false, false).size() == 1);
    // negative: a wrong level still matches nothing
    CHECK(r.cells("g[0]", false, false).empty());
    CHECK(r.cells("u", false, false).empty());
}

TEST_CASE("Scoped constraints run in every instance of their module (read_xdc -ref)", "[constraints][scoped]") {
    auto b = test::buildDesign("module sync2(input clk_in, input clk_out, input d, output q);\n"
                               "  reg d_q, s1, s2;\n"
                               "  always @(posedge clk_in) d_q <= d;\n"
                               "  always @(posedge clk_out) begin s1 <= d_q; s2 <= s1; end\n"
                               "  assign q = s2;\n"
                               "endmodule\n"
                               "module top(input clk_a, input clk_b, input d, output q0, output q1);\n"
                               "  sync2 u0 (.clk_in(clk_a), .clk_out(clk_b), .d(d), .q(q0));\n"
                               "  sync2 u1 (.clk_in(clk_b), .clk_out(clk_a), .d(d), .q(q1));\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    test::TempDir dir;
    // as hdl-modules / open-logic write them: names relative to the instance,
    // get_ports for the instance's ports, abbreviated get_cell
    const auto scoped = dir.write("sync2.tcl",
                                  "set clk_in [get_clocks -quiet -of_objects [get_ports clk_in]]\n"
                                  "set first [get_cell s1_reg]\n"
                                  "set_max_delay -datapath_only -from $clk_in -to $first 5\n"
                                  "set n [llength [get_cells -hier *s?_reg]]\n"
                                  "if {$n != 2} { create_clock -name wrong_scope -period 1 }\n");
    auto p = parse("create_clock -name a -period 10 [get_ports clk_a]\n"
                   "create_clock -name b -period 7 [get_ports clk_b]\n"
                   "read_xdc -quiet -ref sync2 " + toUtf8(scoped) + " -unmanaged\n",
                   r);
    CHECK(p.countCode("CON-UNMATCHED") == 0);
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 0);
    // one exception per instance, to that instance's first stage
    REQUIRE(p.set.pathExceptions.size() == 2);
    std::vector<std::string> to;
    for (const auto& e : p.set.pathExceptions) {
        REQUIRE(e.to.size() == 1);
        to.push_back(view.name(e.to.front().bit));
        CHECK(e.fromClockBits.size() == 1); // the clock on the instance's clk_in pin
    }
    std::ranges::sort(to);
    CHECK(to == std::vector<std::string>{"top.u0.s1", "top.u1.s1"});
    // -hierarchical searches below the instance only (2 stages, not 4)
    CHECK(std::ranges::none_of(p.set.clocks, [](const ClockDef& c) { return c.name == "wrong_scope"; }));
    // each instance run has its own variables; the outer script keeps its own
    CHECK(p.set.files.size() == 1);

    // negative: a module the design does not have
    auto none = parse("read_xdc -ref nosuch " + toUtf8(scoped) + "\n", r);
    CHECK(none.countCode("CON-UNMATCHED") == 1);
    CHECK(none.set.pathExceptions.empty());

    // scoped to the top: names as they are
    const auto topScoped = dir.write("top.tcl", "set_false_path -to [get_cells u0/s1_reg]\n");
    auto t = parse("read_xdc -ref top " + toUtf8(topScoped) + "\n", r);
    CHECK(t.set.pathExceptions.size() == 1);

    // without -ref: a plain constraint file
    auto plain = parse("read_xdc " + toUtf8(topScoped) + "\n", r);
    CHECK(plain.set.pathExceptions.size() == 1);
}

namespace {
struct TwoSyncs {
    test::BuiltDesign b = test::buildDesign("module sync2(input clk_in, input clk_out, input d, output q);\n"
                                            "  reg d_q, s1, s2;\n"
                                            "  always @(posedge clk_in) d_q <= d;\n"
                                            "  always @(posedge clk_out) begin s1 <= d_q; s2 <= s1; end\n"
                                            "  assign q = s2;\n"
                                            "endmodule\n"
                                            "module wrap(input ca, input cb, input d, output q);\n"
                                            "  sync2 inner (.clk_in(ca), .clk_out(cb), .d(d), .q(q));\n"
                                            "endmodule\n"
                                            "module top(input clk_a, input clk_b, input d, output q0, output q1);\n"
                                            "  sync2 u0 (.clk_in(clk_a), .clk_out(clk_b), .d(d), .q(q0));\n"
                                            "  wrap u1 (.ca(clk_b), .cb(clk_a), .d(d), .q(q1));\n"
                                            "endmodule\n",
                                            "top");
    graph::DesignView view{*b.design};
    DesignObjectResolver r{view};

    [[nodiscard]] std::vector<std::string> toNames(const Parsed& p) const {
        std::vector<std::string> out;
        for (const auto& e : p.set.pathExceptions)
            for (const auto& t : e.to)
                out.push_back(view.name(t.bit));
        std::ranges::sort(out);
        return out;
    }
};
} // namespace

TEST_CASE("current_instance: names relative to an instance, '..', reset, get_ports", "[constraints][scoped]") {
    TwoSyncs d;
    // as Digilent's synchronizer example writes it
    auto& r = d.r;
    auto a = parse("create_clock -name a -period 10 [get_ports clk_a]\n"
                   "current_instance -quiet\n"
                   "current_instance [get_cells u0]\n"
                   "set_false_path -to [get_cells s1_reg]\n"
                   "if {[current_instance .] != \"u0\"} { create_clock -name wrong_value -period 1 }\n"
                   // plain get_ports still names the top-level ports
                   "create_clock -name b -period 7 [get_ports clk_b]\n"
                   // the instance's own pins
                   "set ci [get_ports -scoped_to_current_instance clk_out]\n"
                   "if {[llength $ci] != 1} { create_clock -name wrong_pins -period 1 }\n"
                   "current_instance -quiet\n"
                   "current_instance u1/inner\n"
                   "set_false_path -to [get_cells s2_reg]\n"
                   "current_instance ..\n"
                   "set_false_path -to [get_cells inner/d_q_reg]\n"
                   "current_instance\n"
                   "set_false_path -to [get_cells u0/s2_reg]\n",
                   r);
    CHECK(a.countCode("CON-UNMATCHED") == 0);
    CHECK(a.countCode("CON-IGNORED") == 0); // current_instance is a command now, not an ignored one
    CHECK(d.toNames(a) == std::vector<std::string>{"top.u0.s1", "top.u0.s2", "top.u1.inner.d_q", "top.u1.inner.s2"});
    CHECK(std::ranges::count_if(a.set.clocks, [](const ClockDef& c) { return c.name.starts_with("wrong"); }) == 0);
    CHECK(std::ranges::count_if(a.set.clocks, [](const ClockDef& c) { return c.name == "b" && !c.sources.empty(); }) == 1);

    // negative: get_ports does not see the instance's pins without the option
    auto n = parse("current_instance u0\nset x [get_ports clk_out]\n", r);
    CHECK(n.countCode("CON-UNMATCHED") == 1);

    // negative: an instance that does not exist, or '..' above the top,
    // leaves the current instance unchanged
    auto bad = parse("current_instance nosuch\n"
                     "set_false_path -to [get_cells u0/s1_reg]\n"
                     "current_instance ..\n"
                     "current_instance [get_cells u0/s1_reg]\n"
                     "set_false_path -to [get_cells u0/s2_reg]\n",
                     r);
    CHECK(bad.countCode("CON-UNMATCHED") == 3);
    CHECK(d.toNames(bad) == std::vector<std::string>{"top.u0.s1", "top.u0.s2"});

    // the current instance ends with its file
    ConstraintSet set;
    CollectingDiagnosticSink sink;
    SdcReader reader(r, set, sink);
    reader.parse("current_instance u0\nset_false_path -to [get_cells s1_reg]\n", "a.xdc");
    reader.parse("set_false_path -to [get_cells u0/s2_reg]\n", "b.xdc");
    const auto diags = sink.take();
    CHECK(std::ranges::count_if(diags, [](const Diagnostic& x) { return x.code == "CON-UNMATCHED"; }) == 0);
    CHECK(set.pathExceptions.size() == 2);
}

TEST_CASE("current_instance inside scoped constraints stays within the instance", "[constraints][scoped]") {
    TwoSyncs d;
    test::TempDir dir;
    const auto scoped = dir.write("wrap.tcl", "current_instance inner\n"
                                              "set_false_path -to [get_cells s1_reg]\n"
                                              "current_instance\n"
                                              "set_false_path -to [get_cells inner/s2_reg]\n"
                                              "current_instance ..\n" // above the scoped instance: refused
                                              "set_false_path -to [get_cells inner/d_q_reg]\n");
    auto p = parse("read_xdc -ref wrap " + toUtf8(scoped) + "\n", d.r);
    CHECK(p.countCode("CON-UNMATCHED") == 1);
    CHECK(d.toNames(p) == std::vector<std::string>{"top.u1.inner.d_q", "top.u1.inner.s1", "top.u1.inner.s2"});
}

TEST_CASE("get_ports -of_objects: the instance ports on nets (open-logic olo_intf_sync)", "[constraints][scoped]") {
    TwoSyncs d;
    test::TempDir dir;
    const auto scoped = dir.write(
        "sync2.tcl",
        "set in_ffs [get_pins -of_objects [get_cells d_q_reg] -filter {REF_PIN_NAME == D}]\n"
        "set in_ports [get_ports -scoped_to_current_instance -prop_thru_buffers -of_objects [get_nets -of_objects "
        "$in_ffs]]\n"
        "if {[llength $in_ports] != 1} { create_clock -name wrong_ports -period 1 }\n"
        "set_max_delay -datapath_only 2 -from $in_ports\n");
    auto p = parse("read_xdc -ref sync2 " + toUtf8(scoped) + "\n", d.r);
    CHECK(p.countCode("CON-UNMATCHED") == 0);
    CHECK(p.countCode("CON-UNSUPPORTED-OPTION") == 0);
    CHECK(std::ranges::none_of(p.set.clocks, [](const ClockDef& c) { return c.name == "wrong_ports"; }));
    // two instances of sync2: an exception from each instance's d pin
    REQUIRE(p.set.pathExceptions.size() == 2);
    for (const auto& e : p.set.pathExceptions)
        CHECK(!e.from.empty());
    // negative: no port on a net that stays inside the instance
    auto n = parse("set x [get_ports -of_objects [get_nets u0/s1]]\n", d.r);
    CHECK(n.countCode("CON-UNMATCHED") == 1);
}

TEST_CASE("Filters see one flip-flop per bit, as Vivado names them", "[constraints]") {
    auto b = test::buildDesign("module top(input clk, input [3:0] d, output [3:0] q, output [0:0] o, output s);\n"
                               "  reg [3:0] v; reg [0:0] one; reg sc;\n"
                               "  always @(posedge clk) begin v <= d; one <= d[0]; sc <= d[1]; end\n"
                               "  assign q = v; assign o = one; assign s = sc;\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    DesignObjectResolver r(view);
    auto p = parse("set a [get_cells -hier -filter {NAME =~ *v_reg[2]}]\n"
                   "if {[llength $a] != 1} { create_clock -name wrong_a -period 1 }\n"
                   "set_false_path -to $a\n"
                   "set c [get_pins -hier -filter {NAME =~ *v_reg[*]/C}]\n"
                   "if {[llength $c] != 4} { create_clock -name wrong_c -period 1 }\n"
                   // a vector of one bit is named with its index
                   "set o [get_cells -hier -filter {NAME =~ *one_reg[0]}]\n"
                   "if {[llength $o] != 1} { create_clock -name wrong_o -period 1 }\n",
                   r);
    CHECK(std::ranges::none_of(p.set.clocks, [](const ClockDef& c) { return c.name.starts_with("wrong"); }));
    REQUIRE(p.set.pathExceptions.size() == 1);
    REQUIRE(p.set.pathExceptions.front().to.size() == 1);
    CHECK(view.name(p.set.pathExceptions.front().to.front().bit) == "top.v[2]");
    // negative: a scalar register is not indexed
    auto n = parse("set x [get_cells -hier -filter {NAME =~ *sc_reg[0]}]\n", r);
    CHECK(n.countCode("CON-UNMATCHED") == 1);
}

TEST_CASE("Quartus' set_clock_groups -exclusive is read as -asynchronous", "[constraints][quartus]") {
    FakeResolver r;
    auto p = parse("set_clock_groups -exclusive -group {a} -group [get_clocks {b}]\n"
                   "set_clock_groups -exclusive -physically_exclusive -group a -group b\n",
                   r);
    REQUIRE(p.set.groups.size() == 2);
    CHECK(p.set.groups[0].kind == GroupKind::Asynchronous);
    CHECK(p.set.groups[0].groups == std::vector<std::vector<std::string>>{{"a"}, {"b"}});
    CHECK(std::ranges::any_of(p.diags, [](const Diagnostic& d) { return d.code == "CON-EXCLUSIVE-AS-ASYNC"; }));
    // negative: an explicit kind stays as given
    CHECK(p.set.groups[1].kind == GroupKind::PhysicallyExclusive);
    // negative: no kind at all is still an error
    auto none = parse("set_clock_groups -group a -group b\n", r);
    CHECK(none.set.groups.empty());
    CHECK(std::ranges::any_of(none.diags, [](const Diagnostic& d) { return d.code == "CON-PARSE"; }));
}

TEST_CASE("Gowin SDC: '//' comments on their own line and after a command", "[constraints][gowin]") {
    // nestang nestang.sdc
    FakeResolver r;
    auto p = parse("// NES clocks\n"
                   "create_clock -name clk -period 37.04 [get_ports {clk}]       // 27 Mhz\n"
                   "//create_generated_clock -name x -source [get_ports {clk}] -divide_by 3 [get_ports {clk_b}]\n"
                   "report_timing -hold -from_clock [get_clocks {clk*}] -to_clock [get_clocks {clk*}] -max_paths 25\n",
                   r);
    CHECK(p.countCode("CON-PARSE") == 0);
    CHECK(p.countCode("CON-UNKNOWN-COMMAND") == 0);
    CHECK(p.countCode("CON-UNMATCHED") == 0);
    REQUIRE(p.set.clocks.size() == 1);
    CHECK(p.set.clocks[0].name == "clk");
    CHECK(p.set.clocks[0].periodNs == 37.04);
    // negative: '//' inside a word or in braces is not a comment
    auto q = parse("create_clock -name {a//b} -period 10 [get_ports clk]\n", r);
    REQUIRE(q.set.clocks.size() == 1);
    CHECK(q.set.clocks[0].name == "a//b");
}

TEST_CASE("Lattice preferences (.lpf): clocks and blocked clock paths as SDC", "[constraints][lpf]") {
    // f32c ulx3s_fer.lpf
    CollectingDiagnosticSink sink;
    const std::string lpf = "COMMERCIAL;\n"
                            "BLOCK RESETPATHS;\n"
                            "BLOCK ASYNCPATHS;\n"
                            "# a comment\n"
                            "FREQUENCY PORT \"clk\" 25 MHZ;\n"
                            "FREQUENCY NET \"u/pll_out\" 100.0 MHz HOLD_MARGIN 1 ns;\n"
                            "PERIOD PORT \"clk_b\" 8 NS;\n"
                            "LOCATE COMP \"clk\" SITE \"G2\";\n"
                            "IOBUF PORT \"clk\" IO_TYPE=LVCMOS33;\n"
                            "BLOCK PATH FROM CLKNET \"clk_90m\"\n"
                            "  TO CLKNET \"pixclk\";\n"
                            "MULTICYCLE FROM CLKNET \"a\" TO CLKNET \"b\" 2 X;\n"
                            "FREQUENCY 50 MHZ;\n";
    const std::string sdc = lpfToSdc(lpf, "t.lpf", sink);
    const auto diags = sink.take();
    for (const auto& d : diags)
        UNSCOPED_INFO(format(d));
    UNSCOPED_INFO(sdc);
    // one SDC line per preference line: diagnostics keep the preference's line
    std::vector<std::string> lines;
    std::istringstream in(sdc);
    for (std::string l; std::getline(in, l);)
        lines.push_back(l);
    REQUIRE(lines.size() >= 12);
    CHECK(lines[4] == "create_clock -name {clk} -period 40 [get_ports {clk}]");
    CHECK(lines[5] == "create_clock -name {u/pll_out} -period 10 [get_nets {u/pll_out}]");
    CHECK(lines[6] == "create_clock -name {clk_b} -period 8 [get_ports {clk_b}]");
    CHECK(lines[7].empty()); // LOCATE: physical
    CHECK(lines[9] == "set_false_path -from [get_clocks -of_objects [get_nets {clk_90m}]] -to "
                      "[get_clocks -of_objects [get_nets {pixclk}]]");
    CHECK(lines[11].empty());
    REQUIRE(std::ranges::count_if(diags, [](const Diagnostic& d) { return d.code == "CON-LPF-IGNORED"; }) == 1);
    const auto ign = std::ranges::find(diags, "CON-LPF-IGNORED", &Diagnostic::code);
    CHECK(std::ranges::find(ign->notes, "MULTICYCLE") != ign->notes.end());
    CHECK(std::ranges::find(ign->notes, "FREQUENCY (global)") != ign->notes.end());
    CHECK(std::ranges::none_of(diags, [](const Diagnostic& d) { return d.code == "CON-LPF-PARSE"; }));

    // the SDC reader takes the clocks
    FakeResolver r;
    auto p = parse(sdc, r);
    REQUIRE(p.set.clocks.size() >= 2);
    CHECK(p.set.clocks[0].name == "clk");
    CHECK(p.set.clocks[0].periodNs == 40.0);
    CHECK(isLpfFile("a/B.LPF"));
    CHECK_FALSE(isLpfFile("a.sdc"));
}

TEST_CASE("Lattice preferences negative: malformed and unsupported timing preferences", "[constraints][lpf]") {
    CollectingDiagnosticSink sink;
    const std::string sdc = lpfToSdc("FREQUENCY PORT \"clk\" fast;\n"
                                     "BLOCK PATH FROM GROUP \"g\" TO CLKNET \"b\";\n"
                                     "BLOCK INTERCLOCKDOMAIN PATHS;\n",
                                     "t.lpf", sink);
    const auto diags = sink.take();
    CHECK(std::ranges::count_if(diags, [](const Diagnostic& d) { return d.code == "CON-LPF-PARSE"; }) == 3);
    CHECK(sdc.find("create_clock") == std::string::npos);
    CHECK(sdc.find("set_false_path") == std::string::npos);
}

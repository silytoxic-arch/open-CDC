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
                   "set_false_path -through [get_ports clk]\n"            // ignored (args not evaluated)
                   "set_output_delay 1 -clock x [get_ports rst_n]\n"
                   "set_output_delay 2 -clock x [get_ports rst_n]\n"
                   "proc f {} { puts x }\n"                               // unsupported Tcl
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
                   "    if {[llength $ffs] == 2 && [llength $bits] == 1 && [llength $async] == 2} { incr n }\n"
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

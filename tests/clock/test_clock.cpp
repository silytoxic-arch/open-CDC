#include "clock/ClockAnalyzer.h"

#include "../DesignUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;
using namespace opencdc::clock;

namespace {

struct ClockRun {
    test::BuiltDesign built;
    std::unique_ptr<graph::DesignView> view;
    constraints::ConstraintSet cons;
    std::shared_ptr<const ClockModel> model;
    std::vector<Diagnostic> diags;

    [[nodiscard]] const RegisterClocking& reg(std::string_view hierQ) const {
        auto [inst, r] = test::findRegister(*built.design, hierQ);
        return model->registers.at(view->globalRegister(inst, r));
    }
    [[nodiscard]] ClockId id(std::string_view name) const {
        auto c = model->find(name);
        REQUIRE(c);
        return *c;
    }
    [[nodiscard]] const Clock& clock(std::string_view name) const { return model->clock(id(name)); }
    [[nodiscard]] ClockRelation rel(std::string_view a, std::string_view b) const {
        return model->relation(id(a), id(b));
    }
    [[nodiscard]] std::string clockName(std::string_view hierQ, std::size_t k = 0) const {
        const auto& r = reg(hierQ);
        REQUIRE(r.clocks.size() > k);
        return model->clock(r.clocks[k].clock).name;
    }
    [[nodiscard]] std::size_t countCode(std::string_view c) const {
        return static_cast<std::size_t>(std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == c; }));
    }
};

ClockRun analyze(std::string_view verilog, std::string_view sdc = "", std::string top = "top") {
    ClockRun run;
    run.built = test::buildDesign(verilog, std::move(top));
    run.view = std::make_unique<graph::DesignView>(*run.built.design);
    CollectingDiagnosticSink sink;
    constraints::DesignObjectResolver resolver(*run.view);
    if (!sdc.empty())
        constraints::parseSdc(sdc, "t.sdc", resolver, run.cons, sink);
    run.model = analyzeClocks(*run.view, run.cons, sink);
    run.diags = sink.take();
    for (const auto& d : run.diags)
        UNSCOPED_INFO(format(d));
    return run;
}

constexpr const char* kTwoClocks = "module top(input clk_a, input clk_b, input d, output reg qa, output reg qb);\n"
                                   "  always @(posedge clk_a) qa <= d;\n"
                                   "  always @(posedge clk_b) qb <= qa;\n"
                                   "endmodule\n";

} // namespace

TEST_CASE("Single primary clock", "[clock]") {
    auto r = analyze("module top(input clk, input d, output reg q1, output reg q2);\n"
                     "  always @(posedge clk) begin q1 <= d; q2 <= q1; end\n"
                     "endmodule\n");
    REQUIRE(r.model->clocks.size() == 1);
    const auto& c = r.clock("clk");
    CHECK(c.kind == ClockKind::Primary);
    CHECK(c.posedgeRegisters == 2);
    CHECK(r.reg("top.q1").status == ClockStatus::Resolved);
    CHECK(r.reg("top.q1").clocks[0].edge == ir::Edge::Pos);
}

TEST_CASE("Different clock names alone do not mean asynchronous", "[clock][relation]") {
    auto r = analyze(kTwoClocks);
    REQUIRE(r.model->clocks.size() == 2);
    CHECK(r.rel("clk_a", "clk_b") == ClockRelation::Unknown);
    CHECK(r.model->reason(r.id("clk_a"), r.id("clk_b")).find("independent sources") != std::string::npos);
    CHECK(r.countCode("CLK-RELATION-UNKNOWN") == 1);
}

TEST_CASE("set_clock_groups -asynchronous", "[clock][relation][constraints]") {
    auto r = analyze(kTwoClocks, "create_clock -name A -period 10 [get_ports clk_a]\n"
                                 "create_clock -name B -period 4 [get_ports clk_b]\n"
                                 "set_clock_groups -asynchronous -group A -group B\n");
    REQUIRE(r.model->clocks.size() == 2);
    CHECK(r.clockName("top.qa") == "A");
    CHECK(r.clockName("top.qb") == "B");
    CHECK(r.clock("A").periodNs == 10.0);
    CHECK(r.clock("A").kind == ClockKind::Constraint);
    CHECK(r.rel("A", "B") == ClockRelation::Asynchronous);
    CHECK(r.model->reason(r.id("A"), r.id("B")).find("t.sdc:3") != std::string::npos);
    CHECK(r.countCode("CLK-RELATION-UNKNOWN") == 0);
}

TEST_CASE("A single -group is asynchronous to every other clock", "[clock][relation][constraints]") {
    auto r = analyze(kTwoClocks, "set_clock_groups -asynchronous -group [get_clocks clk_a]\n");
    CHECK(r.rel("clk_a", "clk_b") == ClockRelation::Asynchronous);
}

TEST_CASE("Exclusive clock groups", "[clock][relation][constraints]") {
    auto r = analyze(kTwoClocks, "set_clock_groups -logically_exclusive -group clk_a -group clk_b\n");
    CHECK(r.rel("clk_a", "clk_b") == ClockRelation::Exclusive);
}

TEST_CASE("Inverted clocks keep the clock and flip the edge", "[clock]") {
    auto r = analyze("module top(input clk, input d, output reg q1, output reg q2, output reg q3);\n"
                     "  wire nclk = ~clk;\n"
                     "  always @(posedge nclk) q1 <= d;\n"
                     "  always @(negedge clk) q2 <= d;\n"
                     "  always @(negedge nclk) q3 <= d;\n"
                     "endmodule\n");
    REQUIRE(r.model->clocks.size() == 1);
    CHECK(r.reg("top.q1").clocks[0].edge == ir::Edge::Neg);
    CHECK(r.reg("top.q2").clocks[0].edge == ir::Edge::Neg);
    CHECK(r.reg("top.q3").clocks[0].edge == ir::Edge::Pos);
    CHECK(r.clock("clk").negedgeRegisters == 2);
}

TEST_CASE("Clocks are traced through hierarchy and assignments", "[clock]") {
    auto r = analyze("module leaf(input ck, input d, output reg q); always @(posedge ck) q <= d; endmodule\n"
                     "module mid(input c, input d, output q); wire cc; assign cc = c; leaf u (.ck(cc), .d(d), .q(q)); endmodule\n"
                     "module top(input sysclk, input d, output q);\n"
                     "  wire k;\n"
                     "  assign k = sysclk;\n"
                     "  mid m (.c(k), .d(d), .q(q));\n"
                     "endmodule\n");
    CHECK(r.clockName("top.m.u.q") == "sysclk");
}

TEST_CASE("Gated clock belongs to its master clock", "[clock][gating]") {
    auto r = analyze("module top(input clk, input en_in, input d, output reg q, output reg en);\n"
                     "  always @(posedge clk) en <= en_in;\n"
                     "  wire gclk = clk & en;\n"
                     "  always @(posedge gclk) q <= d;\n"
                     "endmodule\n");
    const auto& rc = r.reg("top.q");
    CHECK(rc.status == ClockStatus::Resolved);
    CHECK(r.clockName("top.q") == "clk");
    CHECK(rc.gated);
    REQUIRE(rc.gatingSignals.size() == 1);
    CHECK(r.view->name(rc.gatingSignals[0]) == "top.en");
    CHECK(r.model->clocks.size() == 1); // the enable register is not a clock
    CHECK(r.countCode("CLK-GATED") == 1);
}

TEST_CASE("Latch-based clock gating cell (ICG)", "[clock][gating]") {
    auto r = analyze("module icg(input clk, input en, output gclk);\n"
                     "  reg en_l;\n"
                     "  always @* if (!clk) en_l = en;\n"
                     "  assign gclk = clk & en_l;\n"
                     "endmodule\n"
                     "module top(input clk, input en, input d, output reg q);\n"
                     "  wire g;\n"
                     "  icg u_icg (.clk(clk), .en(en), .gclk(g));\n"
                     "  always @(posedge g) q <= d;\n"
                     "endmodule\n");
    CHECK(r.clockName("top.q") == "clk");
    CHECK(r.reg("top.q").gated);
}

TEST_CASE("Clock multiplexer: register belongs to both clocks", "[clock][mux]") {
    auto r = analyze("module top(input clk_a, input clk_b, input sel, input d, output reg q, output reg qa, output reg qb);\n"
                     "  wire mclk = sel ? clk_b : clk_a;\n"
                     "  always @(posedge mclk) q <= d;\n"
                     "  always @(posedge clk_a) qa <= d;\n"
                     "  always @(posedge clk_b) qb <= d;\n"
                     "endmodule\n");
    const auto& rc = r.reg("top.q");
    CHECK(rc.muxed);
    REQUIRE(rc.clocks.size() == 2);
    CHECK(r.clockName("top.q", 0) == "clk_a");
    CHECK(r.clockName("top.q", 1) == "clk_b");
    CHECK(r.countCode("CLK-MUX") == 1);
}

TEST_CASE("Divider register output used as a clock", "[clock][divider]") {
    auto r = analyze("module top(input clk, input d, output reg q);\n"
                     "  reg div = 1'b0;\n"
                     "  always @(posedge clk) div <= ~div;\n"
                     "  always @(posedge div) q <= d;\n"
                     "endmodule\n");
    REQUIRE(r.model->clocks.size() == 2);
    const auto& div = r.clock("top.div");
    CHECK(div.kind == ClockKind::Divider);
    CHECK(r.model->clock(div.master).name == "clk");
    CHECK(r.rel("clk", "top.div") == ClockRelation::Synchronous);
    CHECK(r.model->reason(r.id("clk"), r.id("top.div")).find("derived from 'clk'") != std::string::npos);
    CHECK(r.clockName("top.q") == "top.div");
}

TEST_CASE("Generated clock constraint on a divider", "[clock][divider][constraints]") {
    auto r = analyze("module top(input clk, input d, output reg q);\n"
                     "  reg div = 1'b0;\n"
                     "  always @(posedge clk) div <= ~div;\n"
                     "  always @(posedge div) q <= d;\n"
                     "endmodule\n",
                     "create_clock -name sys -period 10 [get_ports clk]\n"
                     "create_generated_clock -name half -source [get_pins div_reg/C] -divide_by 2 [get_pins div_reg/Q]\n");
    REQUIRE(r.model->clocks.size() == 2);
    const auto& half = r.clock("half");
    CHECK(half.kind == ClockKind::Generated);
    CHECK(r.model->clock(half.master).name == "sys");
    CHECK(half.periodNs == 20.0);
    CHECK(r.clockName("top.q") == "half");
    CHECK(r.rel("sys", "half") == ClockRelation::Synchronous);
}

TEST_CASE("Clock groups apply to derived clocks through their master", "[clock][relation][constraints]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, output reg q, output reg qb);\n"
                     "  reg div = 1'b0;\n"
                     "  always @(posedge clk_a) div <= ~div;\n"
                     "  always @(posedge div) q <= d;\n"
                     "  always @(posedge clk_b) qb <= q;\n"
                     "endmodule\n",
                     "set_clock_groups -asynchronous -group clk_a -group clk_b\n");
    CHECK(r.rel("top.div", "clk_b") == ClockRelation::Asynchronous);
    CHECK(r.model->reason(r.id("top.div"), r.id("clk_b")).find("via master clock 'clk_a'") != std::string::npos);
}

TEST_CASE("Two clocks generated from the same source are synchronous", "[clock][relation]") {
    auto r = analyze("module top(input clk, input d, output reg q1, output reg q2);\n"
                     "  reg d2 = 1'b0;\n"
                     "  reg [1:0] d4 = 2'b00;\n"
                     "  always @(posedge clk) begin d2 <= ~d2; d4 <= d4 + 2'd1; end\n"
                     "  always @(posedge d2) q1 <= d;\n"
                     "  always @(posedge d4[1]) q2 <= q1;\n"
                     "endmodule\n");
    CHECK(r.rel("top.d2", "top.d4[1]") == ClockRelation::Synchronous);
    CHECK(r.model->reason(r.id("top.d2"), r.id("top.d4[1]")).find("both are derived from 'clk'") != std::string::npos);
}

TEST_CASE("Clock on an internal net defined by create_clock", "[clock][constraints]") {
    auto r = analyze("module top(input osc, input d, output reg q);\n"
                     "  wire pll_out = ~osc;\n"
                     "  always @(posedge pll_out) q <= d;\n"
                     "endmodule\n",
                     "create_clock -name pll -period 2.5 [get_nets pll_out]\n");
    CHECK(r.clockName("top.q") == "pll");
    CHECK(r.reg("top.q").clocks[0].edge == ir::Edge::Pos); // tracing stopped at the constrained net
    CHECK(r.model->clocks.size() == 1);
}

TEST_CASE("Constant and undriven clocks", "[clock][unresolved]") {
    auto r = analyze("module top(input d, output reg q1, output reg q2);\n"
                     "  wire nc;\n"
                     "  always @(posedge 1'b0) q1 <= d;\n"
                     "  always @(posedge nc) q2 <= d;\n"
                     "endmodule\n");
    CHECK(r.reg("top.q1").status == ClockStatus::Constant);
    CHECK(r.reg("top.q2").status == ClockStatus::Unresolved);
    CHECK(r.reg("top.q2").reason.find("undriven") != std::string::npos);
    CHECK(r.countCode("CLK-CONSTANT") == 1);
    CHECK(r.countCode("CLK-UNRESOLVED") == 1);
    CHECK(r.model->unresolvedRegisters() == 2);
}

TEST_CASE("Ambiguous gating is reported instead of guessed", "[clock][unresolved]") {
    // Neither input is used as a clock elsewhere nor named like a clock.
    auto r = analyze("module top(input a, input b, input d, output reg q);\n"
                     "  wire g = a & b;\n"
                     "  always @(posedge g) q <= d;\n"
                     "endmodule\n");
    CHECK(r.reg("top.q").status == ClockStatus::Unresolved);
    CHECK(r.reg("top.q").reason.find("cannot tell which input") != std::string::npos);
}

TEST_CASE("Clock name heuristic breaks gating ties between inputs", "[clock][gating]") {
    auto r = analyze("module top(input sys_clk, input enable, input d, output reg q);\n"
                     "  always @(posedge (sys_clk | enable)) q <= d;\n"
                     "endmodule\n");
    CHECK(r.clockName("top.q") == "sys_clk");
    CHECK(r.reg("top.q").gated);
}

TEST_CASE("Clock through arithmetic logic is unresolved", "[clock][unresolved]") {
    auto r = analyze("module top(input [1:0] c, input d, output reg q);\n"
                     "  wire k = (c + 2'd1) == 2'd0;\n"
                     "  always @(posedge k) q <= d;\n"
                     "endmodule\n");
    CHECK(r.reg("top.q").status == ClockStatus::Unresolved);
    CHECK(r.reg("top.q").reason.find("'eq' logic") != std::string::npos);
}

TEST_CASE("Self-clocked register does not loop forever", "[clock][unresolved]") {
    auto r = analyze("module top(input d, output reg t);\n"
                     "  always @(posedge t) t <= ~t;\n"
                     "endmodule\n");
    // t clocks itself: a divider clock whose master cannot be found.
    CHECK(r.countCode("CLK-UNRESOLVED") >= 1);
}

TEST_CASE("Async FIFO example with its XDC", "[clock][example]") {
    ClockRun run;
    const auto dir = test::examplesDir() / "async_fifo";
    frontend::FrontendInput in;
    for (const char* f : {"rtl/fifomem.v", "rtl/sync_r2w.v", "rtl/sync_w2r.v", "rtl/rptr_empty.v", "rtl/wptr_full.v",
                          "rtl/afifo.v"})
        in.sourceFiles.push_back(dir / f);
    in.top = "afifo";
    CollectingDiagnosticSink sink;
    auto out = frontend::makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    run.built.design = out.value().design;
    run.view = std::make_unique<graph::DesignView>(*run.built.design);
    constraints::DesignObjectResolver resolver(*run.view);
    REQUIRE(constraints::readSdcFile(dir / "constraints" / "afifo.xdc", resolver, run.cons, sink));
    run.model = analyzeClocks(*run.view, run.cons, sink);
    run.diags = sink.take();

    CHECK(run.countCode("CON-UNMATCHED") == 0);
    CHECK(run.clock("wclk").registers() == 6);
    CHECK(run.clock("rclk").registers() == 5);
    CHECK(run.clockName("afifo.u_sync_r2w.wq2_rptr") == "wclk");
    CHECK(run.clockName("afifo.u_sync_w2r.rq2_wptr") == "rclk");
    CHECK(run.rel("wclk", "rclk") == ClockRelation::Asynchronous);
    CHECK(run.model->unresolvedRegisters() == 0);
}

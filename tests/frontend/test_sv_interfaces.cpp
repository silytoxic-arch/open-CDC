// SystemVerilog interfaces (stage 10): interface instances flattened into the
// declaring module, interface ports turned into one port per used member,
// modports, generic interface ports, interface arrays, ports passed down.

#include "../TestUtil.h"
#include "cdc/CdcModel.h"
#include "engine/AnalysisSession.h"
#include "ir/Dump.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace opencdc;
using namespace opencdc::cdc;

namespace {

struct Run {
    std::shared_ptr<const engine::AnalysisResult> r;
    test::TempDir dir;
    std::string dump;

    [[nodiscard]] const CdcPath* path(std::string_view src, std::string_view dst) const {
        for (const auto& p : r->cdc->paths)
            if (p.srcName == src && p.dstName == dst)
                return &p;
        for (const auto& p : r->cdc->paths)
            UNSCOPED_INFO("path: " << p.srcName << " -> " << p.dstName);
        return nullptr;
    }
    [[nodiscard]] std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(r->diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
    }
    [[nodiscard]] bool has(std::string_view text) const { return dump.find(text) != std::string::npos; }
};

std::unique_ptr<Run> analyze(const std::string& text) {
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "sv";
    cfg.top = "top";
    cfg.language = frontend::LanguageStd::SystemVerilog2017;
    cfg.sources = {run->dir.write("t.sv", text)};
    cfg.sourceLibraries = {""};
    cfg.constraints = {run->dir.write("c.sdc", "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                                               "set_clock_groups -asynchronous -group [get_clocks clk_a] "
                                               "-group [get_clocks clk_b]\n")};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    run->r = session.run(cfg, progress, cancel);
    REQUIRE(run->r);
    for (const auto& d : run->r->diagnostics)
        UNSCOPED_INFO(format(d));
    if (run->r->design)
        run->dump = ir::dumpDesign(*run->r->design);
    UNSCOPED_INFO(run->dump);
    return run;
}

} // namespace

TEST_CASE("SV interfaces: modport ports carry signals between modules", "[sv][interface]") {
    auto run = analyze("interface bus_if #(parameter W = 8) (input logic clk);\n"
                       "  logic [W-1:0] data; logic valid;\n"
                       "  modport src (output data, output valid, input clk);\n"
                       "  modport dst (input data, input valid, input clk);\n"
                       "endinterface\n"
                       "module producer(bus_if.src b, input logic [7:0] d);\n"
                       "  always_ff @(posedge b.clk) begin b.data <= d; b.valid <= 1'b1; end\n"
                       "endmodule\n"
                       "module consumer(bus_if.dst b, output logic [7:0] q);\n"
                       "  logic [7:0] s1, s2;\n"
                       "  always_ff @(posedge b.clk) begin s1 <= b.data; s2 <= s1; end\n"
                       "  assign q = s2;\n"
                       "endmodule\n"
                       "module top(input logic clk_a, input logic clk_b, input logic [7:0] d, output logic [7:0] q);\n"
                       "  bus_if #(8) ia (clk_a);\n"
                       "  bus_if #(8) ib (clk_b);\n"
                       "  producer p (.b(ia), .d(d));\n"
                       "  assign ib.data = ia.data;\n"
                       "  consumer c (.b(ib), .q(q));\n"
                       "endmodule\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("IR-UNSUPPORTED-PORT") == 0);
    CHECK(run->count("IR-UNSUPPORTED-EXPR") == 0);
    CHECK(run->count("IR-UNSUPPORTED-INSTANCE") == 0);
    CHECK(run->has("output var [7:0] b.data"));
    CHECK(run->has("input var [7:0] b.data"));
    CHECK(run->has("inst p : producer (.b.clk(ia.clk), .b.data(ia.data), .b.valid(ia.valid), .d(d))"));
    const auto* p = run->path("top.p.b.data", "top.c.s1");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::TwoFlop);
    CHECK(p->width == 8);
}

TEST_CASE("SV interfaces: logic inside the interface, ports passed down, shared bodies", "[sv][interface]") {
    auto run = analyze("interface sync_if (input logic clk);\n"
                       "  logic req, req_s1, req_s2;\n"
                       "  always_ff @(posedge clk) begin req_s1 <= req; req_s2 <= req_s1; end\n"
                       "  modport dst (input req_s2, input clk);\n"
                       "  modport src (output req);\n"
                       "endinterface\n"
                       "module leaf(sync_if.dst s, output logic o); always_ff @(posedge s.clk) o <= s.req_s2; endmodule\n"
                       "module mid(sync_if.dst s, output logic o); leaf u (.s(s), .o(o)); endmodule\n"
                       "module drv(input logic clk, sync_if.src s, input logic d); always_ff @(posedge clk) s.req <= d; endmodule\n"
                       "module top(input logic clk_a, input logic clk_b, input logic d, output logic o1, output logic o2);\n"
                       "  sync_if x (clk_b);\n"
                       "  sync_if y (clk_b);\n"
                       "  drv dx (.clk(clk_a), .s(x), .d(d));\n"
                       "  drv dy (.clk(clk_a), .s(y), .d(~d));\n"
                       "  mid m1 (.s(x), .o(o1));\n"
                       "  mid m2 (.s(y), .o(o2));\n"
                       "endmodule\n");
    REQUIRE(run->r->cdc);
    CHECK(run->r->design->bodies.size() == 4); // drv, leaf, mid, top: one body per module
    for (const char* src : {"top.dx.s.req", "top.dy.s.req"}) {
        const std::string inst = std::string(src).substr(4, 2) == "dx" ? "x" : "y";
        const auto* p = run->path(src, "top." + inst + ".req_s1");
        REQUIRE(p);
        CHECK(p->syncType == SyncType::ThreeFlop); // req_s1, req_s2 in the interface, o in leaf
        CHECK(p->status == AnalysisStatus::Safe);
    }
}

TEST_CASE("SV interfaces: generic interface ports and interface arrays", "[sv][interface]") {
    auto run = analyze("interface bus_if (input logic clk); logic v; endinterface\n"
                       "module gen(interface g, input logic d); always_ff @(posedge g.clk) g.v <= d; endmodule\n"
                       "module arr(bus_if b[2], output logic o);\n"
                       "  logic s; always_ff @(posedge b[1].clk) begin s <= b[0].v; o <= s; end\n"
                       "endmodule\n"
                       "module top(input logic clk_a, input logic clk_b, input logic d, output logic o);\n"
                       "  bus_if ia[2] ();\n"
                       "  assign ia[0].clk = clk_a;\n"
                       "  assign ia[1].clk = clk_b;\n"
                       "  gen g (.g(ia[0]), .d(d));\n" // direction from use: g.v is written
                       "  arr a (.b(ia), .o(o));\n"
                       "endmodule\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("IR-UNSUPPORTED-PORT") == 0);
    CHECK(run->has("output var g.v"));
    CHECK(run->has("input var b[0].v"));
    const auto* p = run->path("top.g.g.v", "top.a.s");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::TwoFlop);
}

TEST_CASE("SV interfaces negative: plain hierarchical references stay unsupported", "[sv][interface]") {
    auto run = analyze("module sub(input logic clk_a, input logic d); logic r; always_ff @(posedge clk_a) r <= d; endmodule\n"
                       "module top(input logic clk_a, input logic clk_b, input logic d, output logic q);\n"
                       "  sub s (.clk_a(clk_a), .d(d));\n"
                       "  always_ff @(posedge clk_b) q <= s.r;\n" // not through an interface
                       "endmodule\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("IR-UNSUPPORTED-EXPR") == 1);
    CHECK(run->r->cdc->paths.empty()); // the reference is opaque: no edge from s.r
}

TEST_CASE("SV: always blocks holding only assertions are not hardware", "[sv]") {
    auto run = analyze("module top(input logic clk_a, input logic clk_b, input logic a, output logic q);\n"
                       "  always_ff @(posedge clk_a) q <= a;\n"
                       "  if (1) begin : g_chk\n"
                       "    chk : assert property (@(posedge clk_a) a |-> q) else $warning(\"x\");\n"
                       "  end\n"
                       "  always begin : blk\n"
                       "    assert property (@(posedge clk_b) q |-> a);\n"
                       "  end\n"
                       "endmodule\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("IR-UNSUPPORTED-PROCESS") == 0);
}

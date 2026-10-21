#include "CdcTestUtil.h"

using namespace opencdc;
using namespace opencdc::cdc;
using namespace opencdc::test::cdc;

namespace {

constexpr const char* kTwoFlop = "module top(input clk_a, input clk_b, input d, output y);\n"
                                 "  reg a, s1, s2;\n"
                                 "  always @(posedge clk_a) a <= d;\n"
                                 "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; end\n"
                                 "  assign y = s2;\n"
                                 "endmodule\n";

} // namespace

// ---------------------------------------------------------------------------
// Sequential dependency graph
// ---------------------------------------------------------------------------

TEST_CASE("SeqGraph: direct, inverted, logic and select sources", "[cdc][seqgraph]") {
    auto b = test::buildDesign("module top(input clk, input s, input [3:0] i, output reg [3:0] q, output reg n,\n"
                               "           output reg m, output reg [1:0] w);\n"
                               "  reg [3:0] a; reg c;\n"
                               "  always @(posedge clk) begin\n"
                               "    a <= i; c <= s;\n"
                               "    q <= a;            // direct, bit by bit\n"
                               "    n <= ~c;           // inverted\n"
                               "    m <= a[0] & a[1];  // logic\n"
                               "    w <= c ? a[1:0] : a[3:2]; // select\n"
                               "  end\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    CollectingDiagnosticSink sink;
    SeqGraph sg(view, sink);
    auto regOf = [&](std::string_view name) {
        auto [inst, r] = test::findRegister(*b.design, name);
        return view.globalRegister(inst, r);
    };
    const auto a = SeqGraph::registerNode(regOf("top.a"));
    const auto c = SeqGraph::registerNode(regOf("top.c"));

    auto q = sg.pinBits(regOf("top.q"), Pin::Data);
    REQUIRE(q.size() == 4);
    CHECK(q[2].directNode == a);
    CHECK(q[2].directBit == 2);
    CHECK_FALSE(q[2].directInverted);

    auto n = sg.pinBits(regOf("top.n"), Pin::Data);
    CHECK(n[0].directNode == c);
    CHECK(n[0].directInverted);

    auto m = sg.pinBits(regOf("top.m"), Pin::Data);
    CHECK_FALSE(m[0].isDirect());
    ConeEntry scratch;
    auto mc = sg.entries(m[0], scratch);
    REQUIRE(mc.size() == 1);
    CHECK(mc[0].node == a);
    CHECK(mc[0].lo == 0);
    CHECK(mc[0].hi == 1);
    CHECK((mc[0].flags & ConeEntry::kLogic) != 0);

    auto w = sg.pinBits(regOf("top.w"), Pin::Data);
    ConeEntry scratch2;
    auto wc = sg.entries(w[0], scratch2);
    REQUIRE(wc.size() == 2);
    const auto& sel = wc[0].node == c ? wc[0] : wc[1];
    CHECK(sel.node == c);
    CHECK((sel.flags & ConeEntry::kSelect) != 0);

    auto consumers = sg.consumersOf({c});
    // n (direct, inverted), and w[0..1] merged into one record: both bits
    // read c the same way through the mux select.
    REQUIRE(consumers[c].size() == 2);
    const auto& wRec = consumers[c][0].reg == regOf("top.w") ? consumers[c][0] : consumers[c][1];
    CHECK(wRec.bit == 0);
    CHECK(wRec.bitEnd == 1);
    CHECK_FALSE(wRec.direct);
}

TEST_CASE("SeqGraph cuts combinational loops", "[cdc][seqgraph]") {
    auto b = test::buildDesign("module top(input clk, input i, output reg q);\n"
                               "  wire x, y;\n"
                               "  assign x = y ^ i;\n"
                               "  assign y = x & i;\n"
                               "  always @(posedge clk) q <= x;\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    CollectingDiagnosticSink sink;
    SeqGraph sg(view, sink);
    CHECK(sg.combLoops() >= 1);
    CHECK(std::ranges::any_of(sink.diagnostics(), [](const Diagnostic& d) { return d.code == "GR-COMB-LOOP"; }));
}

// ---------------------------------------------------------------------------
// Crossing detection
// ---------------------------------------------------------------------------

TEST_CASE("No crossing within one clock, half-cycle or synchronous clocks", "[cdc][crossing]") {
    auto same = analyze("module top(input clk_a, input d, output reg q2);\n"
                        "  reg q1;\n"
                        "  always @(posedge clk_a) q1 <= d;\n"
                        "  always @(negedge clk_a) q2 <= q1;\n"
                        "endmodule\n",
                        "");
    CHECK(same.cdc->paths.empty());

    auto derived = analyze("module top(input clk_a, input d, output reg q);\n"
                           "  reg div = 1'b0; reg a;\n"
                           "  always @(posedge clk_a) begin div <= ~div; a <= d; end\n"
                           "  always @(posedge div) q <= a;\n"
                           "endmodule\n",
                           "");
    CHECK(derived.cdc->paths.empty());

    auto exclusive = analyze(kTwoFlop, "set_clock_groups -physically_exclusive -group clk_a -group clk_b\n");
    CHECK(exclusive.cdc->paths.empty());
}

TEST_CASE("Crossings are found across the hierarchy with the full path", "[cdc][crossing]") {
    auto r = analyze("module src(input clk, input d, output reg q); always @(posedge clk) q <= d; endmodule\n"
                     "module dst(input clk, input d, input e, output reg q); always @(posedge clk) q <= d & e; endmodule\n"
                     "module top(input clk_a, input clk_b, input d, input e, output y);\n"
                     "  wire w;\n"
                     "  src u_s (.clk(clk_a), .d(d), .q(w));\n"
                     "  dst u_d (.clk(clk_b), .d(w), .e(e), .q(y));\n"
                     "endmodule\n");
    const auto& p = r.path("top.u_s.q", "top.u_d.q");
    CHECK(p.srcClockName == "clk_a");
    CHECK(p.dstClockName == "clk_b");
    CHECK(p.relation == clock::ClockRelation::Asynchronous);
    CHECK(p.throughLogic);
    CHECK(p.crossingSignal == "top.u_d.d");
    REQUIRE(p.steps.size() >= 4);
    CHECK(p.steps.front().name == "top.u_s.q");
    CHECK(p.steps.back().name == "top.u_d.q");
    CHECK(std::ranges::any_of(p.steps, [](const PathStep& s) { return s.name == "top.w"; }));
    CHECK(std::ranges::any_of(p.steps, [](const PathStep& s) { return s.kind == "logic:and"; }));
    CHECK(p.fingerprint.size() == 16);
}

TEST_CASE("Unclocked inputs are excluded unless set_input_delay assigns a clock", "[cdc][crossing]") {
    constexpr const char* v = "module top(input clk_a, input clk_b, input async_in, output y);\n"
                              "  reg s1;\n"
                              "  always @(posedge clk_b) s1 <= async_in;\n"
                              "  assign y = s1;\n"
                              "endmodule\n";
    auto none = analyze(v);
    CHECK(none.cdc->paths.empty());
    CHECK(none.cdc->stats.excludedNoDomainSources == 1);
    CHECK(none.countCode("CDC-NO-DOMAIN") == 1);

    // clk_a clocks no register, so it must be created explicitly to be referenced.
    auto clocked = analyze(v, "create_clock -name clk_a -period 10 [get_ports clk_a]\n" + std::string(kAsync) +
                                  "set_input_delay -clock clk_a 1.0 [get_ports async_in]\n");
    const auto& p = clocked.path("top.async_in", "top.s1");
    CHECK(p.srcClockName == "clk_a");
}

TEST_CASE("Asynchronous reset crossings become reset paths (stage 5)", "[cdc][crossing]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, output reg q);\n"
                     "  reg rst_a;\n"
                     "  always @(posedge clk_a) rst_a <= d;\n"
                     "  always @(posedge clk_b or posedge rst_a) if (rst_a) q <= 1'b0; else q <= d;\n"
                     "endmodule\n");
    REQUIRE(r.cdc->paths.size() == 1);
    const auto& p = r.path("top.rst_a", "top.q");
    CHECK(p.pin == Pin::AsyncReset);
    CHECK(r.cdc->stats.asyncResetCrossings == 1);
    CHECK(hasHit(p, "CDC-006"));
    CHECK_FALSE(hasHit(p, "CDC-001")); // reset pins are judged by CDC-006 only
}

// ---------------------------------------------------------------------------
// Synchronizers
// ---------------------------------------------------------------------------

TEST_CASE("Two-flop synchronizer is recognized and verified", "[cdc][sync]") {
    auto r = analyze(kTwoFlop);
    REQUIRE(r.cdc->paths.size() == 1);
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(p.syncStages == 2);
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(std::ranges::all_of(p.conditions, [](const Condition& c) { return c.satisfied; }));
    CHECK(p.syncRegisters == std::vector<std::string>{"top.s1", "top.s2"});
    CHECK(p.status == AnalysisStatus::Safe);
    CHECK(p.hits.empty());
    CHECK(r.cdc->stats.synchronizers == 1);
}

TEST_CASE("Three-flop chain inside a synchronizer module, ASYNC_REG noted", "[cdc][sync]") {
    auto r = analyze("module sync3(input clk, input d, output q);\n"
                     "  (* ASYNC_REG = \"TRUE\" *) reg [2:0] s;\n"
                     "  always @(posedge clk) s <= {s[1:0], d};\n"
                     "  assign q = s[2];\n"
                     "endmodule\n"
                     "module top(input clk_a, input clk_b, input d, output y);\n"
                     "  reg a;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  sync3 u (.clk(clk_b), .d(a), .q(y));\n"
                     "endmodule\n");
    // The whole vector is one register; the chain runs through its bits.
    const auto& p = r.path("top.a", "top.u.s");
    CHECK(p.syncType == SyncType::ThreeFlop);
    CHECK(p.syncRegisters == std::vector<std::string>{"top.u.s[0]", "top.u.s[1]", "top.u.s[2]"});
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(std::ranges::any_of(p.notes, [](const std::string& n) { return n.find("ASYNC_REG") != std::string::npos &&
                                                                         n.find("all stages") != std::string::npos; }));
}

TEST_CASE("Chain with logic before the first stage is structure-recognized (CDC-004)", "[cdc][sync][CDC-004]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output y);\n"
                     "  reg a, c, s1, s2;\n"
                     "  always @(posedge clk_a) begin a <= d; c <= e; end\n"
                     "  always @(posedge clk_b) begin s1 <= a | c; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(p.syncStatus == SyncStatus::StructureRecognized);
    CHECK_FALSE(p.conditions.front().satisfied);
    CHECK(p.status == AnalysisStatus::Violation);
    REQUIRE(p.hits.size() == 1);
    CHECK(p.hits.front().ruleId == "CDC-004");
}

TEST_CASE("Multi-bit value synchronized bit by bit leaves the coherency condition unverified", "[cdc][sync]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [3:0] d, output [3:0] y);\n"
                     "  reg [3:0] g, s1, s2;\n"
                     "  always @(posedge clk_a) g <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= g; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.g", "top.s1");
    CHECK(p.width == 4);
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(p.syncStatus == SyncStatus::ProtocolUnverified);
    REQUIRE(p.unverified.size() == 1);
    CHECK(p.unverified[0].find("Gray") != std::string::npos);
    CHECK(r.hits("CDC-003") == 0);
}

TEST_CASE("Data captured under a synchronized enable is qualified data", "[cdc][sync][CDC-003]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [7:0] d, input v, output reg [7:0] q);\n"
                     "  reg [7:0] data; reg valid, v1, v2;\n"
                     "  always @(posedge clk_a) begin data <= d; valid <= v; end\n"
                     "  always @(posedge clk_b) begin v1 <= valid; v2 <= v1; if (v2) q <= data; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.data", "top.q");
    CHECK(p.syncType == SyncType::QualifiedData);
    CHECK(p.syncStatus == SyncStatus::ProtocolUnverified);
    CHECK(p.syncRegisters == std::vector<std::string>{"top.v2"});
    CHECK(p.status == AnalysisStatus::Safe);
    CHECK(r.path("top.valid", "top.v1").syncType == SyncType::TwoFlop);
    CHECK(r.hits("CDC-003") == 0);
}

TEST_CASE("Qualifier through one registered level of control logic", "[cdc][sync]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [7:0] d, input v, output reg [7:0] q);\n"
                     "  reg [7:0] data; reg valid, v1, v2, v3, load;\n"
                     "  always @(posedge clk_a) begin data <= d; valid <= v; end\n"
                     "  always @(posedge clk_b) begin\n"
                     "    v1 <= valid; v2 <= v1; v3 <= v2;\n"
                     "    load <= v2 & ~v3;           // edge detect, registered\n"
                     "    q <= load ? data : q;\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(r.path("top.data", "top.q").syncType == SyncType::QualifiedData);
}

// ---------------------------------------------------------------------------
// Rules: positive and negative examples
// ---------------------------------------------------------------------------

TEST_CASE("CDC-001 positive: single bit through logic", "[CDC-001]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output reg q);\n"
                     "  reg a;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) q <= a & e;\n"
                     "endmodule\n");
    const auto& p = r.path("top.a", "top.q");
    CHECK(p.syncType == SyncType::None);
    REQUIRE(hasHit(p, "CDC-001"));
    CHECK(p.hits.front().severity == Severity::Error);
    CHECK(p.status == AnalysisStatus::Violation);
    CHECK(p.cls == CrossingClass::SingleBitData);
}

TEST_CASE("CDC-001 positive: crossing into a clock enable", "[CDC-001]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, output reg [3:0] cnt);\n"
                     "  reg go;\n"
                     "  always @(posedge clk_a) go <= d;\n"
                     "  always @(posedge clk_b) if (go) cnt <= cnt + 4'd1;\n"
                     "endmodule\n");
    const auto& p = r.path("top.go", "top.cnt");
    CHECK(p.pin == Pin::Enable);
    CHECK(hasHit(p, "CDC-001"));
    CHECK(p.cls == CrossingClass::SingleBitControl);
}

TEST_CASE("CDC-001 negative: synchronized or related clocks", "[CDC-001]") {
    CHECK(analyze(kTwoFlop).hits("CDC-001") == 0);
    auto sync = analyze("module top(input clk_a, input clk_b, input d, input e, output reg q);\n"
                        "  reg a;\n"
                        "  always @(posedge clk_a) a <= d;\n"
                        "  always @(posedge clk_b) q <= a & e;\n"
                        "endmodule\n",
                        "create_clock -name A -period 10 [get_ports clk_a]\n"
                        "create_generated_clock -name B -source [get_ports clk_a] -divide_by 2 [get_ports clk_b]\n");
    CHECK(sync.cdc->paths.empty());
}

TEST_CASE("CDC-002 positive: single flop used directly", "[CDC-002]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output reg y);\n"
                     "  reg a, s1;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a; y <= s1 ^ e; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.syncType == SyncType::SingleFlop);
    REQUIRE(hasHit(p, "CDC-002"));
    CHECK_FALSE(hasHit(p, "CDC-001"));
    CHECK(p.hits.front().severity == Severity::Warning);
}

TEST_CASE("CDC-002 positive: first synchronizer stage tapped", "[CDC-002]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output y, output reg z);\n"
                     "  reg a, s1, s2;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; z <= s1 & e; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(p.syncStatus == SyncStatus::StructureRecognized);
    CHECK(hasHit(p, "CDC-002"));
}

TEST_CASE("CDC-002 negative and minSyncStages option", "[CDC-002]") {
    CHECK(analyze(kTwoFlop).hits("CDC-002") == 0);
    CdcOptions three;
    three.minSyncStages = 3;
    // With a minimum of three stages, a clean two-flop chain is reported.
    auto r = analyze(kTwoFlop, kAsync, three);
    CHECK(r.hits("CDC-002") == 1);
    CdcOptions one;
    one.minSyncStages = 1;
    auto single = analyze("module top(input clk_a, input clk_b, input d, output reg y);\n"
                          "  reg a, s1;\n"
                          "  always @(posedge clk_a) a <= d;\n"
                          "  always @(posedge clk_b) begin s1 <= a; y <= s1; end\n"
                          "endmodule\n",
                          kAsync, one);
    // s1 -> y is a two-flop chain; nothing to report either way.
    CHECK(single.hits("CDC-002") == 0);
}

TEST_CASE("CDC-003 positive: bus captured directly", "[CDC-003]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [7:0] d, output reg [7:0] q, output reg [7:0] r2);\n"
                     "  reg [7:0] bus;\n"
                     "  always @(posedge clk_a) bus <= d;\n"
                     "  always @(posedge clk_b) begin q <= bus; r2 <= q; end\n"
                     "endmodule\n");
    // q -> r2 makes every bit look like a two-flop chain, but q is used as a
    // value: bits synchronized independently are reported later by CDC-008.
    const auto& p = r.path("top.bus", "top.q");
    CHECK(p.width == 8);
    CHECK(p.syncStatus == SyncStatus::ProtocolUnverified);

    auto direct = analyze("module top(input clk_a, input clk_b, input [7:0] d, input [7:0] e, output reg [7:0] q);\n"
                          "  reg [7:0] bus;\n"
                          "  always @(posedge clk_a) bus <= d;\n"
                          "  always @(posedge clk_b) q <= bus + e;\n"
                          "endmodule\n");
    const auto& pd = direct.path("top.bus", "top.q");
    REQUIRE(hasHit(pd, "CDC-003"));
    CHECK(pd.cls == CrossingClass::MultiBitData);
    CHECK(pd.hits.front().severity == Severity::Error);
}

TEST_CASE("CDC-003 positive: memory read in another domain", "[CDC-003]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [1:0] wa, input [1:0] ra, input [7:0] wd,\n"
                     "           output reg [7:0] rd);\n"
                     "  reg [7:0] mem [0:3];\n"
                     "  always @(posedge clk_a) mem[wa] <= wd;\n"
                     "  always @(posedge clk_b) rd <= mem[ra];\n"
                     "endmodule\n");
    const auto& p = r.path("top.mem", "top.rd");
    CHECK(hasHit(p, "CDC-003"));
    CHECK(p.width == 8); // one element per read, not the whole array
}

TEST_CASE("A reset synchronizer is not a data qualifier", "[CDC-003][sync]") {
    auto r = analyze("module top(input clk_a, input clk_b, input rst_a, input [7:0] d, output reg [7:0] q);\n"
                     "  reg [7:0] data; reg r1, r2, r3;\n"
                     "  always @(posedge clk_a) data <= d;\n"
                     "  always @(posedge clk_a or posedge rst_a) if (rst_a) r1 <= 1'b1; else r1 <= 1'b0;\n"
                     "  always @(posedge clk_b) begin r2 <= r1; r3 <= r2; end\n"
                     "  always @(posedge clk_b) if (r3) q <= 8'd0; else q <= data;\n"
                     "endmodule\n");
    CHECK(r.path("top.r1", "top.r2").syncType == SyncType::TwoFlop);
    const auto& p = r.path("top.data", "top.q");
    CHECK(p.syncType != SyncType::QualifiedData);
    CHECK(hasHit(p, "CDC-003"));
}

TEST_CASE("CDC-003 negative: qualified data", "[CDC-003]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [7:0] d, input v, output reg [7:0] q);\n"
                     "  reg [7:0] data; reg valid, v1, v2;\n"
                     "  always @(posedge clk_a) begin data <= d; valid <= v; end\n"
                     "  always @(posedge clk_b) begin v1 <= valid; v2 <= v1; if (v2) q <= data; end\n"
                     "endmodule\n");
    CHECK(r.hits("CDC-003") == 0);
}

TEST_CASE("CDC-010 positive: unconstrained clocks; other hits need review", "[CDC-010]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output reg q);\n"
                     "  reg a;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) q <= a & e;\n"
                     "endmodule\n",
                     "");
    const auto& p = r.path("top.a", "top.q");
    CHECK(p.relation == clock::ClockRelation::Unknown);
    REQUIRE(hasHit(p, "CDC-010"));
    REQUIRE(hasHit(p, "CDC-001"));
    for (const auto& h : p.hits)
        CHECK(h.status == AnalysisStatus::NeedsReview);
    CHECK(p.status == AnalysisStatus::NeedsReview);
}

TEST_CASE("CDC-010 negative: constrained, assume_async, assume_sync", "[CDC-010]") {
    CHECK(analyze(kTwoFlop).hits("CDC-010") == 0);

    CdcOptions async;
    async.unknownRelation = UnknownRelationPolicy::AssumeAsync;
    auto a = analyze("module top(input clk_a, input clk_b, input d, input e, output reg q);\n"
                     "  reg x;\n"
                     "  always @(posedge clk_a) x <= d;\n"
                     "  always @(posedge clk_b) q <= x & e;\n"
                     "endmodule\n",
                     "", async);
    CHECK(a.hits("CDC-010") == 0);
    CHECK(a.path("top.x", "top.q").status == AnalysisStatus::Violation);

    CdcOptions sync;
    sync.unknownRelation = UnknownRelationPolicy::AssumeSync;
    CHECK(analyze(kTwoFlop, "", sync).cdc->paths.empty());
}

// ---------------------------------------------------------------------------
// Examples (designs with known, hand-placed crossings)
// ---------------------------------------------------------------------------


TEST_CASE("Smoke example: valid synchronized, data qualified, status bit by bit", "[cdc][example]") {
    const auto dir = test::examplesDir() / "smoke";
    auto r = analyzeExample("smoke", {"rtl/sync_2ff.v", "rtl/tx_domain.v", "rtl/rx_domain.v", "rtl/top.v"}, "top",
                            "constraints/clocks.sdc", {dir / "rtl" / "include"});
    CHECK(r.cdc->paths.size() == 7);
    CHECK(r.path("top.u_tx.valid", "top.u_rx.u_valid_sync.meta").syncType == SyncType::TwoFlop);
    CHECK(r.path("top.u_tx.data", "top.u_rx.data_out").syncType == SyncType::QualifiedData);
    for (int i = 0; i < 4; ++i) {
        const auto& p = r.path("top.u_tx.status", "top.u_rx.g_status[" + std::to_string(i) + "].u_sync.meta");
        CHECK(p.syncType == SyncType::TwoFlop);
        CHECK(p.splitWith.size() == 3);
        // status[3:0] is split over four synchronizers (CDC-008); the bits only
        // leave the design through a port, so they are not read together inside it.
        CHECK(hasHit(p, "CDC-008"));
        CHECK(p.status == AnalysisStatus::NeedsReview);
    }
    // rst_b_n is an input without clock domain resetting 11 clk_b registers.
    const auto& rst = r.path("top.rst_b_n", "top.u_rx.data_out");
    CHECK(rst.pin == Pin::AsyncReset);
    CHECK(rst.groupSize == 11);
    CHECK(rst.srcUnclocked);
    CHECK(hasHit(rst, "CDC-006"));
    CHECK(rst.status == AnalysisStatus::PotentialRisk);
    CHECK(r.hits("CDC-001") + r.hits("CDC-002") + r.hits("CDC-003") + r.hits("CDC-010") == 0);
}

TEST_CASE("Async FIFO example: Gray pointers recognized and verified", "[cdc][example][gray]") {
    auto r = analyzeExample("async_fifo",
                            {"rtl/fifomem.v", "rtl/sync_r2w.v", "rtl/sync_w2r.v", "rtl/rptr_empty.v", "rtl/wptr_full.v",
                             "rtl/afifo.v"},
                            "afifo", "constraints/afifo.xdc");
    REQUIRE(r.cdc->paths.size() == 4); // two Gray pointers, two external resets
    for (const auto& [src, dst] : {std::pair{"afifo.u_wptr_full.wptr", "afifo.u_sync_w2r.rq1_wptr"},
                                   std::pair{"afifo.u_rptr_empty.rptr", "afifo.u_sync_r2w.wq1_rptr"}}) {
        const auto& p = r.path(src, dst);
        CHECK(p.width == 5);
        CHECK(p.syncType == SyncType::Gray);
        CHECK(p.syncStages == 2);
        // bin + (inc & ~full): the increment is proven to be at most one.
        CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
        CHECK(p.unverified.empty());
        CHECK(p.hits.empty());
    }
    CHECK(r.hits("CDC-006") == 2); // wrst_n / rrst_n: inputs without clock domain
    CHECK(r.hits("CDC-008") == 0);
}

TEST_CASE("cdc_rules example: every hand-placed crossing is classified", "[cdc][example]") {
    auto r = analyzeExample("cdc_rules", {"rtl/cdc_rules.v"}, "cdc_rules", "constraints/cdc_rules.sdc");
    // See examples/cdc_rules/README.md for the intended result of each case.
    CHECK(hasHit(r.path("cdc_rules.a_flag", "cdc_rules.b_flag_and"), "CDC-001"));
    CHECK(hasHit(r.path("cdc_rules.a_go", "cdc_rules.b_cnt"), "CDC-001"));
    CHECK(hasHit(r.path("cdc_rules.a_lvl", "cdc_rules.b_lvl_s1"), "CDC-002"));
    CHECK(hasHit(r.path("cdc_rules.a_tap", "cdc_rules.b_tap_s1"), "CDC-002"));
    CHECK(hasHit(r.path("cdc_rules.a_bus", "cdc_rules.b_bus_sum"), "CDC-003"));
    CHECK(r.path("cdc_rules.a_req", "cdc_rules.u_req_sync.meta").syncType == SyncType::TwoFlop);
    CHECK(r.path("cdc_rules.a_x", "cdc_rules.b_x_s1").syncType == SyncType::ThreeFlop);
    CHECK(r.path("cdc_rules.a_data", "cdc_rules.b_data").syncType == SyncType::QualifiedData);
    CHECK(r.path("cdc_rules.a_p", "cdc_rules.b_or_s1").syncStatus == SyncStatus::StructureRecognized);
    CHECK(hasHit(r.path("cdc_rules.a_p", "cdc_rules.b_or_s1"), "CDC-004"));
    CHECK(hasHit(r.path("cdc_rules.a_q", "cdc_rules.b_or_s1"), "CDC-004"));
    const auto& u = r.path("cdc_rules.a_tog", "cdc_rules.c_s1");
    CHECK(hasHit(u, "CDC-010"));
    CHECK(u.syncType == SyncType::Toggle);
    CHECK(r.path("cdc_rules.a_valid", "cdc_rules.u_valid_sync.meta").syncType == SyncType::Toggle);
    // dout XORs b_data (captured under the synchronized a_valid) into every
    // bit, together with five other synchronizer outputs (README, case 11).
    for (const auto& [src, dst] :
         {std::pair{"cdc_rules.a_tap", "cdc_rules.b_tap_s1"}, std::pair{"cdc_rules.a_x", "cdc_rules.b_x_s1"},
          std::pair{"cdc_rules.a_p", "cdc_rules.b_or_s1"}, std::pair{"cdc_rules.a_q", "cdc_rules.b_or_s1"},
          std::pair{"cdc_rules.a_req", "cdc_rules.u_req_sync.meta"}})
        CHECK(hasHit(r.path(src, dst), "CDC-005"));
    const auto& valid = r.path("cdc_rules.a_valid", "cdc_rules.u_valid_sync.meta");
    REQUIRE(valid.reconvergences.size() == 1);
    CHECK(valid.reconvergences[0].at == "cdc_rules.dout");
    CHECK(valid.reconvergences[0].paths.size() == 5);
    CHECK(r.hits("CDC-001") == 2);
    CHECK(r.hits("CDC-002") == 2);
    CHECK(r.hits("CDC-003") == 1);
    CHECK(r.hits("CDC-004") == 2);
    CHECK(r.hits("CDC-005") == 6);
    CHECK(r.hits("CDC-010") == 1);
}

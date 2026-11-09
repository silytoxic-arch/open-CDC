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

TEST_CASE("SeqGraph: consumers of a wide array register are merged runs", "[cdc][seqgraph]") {
    // a RAM's array register: its data bits share the sources of the wide write logic;
    // consumersOf() takes such a run in one pass (Atari800_MiSTer: 9M register bits)
    auto b = test::buildDesign("module top(input clk, input [1:0] a, input [7:0] d, input we, output reg [7:0] q,\n"
                               "           output reg [3:0] t);\n"
                               "  reg [7:0] src_d, src_a; reg src_we;\n"
                               "  always @(posedge clk) begin src_d <= d; src_a <= {6'b0, a}; src_we <= we; end\n"
                               "  reg [7:0] mem [0:63];\n"
                               "  always @(posedge clk) begin if (src_we) mem[src_a] <= src_d; q <= mem[a]; end\n"
                               // direct bits next to each other: one record per bit
                               "  always @(posedge clk) t <= src_d[3:0];\n"
                               "endmodule\n",
                               "top");
    graph::DesignView view(*b.design);
    CollectingDiagnosticSink sink;
    SeqGraph sg(view, sink);
    auto regOf = [&](std::string_view name) {
        auto [inst, r] = test::findRegister(*b.design, name);
        return view.globalRegister(inst, r);
    };
    const auto srcD = SeqGraph::registerNode(regOf("top.src_d"));
    auto consumers = sg.consumersOf({srcD});
    const auto& list = consumers[srcD];
    std::size_t memRecords = 0, memBits = 0, tRecords = 0;
    for (const auto& c : list) {
        if (c.reg == regOf("top.mem")) {
            ++memRecords;
            memBits += c.bitEnd - c.bit + 1;
            CHECK_FALSE(c.direct);
        }
        if (c.reg == regOf("top.t")) {
            ++tRecords;
            CHECK(c.direct);
            CHECK(c.bit == c.bitEnd);
        }
    }
    // all 512 bits of the array in one record (the same sources for every bit)
    CHECK(memRecords == 1);
    CHECK(memBits == 512);
    // negative: direct connections are never merged
    CHECK(tRecords == 4);
}

TEST_CASE("SeqGraph: registers that hold at most one set bit", "[cdc][seqgraph][CDC-005]") {
    auto b = test::buildDesign(
        "module top(input clk, input clk2, input [1:0] op, input [1:0] raw, input [1:0] idx, output [76:0] y);\n"
        "  reg [3:0] fsm = 4'b0001;  // constants and hold\n"
        "  always @(posedge clk) case (op) 2'd1: fsm <= 4'b0010; 2'd2: fsm <= 4'b0100; default: ; endcase\n"
        "  reg [3:0] ring = 4'b0001; // rotation\n"
        "  always @(posedge clk) if (op[0]) ring <= {ring[2:0], ring[3]};\n"
        "  reg [3:0] shift = 4'b1000; // shift with zero fill\n"
        "  always @(posedge clk) shift <= op[1] ? 4'b1000 : {1'b0, shift[3:1]};\n"
        "  reg [3:0] dec = 0;         // decoder\n"
        "  always @(posedge clk) dec <= 4'd1 << idx;\n"
        "  reg [3:0] dec2 = 0;        // arithmetic shift of a positive value\n"
        "  always @(posedge clk) dec2 <= $signed(4'sb0100) >>> idx;\n"
        "  reg [3:0] ins = 0; reg [3:0] n;  // zeros with one bit set\n"
        "  always @* begin n = 4'b0000; n[idx] = 1'b1; end\n"
        "  always @(posedge clk) ins <= n;\n"
        "  reg [3:0] cp = 0;          // copy of a one-hot register, same clock\n"
        "  always @(posedge clk) cp <= ring;\n"
        "  reg [3:0] ring2 = 4'b0001; // ring through a second register\n"
        "  reg [3:0] mid = 4'b0000;\n"
        "  always @(posedge clk) begin mid <= ring2; ring2 <= {mid[2:0], mid[3]}; end\n"
        // not one-hot
        "  reg [3:0] data = 0;        // input data\n"
        "  always @(posedge clk) data <= {raw, raw};\n"
        "  reg [3:0] two = 4'b0001;   // a constant with two bits set\n"
        "  always @(posedge clk) if (op[0]) two <= 4'b0011;\n"
        "  reg [3:0] dup = 4'b0001;   // a bit used twice\n"
        "  always @(posedge clk) dup <= {dup[0], dup[0], 2'b00};\n"
        "  reg [3:0] fill = 4'b0001;  // shift filling with ones\n"
        "  always @(posedge clk) fill <= {1'b1, fill[3:1]};\n"
        "  reg [3:0] decw = 0;        // decoding a value with two bits set\n"
        "  always @(posedge clk) decw <= 4'd3 << idx;\n"
        "  reg [3:0] neg = 0;         // arithmetic shift of a negative value\n"
        "  always @(posedge clk) neg <= $signed(4'sb1000) >>> idx;\n"
        "  reg [3:0] ins1 = 0; reg [3:0] m;  // one bit set on top of a set bit\n"
        "  always @* begin m = 4'b0001; m[idx] = 1'b1; end\n"
        "  always @(posedge clk) ins1 <= m;\n"
        "  reg [3:0] oc = 0;          // copy of a one-hot register of another clock\n"
        "  always @(posedge clk2) oc <= ring;\n"
        "  reg [3:0] rst2 = 4'b0011;  // power-up value with two bits set\n"
        "  always @(posedge clk) rst2 <= op[0] ? 4'b0001 : rst2;\n"
        "  reg bit1 = 0;              // a single bit is not a one-hot vector\n"
        "  always @(posedge clk) bit1 <= op[0];\n"
        "  assign y = {fsm, ring, shift, dec, dec2, ins, cp, ring2, mid, data, two, dup, fill, decw, neg, ins1, oc,\n"
        "              rst2, bit1};\n"
        "endmodule\n",
        "top");
    graph::DesignView view(*b.design);
    CollectingDiagnosticSink sink;
    SeqGraph sg(view, sink);
    auto oneHot = [&](std::string_view name) {
        auto [inst, r] = test::findRegister(*b.design, name);
        return sg.atMostOneHot(view.globalRegister(inst, r));
    };
    for (const char* name : {"top.fsm", "top.ring", "top.shift", "top.dec", "top.dec2", "top.ins", "top.cp",
                             "top.ring2", "top.mid"}) {
        INFO(name);
        CHECK(oneHot(name));
    }
    for (const char* name : {"top.data", "top.two", "top.dup", "top.fill", "top.decw", "top.neg", "top.ins1",
                             "top.oc", "top.rst2", "top.bit1"}) {
        INFO(name);
        CHECK_FALSE(oneHot(name));
    }
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

    // the same with -clock [get_clocks -of_objects [get_ports clk_a]]
    auto of = analyze(v, "create_clock -name clk_a -period 10 [get_ports clk_a]\n" + std::string(kAsync) +
                             "set_input_delay -clock [get_clocks -of_objects [get_ports clk_a]] 1.0 "
                             "[get_ports async_in]\n");
    CHECK(of.path("top.async_in", "top.s1").srcClockName == "clk_a");
    CHECK(of.countCode("CON-UNMATCHED") == 0);
    // negative: a net without a clock assigns no domain
    auto bad = analyze(v, "create_clock -name clk_a -period 10 [get_ports clk_a]\n" + std::string(kAsync) +
                              "set_input_delay -clock [get_clocks -of_objects [get_nets y]] 1.0 [get_ports async_in]\n");
    CHECK(bad.cdc->paths.empty());
    CHECK(bad.countCode("CON-UNMATCHED") == 1);
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

TEST_CASE("Source data decoded under a synchronized qualifier", "[cdc][sync]") {
    // microwatt DMI: the address from the JTAG domain is decoded in the system
    // domain only while the synchronized request is high
    auto r = analyze("module top(input clk_a, input clk_b, input [3:0] a, input v, input [7:0] x,\n"
                     "           output reg [7:0] q, output reg req, output reg q2, output reg q3);\n"
                     "  reg [3:0] addr; reg valid, v1, v2;\n"
                     "  always @(posedge clk_a) begin addr <= a; valid <= v; end\n"
                     "  always @(posedge clk_b) begin\n"
                     "    v1 <= valid; v2 <= v1;\n"
                     "    if (v2 && addr == 4'd3) q <= x;        // enable from the source, qualified\n"
                     "    req <= (addr == 4'd5) ? v2 : 1'b0;     // the source selects the qualifier\n"
                     "    q2 <= (addr == 4'd6) ? x[0] : 1'b0;    // no qualifier\n"
                     "    q3 <= addr[0] ^ v2;                    // source as data next to the qualifier\n"
                     "  end\n"
                     "endmodule\n");
    const auto& e = r.path("top.addr", "top.q");
    CHECK(e.pin == Pin::Enable);
    CHECK(e.syncType == SyncType::QualifiedData);
    CHECK(e.syncRegisters == std::vector<std::string>{"top.v2"});
    CHECK(r.path("top.addr", "top.req").syncType == SyncType::QualifiedData);
    CHECK(r.path("top.addr", "top.q2").syncStatus == SyncStatus::NotSynchronized);
    CHECK(r.path("top.addr", "top.q3").syncStatus == SyncStatus::NotSynchronized);
}

TEST_CASE("Decoded source data under a qualifier is no flop chain behind logic", "[cdc][sync][CDC-004]") {
    // microwatt DMI write flag: we <= (op == WR) when the synchronized request
    // is high, then registered again; without the qualifier it stays CDC-004
    auto r = analyze("module top(input clk_a, input clk_b, input [1:0] o, input v, output reg w2, output reg u2,\n"
                     "           output reg x1, output reg x2, output reg x3, output reg x4, input le);\n"
                     "  reg [1:0] op; reg valid, v1, v2, w1, u1;\n"
                     "  always @(posedge clk_a) begin op <= o; valid <= v; end\n"
                     "  always @(posedge clk_b) begin\n"
                     "    v1 <= valid; v2 <= v1;\n"
                     "    w1 <= v2 ? (op == 2'd2) : 1'b0; w2 <= w1;\n"
                     "    u1 <= (op == 2'd1); u2 <= u1;\n"
                     "    x1 <= v2 & op[0];                // AND with the qualifier\n"
                     "    x2 <= v2 | op[0];                // OR does not block the source\n"
                     "    x3 <= op[1] & (le & v2);         // AND tree\n"
                     "    x4 <= op[1] & ~v2;               // inverted qualifier\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(r.path("top.op", "top.w1").syncType == SyncType::QualifiedData);
    CHECK(r.path("top.op", "top.x1").syncType == SyncType::QualifiedData);
    CHECK(r.path("top.op", "top.x2").syncStatus != SyncStatus::ProtocolUnverified);
    CHECK(r.path("top.op", "top.x3").syncType == SyncType::QualifiedData);
    CHECK(r.path("top.op", "top.x4").syncType == SyncType::QualifiedData);
    const auto& u = r.path("top.op", "top.u1");
    CHECK(u.syncType == SyncType::TwoFlop);
    CHECK(r.hits("CDC-004") == 1);
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

TEST_CASE("Register-level datapath-only max delays and false paths constrain single crossings", "[cdc][constraints]") {
    constexpr const char* v = "module top(input clk_a, input clk_b, input d, input e, output reg q1, output reg q2);\n"
                              "  reg a, b, s1, s2, t1, t2;\n"
                              "  always @(posedge clk_a) begin a <= d; b <= e; end\n"
                              "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; q1 <= s2; t1 <= b; t2 <= t1; q2 <= t2; end\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n";
    auto r = analyze(v, clocks + "set_max_delay -datapath_only -from [get_cells a_reg] -to [get_cells s1_reg] 10.0\n");
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.relation == clock::ClockRelation::Asynchronous);
    CHECK(std::ranges::any_of(p.notes, [](const std::string& n) { return n.find("set_max_delay -datapath_only") != std::string::npos; }));
    CHECK(p.status == AnalysisStatus::Safe);
    CHECK(r.path("top.b", "top.t1").relation == clock::ClockRelation::Unknown); // not covered
    CHECK(r.countCode("CON-UNSUPPORTED-COMMAND") == 0);

    auto fp = analyze(v, clocks + "set_false_path -from [get_cells {a_reg b_reg}] -to [get_cells -hier *1_reg]\n");
    CHECK(fp.path("top.a", "top.s1").relation == clock::ClockRelation::Asynchronous);
    CHECK(fp.path("top.b", "top.t1").relation == clock::ClockRelation::Asynchronous);
    // negative: a max delay that is not datapath-only constrains nothing for CDC
    auto plain = analyze(v, clocks + "set_max_delay -from [get_cells a_reg] -to [get_cells s1_reg] 10.0\n");
    CHECK(plain.path("top.a", "top.s1").relation == clock::ClockRelation::Unknown);
    // negative: pin queries with filters and -of_objects are not evaluated
    // (no warning, no exception)
    auto filt = analyze(v, clocks + "set_max_delay 2 -quiet -from [get_pins -filter {REF_PIN_NAME == C} -of_objects "
                                    "[get_cells -hier -filter {ars_ff1 == TRUE}]] -to [get_cells s1_reg]\n");
    CHECK(filt.path("top.a", "top.s1").relation == clock::ClockRelation::Unknown);
    CHECK(filt.countCode("CON-UNSUPPORTED-OPTION") == 0);
    CHECK(filt.countCode("CON-UNMATCHED") == 0);
    // negative: a cell filter on a property the design does not have selects
    // nothing (warned), so the exception constrains nothing
    auto unk = analyze(v, clocks + "set_false_path -from [get_cells -hier -filter {X == 1}] -to [get_cells s1_reg]\n");
    CHECK(unk.path("top.a", "top.s1").relation == clock::ClockRelation::Unknown);
    CHECK(unk.countCode("CON-UNSUPPORTED-OPTION") == 1);
    CHECK(unk.countCode("CON-UNMATCHED") == 1);
    // negative: an exception whose -to matches nothing warns
    auto none = analyze(v, clocks + "set_false_path -from [get_cells a_reg] -to [get_cells nothing_reg]\n");
    CHECK(none.countCode("CON-UNMATCHED") >= 1);
    CHECK(none.path("top.a", "top.s1").relation == clock::ClockRelation::Unknown);
}

TEST_CASE("Vendor Tcl constraint scripts: foreach over instances by REF_NAME, regexp, per-instance max delays", "[cdc][constraints]") {
    // the style of alexforencich's sync_reset.tcl / axis_async_fifo.tcl
    constexpr const char* v = "module sync2(input clk, input d, output q);\n"
                              "  reg s1, s2;\n"
                              "  always @(posedge clk) begin s1 <= d; s2 <= s1; end\n"
                              "  assign q = s2;\n"
                              "endmodule\n"
                              "module top(input clk_a, input clk_b, input d, input e, output q0, output q1);\n"
                              "  reg a, b;\n"
                              "  always @(posedge clk_a) begin a <= d; b <= e; end\n"
                              "  sync2 u_s0 (.clk(clk_b), .d(a), .q(q0));\n"
                              "  sync2 u_s1 (.clk(clk_b), .d(b), .q(q1));\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n";
    const std::string script =
        "foreach inst [get_cells -hier -filter {(ORIG_REF_NAME == %M || REF_NAME == %M)}] {\n"
        "    puts \"Inserting timing constraints for sync2 instance $inst\"\n"
        "    set read_clk [get_clocks -of_objects [get_cells \"$inst/s1_reg $inst/s2_reg\"]]\n"
        "    set read_clk_period [if {[llength $read_clk]} {get_property -min PERIOD $read_clk} {expr 1.0}]\n"
        "    set sync_ffs [get_cells -quiet -hier -regexp {.*/s[12]_reg} -filter \"PARENT == $inst\"]\n"
        "    if {[llength $sync_ffs]} {\n"
        "        set_property ASYNC_REG TRUE $sync_ffs\n"
        "        set_max_delay -from [get_cells -hier -regexp {[ab]_reg}] -to [get_cells \"$inst/s1_reg\"] -datapath_only $read_clk_period\n"
        "    }\n"
        "}\n";
    auto withModule = [&](const std::string& m) {
        std::string s = script;
        for (auto pos = s.find("%M"); pos != std::string::npos; pos = s.find("%M"))
            s.replace(pos, 2, m);
        return s;
    };
    auto r = analyze(v, clocks + withModule("sync2"));
    CHECK(r.countCode("CON-UNSUPPORTED-COMMAND") == 0);
    CHECK(r.countCode("CON-PARSE") == 0);
    CHECK(r.countCode("CON-UNSUPPORTED-OPTION") == 0);
    for (const auto* pair : {"a/u_s0.s1", "b/u_s1.s1"}) {
        const std::string pr(pair);
        const auto slash = pr.find('/');
        const auto& p = r.path("top." + pr.substr(0, slash), "top." + pr.substr(slash + 1));
        CHECK(p.relation == clock::ClockRelation::Asynchronous);
        CHECK(std::ranges::any_of(p.notes, [](const std::string& n) { return n.find("set_max_delay -datapath_only") != std::string::npos; }));
    }
    // negative: the script finds no instance of another module: nothing is
    // constrained; only the instance query (without -quiet) warns, as in Vivado
    auto none = analyze(v, clocks + withModule("axis_async_fifo"));
    CHECK(none.path("top.a", "top.u_s0.s1").relation == clock::ClockRelation::Unknown);
    CHECK(none.countSeverity("CON-UNMATCHED", Severity::Warning) == 1);
}

TEST_CASE("Path exceptions: reset source hunted through the netlist, reset pins alone, clock-to-register, -quiet", "[cdc][constraints]") {
    constexpr const char* v = "module top(input clk_a, input clk_b, input rst_a, input d, output q, output r);\n"
                              "  reg a, s1, s2, x1, x2;\n"
                              "  reg rs1, rs2;\n"
                              "  always @(posedge clk_a) begin a <= d; x1 <= d; end\n"
                              "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; x2 <= x1; end\n"
                              "  always @(posedge clk_b or posedge x1)\n"
                              "    if (x1) begin rs1 <= 1'b1; rs2 <= 1'b1; end else begin rs1 <= a; rs2 <= rs1; end\n"
                              "  assign q = s2 ^ x2;\n"
                              "  assign r = rs2;\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n";
    // the "hunt down source" idiom of axis_async_fifo.tcl
    auto hunt = analyze(v, clocks + "set dest [get_cells s1_reg]\n"
                                    "set dest_pins [get_pins -of_objects $dest -filter {REF_PIN_NAME == D}]\n"
                                    "set net [get_nets -segments -of_objects $dest_pins]\n"
                                    "set source_pins [get_pins -of_objects $net -filter {IS_LEAF && DIRECTION == OUT}]\n"
                                    "set source [get_cells -of_objects $source_pins]\n"
                                    "set_max_delay -from $source -to $dest -datapath_only 7.0\n");
    CHECK(hunt.countCode("CON-UNMATCHED") == 0);
    CHECK(hunt.path("top.a", "top.s1").relation == clock::ClockRelation::Asynchronous);
    CHECK(hunt.path("top.x1", "top.x2").relation == clock::ClockRelation::Unknown); // not covered

    // reset pins alone (sync_reset.tcl): the asynchronous reset crossing
    // into rs1/rs2 is covered, the data crossing into rs1 is not
    auto rst = analyze(v, clocks + "set_false_path -to [get_pins -of_objects [get_cells {rs1_reg rs2_reg}] -filter {IS_PRESET || IS_RESET}]\n");
    const auto* resetPath = rst.find("top.x1", "top.rs1");
    REQUIRE(resetPath);
    CHECK(resetPath->pin == Pin::AsyncReset);
    CHECK(resetPath->relation == clock::ClockRelation::Asynchronous);
    CHECK(rst.path("top.a", "top.rs1").relation == clock::ClockRelation::Unknown);

    // -from a clock -to registers (the FIFO output register idiom)
    auto mixed = analyze(v, clocks + "set_false_path -from [get_clocks -of_objects [get_cells a_reg]] -to [get_cells {s1_reg x2_reg}]\n");
    CHECK(mixed.path("top.a", "top.s1").relation == clock::ClockRelation::Asynchronous);
    CHECK(mixed.path("top.x1", "top.x2").relation == clock::ClockRelation::Asynchronous);
    CHECK(mixed.path("top.a", "top.rs1").relation == clock::ClockRelation::Unknown); // not a named destination
    // negative: from the other clock
    auto other = analyze(v, clocks + "set_false_path -from [get_clocks clk_b] -to [get_cells s1_reg]\n");
    CHECK(other.path("top.a", "top.s1").relation == clock::ClockRelation::Unknown);

    // -quiet: an exception that matches nothing is noted, not warned
    auto quiet = analyze(v, clocks + "set_false_path -quiet -to [get_cells -hier -filter {nosuch_attr == TRUE}]\n"
                                     "set_false_path -to [get_cells -hier -filter {nosuch_attr == TRUE}]\n");
    CHECK(quiet.countCode("CON-UNSUPPORTED-OPTION") == 2);
    CHECK(quiet.countSeverity("CON-UNSUPPORTED-OPTION", Severity::Warning) == 1);
}

TEST_CASE("get_pins by name: PRE/CLR/D/CE/R/S pins of flip-flops", "[cdc][constraints]") {
    // ad_rst_constr.xdc: set_false_path -to [get_pins -hier *rst_sync_reg*/PRE]
    constexpr const char* v = "module top(input clk_a, input clk_b, input en, input d, output q, output r, output w);\n"
                              "  reg a, x1, s1, s2, e1;\n"
                              "  reg rst_sync, rst_sync_d, rs_clr;\n"
                              "  always @(posedge clk_a) begin a <= d; x1 <= d; end\n"
                              "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; end\n"
                              "  always @(posedge clk_b) if (en) e1 <= a;\n"
                              "  always @(posedge clk_b or posedge x1)\n"
                              "    if (x1) begin rst_sync <= 1'b1; rst_sync_d <= 1'b1; end\n"
                              "    else begin rst_sync <= 1'b0; rst_sync_d <= rst_sync; end\n"
                              "  always @(posedge clk_b or posedge x1) if (x1) rs_clr <= 1'b0; else rs_clr <= 1'b1;\n"
                              "  assign q = s2 ^ e1;\n"
                              "  assign r = rst_sync_d;\n"
                              "  assign w = rs_clr;\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n";
    auto pre = analyze(v, clocks + "set_false_path -to [get_pins -hier *rst_sync_reg*/PRE]\n");
    CHECK(pre.countCode("CON-UNMATCHED") == 0);
    CHECK(pre.path("top.x1", "top.rst_sync").relation == clock::ClockRelation::Asynchronous);
    CHECK(std::ranges::find(pre.path("top.x1", "top.rst_sync").groupMembers, std::string("top.rst_sync_d")) !=
          pre.path("top.x1", "top.rst_sync").groupMembers.end()); // grouped reset path
    // negative: rs_clr is reset to 0, so its pin is CLR, not PRE
    CHECK(pre.path("top.x1", "top.rs_clr").relation == clock::ClockRelation::Unknown);

    auto clr = analyze(v, clocks + "set_false_path -to [get_pins rs_clr_reg/CLR]\n");
    CHECK(clr.countCode("CON-UNMATCHED") == 0);
    CHECK(clr.path("top.x1", "top.rs_clr").relation == clock::ClockRelation::Asynchronous);
    // negative: the PRE pin of a cleared flop matches nothing
    auto none = analyze(v, clocks + "set_false_path -to [get_pins rs_clr_reg/PRE]\n");
    CHECK(none.countSeverity("CON-UNMATCHED", Severity::Warning) == 1);

    // D and CE: data endpoints
    auto data = analyze(v, clocks + "set_false_path -to [get_pins {s1_reg/D e1_reg/CE}]\n");
    CHECK(data.countCode("CON-UNMATCHED") == 0);
    CHECK(data.path("top.a", "top.s1").relation == clock::ClockRelation::Asynchronous);
    // negative: a D pin does not cover the asynchronous reset of the same flop
    auto d = analyze(v, clocks + "set_false_path -to [get_pins rst_sync_reg/D]\n");
    CHECK(d.path("top.x1", "top.rst_sync").relation == clock::ClockRelation::Unknown);
}

TEST_CASE("Quartus SDC: procedures over instances, max delays between registers, '-from *'", "[cdc][constraints]") {
    // the style of verilog-ethernet's syn/quartus/*.sdc
    constexpr const char* v = "module sync2(input clk, input d, output q);\n"
                              "  reg s1_reg, s2_reg;\n"
                              "  always @(posedge clk) begin s1_reg <= d; s2_reg <= s1_reg; end\n"
                              "  assign q = s2_reg;\n"
                              "endmodule\n"
                              "module top(input clk_a, input clk_b, input d, input e, output q0, output q1);\n"
                              "  reg a_reg, b_reg;\n"
                              "  always @(posedge clk_a) begin a_reg <= d; b_reg <= e; end\n"
                              "  sync2 u_s0 (.clk(clk_b), .d(a_reg), .q(q0));\n"
                              "  sync2 u_s1 (.clk(clk_b), .d(b_reg), .q(q1));\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -period \"10.000 ns\" -name {clk_a} [get_ports {clk_a}]\n"
                               "create_clock -period 7 -name {clk_b} [get_ports {clk_b}]\n"
                               "derive_pll_clocks\n";
    auto r = analyze(v, clocks + "proc constrain_sync2_inst { inst src } {\n"
                                 "    puts \"Inserting timing constraints for sync2 instance $inst\"\n"
                                 "    if {[get_collection_size [get_registers -nowarn \"$inst|s1_reg\"]]} {\n"
                                 "        set_max_delay -from [get_registers \"$src\"] -to [get_registers \"$inst|s1_reg\"] 8.000\n"
                                 "    }\n"
                                 "}\n"
                                 "constrain_sync2_inst \"u_s0\" \"a_reg\"\n"
                                 "set_false_path -from * -to [get_registers \"u_s1|s1_reg\"]\n");
    CHECK(r.countCode("CON-UNKNOWN-COMMAND") == 0);
    CHECK(r.countCode("CON-UNMATCHED") == 0);
    CHECK(r.countCode("CON-PARSE") == 0);
    const auto& p0 = r.path("top.a_reg", "top.u_s0.s1_reg");
    CHECK(p0.relation == clock::ClockRelation::Asynchronous);
    CHECK(std::ranges::any_of(p0.notes, [](const std::string& n) { return n.find("set_max_delay") != std::string::npos; }));
    CHECK(r.path("top.b_reg", "top.u_s1.s1_reg").relation == clock::ClockRelation::Asynchronous);
    // negative: a Vivado-style max delay without -datapath_only still constrains nothing
    auto viv = analyze(v, clocks + "set_max_delay -from [get_cells a_reg_reg] -to [get_cells u_s0/s1_reg_reg] 8.0\n");
    CHECK(viv.path("top.a_reg", "top.u_s0.s1_reg").relation == clock::ClockRelation::Unknown);
    // negative: "-from * -to *" names nothing in particular: no exception
    auto all = analyze(v, clocks + "set_false_path -from * -to *\n");
    CHECK(all.path("top.a_reg", "top.u_s0.s1_reg").relation == clock::ClockRelation::Unknown);
}

TEST_CASE("Source-synchronous inputs: a virtual clock with the capture clock's period is not a crossing", "[cdc][constraints]") {
    constexpr const char* v = "module top(input rx_clk, input sys_clk, input [3:0] rxd, output reg [3:0] q, output reg [3:0] s);\n"
                              "  always @(posedge rx_clk) q <= rxd;\n"
                              "  always @(posedge sys_clk) s <= rxd;\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name rx_clk -period 8 [get_ports rx_clk]\n"
                               "create_clock -name sys_clk -period 10 [get_ports sys_clk]\n";
    // RGMII-style: input delays against a virtual copy of the receive clock
    auto r = analyze(v, clocks + "create_clock -name virt_rx -period 8.000\n"
                                 "set_input_delay -clock virt_rx -max 1.25 [get_ports {rxd[*]}]\n");
    CHECK_FALSE(r.hasPath("top.rxd", "top.q"));      // captured by rx_clk: timed by STA
    CHECK(r.hasPath("top.rxd", "top.s"));            // captured by sys_clk (10 ns): still a crossing
    CHECK(r.path("top.rxd", "top.s").relation == clock::ClockRelation::Unknown);
    CHECK(r.countCode("CDC-SOURCE-SYNC") == 1);
    // negative: a virtual clock with another period
    auto other = analyze(v, clocks + "create_clock -name virt_rx -period 5\n"
                                     "set_input_delay -clock virt_rx 1 [get_ports {rxd[*]}]\n");
    CHECK(other.hasPath("top.rxd", "top.q"));
    CHECK(other.countCode("CDC-SOURCE-SYNC") == 0);
    // negative: the designer declared the virtual clock asynchronous
    auto async = analyze(v, clocks + "create_clock -name virt_rx -period 8\n"
                                     "set_input_delay -clock virt_rx 1 [get_ports {rxd[*]}]\n"
                                     "set_clock_groups -asynchronous -group virt_rx -group rx_clk\n");
    CHECK(async.path("top.rxd", "top.q").relation == clock::ClockRelation::Asynchronous);
    CHECK(async.countCode("CDC-SOURCE-SYNC") == 0);
}


// ---------------------------------------------------------------------------
// Output ports timed against a clock (set_output_delay)
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kOutputs = "module top(input clk_a, input clk_b, input d, input din_b,\n"
                                 "           output reg qa, output qb, output qc, output fwd, output feed);\n"
                                 "  reg rb; always @(posedge clk_b) rb <= d;\n"
                                 "  always @(posedge clk_a) qa <= d;\n"
                                 "  assign qb = rb;\n"
                                 "  assign qc = qa ^ rb;\n"
                                 "  assign fwd = clk_a;\n"
                                 "  assign feed = din_b;\n"
                                 "endmodule\n";

std::string outputSdc(const std::string& extra) {
    return "create_clock -name a -period 10 [get_ports clk_a]\n"
           "create_clock -name b -period 7 [get_ports clk_b]\n"
           "set_clock_groups -asynchronous -group a -group b\n" +
           extra;
}

std::size_t outputHits(const CdcRun& r) { return r.countCode("CDC-OUTPUT-CLOCK"); }

bool outputNamed(const CdcRun& r, std::string_view port) {
    return std::ranges::any_of(r.diags, [&](const Diagnostic& d) {
        return d.code == "CDC-OUTPUT-CLOCK" && d.message.find(port) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Output ports driven by the clock they are timed against", "[cdc][outputs]") {
    // negative: qa is a register of clk_a, timed against a
    auto same = analyze(kOutputs, outputSdc("set_output_delay -clock a 1 [get_ports qa]\n"));
    CHECK(outputHits(same) == 0);
    // negative: a clock forwarded on a port (generated from clk_a) times data of clk_a
    auto fwd = analyze(kOutputs, outputSdc("create_generated_clock -name tx -source [get_ports clk_a] [get_ports fwd]\n"
                                           "set_output_delay -clock tx 1 [get_ports qa]\n"
                                           "set_output_delay -clock tx -clock_fall 1 [get_ports qa]\n"));
    CHECK(outputHits(fwd) == 0);
    // negative: a virtual clock with the period of the driver's clock (timed by STA as one clock)
    auto virt = analyze(kOutputs, outputSdc("create_clock -name va -period 10\n"
                                            "set_output_delay -clock va 1 [get_ports qa]\n"));
    CHECK(outputHits(virt) == 0);
    CHECK(virt.countCode("CDC-SOURCE-SYNC") == 1);
    // negative: exempted by set_false_path -to the port, or from the driver's clock
    auto fp = analyze(kOutputs, outputSdc("set_output_delay -clock a 1 [get_ports {qb qc}]\n"
                                          "set_false_path -to [get_ports qb]\n"
                                          "set_false_path -from [get_clocks b] -to [get_ports qc]\n"));
    CHECK(outputHits(fp) == 0);
}

TEST_CASE("Output ports driven asynchronously to their output delay clock", "[cdc][outputs]") {
    // positive: qb is a register of clk_b timed against a; qc mixes both clocks
    auto r = analyze(kOutputs, outputSdc("set_output_delay -clock a 1 [get_ports {qa qb qc}]\n"));
    CHECK(outputHits(r) == 1); // one message for the clock pair b -> a
    CHECK(outputNamed(r, "'top.qb'"));
    CHECK(outputNamed(r, "'top.qc'"));
    CHECK_FALSE(outputNamed(r, "'top.qa'"));
    CHECK(outputNamed(r, "(asynchronous)"));
    CHECK(r.cdc->stats.outputCrossings == 1);
    // positive: an input timed against b passed straight to an output timed against a
    auto feed = analyze(kOutputs, outputSdc("set_input_delay -clock b 1 [get_ports din_b]\n"
                                            "set_output_delay -clock a 1 [get_ports feed]\n"));
    CHECK(outputNamed(feed, "'top.feed'"));
    // negative: the same input without a clock domain is not checked
    auto nodom = analyze(kOutputs, outputSdc("set_output_delay -clock a 1 [get_ports feed]\n"));
    CHECK(outputHits(nodom) == 0);
    // positive: unrelated clocks (no constraint relates them) are reported as such
    auto unknown = analyze(kOutputs, "create_clock -name a -period 10 [get_ports clk_a]\n"
                                     "create_clock -name b -period 7 [get_ports clk_b]\n"
                                     "set_output_delay -clock a 1 [get_ports qb]\n");
    CHECK(outputNamed(unknown, "relation unknown"));
    // negative: ... unless unknown relations are assumed synchronous
    CdcOptions sync;
    sync.unknownRelation = UnknownRelationPolicy::AssumeSync;
    auto assumed = analyze(kOutputs,
                           "create_clock -name a -period 10 [get_ports clk_a]\n"
                           "create_clock -name b -period 7 [get_ports clk_b]\n"
                           "set_output_delay -clock a 1 [get_ports qb]\n",
                           sync);
    CHECK(outputHits(assumed) == 0);
    // a clock that does not exist
    auto missing = analyze(kOutputs, outputSdc("set_output_delay -clock nope 1 [get_ports qa]\n"));
    CHECK(outputNamed(missing, "unknown clock 'nope'"));
}

TEST_CASE("x && 0 is no combinational path: no loop through it", "[cdc][seqgraph]") {
    // BrianHG DDR3: busy[x] = busy_e[x] ^ (ena[x] && TOGGLE[x]) with TOGGLE[x] = 0, ena = f && !busy
    auto design = [](const char* tog) {
        return std::string("module c #(parameter [1:0] TOG = ") + tog +
               ") (input clk, input [1:0] ena, output [1:0] busy);\n"
               "  reg [1:0] b_e;\n"
               "  always @(posedge clk) b_e <= ena;\n"
               "  genvar x;\n"
               "  generate for (x = 0; x < 2; x = x + 1) begin : g\n"
               "    assign busy[x] = b_e[x] ^ (ena[x] && TOG[x]);\n"
               "  end endgenerate\n"
               "endmodule\n"
               "module top(input clk_a, input f, output w);\n"
               "  wire [1:0] ena; wire [1:0] busy;\n"
               "  assign ena[0] = f && !busy[0];\n"
               "  assign ena[1] = f || 1'b1;\n" // || with a true constant: 1
               "  assign w = ena[0];\n"
               "  c u (.clk(clk_a), .ena(ena), .busy(busy));\n"
               "endmodule\n";
    };
    auto r = analyze(design("2'b10"), "");
    CHECK(r.countCode("GR-COMB-LOOP") == 0);
    // negative: with TOG[0] = 1 the loop is real
    auto l = analyze(design("2'b01"), "");
    CHECK(l.countCode("GR-COMB-LOOP") == 1);
}

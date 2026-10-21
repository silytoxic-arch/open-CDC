// Stage-5 CDC analysis: protocol synchronizers (toggle, Gray, handshake,
// asynchronous FIFO, reset synchronizer) and rules CDC-004 .. CDC-009.
// Every rule has at least one positive and one negative case.

#include "CdcTestUtil.h"

using namespace opencdc;
using namespace opencdc::cdc;
using namespace opencdc::test::cdc;

namespace {

constexpr const char* kAsync3 = "set_clock_groups -asynchronous -group clk_a -group clk_b -group clk_c\n";

bool noteContains(const CdcPath& p, std::string_view text) {
    return std::ranges::any_of(p.notes, [&](const std::string& n) { return n.find(text) != std::string::npos; });
}

const SyncStructure* structureOf(const CdcRun& r, const CdcPath& p) {
    return p.structure < r.cdc->structures.size() ? &r.cdc->structures[p.structure] : nullptr;
}

} // namespace

// ===========================================================================
// Gray code
// ===========================================================================

TEST_CASE("Gray: incrementing pointer is recognized and verified", "[cdc][gray]") {
    auto r = analyze("module top(input clk_a, input clk_b, input inc, output [3:0] y);\n"
                     "  reg [3:0] bin, gray, s1, s2;\n"
                     "  wire [3:0] bin_next = bin + {3'b000, inc};\n"
                     "  always @(posedge clk_a) begin bin <= bin_next; gray <= (bin_next >> 1) ^ bin_next; end\n"
                     "  always @(posedge clk_b) begin s1 <= gray; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.gray", "top.s1");
    CHECK(p.syncType == SyncType::Gray);
    CHECK(p.syncStages == 2);
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(p.unverified.empty());
    CHECK(p.hits.empty());
    CHECK(p.status == AnalysisStatus::Safe);
    const auto* s = structureOf(r, p);
    REQUIRE(s);
    CHECK(s->type == SyncType::Gray);
}

TEST_CASE("Gray: encoder present but the step size cannot be proven", "[cdc][gray]") {
    auto r = analyze("module top(input clk_a, input clk_b, input ld, input [3:0] v, output [3:0] y);\n"
                     "  reg [3:0] bin, gray, s1, s2;\n"
                     "  wire [3:0] bin_next = ld ? v : bin + 4'd1;\n"
                     "  always @(posedge clk_a) begin bin <= bin_next; gray <= bin_next ^ (bin_next >> 1); end\n"
                     "  always @(posedge clk_b) begin s1 <= gray; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.gray", "top.s1");
    CHECK(p.syncType == SyncType::Gray);
    CHECK(p.syncStatus == SyncStatus::ProtocolUnverified);
    REQUIRE(p.unverified.size() == 1);
    CHECK(p.unverified[0].find("at most one") != std::string::npos);
    CHECK_FALSE(hasHit(p, "CDC-008")); // Gray-coded: not an incoherency finding
}

TEST_CASE("Gray: an extra source register stage keeps the Gray property", "[cdc][gray]") {
    // open-logic olo_base_fifo_async: the Gray pointer register feeds a
    // second source-domain register (olo_base_cc_bits RegIn) before the chain.
    auto r = analyze("module top(input clk_a, input clk_b, input inc, input rst, output [3:0] y);\n"
                     "  reg [3:0] bin, gray, gray_q, s1, s2;\n"
                     "  wire [3:0] bin_next = bin + {3'b000, inc};\n"
                     "  always @(posedge clk_a) begin\n"
                     "    if (rst) begin bin <= 0; gray <= 0; end\n"
                     "    else begin bin <= bin_next; gray <= (bin_next >> 1) ^ bin_next; end\n"
                     "    gray_q <= gray;\n"
                     "  end\n"
                     "  always @(posedge clk_b) begin s1 <= gray_q; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.gray_q", "top.s1");
    CHECK(p.syncType == SyncType::Gray);
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(r.hits("CDC-008") == 0);
}

TEST_CASE("Gray: bits of a Gray counter synchronized by separate 2FF cells", "[cdc][gray]") {
    // generate loop of single-bit synchronizer modules (xpm_cdc_gray, dcfifo pointers)
    auto r = analyze("module sync2(input clk, input d, output q);\n"
                     "  reg a, b; always @(posedge clk) begin a <= d; b <= a; end\n"
                     "  assign q = b;\n"
                     "endmodule\n"
                     "module top(input clk_a, input clk_b, input inc, output reg [3:0] y);\n"
                     "  reg [3:0] bin, gray; wire [3:0] s;\n"
                     "  wire [3:0] bin_next = bin + {3'b000, inc};\n"
                     "  always @(posedge clk_a) begin bin <= bin_next; gray <= (bin_next >> 1) ^ bin_next; end\n"
                     "  genvar i;\n"
                     "  generate for (i = 0; i < 4; i = i + 1) begin : g\n"
                     "    sync2 u (.clk(clk_b), .d(gray[i]), .q(s[i]));\n"
                     "  end endgenerate\n"
                     "  always @(posedge clk_b) y <= s + 4'd1;\n"
                     "endmodule\n");
    const auto& p0 = r.path("top.gray", "top.g[0].u.a");
    const auto& p3 = r.path("top.gray", "top.g[3].u.a");
    CHECK(p0.syncType == SyncType::Gray);
    CHECK(p3.syncType == SyncType::Gray);
    CHECK(p0.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(p0.structure == p3.structure);
    CHECK(r.hits("CDC-008") == 0);
    CHECK(r.hits("CDC-005") == 0);
}

TEST_CASE("Gray negative: bits of a binary counter with separate 2FF cells stay CDC-008", "[cdc][gray][CDC-008]") {
    auto r = analyze("module sync2(input clk, input d, output q);\n"
                     "  reg a, b; always @(posedge clk) begin a <= d; b <= a; end\n"
                     "  assign q = b;\n"
                     "endmodule\n"
                     "module top(input clk_a, input clk_b, output reg [3:0] y);\n"
                     "  reg [3:0] cnt; wire [3:0] s;\n"
                     "  always @(posedge clk_a) cnt <= cnt + 4'd1;\n"
                     "  genvar i;\n"
                     "  generate for (i = 0; i < 4; i = i + 1) begin : g\n"
                     "    sync2 u (.clk(clk_b), .d(cnt[i]), .q(s[i]));\n"
                     "  end endgenerate\n"
                     "  always @(posedge clk_b) y <= s + 4'd1;\n"
                     "endmodule\n");
    CHECK(r.path("top.cnt", "top.g[0].u.a").syncType == SyncType::TwoFlop);
    CHECK(r.hits("CDC-008") == 4);
}

TEST_CASE("Gray negative: a register copy of a binary counter is not Gray", "[cdc][gray][CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, output reg [3:0] y);\n"
                     "  reg [3:0] cnt, cnt_q, s1, s2;\n"
                     "  always @(posedge clk_a) begin cnt <= cnt + 4'd1; cnt_q <= cnt; end\n"
                     "  always @(posedge clk_b) begin s1 <= cnt_q; s2 <= s1; y <= s2 + 4'd1; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.cnt_q", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(hasHit(p, "CDC-008"));
}

TEST_CASE("Gray negative: a bit-reversed copy of a Gray register is not Gray-encoded", "[cdc][gray][CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, input inc, output reg [3:0] y);\n"
                     "  reg [3:0] bin, gray, rev, s1, s2;\n"
                     "  wire [3:0] bin_next = bin + {3'b000, inc};\n"
                     "  always @(posedge clk_a) begin\n"
                     "    bin <= bin_next; gray <= (bin_next >> 1) ^ bin_next;\n"
                     "    rev <= {gray[0], gray[1], gray[2], gray[3]};\n"
                     "  end\n"
                     "  always @(posedge clk_b) begin s1 <= rev; s2 <= s1; y <= s2 + 4'd1; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.rev", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(hasHit(p, "CDC-008"));
}

TEST_CASE("Gray negative: a binary counter is not Gray", "[cdc][gray][CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, output reg [3:0] y);\n"
                     "  reg [3:0] cnt, s1, s2;\n"
                     "  always @(posedge clk_a) cnt <= cnt + 4'd1;\n"
                     "  always @(posedge clk_b) begin s1 <= cnt; s2 <= s1; y <= s2 + 4'd1; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.cnt", "top.s1");
    CHECK(p.syncType == SyncType::TwoFlop);
    CHECK(p.bitsCombined);
    CHECK(hasHit(p, "CDC-008"));
    CHECK(p.status == AnalysisStatus::PotentialRisk);
}

// ===========================================================================
// Toggle
// ===========================================================================

TEST_CASE("Toggle synchronizer with edge detection", "[cdc][toggle]") {
    auto r = analyze("module top(input clk_a, input clk_b, input ev, output reg pulse);\n"
                     "  reg t, s1, s2, s3;\n"
                     "  always @(posedge clk_a) if (ev) t <= ~t;\n"
                     "  always @(posedge clk_b) begin s1 <= t; s2 <= s1; s3 <= s2; pulse <= s2 ^ s3; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.t", "top.s1");
    CHECK(p.syncType == SyncType::Toggle);
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(noteContains(p, "detects toggles"));
    CHECK(p.hits.empty());
}

TEST_CASE("Toggle without enable (t <= t ^ p) and without edge detector", "[cdc][toggle]") {
    auto r = analyze("module top(input clk_a, input clk_b, input ev, output y);\n"
                     "  reg t, s1, s2;\n"
                     "  always @(posedge clk_a) t <= t ^ ev;\n"
                     "  always @(posedge clk_b) begin s1 <= t; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.t", "top.s1");
    CHECK(p.syncType == SyncType::Toggle);
    CHECK(noteContains(p, "no register after the synchronizer compares"));
}

TEST_CASE("Toggle negative: a level register is a plain two-flop path", "[cdc][toggle]") {
    auto r = analyze("module top(input clk_a, input clk_b, input set, input clr, output y);\n"
                     "  reg l, s1, s2;\n"
                     "  always @(posedge clk_a) if (set) l <= 1'b1; else if (clr) l <= 1'b0;\n"
                     "  always @(posedge clk_b) begin s1 <= l; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    CHECK(r.path("top.l", "top.s1").syncType == SyncType::TwoFlop);
}

// ===========================================================================
// Handshake
// ===========================================================================

namespace {

std::string handshake(bool withAck) {
    std::string v = "module top(input clk_a, input clk_b, input go, input [7:0] d, output reg [7:0] q,\n"
                    "           output busy);\n"
                    "  reg req; reg [7:0] data;\n"
                    "  reg rq1, rq2;\n"
                    "  reg aq1, aq2;\n"
                    "  always @(posedge clk_a) if (go && !aq2) begin req <= ~req; data <= d; end\n"
                    "  always @(posedge clk_b) begin rq1 <= req; rq2 <= rq1; if (rq2) q <= data; end\n";
    if (withAck)
        v += "  always @(posedge clk_a) begin aq1 <= rq2; aq2 <= aq1; end\n";
    else
        v += "  always @(posedge clk_a) begin aq1 <= go; aq2 <= aq1; end\n";
    v += "  assign busy = aq2;\nendmodule\n";
    return v;
}

} // namespace

TEST_CASE("Handshake: data qualified by a request whose acknowledge returns", "[cdc][handshake]") {
    auto r = analyze(handshake(true));
    const auto& d = r.path("top.data", "top.q");
    CHECK(d.syncType == SyncType::Handshake);
    CHECK(d.syncStatus == SyncStatus::ProtocolUnverified);
    REQUIRE(d.unverified.size() == 1);
    CHECK(d.unverified[0].find("acknowledge") != std::string::npos);
    const auto* s = structureOf(r, d);
    REQUIRE(s);
    CHECK(s->type == SyncType::Handshake);
    CHECK(s->paths.size() == 3); // data, request, acknowledge
    // The request and acknowledge paths belong to the same structure.
    CHECK(r.path("top.req", "top.rq1").structure == d.structure);
    CHECK(r.path("top.rq2", "top.aq1").structure == d.structure);
    CHECK(d.status == AnalysisStatus::Safe);
}

TEST_CASE("Handshake negative: no acknowledge, data stays qualified data", "[cdc][handshake]") {
    auto r = analyze(handshake(false));
    CHECK(r.path("top.data", "top.q").syncType == SyncType::QualifiedData);
}

// ===========================================================================
// Asynchronous FIFO
// ===========================================================================

namespace {

std::string fifo(bool readPointerBack, int aw = 2) {
    // aw address bits; pointers have aw + 1 bits
    const std::string A = std::to_string(aw);
    const std::string P = std::to_string(aw + 1);
    const std::string zeros = std::to_string(aw) + "'d0";
    std::string v =
        "module top(input clk_a, input clk_b, input we, input [7:0] wd, input re, output reg [7:0] rd,\n"
        "           output full);\n"
        "  reg [7:0] mem [0:" + std::to_string((1 << aw) - 1) + "];\n"
        "  reg [" + A + ":0] wbin, wgray, rbin, rgray;\n"
        "  reg [" + A + ":0] w2r1, w2r2, r2w1, r2w2;\n"
        "  wire [" + A + ":0] wbin_n = wbin + {" + zeros + ", we};\n"
        "  wire [" + A + ":0] rbin_n = rbin + {" + zeros + ", re & (rgray != w2r2)};\n"
        "  always @(posedge clk_a) begin\n"
        "    if (we) mem[wbin[" + std::to_string(aw - 1) + ":0]] <= wd;\n"
        "    wbin <= wbin_n; wgray <= wbin_n ^ (wbin_n >> 1);\n"
        "  end\n"
        "  always @(posedge clk_b) begin\n"
        "    rbin <= rbin_n; rgray <= rbin_n ^ (rbin_n >> 1);\n"
        "    w2r1 <= wgray; w2r2 <= w2r1;\n"
        "    if (rgray != w2r2) rd <= mem[rbin[" + std::to_string(aw - 1) + ":0]];\n"
        "  end\n";
    if (readPointerBack)
        v += "  always @(posedge clk_a) begin r2w1 <= rgray; r2w2 <= r2w1; end\n"
             "  assign full = wgray == {~r2w2[" + A + ":" + std::to_string(aw - 1) + "], r2w2[" +
             std::to_string(aw - 2) + ":0]};\n";
    else
        v += "  assign full = 1'b0;\n";
    v += "endmodule\n";
    (void)P;
    return v;
}

} // namespace

TEST_CASE("Asynchronous FIFO: memory read qualified by Gray pointers in both directions", "[cdc][fifo]") {
    auto r = analyze(fifo(true));
    const auto& m = r.path("top.mem", "top.rd");
    CHECK(m.syncType == SyncType::AsyncFifo);
    CHECK(m.width == 8);
    CHECK(m.syncStatus == SyncStatus::ProtocolUnverified);
    CHECK(r.path("top.wgray", "top.w2r1").syncType == SyncType::Gray);
    CHECK(r.path("top.rgray", "top.r2w1").syncType == SyncType::Gray);
    const auto* s = structureOf(r, m);
    REQUIRE(s);
    CHECK(s->type == SyncType::AsyncFifo);
    CHECK(s->paths.size() == 3);
    CHECK(r.hits("CDC-003") == 0);
    CHECK(r.hits("CDC-008") == 0);
}

TEST_CASE("Asynchronous FIFO with a large memory (wide write logic tracked as one unit)", "[cdc][fifo]") {
    // 1024 x 8 bits: the memory's next-state cells are wider than SeqGraph::kWideCell
    auto r = analyze(fifo(true, 10));
    const auto& m = r.path("top.mem", "top.rd");
    CHECK(m.syncType == SyncType::AsyncFifo);
    CHECK(m.width == 8);
    CHECK(r.path("top.wgray", "top.w2r1").syncType == SyncType::Gray);
    CHECK(r.hits("CDC-003") == 0);
    CHECK(r.hits("CDC-008") == 0);
    CHECK(r.countCode("GR-COMB-LOOP") == 0);
}

TEST_CASE("Asynchronous FIFO negative: no read pointer back to the write clock", "[cdc][fifo]") {
    auto r = analyze(fifo(false));
    const auto& m = r.path("top.mem", "top.rd");
    CHECK(m.syncType == SyncType::QualifiedData);
    CHECK(noteContains(m, "not recognized as an asynchronous FIFO"));
}

// ===========================================================================
// CDC-004 combinational logic in front of a synchronizer
// ===========================================================================

TEST_CASE("CDC-004 positive: AND gate in front of a two-flop chain", "[CDC-004]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, input e, output y);\n"
                     "  reg a, c, s1, s2;\n"
                     "  always @(posedge clk_a) begin a <= d; c <= e; end\n"
                     "  always @(posedge clk_b) begin s1 <= a & c; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.a", "top.s1");
    REQUIRE(hasHit(p, "CDC-004"));
    CHECK(p.hits.front().severity == Severity::Error);
    CHECK(p.hits.front().message.find("and") != std::string::npos);
    CHECK(p.status == AnalysisStatus::Violation);
}

TEST_CASE("CDC-004 negative: wire and single inverter are allowed", "[CDC-004]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, output y, output z);\n"
                     "  reg a, s1, s2, n1, n2;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; n1 <= ~a; n2 <= n1; end\n"
                     "  assign y = s2; assign z = n2;\n"
                     "endmodule\n");
    CHECK(r.hits("CDC-004") == 0);
    CHECK(r.path("top.a", "top.n1").syncStatus == SyncStatus::NecessaryConditionsVerified);
}

// ===========================================================================
// CDC-005 reconvergence
// ===========================================================================

namespace {

std::string reconv(const char* combine, const char* srcB = "clk_a") {
    return std::string("module top(input clk_a, input clk_b, input clk_c, input x, input z, input w,\n"
                       "           output reg [1:0] y);\n"
                       "  reg a1, a2, sa1, sa2, sb1, sb2, m1, m2, e;\n"
                       "  always @(posedge clk_a) a1 <= x;\n"
                       "  always @(posedge ") +
           srcB +
           ") a2 <= z;\n"
           "  always @(posedge clk_b) begin sa1 <= a1; sa2 <= sa1; sb1 <= a2; sb2 <= sb1; end\n"
           "  always @(posedge clk_b) begin e <= w; m1 <= sa2 & e; m2 <= m1 & e; end\n"
           "  always @(posedge clk_b) " +
           combine +
           ";\n"
           "endmodule\n";
}

} // namespace

TEST_CASE("CDC-005 positive: two synchronized signals of one clock meet", "[CDC-005]") {
    auto r = analyze(reconv("y <= {1'b0, sa2 & sb2}"), kAsync3);
    const auto& a = r.path("top.a1", "top.sa1");
    const auto& b = r.path("top.a2", "top.sb1");
    REQUIRE(hasHit(a, "CDC-005"));
    REQUIRE(hasHit(b, "CDC-005"));
    REQUIRE(a.reconvergences.size() == 1);
    CHECK(a.reconvergences[0].at == "top.y");
    CHECK(a.reconvergences[0].depth == 0);
    CHECK(a.reconvergences[0].paths == std::vector<std::size_t>{b.id});
    CHECK(a.status == AnalysisStatus::PotentialRisk);
    CHECK(r.cdc->stats.reconvergencePoints == 1);
}

TEST_CASE("CDC-005 positive: meeting after registers, limited by reconvergenceDepth", "[CDC-005]") {
    // sa2 -> m1 -> m2 (two register levels through logic), then meets sb2 in y.
    auto deep = reconv("y <= {1'b0, m2 | sb2}");
    auto r = analyze(deep, kAsync3);
    const auto& a = r.path("top.a1", "top.sa1");
    REQUIRE(hasHit(a, "CDC-005"));
    CHECK(a.reconvergences[0].depth == 2);

    CdcOptions shallow;
    shallow.reconvergenceDepth = 1;
    auto r1 = analyze(deep, kAsync3, shallow);
    CHECK(r1.hits("CDC-005") == 0);
}

TEST_CASE("CDC-005 negative: unrelated source clocks, packed bits, one synchronizer", "[CDC-005]") {
    // Sources in clk_a and clk_c: uncorrelated anyway, no reconvergence finding.
    CHECK(analyze(reconv("y <= {1'b0, sa2 & sb2}", "clk_c"), kAsync3).hits("CDC-005") == 0);
    // Bits packed into one register but never combined.
    CHECK(analyze(reconv("y <= {sa2, sb2}"), kAsync3).hits("CDC-005") == 0);
    // Only one synchronizer used.
    CHECK(analyze(reconv("y <= {sa2, m2}"), kAsync3).hits("CDC-005") == 0);
}

// ===========================================================================
// CDC-006 asynchronous reset release
// ===========================================================================

namespace {

constexpr const char* kResetSync =
    "module top(input clk_b, input rst_n, input d, output reg q);\n"
    "  reg r1, r2;\n"
    "  always @(posedge clk_b or negedge rst_n) if (!rst_n) {r2, r1} <= 2'b00; else {r2, r1} <= {r1, 1'b1};\n"
    "  always @(posedge clk_b or negedge r2) if (!r2) q <= 1'b0; else q <= d;\n"
    "endmodule\n";

} // namespace

TEST_CASE("CDC-006 negative: reset synchronizer recognized", "[CDC-006][reset]") {
    auto r = analyze(kResetSync, "create_clock -name clk_b -period 10 [get_ports clk_b]\n");
    CHECK(r.cdc->stats.resetSynchronizers == 1);
    REQUIRE(r.cdc->paths.size() == 1);
    const auto& p = r.cdc->paths.front();
    CHECK(p.pin == Pin::AsyncReset);
    CHECK(p.srcName == "top.rst_n");
    CHECK(p.srcUnclocked);
    CHECK(p.syncType == SyncType::ResetSync);
    CHECK(p.syncStages == 2);
    CHECK(p.groupSize == 2); // r1 and r2 grouped
    CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
    CHECK(noteContains(p, "1 register(s)")); // q is protected
    CHECK(p.hits.empty());
    CHECK(p.status == AnalysisStatus::Safe);
}

TEST_CASE("CDC-006 positive: external reset used directly", "[CDC-006][reset]") {
    auto r = analyze("module top(input clk_b, input rst_n, input d, output reg q, output reg w);\n"
                     "  always @(posedge clk_b or negedge rst_n) if (!rst_n) q <= 1'b0; else q <= d;\n"
                     "  always @(posedge clk_b or negedge rst_n) if (!rst_n) w <= 1'b0; else w <= q;\n"
                     "endmodule\n",
                     "create_clock -name clk_b -period 10 [get_ports clk_b]\n");
    REQUIRE(r.cdc->paths.size() == 1);
    const auto& p = r.cdc->paths.front();
    CHECK(p.groupSize == 2);
    REQUIRE(hasHit(p, "CDC-006"));
    CHECK(p.hits.front().severity == Severity::Error);
    CHECK(p.status == AnalysisStatus::PotentialRisk);
}

TEST_CASE("CDC-006 negative: input declared synchronous with set_input_delay", "[CDC-006][reset]") {
    auto r = analyze("module top(input clk_b, input rst_n, input d, output reg q);\n"
                     "  always @(posedge clk_b or negedge rst_n) if (!rst_n) q <= 1'b0; else q <= d;\n"
                     "endmodule\n",
                     "create_clock -name clk_b -period 10 [get_ports clk_b]\n"
                     "set_input_delay -clock clk_b 1 [get_ports rst_n]\n");
    CHECK(r.cdc->paths.empty());
}

TEST_CASE("CDC-006 positive: reset from another clock domain, or synchronized for the wrong clock",
          "[CDC-006][reset]") {
    auto r = analyze("module top(input clk_a, input clk_b, input rst_n, input d, output reg q, output reg w);\n"
                     "  reg soft, r1, r2;\n"
                     "  always @(posedge clk_a) soft <= d;\n"
                     "  always @(posedge clk_b or posedge soft) if (soft) q <= 1'b0; else q <= d;\n"
                     "  always @(posedge clk_a or negedge rst_n) if (!rst_n) {r2, r1} <= 2'b00; "
                     "else {r2, r1} <= {r1, 1'b1};\n"
                     "  always @(posedge clk_b or negedge r2) if (!r2) w <= 1'b0; else w <= d;\n"
                     "endmodule\n");
    const auto& soft = r.path("top.soft", "top.q");
    REQUIRE(hasHit(soft, "CDC-006"));
    CHECK(soft.status == AnalysisStatus::Violation);
    const auto& wrong = r.path("top.r2", "top.w");
    REQUIRE(hasHit(wrong, "CDC-006"));
    CHECK(noteContains(wrong, "need their own reset synchronizer"));
}

TEST_CASE("CDC-006 positive: reset synchronizer with a single stage", "[CDC-006][reset]") {
    auto r = analyze("module top(input clk_b, input rst_n, input d, output reg q);\n"
                     "  reg r1;\n"
                     "  always @(posedge clk_b or negedge rst_n) if (!rst_n) r1 <= 1'b0; else r1 <= 1'b1;\n"
                     "  always @(posedge clk_b or negedge r1) if (!r1) q <= 1'b0; else q <= d;\n"
                     "endmodule\n",
                     "create_clock -name clk_b -period 10 [get_ports clk_b]\n");
    REQUIRE(r.cdc->paths.size() == 1);
    const auto& p = r.cdc->paths.front();
    CHECK(p.syncType == SyncType::ResetSync);
    CHECK(p.syncStatus == SyncStatus::StructureRecognized);
    REQUIRE(hasHit(p, "CDC-006"));
    CHECK(p.hits.front().message.find("at least 2 stages") != std::string::npos);
    CHECK(p.status == AnalysisStatus::Violation);
}

// ===========================================================================
// CDC-007 pulse loss
// ===========================================================================

namespace {

constexpr const char* kPulse = "module top(input clk_a, input clk_b, input x, input z, output y);\n"
                               "  reg p, s1, s2;\n"
                               "  always @(posedge clk_a) p <= x & ~z;\n"
                               "  always @(posedge clk_b) begin s1 <= p; s2 <= s1; end\n"
                               "  assign y = s2;\n"
                               "endmodule\n";

std::string clocks(const char* periodA, const char* periodB) {
    return std::string("create_clock -name clk_a -period ") + periodA + " [get_ports clk_a]\n" +
           "create_clock -name clk_b -period " + periodB + " [get_ports clk_b]\n" + kAsync;
}

} // namespace

TEST_CASE("CDC-007 positive: pulse from a faster clock", "[CDC-007]") {
    auto r = analyze(kPulse, clocks("10", "8"));
    const auto& p = r.path("top.p", "top.s1");
    CHECK(p.sourceMayPulse);
    REQUIRE(hasHit(p, "CDC-007"));
    CHECK(p.hits.front().status == AnalysisStatus::PotentialRisk);
    CHECK(p.hits.front().message.find("1.5") != std::string::npos);
}

TEST_CASE("CDC-007 without clock periods needs review", "[CDC-007]") {
    auto r = analyze(kPulse);
    const auto& p = r.path("top.p", "top.s1");
    REQUIRE(hasHit(p, "CDC-007"));
    CHECK(p.hits.front().status == AnalysisStatus::NeedsReview);
}

TEST_CASE("CDC-007 negative: slow source, level with hold, toggle, copied input", "[CDC-007]") {
    CHECK(analyze(kPulse, clocks("20", "8")).hits("CDC-007") == 0);
    // A register with an enable holds its value.
    CHECK(analyze("module top(input clk_a, input clk_b, input x, input e, output y);\n"
                  "  reg p, s1, s2;\n"
                  "  always @(posedge clk_a) if (e) p <= x;\n"
                  "  always @(posedge clk_b) begin s1 <= p; s2 <= s1; end\n"
                  "  assign y = s2;\n"
                  "endmodule\n",
                  clocks("10", "8"))
              .hits("CDC-007") == 0);
    // A plain copy of an input: waveform unknown, not judged.
    CHECK(analyze("module top(input clk_a, input clk_b, input x, output y);\n"
                  "  reg p, s1, s2;\n"
                  "  always @(posedge clk_a) p <= x;\n"
                  "  always @(posedge clk_b) begin s1 <= p; s2 <= s1; end\n"
                  "  assign y = s2;\n"
                  "endmodule\n",
                  clocks("10", "8"))
              .hits("CDC-007") == 0);
    // Toggle synchronizer: the event survives.
    CHECK(analyze("module top(input clk_a, input clk_b, input x, output y);\n"
                  "  reg t, s1, s2;\n"
                  "  always @(posedge clk_a) t <= t ^ x;\n"
                  "  always @(posedge clk_b) begin s1 <= t; s2 <= s1; end\n"
                  "  assign y = s2;\n"
                  "endmodule\n",
                  clocks("10", "8"))
              .hits("CDC-007") == 0);
}

// ===========================================================================
// CDC-008 multi-bit incoherency
// ===========================================================================

TEST_CASE("CDC-008 positive: bus synchronized bit by bit and used as a value", "[CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [3:0] d, output reg [3:0] y);\n"
                     "  reg [3:0] g, s1, s2;\n"
                     "  always @(posedge clk_a) g <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= g; s2 <= s1; y <= s2 + 4'd3; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.g", "top.s1");
    REQUIRE(hasHit(p, "CDC-008"));
    CHECK(p.status == AnalysisStatus::PotentialRisk);
    CHECK(p.hits.front().severity == Severity::Error);
}

TEST_CASE("CDC-008 positive: bits of one register split over separate synchronizers", "[CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [1:0] d, output reg [1:0] y);\n"
                     "  reg [1:0] g; reg a1, a2, b1, b2;\n"
                     "  always @(posedge clk_a) g <= d;\n"
                     "  always @(posedge clk_b) begin a1 <= g[0]; a2 <= a1; b1 <= g[1]; b2 <= b1; y <= {b2, a2} + 2'd1; end\n"
                     "endmodule\n");
    const auto& a = r.path("top.g", "top.a1");
    CHECK(a.splitWith.size() == 1);
    CHECK(a.bitsCombined);
    REQUIRE(hasHit(a, "CDC-008"));
    CHECK(a.status == AnalysisStatus::PotentialRisk);
    CHECK_FALSE(hasHit(a, "CDC-005")); // reported as incoherency, not as reconvergence
}

TEST_CASE("CDC-008 needs review when the bits are never read together", "[CDC-008]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [3:0] d, output [3:0] y);\n"
                     "  reg [3:0] g, s1, s2;\n"
                     "  always @(posedge clk_a) g <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= g; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.g", "top.s1");
    REQUIRE(hasHit(p, "CDC-008"));
    CHECK(p.status == AnalysisStatus::NeedsReview);
}

TEST_CASE("CDC-008 negative: Gray-coded value, one bit synchronized twice", "[CDC-008]") {
    auto gray = analyze("module top(input clk_a, input clk_b, output reg [3:0] y);\n"
                        "  reg [3:0] bin, gray, s1, s2;\n"
                        "  always @(posedge clk_a) begin bin <= bin + 4'd1; gray <= (bin + 4'd1) ^ ((bin + 4'd1) >> 1); "
                        "end\n"
                        "  always @(posedge clk_b) begin s1 <= gray; s2 <= s1; y <= s2; end\n"
                        "endmodule\n");
    CHECK(gray.hits("CDC-008") == 0);
    // The same bit synchronized twice is divergence (CDC-005), not CDC-008.
    auto twice = analyze("module top(input clk_a, input clk_b, input [1:0] d, output reg y);\n"
                         "  reg [1:0] g; reg a1, a2, b1, b2;\n"
                         "  always @(posedge clk_a) g <= d;\n"
                         "  always @(posedge clk_b) begin a1 <= g[0]; a2 <= a1; b1 <= g[0]; b2 <= b1; y <= a2 ^ b2; end\n"
                         "endmodule\n");
    CHECK(twice.hits("CDC-008") == 0);
    CHECK(twice.hits("CDC-005") == 2);
}

// ===========================================================================
// CDC-009 multi-domain logic
// ===========================================================================

TEST_CASE("CDC-009 positive: logic mixes two foreign clock domains before a synchronizer", "[CDC-009]") {
    auto r = analyze("module top(input clk_a, input clk_b, input clk_c, input x, input z, output y);\n"
                     "  reg a, c, s1, s2;\n"
                     "  always @(posedge clk_a) a <= x;\n"
                     "  always @(posedge clk_c) c <= z;\n"
                     "  always @(posedge clk_b) begin s1 <= a | c; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n",
                     kAsync3);
    const auto& p = r.path("top.a", "top.s1");
    CHECK(p.mixedClocks == std::vector<std::string>{"clk_c"});
    CHECK(hasHit(p, "CDC-004"));
    REQUIRE(hasHit(p, "CDC-009"));
    CHECK(r.path("top.c", "top.s1").mixedClocks == std::vector<std::string>{"clk_a"});
}

TEST_CASE("CDC-009 negative: destination-domain logic, or already CDC-001", "[CDC-009]") {
    auto local = analyze("module top(input clk_a, input clk_b, input x, input e, output y);\n"
                         "  reg a, en, s1, s2;\n"
                         "  always @(posedge clk_a) a <= x;\n"
                         "  always @(posedge clk_b) begin en <= e; s1 <= a & en; s2 <= s1; end\n"
                         "  assign y = s2;\n"
                         "endmodule\n");
    // Gating by a destination-clock enable has one asynchronous input per bit: no glitch, no CDC-004.
    CHECK(local.hits("CDC-004") == 0);
    CHECK(local.hits("CDC-009") == 0);
    auto unsync = analyze("module top(input clk_a, input clk_b, input clk_c, input x, input z, output reg y);\n"
                          "  reg a, c;\n"
                          "  always @(posedge clk_a) a <= x;\n"
                          "  always @(posedge clk_c) c <= z;\n"
                          "  always @(posedge clk_b) y <= a | c;\n"
                          "endmodule\n",
                          kAsync3);
    CHECK(unsync.hits("CDC-001") == 2);
    CHECK(unsync.hits("CDC-009") == 0);
}

// ===========================================================================
// Rule registry and structures
// ===========================================================================

TEST_CASE("Rule registry holds CDC-001 .. CDC-010 with complete metadata", "[rules]") {
    const auto registry = rules::RuleRegistry::standard();
    REQUIRE(registry.rules().size() == 10);
    for (int i = 1; i <= 10; ++i) {
        const std::string id = std::string("CDC-0") + (i < 10 ? "0" : "") + std::to_string(i);
        const auto* r = registry.find(id);
        REQUIRE(r);
        const auto& m = r->meta();
        CHECK_FALSE(m.title.empty());
        CHECK_FALSE(m.purpose.empty());
        CHECK_FALSE(m.applicability.empty());
        CHECK_FALSE(m.falsePositiveNotes.empty());
        CHECK_FALSE(m.fixSuggestion.empty());
    }
}

TEST_CASE("Unknown clock relation downgrades potential risks to needs-review", "[CDC-010]") {
    auto r = analyze(reconv("y <= {1'b0, sa2 & sb2}"), "");
    const auto& a = r.path("top.a1", "top.sa1");
    REQUIRE(hasHit(a, "CDC-005"));
    CHECK(hasHit(a, "CDC-010"));
    CHECK(a.status == AnalysisStatus::NeedsReview);
}

// ===========================================================================
// End-to-end example (examples/cdc_advanced, see its README)
// ===========================================================================

TEST_CASE("cdc_advanced example: protocol synchronizers and one bug per stage-5 rule", "[cdc][example]") {
    auto r = analyzeExample("cdc_advanced",
                            {"rtl/sync_2ff.v", "rtl/sync_vec.v", "rtl/rst_sync.v", "rtl/pulse_sync.v",
                             "rtl/gray_fifo.v", "rtl/handshake.v", "rtl/cdc_advanced.v"},
                            "cdc_advanced", "constraints/cdc_advanced.sdc");
    CHECK(r.cdc->paths.size() == 16);
    CHECK(r.cdc->structures.size() == 11);
    CHECK(r.cdc->stats.resetSynchronizers == 2);

    // Correct structures: safe.
    const auto& fifo = r.path("cdc_advanced.u_fifo.mem", "cdc_advanced.u_fifo.rdata");
    CHECK(fifo.syncType == SyncType::AsyncFifo);
    CHECK(fifo.status == AnalysisStatus::Safe);
    for (const auto* src : {"cdc_advanced.u_fifo.wptr", "cdc_advanced.u_fifo.rptr"})
        for (const auto& p : r.cdc->paths)
            if (p.srcName == src) {
                CHECK(p.syncType == SyncType::Gray);
                CHECK(p.syncStatus == SyncStatus::NecessaryConditionsVerified);
            }
    CHECK(r.path("cdc_advanced.u_pulse.toggle", "cdc_advanced.u_pulse.u_sync.meta").syncType == SyncType::Toggle);
    const auto& hs = r.path("cdc_advanced.u_hs.data", "cdc_advanced.u_hs.dout");
    CHECK(hs.syncType == SyncType::Handshake);
    CHECK(hs.status == AnalysisStatus::Safe);
    for (const auto& p : r.cdc->paths)
        if (p.srcName == "cdc_advanced.rst_n") {
            CHECK(p.syncType == SyncType::ResetSync);
            CHECK(p.status == AnalysisStatus::Safe);
        }

    // Deliberate bugs.
    const auto& wrongReset = r.path("cdc_advanced.u_rst_a.r", "cdc_advanced.c_y");
    CHECK(hasHit(wrongReset, "CDC-006"));
    CHECK(wrongReset.groupSize == 6);
    CHECK(hasHit(r.path("cdc_advanced.a_start", "cdc_advanced.u_start.meta"), "CDC-005"));
    CHECK(hasHit(r.path("cdc_advanced.a_mode", "cdc_advanced.u_mode.meta"), "CDC-005"));
    CHECK(hasHit(r.path("cdc_advanced.a_kick", "cdc_advanced.u_kick.meta"), "CDC-007"));
    CHECK(hasHit(r.path("cdc_advanced.a_cfg", "cdc_advanced.u_cfg.s1"), "CDC-008"));
    for (const auto* src : {"cdc_advanced.a_x", "cdc_advanced.c_y"}) {
        const auto& p = r.path(src, "cdc_advanced.u_mix.meta");
        CHECK(hasHit(p, "CDC-004"));
        CHECK(hasHit(p, "CDC-009"));
    }
    CHECK(r.hits("CDC-001") + r.hits("CDC-002") + r.hits("CDC-003") + r.hits("CDC-010") == 0);
    CHECK(r.hits("CDC-004") == 2);
    CHECK(r.hits("CDC-005") == 2);
    CHECK(r.hits("CDC-006") == 1);
    CHECK(r.hits("CDC-007") == 1);
    CHECK(r.hits("CDC-008") == 1);
    CHECK(r.hits("CDC-009") == 2);
}

TEST_CASE("CDC-005: every synchronizer meeting in one register gets its partners", "[CDC-005]") {
    // y[i] = s[i] ^ s[i+1]: four synchronizers, each meets its neighbours.
    auto r = analyze("module sy(input clk_a, input clk_b, input d, output reg s2);\n"
                     "  reg a, s1;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; end\n"
                     "endmodule\n"
                     "module top(input clk_a, input clk_b, input [3:0] d, output reg [2:0] y);\n"
                     "  wire [3:0] s;\n"
                     "  genvar i;\n"
                     "  generate for (i = 0; i < 4; i = i + 1) begin : g\n"
                     "    sy u (.clk_a(clk_a), .clk_b(clk_b), .d(d[i]), .s2(s[i]));\n"
                     "  end endgenerate\n"
                     "  always @(posedge clk_b) y <= s[2:0] ^ s[3:1];\n"
                     "endmodule\n");
    CHECK(r.hits("CDC-005") == 4);
    CHECK(r.cdc->stats.reconvergencePoints == 1);
    const auto& inner = r.path("top.g[1].u.a", "top.g[1].u.s1");
    REQUIRE(inner.reconvergences.size() == 1);
    CHECK(inner.reconvergences[0].paths.size() == 2); // g[0] and g[2]
}

TEST_CASE("CDC-004 negative: first stage gated by a destination-clock signal", "[CDC-004]") {
    // A named clear (if (clr) s1 <= 0) becomes a synchronous reset pin, not
    // logic; an AND with a destination-clock enable stays logic.
    auto r = analyze("module top(input clk_a, input clk_b, input [3:0] d, input c, output [3:0] y);\n"
                     "  reg [3:0] g, s1, s2; reg en;\n"
                     "  always @(posedge clk_a) g <= d;\n"
                     "  always @(posedge clk_b) begin en <= c; s1 <= g & {4{en}}; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    const auto& p = r.path("top.g", "top.s1");
    CHECK(p.throughLogic);
    CHECK(p.conditions.front().satisfied);
    CHECK(noteContains(p, "single asynchronous input"));
    CHECK(r.hits("CDC-004") == 0);
}

TEST_CASE("CDC-004 positive: two bits of one source decoded in front of a synchronizer", "[CDC-004]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [1:0] d, output y);\n"
                     "  reg [1:0] a; reg s1, s2;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a == 2'd3; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    CHECK(r.hits("CDC-004") == 1);
}

TEST_CASE("Toggle vector: independent event bits are not a multi-bit value", "[cdc][toggle][CDC-008]") {
    // verilog-ethernet eth_mac_1g_fifo: rx_sync_reg_1 <= rx_sync_reg_1 ^ {bad_fcs, bad_frame};
    auto r = analyze("module top(input clk_a, input clk_b, input [1:0] ev, output [1:0] y);\n"
                     "  reg [1:0] t, s1, s2, s3;\n"
                     "  always @(posedge clk_a) t <= t ^ ev;\n"
                     "  always @(posedge clk_b) begin s1 <= t; s2 <= s1; s3 <= s2; end\n"
                     "  assign y = s2 ^ s3;\n"
                     "endmodule\n");
    const auto& p = r.path("top.t", "top.s1");
    CHECK(p.width == 2);
    CHECK(p.syncType == SyncType::Toggle);
    CHECK(p.unverified.empty());
    CHECK(r.hits("CDC-008") == 0);
}

TEST_CASE("Gray through a mux of Gray-encoded arms; jump not provable", "[cdc][gray]") {
    // verilog-axis frame FIFO: wr_ptr_gray <= bin2gray(commit) or bin2gray(wr_ptr + 1).
    auto r = analyze("module top(input clk_a, input clk_b, input drop, input inc, output reg [3:0] y);\n"
                     "  reg [3:0] bin, commit, gray, s1, s2;\n"
                     "  wire [3:0] nxt = bin + 4'd1;\n"
                     "  always @(posedge clk_a) begin\n"
                     "    if (drop) begin bin <= commit; gray <= commit ^ (commit >> 1); end\n"
                     "    else if (inc) begin bin <= nxt; gray <= nxt ^ (nxt >> 1); commit <= nxt; end\n"
                     "  end\n"
                     "  always @(posedge clk_b) begin s1 <= gray; s2 <= s1; y <= s2 + 4'd1; end\n"
                     "endmodule\n");
    const auto& p = r.path("top.gray", "top.s1");
    CHECK(p.syncType == SyncType::Gray);
    CHECK(p.syncStatus == SyncStatus::ProtocolUnverified); // the drop arm jumps
    CHECK(r.hits("CDC-008") == 0);
}

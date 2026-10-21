// Built-in vendor primitive models (stage 9): Xilinx UNISIM flip-flops and
// buffers, XPM CDC macros, Intel synchronizers, from Verilog and from VHDL.

#include "../TestUtil.h"
#include "cdc/CdcModel.h"
#include "engine/AnalysisSession.h"
#include "frontend/Primitives.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace opencdc;
using namespace opencdc::cdc;

namespace {

constexpr const char* kSdc = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                             "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                             "set_clock_groups -asynchronous -group [get_clocks clk_a] -group [get_clocks clk_b]\n";

struct Run {
    std::shared_ptr<const engine::AnalysisResult> r;
    test::TempDir dir;

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
    [[nodiscard]] std::size_t hits(std::string_view rule) const {
        std::size_t n = 0;
        for (const auto& p : r->cdc->paths)
            for (const auto& h : p.hits)
                n += h.ruleId == rule ? 1U : 0U;
        return n;
    }
};

std::unique_ptr<Run> analyze(const std::string& file, const std::string& text, const std::string& top) {
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "prim";
    cfg.top = top;
    cfg.sources = {run->dir.write(file, text)};
    cfg.sourceLibraries = {""};
    cfg.constraints = {run->dir.write("c.sdc", kSdc)};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    run->r = session.run(cfg, progress, cancel);
    REQUIRE(run->r);
    for (const auto& d : run->r->diagnostics)
        UNSCOPED_INFO(format(d));
    return run;
}

} // namespace

TEST_CASE("Primitive library: models and component packages are embedded", "[primitives]") {
    const auto& lib = frontend::PrimitiveLibrary::instance();
    for (const char* n : {"FDRE", "FDCE", "FDPE", "FDSE", "BUFG", "BUFGMUX", "IBUF", "xpm_cdc_single",
                          "xpm_cdc_array_single", "xpm_cdc_gray", "xpm_cdc_handshake", "xpm_cdc_pulse",
                          "xpm_cdc_async_rst", "xpm_cdc_sync_rst", "xpm_fifo_async", "dcfifo",
                          "altera_std_synchronizer", "IDDR", "ODDR", "IDDRE1", "ODDRE1", "IDELAYE2", "ODELAYE2",
                          "IDELAYE3", "ODELAYE3", "IDELAYCTRL", "RAMB18E1", "RAMB36E1", "RAMB18E2", "RAMB36E2",
                          "xpm_memory_sdpram", "xpm_memory_tdpram", "xpm_memory_spram", "xpm_memory_dpdistram",
                          "altsyncram", "altddio_in", "altddio_out"})
        CHECK(lib.find(n).has_value());
    CHECK(lib.find("fdre") == std::optional<std::string>("FDRE")); // VHDL spelling
    CHECK(lib.find("MMCME2_ADV").has_value()); // clock generators are generated on demand
    CHECK(lib.isClockGenerator("mmcme2_adv"));
    CHECK_FALSE(lib.isClockGenerator("FDRE"));
    CHECK_FALSE(lib.find("XYZ_NOT_A_PRIMITIVE").has_value());
    // a model brings the models it instantiates
    const auto bundle = lib.verilogModel("altera_std_synchronizer_bundle");
    CHECK(bundle.find("module altera_std_synchronizer #") != std::string::npos);
    CHECK(lib.verilogModel("FDRE").find("module FD ") == std::string::npos);
    CHECK(lib.vhdlPackages().size() == 3);
}

TEST_CASE("Primitives: FDRE chain with ASYNC_REG on the instances is a two-flop synchronizer", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, output q);\n"
                       "  reg a; wire m;\n"
                       "  always @(posedge clk_a) a <= d;\n"
                       "  (* ASYNC_REG = \"TRUE\" *) FDRE s1 (.Q(m), .C(clk_b), .CE(1'b1), .D(a), .R(1'b0));\n"
                       "  (* ASYNC_REG = \"TRUE\" *) FDRE s2 (.Q(q), .C(clk_b), .CE(1'b1), .D(m), .R(1'b0));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* p = run->path("top.a", "top.s1.Q");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::TwoFlop);
    CHECK(p->status == AnalysisStatus::Safe);
    CHECK(std::ranges::any_of(p->notes, [](const std::string& n) { return n.find("all stages carry") == 0; }));
    CHECK(run->count("FE-PRIMITIVES") == 1);
}

TEST_CASE("Primitives negative: FDRE with a clock enable is a capture register, not a stage", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, input e, output q);\n"
                       "  reg a, en; wire m;\n"
                       "  always @(posedge clk_a) begin a <= d; en <= e; end\n"
                       "  FDRE s1 (.Q(m), .C(clk_b), .CE(1'b1), .D(a), .R(1'b0));\n"
                       "  FDRE s2 (.Q(q), .C(clk_b), .CE(en), .D(m), .R(1'b0));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* p = run->path("top.a", "top.s1.Q");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::SingleFlop);
    CHECK(run->hits("CDC-001") == 1); // en crosses into the enable of s2
}

TEST_CASE("Primitives: XPM macros from Verilog", "[primitives]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input [7:0] din, input send, output [7:0] dout, output rcv,\n"
        "           input p, output po, input [3:0] cnt, output [3:0] cnto, input we, input re, input rst,\n"
        "           output [7:0] fo);\n"
        "  reg [7:0] data; reg sendr, pr; reg [3:0] c;\n"
        "  always @(posedge clk_a) begin data <= din; sendr <= send; pr <= p; c <= c + {3'b000, we}; end\n"
        "  xpm_cdc_handshake #(.WIDTH(8), .DEST_EXT_HSK(0)) hs (.src_clk(clk_a), .src_in(data), .src_send(sendr),\n"
        "     .src_rcv(rcv), .dest_clk(clk_b), .dest_out(dout), .dest_ack(1'b0));\n"
        "  xpm_cdc_pulse up (.src_clk(clk_a), .src_pulse(pr), .src_rst(1'b0), .dest_clk(clk_b), .dest_rst(1'b0),\n"
        "     .dest_pulse(po));\n"
        "  xpm_cdc_gray #(.WIDTH(4)) ug (.src_clk(clk_a), .src_in_bin(c), .dest_clk(clk_b), .dest_out_bin(cnto));\n"
        "  xpm_fifo_async #(.FIFO_WRITE_DEPTH(16), .WRITE_DATA_WIDTH(8), .READ_DATA_WIDTH(8)) uf (.wr_clk(clk_a),\n"
        "     .rst(rst), .din(din), .wr_en(we), .rd_clk(clk_b), .rd_en(re), .dout(fo), .sleep(1'b0),\n"
        "     .injectsbiterr(1'b0), .injectdbiterr(1'b0));\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    const auto* hs = run->path("top.hs.src_hsdata_ff", "top.hs.dest_hsdata_ff");
    REQUIRE(hs);
    CHECK(hs->syncType == SyncType::Handshake);
    const auto* up = run->path("top.up.src_level_ff", "top.up.syncstages_ff");
    REQUIRE(up);
    CHECK(up->syncType == SyncType::Toggle);
    const auto* g = run->path("top.ug.src_gray_ff", "top.ug.bit_sync[0].syncstages_ff");
    REQUIRE(g);
    CHECK(g->syncType == SyncType::Gray);
    const auto* f = run->path("top.uf.mem", "top.uf.dout_ff");
    REQUIRE(f);
    CHECK(f->syncType == SyncType::AsyncFifo);
    // the FIFO structure holds every pointer bit
    REQUIRE(f->structure < run->r->cdc->structures.size());
    CHECK(run->r->cdc->structures[f->structure].paths.size() == 11);
    CHECK(run->hits("CDC-008") == 0);
    CHECK(run->hits("CDC-005") == 0);
}

TEST_CASE("Primitives: VHDL with unisim and xpm component libraries", "[primitives][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "library unisim; use unisim.vcomponents.all;\n"
                       "library xpm; use xpm.vcomponents.all;\n"
                       "entity top is port (clk_a, clk_b, d : in std_logic; q, so : out std_logic); end;\n"
                       "architecture rtl of top is\n"
                       "  signal a, m : std_logic;\n"
                       "  attribute ASYNC_REG : string;\n"
                       "  attribute ASYNC_REG of s1, s2 : label is \"TRUE\";\n"
                       "begin\n"
                       "  process (clk_a) begin if rising_edge(clk_a) then a <= d; end if; end process;\n"
                       "  s1 : FDRE generic map (INIT => '0') port map (Q => m, C => clk_b, CE => '1', D => a, R => '0');\n"
                       "  s2 : FDRE port map (Q => q, C => clk_b, CE => '1', D => m, R => '0');\n"
                       "  x : xpm_cdc_single generic map (DEST_SYNC_FF => 3, SRC_INPUT_REG => 0)\n"
                       "    port map (src_clk => clk_a, src_in => a, dest_clk => clk_b, dest_out => so);\n"
                       "end;\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* p = run->path("top.a", "top.s1.Q");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::TwoFlop);
    CHECK(std::ranges::any_of(p->notes, [](const std::string& n) { return n.find("all stages carry") == 0; }));
    const auto* x = run->path("top.a", "top.x.syncstages_ff");
    REQUIRE(x);
    CHECK(x->syncType == SyncType::ThreeFlop);
}

TEST_CASE("Primitives: Intel dcfifo and altera_std_synchronizer", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input [7:0] d, input w, input r, input s, output so,\n"
                       "           output [7:0] q);\n"
                       "  reg sr;\n"
                       "  always @(posedge clk_a) sr <= s;\n"
                       "  altera_std_synchronizer #(.depth(2)) u (.clk(clk_b), .reset_n(1'b1), .din(sr), .dout(so));\n"
                       "  dcfifo #(.lpm_width(8), .lpm_widthu(4), .lpm_numwords(16)) f (.aclr(1'b0), .data(d),\n"
                       "     .wrclk(clk_a), .wrreq(w), .rdclk(clk_b), .rdreq(r), .q(q));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* p = run->path("top.sr", "top.u.din_s");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::TwoFlop);
    const auto* f = run->path("top.f.mem", "top.f.q_ff");
    REQUIRE(f);
    CHECK(f->syncType == SyncType::AsyncFifo);
}

TEST_CASE("Primitives negative: a user module of the same name replaces the model", "[primitives]") {
    auto run = analyze("t.v",
                       "module FDRE(output Q, input C, input CE, input D, input R); assign Q = D; endmodule\n"
                       "module top(input clk_a, input clk_b, input d, output q);\n"
                       "  reg a; always @(posedge clk_a) a <= d;\n"
                       "  FDRE s1 (.Q(q), .C(clk_b), .CE(1'b1), .D(a), .R(1'b0));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-PRIMITIVES") == 0);
    CHECK(run->r->cdc->paths.empty()); // the user's FDRE is a wire
}

TEST_CASE("Primitives negative: other unknown modules are still errors", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input d, output q);\n"
                       "  FDRE s1 (.Q(q), .C(clk_a), .CE(1'b1), .D(d), .R(1'b0));\n"
                       "  XYZ_NOT_A_PRIMITIVE pll ();\n"
                       "endmodule\n",
                       "top");
    CHECK_FALSE(run->r->cdc);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 1); // XYZ_NOT_A_PRIMITIVE; FDRE is modelled
    CHECK(run->count("FE-SLANG-UnknownModule") == 0);
}

namespace {
constexpr const char* kMmcmTop =
    "module top(input clk_a, input clk_b, input d, output reg q0, output reg q1, output reg qb);\n"
    "  wire fb, c0u, c1u, c0, c1, locked;\n"
    "  MMCME2_ADV #(.CLKFBOUT_MULT_F(10.0), .CLKIN1_PERIOD(10.0), .CLKOUT0_DIVIDE_F(5.0), .CLKOUT1_DIVIDE(20),\n"
    "               .COMPENSATION(\"ZHOLD\")) mmcm (.CLKIN1(clk_a), .CLKIN2(1'b0), .CLKINSEL(1'b1), .CLKFBIN(fb),\n"
    "     .CLKFBOUT(fb), .CLKOUT0(c0u), .CLKOUT1(c1u), .LOCKED(locked), .RST(1'b0), .PWRDWN(1'b0));\n"
    "  BUFG b0 (.I(c0u), .O(c0));\n"
    "  BUFG b1 (.I(c1u), .O(c1));\n"
    "  reg a;\n"
    "  always @(posedge c0) a <= d;\n"
    "  always @(posedge c1) q1 <= a;\n"
    "  always @(posedge c0) q0 <= a;\n"
    "  always @(posedge clk_b) qb <= a;\n"
    "endmodule\n";
} // namespace

TEST_CASE("Primitives: MMCM outputs are clocks derived from CLKIN1", "[primitives]") {
    auto run = analyze("t.v", kMmcmTop, "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->path("top.a", "top.q1") == nullptr); // CLKOUT0 -> CLKOUT1: same MMCM, synchronous
    CHECK(run->path("top.a", "top.q0") == nullptr);
    const auto* p = run->path("top.a", "top.qb"); // derived from clk_a -> clk_b: asynchronous
    REQUIRE(p);
    CHECK(p->syncType == SyncType::SingleFlop); // unsynchronized: one flop, used directly
    CHECK(p->status != AnalysisStatus::Safe);
    CHECK(run->r->cdc->paths.size() == 1);
}

TEST_CASE("Primitives negative: outputs of MMCMs on independent inputs are asynchronous", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, output reg q);\n"
                       "  wire ca, cb;\n"
                       "  MMCME2_BASE #(.CLKFBOUT_MULT_F(8.0)) ma (.CLKIN1(clk_a), .CLKOUT0(ca), .RST(1'b0), .PWRDWN(1'b0));\n"
                       "  PLLE2_ADV #(.CLKFBOUT_MULT(4)) mb (.CLKIN1(clk_b), .CLKOUT2(cb), .RST(1'b0), .PWRDWN(1'b0));\n"
                       "  reg a;\n"
                       "  always @(posedge ca) a <= d;\n"
                       "  always @(posedge cb) q <= a;\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* p = run->path("top.a", "top.q");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::SingleFlop); // unsynchronized: one flop, used directly
    CHECK(p->status != AnalysisStatus::Safe);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
}

TEST_CASE("Primitives: MMCM from VHDL through unisim", "[primitives][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "library unisim; use unisim.vcomponents.all;\n"
                       "entity top is port (clk_a, clk_b, d : in std_logic; q0, qb : out std_logic); end;\n"
                       "architecture rtl of top is\n"
                       "  signal fb, c0u, c1u, c0, c1, a : std_logic;\n"
                       "begin\n"
                       "  m : MMCME2_ADV generic map (CLKFBOUT_MULT_F => 10.0, CLKOUT0_DIVIDE_F => 5.0, CLKOUT1_DIVIDE => 20)\n"
                       "    port map (CLKIN1 => clk_a, CLKIN2 => '0', CLKINSEL => '1', CLKFBIN => fb, CLKFBOUT => fb,\n"
                       "              CLKOUT0 => c0u, CLKOUT1 => c1u, RST => '0', PWRDWN => '0');\n"
                       "  b0 : BUFG port map (I => c0u, O => c0);\n"
                       "  b1 : BUFG port map (I => c1u, O => c1);\n"
                       "  process (c0) begin if rising_edge(c0) then a <= d; end if; end process;\n"
                       "  process (c1) begin if rising_edge(c1) then q0 <= a; end if; end process;\n"
                       "  process (clk_b) begin if rising_edge(clk_b) then qb <= a; end if; end process;\n"
                       "end;\n",
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->path("top.a", "top.q0") == nullptr);
    const auto* p = run->path("top.a", "top.qb");
    REQUIRE(p);
    CHECK(run->r->cdc->paths.size() == 1);
}

TEST_CASE("Primitives: MMCM LOCKED is asynchronous to every clock", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, output reg q, output reg q2);\n"
                       "  wire c0u, c0, locked;\n"
                       "  MMCME2_BASE m (.CLKIN1(clk_a), .CLKOUT0(c0u), .LOCKED(locked), .RST(1'b0), .PWRDWN(1'b0));\n"
                       "  BUFG b (.I(c0u), .O(c0));\n"
                       "  reg r1, r2;\n" // reset synchronizer released by LOCKED
                       "  always @(posedge c0 or negedge locked) if (!locked) {r2, r1} <= 2'b00; else {r2, r1} <= {r1, 1'b1};\n"
                       "  always @(posedge c0 or negedge r2) if (!r2) q <= 1'b0; else q <= d;\n"
                       "  always @(posedge c0 or negedge locked) if (!locked) q2 <= 1'b0; else q2 <= d;\n" // raw
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    const auto* sync = run->path("top.m.LOCKED", "top.r1");
    REQUIRE(sync);
    CHECK(sync->syncType == SyncType::ResetSync);
    const auto* raw = run->path("top.m.LOCKED", "top.q2");
    REQUIRE(raw);
    CHECK(raw->syncType == SyncType::None);
    CHECK(run->hits("CDC-006") == 1);
}

TEST_CASE("Primitives: MMCM/PLL output periods follow the multiply and divide parameters", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, input e, output reg q, output reg qp);\n"
                       "  wire c0, c1, p2;\n"
                       "  MMCME2_ADV #(.CLKFBOUT_MULT_F(10.0), .DIVCLK_DIVIDE(1), .CLKOUT0_DIVIDE_F(5.0),\n"
                       "    .CLKOUT1_DIVIDE(20), .BANDWIDTH(\"OPTIMIZED\")) m (.CLKIN1(clk_a), .CLKOUT0(c0),\n"
                       "    .CLKOUT1(c1), .RST(1'b0), .PWRDWN(1'b0));\n"
                       "  PLLE2_BASE #(.CLKFBOUT_MULT(8), .DIVCLK_DIVIDE(2), .CLKOUT2_DIVIDE(4), .CLKIN1_PERIOD(7.0))\n"
                       "    p (.CLKIN1(clk_b), .CLKOUT2(p2), .RST(1'b0), .PWRDWN(1'b0));\n"
                       "  reg a, pa, s1, sp;\n"
                       "  always @(posedge c0) a <= d & e;\n" // one-cycle pulses in the 5 ns domain
                       "  always @(posedge clk_b) begin s1 <= a; q <= s1; end\n"
                       "  always @(posedge c1) pa <= d & e;\n" // 20 ns domain: long enough for clk_b
                       "  always @(posedge p2) begin sp <= pa; qp <= sp; end\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    REQUIRE(run->r->clocks);
    auto period = [&](std::string_view name) -> std::optional<double> {
        std::optional<double> found;
        bool exists = false;
        for (const auto& c : run->r->clocks->clocks)
            if (c.name == name) {
                found = c.periodNs;
                exists = true;
            }
        CHECK(exists); // single exit: MSVC C4702 after FAIL()
        return found;
    };
    CHECK(period("top.m.CLKOUT0") == 5.0);  // 10 x 1 x 5 / 10
    CHECK(period("top.m.CLKOUT1") == 20.0); // 10 x 1 x 20 / 10
    CHECK(period("top.p.CLKOUT2") == 7.0);  // master clk_b is 7 ns: 7 x 2 x 4 / 8
    CHECK(run->count("FE-SLANG-UnconnectedOutputPort") == 0);
    const auto* fast = run->path("top.a", "top.s1");
    REQUIRE(fast);
    CHECK(std::ranges::any_of(fast->hits, [](const auto& h) { return h.ruleId == "CDC-007"; })); // 5 < 1.5 x 7
    const auto* slow = run->path("top.pa", "top.sp");
    REQUIRE(slow);
    CHECK(std::ranges::none_of(slow->hits, [](const auto& h) { return h.ruleId == "CDC-007"; })); // 20 >= 1.5 x 7
}

TEST_CASE("Primitives: generator input period from CLKIN1_PERIOD when the input clock has none", "[primitives]") {
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "prim";
    cfg.top = "top";
    cfg.sources = {run->dir.write("t.v", "module top(input clk, input d, output reg q);\n"
                                         "  wire c0;\n"
                                         "  MMCME2_BASE #(.CLKIN1_PERIOD(8.0), .CLKFBOUT_MULT_F(4.0), .CLKOUT0_DIVIDE_F(2.0))\n"
                                         "    m (.CLKIN1(clk), .CLKOUT0(c0), .RST(1'b0), .PWRDWN(1'b0));\n"
                                         "  always @(posedge c0) q <= d;\n"
                                         "endmodule\n")};
    cfg.sourceLibraries = {""};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    run->r = session.run(cfg, progress, cancel);
    REQUIRE(run->r);
    REQUIRE(run->r->clocks);
    bool found = false;
    for (const auto& c : run->r->clocks->clocks)
        if (c.name == "top.m.CLKOUT0") {
            found = true;
            CHECK(c.periodNs == 4.0); // 8 x 1 x 2 / 4
        }
    CHECK(found);
}

namespace {
[[nodiscard]] bool crosses(const Run& run, std::string_view srcPrefix, std::string_view dstPrefix) {
    return std::ranges::any_of(run.r->cdc->paths, [&](const CdcPath& p) {
        return p.srcName.starts_with(srcPrefix) && p.dstName.starts_with(dstPrefix);
    });
}
void dumpPaths(const Run& run) {
    for (const auto& p : run.r->cdc->paths)
        UNSCOPED_INFO("path: " << p.srcName << " -> " << p.dstName);
}
} // namespace

TEST_CASE("Primitives: XPM memories cross clocks only in independent-clock mode", "[primitives]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input [7:0] d, input [3:0] wa, input [3:0] ra, input we,\n"
        "           output [7:0] q1, output [7:0] q2, output [7:0] q3, output [7:0] q4);\n"
        "  reg [7:0] dr; reg [3:0] war, rar, rbr; reg wer;\n"
        "  always @(posedge clk_a) begin dr <= d; war <= wa; wer <= we; end\n"
        "  always @(posedge clk_b) rbr <= ra;\n"
        "  xpm_memory_sdpram #(.ADDR_WIDTH_A(4), .ADDR_WIDTH_B(4), .MEMORY_SIZE(128), .WRITE_DATA_WIDTH_A(8),\n"
        "    .READ_DATA_WIDTH_B(8), .BYTE_WRITE_WIDTH_A(8), .CLOCKING_MODE(\"independent_clock\"),\n"
        "    .READ_LATENCY_B(2)) ind (.clka(clk_a), .ena(1'b1), .wea(wer), .addra(war), .dina(dr),\n"
        "    .clkb(clk_b), .enb(1'b1), .addrb(rbr), .doutb(q1), .rstb(1'b0), .regceb(1'b1), .sleep(1'b0),\n"
        "    .injectsbiterra(1'b0), .injectdbiterra(1'b0));\n"
        "  xpm_memory_sdpram #(.ADDR_WIDTH_A(4), .ADDR_WIDTH_B(4), .MEMORY_SIZE(128), .WRITE_DATA_WIDTH_A(8),\n"
        "    .READ_DATA_WIDTH_B(8), .BYTE_WRITE_WIDTH_A(8), .CLOCKING_MODE(\"common_clock\"))\n"
        "    com (.clka(clk_a), .ena(1'b1), .wea(wer), .addra(war), .dina(dr), .clkb(clk_b), .enb(1'b1),\n"
        "    .addrb(war), .doutb(q2), .rstb(1'b0), .regceb(1'b1), .sleep(1'b0), .injectsbiterra(1'b0),\n"
        "    .injectdbiterra(1'b0));\n"
        "  xpm_memory_tdpram #(.ADDR_WIDTH_A(4), .ADDR_WIDTH_B(4), .MEMORY_SIZE(128), .WRITE_DATA_WIDTH_A(8),\n"
        "    .WRITE_DATA_WIDTH_B(8), .READ_DATA_WIDTH_A(8), .READ_DATA_WIDTH_B(8), .BYTE_WRITE_WIDTH_A(8),\n"
        "    .BYTE_WRITE_WIDTH_B(8), .CLOCKING_MODE(\"independent_clock\")) td (.clka(clk_a), .ena(1'b1),\n"
        "    .wea(wer), .addra(war), .dina(dr), .douta(q3), .clkb(clk_b), .enb(1'b1), .web(1'b0),\n"
        "    .addrb(rbr), .dinb(8'd0), .doutb(q4), .rsta(1'b0), .rstb(1'b0), .regcea(1'b1), .regceb(1'b1));\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(crosses(*run, "top.ind.mem", "top.ind.pipe"));   // written in clk_a, read in clk_b
    CHECK_FALSE(crosses(*run, "top.com.", "top.com."));     // one clock
    CHECK_FALSE(crosses(*run, "top.", "top.com."));
    CHECK(crosses(*run, "top.td.mem_a", "top.td.pb"));      // port A write -> port B read
    CHECK_FALSE(crosses(*run, "top.td.mem_a", "top.td.pa")); // same port, same clock
}

TEST_CASE("Primitives: block RAM, DDR and delay primitives", "[primitives]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input d, input [31:0] din, input [15:0] aw, input [15:0] ar,\n"
        "           output [31:0] dout, output q1, output q2, output o, output reg r, output [31:0] dout2,\n"
        "           output [3:0] casc);\n"
        "  reg [31:0] dr; reg [15:0] awr, arr;\n"
        "  always @(posedge clk_a) begin dr <= din; awr <= aw; end\n"
        "  always @(posedge clk_b) arr <= ar;\n"
        "  RAMB36E1 #(.READ_WIDTH_A(36), .WRITE_WIDTH_B(36), .RAM_MODE(\"TDP\"), .INIT_00(256'h0)) bram (\n"
        "    .CLKARDCLK(clk_b), .ENARDEN(1'b1), .ADDRARDADDR(arr), .DOADO(dout), .WEA(4'b0),\n"
        "    .CLKBWRCLK(clk_a), .ENBWREN(1'b1), .ADDRBWRADDR(awr), .DIBDI(dr), .WEBWE(8'hff),\n"
        "    .DIADI(32'd0), .DIPADIP(4'd0), .DIPBDIP(4'd0), .REGCEAREGCE(1'b1), .REGCEB(1'b1),\n"
        "    .RSTRAMARSTRAM(1'b0), .RSTRAMB(1'b0), .RSTREGARSTREG(1'b0), .RSTREGB(1'b0),\n"
        "    .CASCADEINA(1'b0), .CASCADEINB(1'b0), .INJECTDBITERR(1'b0), .INJECTSBITERR(1'b0));\n"
        "  RAMB36E2 #(.READ_WIDTH_A(36)) bram2 (.CLKARDCLK(clk_b), .ENARDEN(1'b1), .ADDRARDADDR(arr[14:0]),\n"
        "    .DOUTADOUT(dout2), .CLKBWRCLK(clk_a), .ENBWREN(1'b1), .ADDRBWRADDR(awr[14:0]), .DINBDIN(dr),\n"
        "    .WEBWE(8'hff), .CASDINA(32'd0), .CASDOUTPA(casc));\n" // cascade pins are not modelled
        "  IDDR #(.DDR_CLK_EDGE(\"SAME_EDGE_PIPELINED\")) iddr (.Q1(q1), .Q2(q2), .C(clk_a), .CE(1'b1), .D(d),\n"
        "    .R(1'b0), .S(1'b0));\n"
        "  ODDR oddr (.Q(o), .C(clk_b), .CE(1'b1), .D1(q1), .D2(q2), .R(1'b0), .S(1'b0));\n"
        "  wire dly, rdy;\n"
        "  IDELAYE2 #(.IDELAY_TYPE(\"FIXED\")) idl (.IDATAIN(d), .DATAOUT(dly), .C(1'b0), .CE(1'b0),\n"
        "    .CINVCTRL(1'b0), .CNTVALUEIN(5'd0), .DATAIN(1'b0), .INC(1'b0), .LD(1'b0), .LDPIPEEN(1'b0),\n"
        "    .REGRST(1'b0));\n"
        "  IDELAYCTRL ictl (.RDY(rdy), .REFCLK(clk_a), .RST(1'b0));\n"
        "  always @(posedge clk_b or negedge rdy) if (!rdy) r <= 1'b0; else r <= dly;\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 2); // CASDINA, CASDOUTPA
    CHECK(run->count("FE-SLANG-ParameterDoesNotExist") == 0);
    CHECK(crosses(*run, "top.bram.mem_b", "top.bram.da")); // written on port B (clk_a), read on port A (clk_b)
    CHECK(crosses(*run, "top.bram2.mem_b", "top.bram2.da"));
    CHECK(crosses(*run, "top.iddr.", "top.oddr.")); // clk_a DDR capture -> clk_b output register
    CHECK(run->path("top.ictl.RDY", "top.r") != nullptr); // RDY releases a clk_b reset unsynchronized
    CHECK(run->hits("CDC-006") == 1);
}

TEST_CASE("Primitives negative: an unknown port of a user module is still an error", "[primitives]") {
    auto run = analyze("t.v",
                       "module mine(input a, output b); assign b = a; endmodule\n"
                       "module top(input clk_a, input d, output q, output z);\n"
                       "  FDRE f (.Q(q), .C(clk_a), .CE(1'b1), .D(d), .R(1'b0));\n"
                       "  mine m (.a(d), .b(z), .c(d));\n"
                       "endmodule\n",
                       "top");
    CHECK_FALSE(run->r->cdc);
    CHECK(run->count("FE-SLANG-PortDoesNotExist") == 1);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 0);
}

TEST_CASE("Primitives: Intel altsyncram and DDR cells", "[primitives]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input [7:0] d, input [3:0] a, output [7:0] q, output [7:0] q1,\n"
        "           output [1:0] h, output [1:0] l, output [1:0] o);\n"
        "  reg [7:0] dr; reg [3:0] ar, br;\n"
        "  always @(posedge clk_a) begin dr <= d; ar <= a; end\n"
        "  always @(posedge clk_b) br <= a;\n"
        "  altsyncram #(.operation_mode(\"DUAL_PORT\"), .width_a(8), .widthad_a(4), .numwords_a(16), .width_b(8),\n"
        "    .widthad_b(4), .numwords_b(16), .address_reg_b(\"CLOCK1\"), .outdata_reg_b(\"UNREGISTERED\"))\n"
        "    dual (.clock0(clk_a), .wren_a(1'b1), .address_a(ar), .data_a(dr), .clock1(clk_b), .address_b(br),\n"
        "    .q_b(q));\n"
        "  altsyncram #(.operation_mode(\"DUAL_PORT\"), .width_a(8), .widthad_a(4), .numwords_a(16), .width_b(8),\n"
        "    .widthad_b(4), .numwords_b(16), .address_reg_b(\"CLOCK0\"), .outdata_reg_b(\"UNREGISTERED\"))\n"
        "    single (.clock0(clk_a), .wren_a(1'b1), .address_a(ar), .data_a(dr), .clock1(clk_b), .address_b(ar),\n"
        "    .q_b(q1));\n"
        "  altddio_in #(.width(2)) din (.datain(d[1:0]), .inclock(clk_a), .dataout_h(h), .dataout_l(l));\n"
        "  altddio_out #(.width(2)) dout (.datain_h(h), .datain_l(l), .outclock(clk_b), .dataout(o));\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(crosses(*run, "top.dual.mem_a", "top.dual.qb"));
    CHECK_FALSE(crosses(*run, "top.", "top.single."));
    CHECK(crosses(*run, "top.din.", "top.dout."));
}

TEST_CASE("Primitives: VHDL block RAM and XPM memory", "[primitives][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "library unisim; use unisim.vcomponents.all;\n"
                       "library xpm; use xpm.vcomponents.all;\n"
                       "entity top is port (clk_a, clk_b : in std_logic; d : in std_logic_vector(31 downto 0);\n"
                       "  a : in std_logic_vector(15 downto 0); q, q2 : out std_logic_vector(31 downto 0)); end;\n"
                       "architecture rtl of top is\n"
                       "  signal dr : std_logic_vector(31 downto 0); signal ar, br : std_logic_vector(15 downto 0);\n"
                       "begin\n"
                       "  process (clk_a) begin if rising_edge(clk_a) then dr <= d; ar <= a; end if; end process;\n"
                       "  process (clk_b) begin if rising_edge(clk_b) then br <= a; end if; end process;\n"
                       "  b : RAMB36E1 generic map (READ_WIDTH_A => 36, WRITE_WIDTH_B => 36, INIT_00 => (others => '0'))\n"
                       "    port map (CLKARDCLK => clk_b, ENARDEN => '1', ADDRARDADDR => br, DOADO => q,\n"
                       "              CLKBWRCLK => clk_a, ENBWREN => '1', ADDRBWRADDR => ar, DIBDI => dr,\n"
                       "              WEBWE => x\"FF\", WEA => x\"0\", DIADI => (others => '0'), DIPADIP => x\"0\",\n"
                       "              DIPBDIP => x\"0\", REGCEAREGCE => '1', REGCEB => '1', RSTRAMARSTRAM => '0',\n"
                       "              RSTRAMB => '0', RSTREGARSTREG => '0', RSTREGB => '0', CASCADEINA => '0',\n"
                       "              CASCADEINB => '0', INJECTDBITERR => '0', INJECTSBITERR => '0',\n"
                       "              CASCADEOUTA => open, NOTAPORT => open);\n"
                       "  m : xpm_memory_sdpram generic map (ADDR_WIDTH_A => 4, ADDR_WIDTH_B => 4, MEMORY_SIZE => 512,\n"
                       "      WRITE_DATA_WIDTH_A => 32, READ_DATA_WIDTH_B => 32, BYTE_WRITE_WIDTH_A => 32,\n"
                       "      CLOCKING_MODE => \"independent_clock\")\n"
                       "    port map (clka => clk_a, ena => '1', wea => \"1\", addra => ar(3 downto 0), dina => dr,\n"
                       "              clkb => clk_b, enb => '1', addrb => br(3 downto 0), doutb => q2, rstb => '0',\n"
                       "              regceb => '1', sleep => '0', injectsbiterra => '0', injectdbiterra => '0');\n"
                       "end;\n",
                       "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 1); // NOTAPORT
    CHECK(crosses(*run, "top.b.mem_b", "top.b.da"));
    CHECK(crosses(*run, "top.m.mem", "top.m.pipe"));
}

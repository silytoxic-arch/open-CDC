// Built-in vendor primitive models (stage 9): Xilinx UNISIM flip-flops and
// buffers, XPM CDC macros, Intel synchronizers, from Verilog and from VHDL.

#include "../TestUtil.h"
#include "cdc/CdcModel.h"
#include "engine/AnalysisSession.h"
#include "frontend/Primitives.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
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

std::unique_ptr<Run> analyze(const std::string& file, const std::string& text, const std::string& top,
                             const std::string& sdc = kSdc) {
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "prim";
    cfg.top = top;
    cfg.sources = {run->dir.write(file, text)};
    cfg.sourceLibraries = {""};
    cfg.constraints = {run->dir.write("c.sdc", sdc)};
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
                          "xpm_cdc_async_rst", "xpm_cdc_sync_rst", "xpm_fifo_async", "xpm_fifo_sync", "dcfifo",
                          "altera_std_synchronizer", "IDDR", "ODDR", "IDDRE1", "ODDRE1", "IDELAYE2", "ODELAYE2",
                          "IDELAYE3", "ODELAYE3", "IDELAYCTRL", "RAMB18E1", "RAMB36E1", "RAMB18E2", "RAMB36E2",
                          "xpm_memory_sdpram", "xpm_memory_tdpram", "xpm_memory_spram", "xpm_memory_dpdistram",
                          "altsyncram", "altddio_in", "altddio_out", "BUFGCE_DIV", "ISERDESE2", "OSERDESE2",
                          "ISERDESE3", "OSERDESE3", "URAM288", "altera_syncram"})
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
    std::set<std::string> libs;
    for (const auto& pkg : lib.vhdlPackages())
        libs.insert(pkg.library);
    for (const char* l : {"unisim", "xpm", "altera_mf", "ecp5u", "sb_ice40_components_syn", "ice40up", "gw1n", "gw2a"})
        CHECK(libs.contains(l));
    for (const char* n : {"DP16KD", "IDDRX1F", "ODDRX1F", "SB_RAM40_4K", "SB_RAM40_4KNRNW", "SDPB", "DPB"})
        CHECK(lib.find(n).has_value());
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
    const auto* p = run->path("top.sr", "top.u.din_s1");
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
    for (const auto& c : run->r->clocks->clocks)
        if (c.name == "top.m.CLKOUT1") {
            CHECK(c.periodRatio == 2.0);
            CHECK(c.periodBasis == "clk_a");
        }
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
            CHECK(c.periodRatio == 0.5);
            CHECK(c.periodBasis == "CLKIN1_PERIOD"); // the input clock has no constraint
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
    REQUIRE(run->count("FE-PRIMITIVE-PORT") == 1); // one note for the instance
    for (const auto& d : run->r->diagnostics)
        if (d.code == "FE-PRIMITIVE-PORT") {
            CHECK(d.message.find("2 ports of vendor primitive 'RAMB36E2'") == 0);
            CHECK(d.message.find("CASDINA, CASDOUTPA") != std::string::npos);
        }
    CHECK(run->count("FE-SLANG-ParameterDoesNotExist") == 0);
    CHECK(crosses(*run, "top.bram.mem_b", "top.bram.da")); // written on port B (clk_a), read on port A (clk_b)
    CHECK_FALSE(crosses(*run, "top.bram.mem_a", "top.bram.db")); // WEA = 0: port A never writes
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

TEST_CASE("Primitives: BUFR / BUFGCE_DIV dividers, SERDES and UltraRAM", "[primitives]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input d, input [7:0] w, output [7:0] q, output o,\n"
        "           output [71:0] u, output reg r);\n"
        "  wire div4, byp, div2;\n"
        "  BUFR #(.BUFR_DIVIDE(\"4\")) br (.I(clk_a), .O(div4), .CE(1'b1), .CLR(1'b0));\n"
        "  BUFR #(.BUFR_DIVIDE(\"BYPASS\")) bb (.I(clk_a), .O(byp), .CE(1'b1), .CLR(1'b0));\n"
        "  BUFGCE_DIV #(.BUFGCE_DIVIDE(2)) bd (.I(clk_b), .O(div2), .CE(1'b1), .CLR(1'b0));\n"
        "  ISERDESE2 #(.DATA_WIDTH(8), .IOBDELAY(\"NONE\")) is (.D(d), .CLK(clk_a), .CLKB(~clk_a), .CLKDIV(div4),\n"
        "    .CE1(1'b1), .CE2(1'b1), .RST(1'b0), .BITSLIP(1'b0), .Q1(q[0]), .Q2(q[1]), .Q3(q[2]), .Q4(q[3]),\n"
        "    .Q5(q[4]), .Q6(q[5]), .Q7(q[6]), .Q8(q[7]));\n"
        "  reg [7:0] wb;\n"
        "  always @(posedge div2) wb <= w;\n" // clk_b domain data into an OSERDES clocked from clk_a
        "  OSERDESE2 #(.DATA_WIDTH(8)) os (.OQ(o), .CLK(byp), .CLKDIV(div4), .D1(wb[0]), .D2(wb[1]), .D3(wb[2]),\n"
        "    .D4(wb[3]), .D5(wb[4]), .D6(wb[5]), .D7(wb[6]), .D8(wb[7]), .OCE(1'b1), .TCE(1'b0), .RST(1'b0));\n"
        "  URAM288 ur (.CLK(clk_a), .ADDR_A(23'd0), .DIN_A(72'd0), .EN_A(1'b1), .RDB_WR_A(1'b0), .DOUT_A(u),\n"
        "    .CAS_IN_ADDR_A(23'd0));\n"
        "  always @(posedge byp) r <= q[0];\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    REQUIRE(run->r->clocks);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 1); // CAS_IN_ADDR_A
    std::map<std::string, std::optional<double>> periods;
    for (const auto& c : run->r->clocks->clocks)
        periods[c.name] = c.periodNs;
    CHECK(periods.size() == 4); // clk_a, clk_b, br.O, bd.O; BYPASS is clk_a itself
    CHECK(periods["top.br.O"] == 40.0);
    CHECK(periods["top.bd.O"] == 14.0);
    CHECK_FALSE(crosses(*run, "top.is.", "top.is.")); // CLK and CLKDIV from one source
    CHECK_FALSE(crosses(*run, "top.is.", "top.r"));
    CHECK(crosses(*run, "top.wb", "top.os.word")); // clk_b -> clk_a
    CHECK_FALSE(crosses(*run, "top.ur.", "top.ur."));
}

TEST_CASE("Primitives: BUFG_GT divides by its DIV port + 1", "[primitives]") {
    // SURF TenGigEthGthUltraScaleRst: CLK312_BUFG_GT (DIV "000") and CLK156_BUFG_GT (DIV "001") on TXOUTCLK
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input [2:0] sel, input d, output reg q0, output reg q1,\n"
                       "           output reg q3, output reg qx);\n"
                       "  wire c0, c1, c3, cx;\n"
                       "  BUFG_GT b0 (.O(c0), .CE(1'b1), .CEMASK(1'b0), .CLR(1'b0), .CLRMASK(1'b0), .DIV(3'b000), .I(clk_a));\n"
                       "  BUFG_GT b1 (.O(c1), .CE(1'b1), .CEMASK(1'b0), .CLR(1'b0), .CLRMASK(1'b0), .DIV(3'b001), .I(clk_a));\n"
                       "  BUFG_GT b3 (.O(c3), .CE(1'b1), .CEMASK(1'b0), .CLR(1'b0), .CLRMASK(1'b0), .DIV(3'd3), .I(clk_a));\n"
                       "  BUFG_GT bx (.O(cx), .CE(1'b1), .CEMASK(1'b0), .CLR(1'b0), .CLRMASK(1'b0), .DIV(sel), .I(clk_b));\n"
                       "  always @(posedge c0) q0 <= d;\n"
                       "  always @(posedge c1) q1 <= q0;\n"
                       "  always @(posedge c3) q3 <= q1;\n"
                       "  always @(posedge cx) qx <= d;\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->clocks);
    std::map<std::string, std::optional<double>> periods;
    for (const auto& c : run->r->clocks->clocks)
        periods[c.name] = c.periodNs;
    for (const auto& [n, p] : periods)
        UNSCOPED_INFO(n << " " << (p ? std::to_string(*p) : "-"));
    // DIV 0: the input clock itself (no clock of its own); DIV 1: x2; DIV 3: x4
    CHECK_FALSE(periods.contains("top.b0.O"));
    CHECK(periods["top.b1.O"] == 20.0);
    CHECK(periods["top.b3.O"] == 40.0);
    // negative: a DIV that is not constant gives a divided clock without a period, reported
    REQUIRE(periods.contains("top.bx.O"));
    CHECK_FALSE(periods["top.bx.O"].has_value());
    CHECK(run->count("CLK-DIVIDE-UNKNOWN") == 1);
    // the divider register of b0 (DIV 0) belongs to no domain: it is not part of the design
    const auto& regs = run->r->clocks->registers;
    CHECK(std::ranges::count_if(regs, [](const clock::RegisterClocking& rc) {
              return rc.clocks.empty() && rc.reason.find("passes its input through") != std::string::npos;
          }) == 1);
    CHECK(run->r->clocks->resolvedRegisters() == regs.size());
    // q0 (clk_a) -> q1 (clk_a / 2): synchronous, no crossing
    REQUIRE(run->r->cdc);
    CHECK(run->path("top.q0", "top.q1") == nullptr);
}

TEST_CASE("Primitives: IBUFDS_GTE ODIV2 dividers", "[primitives]") {
    // SURF TenGigEthGtx7Clk: refClockDiv2 from IBUFDS_GTE2 ODIV2; UltraScale REFCLK_HROW_CK_SEL
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, output reg q2, output reg q3, output reg q4,\n"
                       "           output reg q5);\n"
                       "  wire o2, h2, h3, z3, u;\n"
                       "  IBUFDS_GTE2 g2 (.O(u), .ODIV2(h2), .CEB(1'b0), .I(clk_a), .IB(1'b0));\n"
                       "  IBUFDS_GTE3 #(.REFCLK_HROW_CK_SEL(2'b01)) g3 (.O(), .ODIV2(h3), .CEB(1'b0), .I(clk_b), .IB(1'b0));\n"
                       "  IBUFDS_GTE4 #(.REFCLK_HROW_CK_SEL(2'b00)) g4 (.O(), .ODIV2(o2), .CEB(1'b0), .I(clk_b), .IB(1'b0));\n"
                       "  IBUFDS_GTE4 #(.REFCLK_HROW_CK_SEL(2'b10)) g5 (.O(), .ODIV2(z3), .CEB(1'b0), .I(clk_b), .IB(1'b0));\n"
                       "  always @(posedge h2) q2 <= d;\n"
                       "  always @(posedge h3) q3 <= d;\n"
                       "  always @(posedge o2) q4 <= d;\n"
                       "  always @(posedge z3) q5 <= d;\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->clocks);
    std::map<std::string, std::optional<double>> periods;
    for (const auto& c : run->r->clocks->clocks)
        periods[c.name] = c.periodNs;
    CHECK(periods["top.g2.ODIV2"] == 20.0); // clk_a 10 ns / 2
    CHECK(periods["top.g3.ODIV2"] == 14.0); // clk_b 7 ns / 2
    // negative: 00 is clk_b itself, 10 drives a constant (q5 has no clock)
    CHECK_FALSE(periods.contains("top.g4.ODIV2"));
    CHECK_FALSE(periods.contains("top.g5.ODIV2"));
    CHECK(run->count("CLK-CONSTANT") == 1);
}

TEST_CASE("Primitives: BUFG_GT divider from VHDL through unisim", "[primitives][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee;\nuse ieee.std_logic_1164.all;\nlibrary unisim;\nuse unisim.vcomponents.all;\n"
                       "entity top is port (clk_a, d : in std_logic; q : out std_logic); end;\n"
                       "architecture rtl of top is\n  signal c : std_logic;\nbegin\n"
                       "  b : BUFG_GT port map (I => clk_a, CE => '1', CEMASK => '1', CLR => '0', CLRMASK => '1',\n"
                       "                        DIV => \"001\", O => c);\n"
                       "  process (c) begin if rising_edge(c) then q <= d; end if; end process;\n"
                       "end;\n",
                       "top");
    REQUIRE(run->r->clocks);
    bool found = false;
    for (const auto& c : run->r->clocks->clocks)
        if (c.name == "top.b.O") {
            REQUIRE(c.periodNs);
            CHECK(*c.periodNs == 20.0);
            found = true;
        }
    CHECK(found);
    CHECK(run->count("VHDL-COMPONENT") == 0);
}

TEST_CASE("Primitives: Intel altera_syncram is altsyncram", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input [7:0] d, input [3:0] a, output [7:0] q);\n"
                       "  reg [7:0] dr; reg [3:0] ar, br;\n"
                       "  always @(posedge clk_a) begin dr <= d; ar <= a; end\n"
                       "  always @(posedge clk_b) br <= a;\n"
                       "  altera_syncram #(.operation_mode(\"DUAL_PORT\"), .width_a(8), .widthad_a(4), .numwords_a(16),\n"
                       "    .width_b(8), .widthad_b(4), .numwords_b(16), .address_reg_b(\"CLOCK1\"),\n"
                       "    .enable_force_to_zero(\"FALSE\")) m (.clock0(clk_a), .wren_a(1'b1), .address_a(ar),\n"
                       "    .data_a(dr), .clock1(clk_b), .address_b(br), .q_b(q));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(crosses(*run, "top.m.mem_a", "top.m.qb"));
}

TEST_CASE("Registers whose enable is constant 0 through the hierarchy are no sources", "[primitives]") {
    auto run = analyze("t.v",
                       "module holder(input clk, input we, input [1:0] wm, input d, output reg q);\n"
                       "  always @(posedge clk) if (we && |wm) q <= d;\n"
                       "endmodule\n"
                       "module top(input clk_a, input clk_b, input d, input e, output reg o1, output reg o2);\n"
                       "  wire q1, q2;\n"
                       "  holder off (.clk(clk_a), .we(1'b1), .wm(2'b00), .d(d), .q(q1));\n" // never loads
                       "  holder on (.clk(clk_a), .we(1'b1), .wm({e, 1'b0}), .d(d), .q(q2));\n" // negative: e decides
                       "  always @(posedge clk_b) begin o1 <= q1; o2 <= q2; end\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK_FALSE(crosses(*run, "top.off.q", "top.o1"));
    CHECK(crosses(*run, "top.on.q", "top.o2"));
}

namespace {
constexpr const char* kGtTop =
    "module top(input refclk_p, input refclk_n, input rxp, input rxn, output txp, output txn, input clk_a,\n"
    "           input [31:0] tx_in, output reg [31:0] rx_out, output reg flag);\n"
    "  wire refclk, rxoutclk, txoutclk, rxusrclk, txusrclk, rxdone;\n"
    "  wire [63:0] rxdata;\n"
    "  reg [31:0] tx_q;\n"
    "  IBUFDS_GTE2 rb (.I(refclk_p), .IB(refclk_n), .CEB(1'b0), .O(refclk));\n"
    "  GTXE2_CHANNEL ch (.GTREFCLK0(refclk), .GTXRXP(rxp), .GTXRXN(rxn), .GTXTXP(txp), .GTXTXN(txn),\n"
    "    .RXOUTCLK(rxoutclk), .TXOUTCLK(txoutclk), .RXUSRCLK(rxusrclk), .RXUSRCLK2(rxusrclk),\n"
    "    .TXUSRCLK(txusrclk), .TXUSRCLK2(txusrclk), .TXDATA({32'd0, tx_q}), .RXDATA(rxdata),\n"
    "    .RXRESETDONE(rxdone), .RXBUFSTATUS());\n"
    "  BUFG b1 (.I(rxoutclk), .O(rxusrclk));\n"
    "  BUFG b2 (.I(txoutclk), .O(txusrclk));\n"
    "  always @(posedge clk_a) tx_q <= tx_in;\n"
    "  always @(posedge rxusrclk) rx_out <= rxdata[31:0];\n"
    "  always @(posedge clk_a or negedge rxdone) if (!rxdone) flag <= 1'b0; else flag <= 1'b1;\n"
    "endmodule\n";
} // namespace

TEST_CASE("Primitives: GT transceiver clocks are sources of their own", "[primitives][gt]") {
    auto run = analyze("t.v", kGtTop, "top", "create_clock -name clk_a -period 10 [get_ports clk_a]\n");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 1); // RXBUFSTATUS
    CHECK(run->count("CLK-UNRESOLVED") == 0);
    std::vector<std::string> names;
    for (const auto& c : run->r->clocks->clocks)
        names.push_back(c.name);
    CHECK(std::ranges::find(names, "top.ch.RXOUTCLK") != names.end());
    CHECK(std::ranges::find(names, "top.ch.TXOUTCLK") != names.end());
    // unconstrained: the relation to clk_a is unknown, the crossing needs review
    const auto* p = run->path("top.tx_q", "top.ch.tx_q");
    REQUIRE(p);
    CHECK(std::ranges::any_of(p->hits, [](const auto& h) { return h.ruleId == "CDC-010"; }));
    CHECK(run->hits("CDC-006") == 1); // RXRESETDONE releases a clk_a reset unsynchronized
}

TEST_CASE("Primitives: GT output pins constrained like in Vivado", "[primitives][gt]") {
    auto run = analyze("t.v", kGtTop, "top",
                       "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                       "create_clock -name rxclk -period 3.2 [get_pins ch/RXOUTCLK]\n"
                       "create_clock -name txclk -period 3.2 [get_pins ch/TXOUTCLK]\n"
                       "set_clock_groups -asynchronous -group clk_a -group rxclk -group txclk\n");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    const auto* p = run->path("top.tx_q", "top.ch.tx_q");
    REQUIRE(p);
    CHECK(p->dstClockName == "txclk");
    CHECK(std::ranges::any_of(p->hits, [](const auto& h) { return h.ruleId == "CDC-003"; }));
    CHECK(run->path("top.ch.rx_q", "top.rx_out") == nullptr); // same recovered clock
}

TEST_CASE("Primitives: UltraScale GT channel and common from VHDL", "[primitives][gt][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "library unisim; use unisim.vcomponents.all;\n"
                       "entity top is port (clk_a, refclk_p, refclk_n, rxp, rxn : in std_logic; txp, txn : out std_logic;\n"
                       "  q : out std_logic_vector(127 downto 0)); end;\n"
                       "architecture rtl of top is\n"
                       "  signal refclk, rxoutclk, rxusr, qclk, qref : std_logic;\n"
                       "  signal rxdata : std_logic_vector(127 downto 0);\n"
                       "begin\n"
                       "  rb : IBUFDS_GTE4 port map (I => refclk_p, IB => refclk_n, CEB => '0', O => refclk, ODIV2 => open);\n"
                       "  cm : GTYE4_COMMON port map (GTREFCLK00 => refclk, QPLL0OUTCLK => qclk, QPLL0OUTREFCLK => qref,\n"
                       "                              QPLL0LOCKDETCLK => clk_a, QPLL0RESET => '0', BGBYPASSB => '1');\n"
                       "  ch : GTYE4_CHANNEL generic map (SIM_RECEIVER_DETECT_PASS => \"TRUE\", RX_DATA_WIDTH => 128)\n"
                       "    port map (GTREFCLK0 => refclk, QPLL0CLK => qclk, QPLL0REFCLK => qref, GTYRXP => rxp,\n"
                       "              GTYRXN => rxn, GTYTXP => txp, GTYTXN => txn, RXOUTCLK => rxoutclk,\n"
                       "              RXUSRCLK => rxusr, RXUSRCLK2 => rxusr, RXDATA => rxdata, TXDATA => (others => '0'));\n"
                       "  b : BUFG_GT port map (I => rxoutclk, O => rxusr, CE => '1', CEMASK => '0', CLR => '0',\n"
                       "                        CLRMASK => '0', DIV => \"000\");\n"
                       "  process (rxusr) begin if rising_edge(rxusr) then q <= rxdata; end if; end process;\n"
                       "end;\n",
                       "top", "create_clock -name clk_a -period 10 [get_ports clk_a]\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 1); // BGBYPASSB
    CHECK(run->r->cdc->paths.empty()); // RX data stays in the recovered clock domain
    bool rx = false;
    for (const auto& c : run->r->clocks->clocks)
        rx = rx || c.name == "top.ch.RXOUTCLK";
    CHECK(rx);
}

TEST_CASE("Primitive library: every model is documented in docs/primitives.md", "[primitives][docs]") {
    std::ifstream in(std::filesystem::path(OPENCDC_TESTS_DIR) / ".." / "docs" / "primitives.md");
    REQUIRE(in);
    const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto& lib = frontend::PrimitiveLibrary::instance();
    for (const auto& n : lib.names()) {
        if (lib.isClockGenerator(n))
            continue; // documented as MMCME2/3/4_BASE ...
        INFO(n);
        CHECK(doc.find("`" + n + "`") != std::string::npos);
    }
    CHECK(doc.find("`MMCME2/3/4_BASE`") != std::string::npos);
    CHECK(doc.find("`PLLE2/3/4_ADV`") != std::string::npos);
}

TEST_CASE("Primitive library: every Xilinx unisim model has a VHDL component in unisim.vcomponents",
          "[primitives][vhdl]") {
    // otherwise VHDL designs that use the primitive through 'use unisim.vcomponents.all'
    // get VHDL-COMPONENT warnings (SURF's BUFG_GT in Pgp2fcGtyCoreWrapper)
    std::ifstream in(std::filesystem::path(OPENCDC_TESTS_DIR) / ".." / "src" / "frontend" / "primitives" /
                     "xilinx_unisim.v");
    REQUIRE(in);
    const std::string models((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string pkg;
    for (const auto& p : frontend::PrimitiveLibrary::instance().vhdlPackages())
        if (p.library == "unisim")
            pkg += p.text;
    REQUIRE_FALSE(pkg.empty());
    std::string upper = pkg;
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const std::regex moduleRe(R"(^module (\w+))", std::regex::multiline);
    int count = 0;
    for (auto it = std::sregex_iterator(models.begin(), models.end(), moduleRe); it != std::sregex_iterator(); ++it) {
        std::string n = (*it)[1].str();
        std::ranges::transform(n, n.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        INFO(n);
        CHECK(upper.find("COMPONENT " + n + " ") != std::string::npos);
        ++count;
    }
    CHECK(count > 50);
    // negative: a name that is not a model is not in the package
    CHECK(upper.find("COMPONENT NOT_A_PRIMITIVE ") == std::string::npos);
}

TEST_CASE("Primitives: Intel, Lattice and Gowin PLL output periods", "[primitives][pll]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input d, output [5:0] q);\n"
        "  wire [4:0] ac; wire [1:0] ap; wire op, os, ice, g, gd;\n"
        "  altpll #(.inclk0_input_frequency(20000), .clk0_multiply_by(5), .clk0_divide_by(2), .width_clock(5))\n"
        "    a1 (.inclk({1'b0, clk_b}), .clk(ac), .areset(1'b0));\n" // 20 ns x 2/5 = 8 ns
        "  altera_pll #(.reference_clock_frequency(\"50.0 MHz\"), .number_of_clocks(2),\n"
        "    .output_clock_frequency0(\"125.000000 MHz\"), .output_clock_frequency1(\"25 MHz\"))\n"
        "    a2 (.refclk(clk_b), .rst(1'b0), .outclk(ap));\n"
        "  EHXPLLL #(.CLKI_DIV(1), .CLKFB_DIV(4), .CLKOP_DIV(6), .CLKOS_DIV(12), .FREQUENCY_PIN_CLKI(25.0))\n"
        "    l1 (.CLKI(clk_b), .CLKFB(op), .CLKOP(op), .CLKOS(os), .RST(1'b0), .STDBY(1'b0));\n"
        "  SB_PLL40_CORE #(.DIVR(4'd0), .DIVF(7'd66), .DIVQ(3'd5)) i1 (.REFERENCECLK(clk_a), .PLLOUTCORE(ice),\n"
        "    .RESETB(1'b1), .BYPASS(1'b0));\n"
        "  rPLL #(.FCLKIN(\"27\"), .IDIV_SEL(3), .FBDIV_SEL(54), .DYN_SDIV_SEL(2)) g1 (.CLKIN(clk_b), .CLKOUT(g),\n"
        "    .CLKOUTD(gd), .RESET(1'b0), .RESET_P(1'b0));\n"
        "  reg [5:0] r;\n"
        "  always @(posedge ac[0]) r[0] <= d;\n"
        "  always @(posedge ap[0]) r[1] <= d;\n"
        "  always @(posedge op) r[2] <= d;\n"
        "  always @(posedge os) r[3] <= d;\n"
        "  always @(posedge ice) r[4] <= d;\n"
        "  always @(posedge gd) r[5] <= d;\n"
        "  assign q = r;\n"
        "endmodule\n",
        "top", "create_clock -name clk_a -period 83.333 [get_ports clk_a]\n");
    REQUIRE(run->r->clocks);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    std::map<std::string, double> period;
    for (const auto& c : run->r->clocks->clocks)
        if (c.periodNs)
            period[c.name] = *c.periodNs;
    auto near = [&](const char* name, double expected) {
        INFO(name);
        REQUIRE(period.contains(name));
        CHECK(std::abs(period[name] - expected) < 1e-3 * expected);
    };
    near("top.a1.clk[0]", 8.0);                       // inclk0 period 20 ns x 2 / 5
    near("top.a2.outclk[0]", 8.0);                    // 1000 / 125 MHz
    near("top.l1.CLKOP", 10.0);                       // 40 ns x 1 / 4
    near("top.l1.CLKOS", 20.0);                       // CLKOP x 12 / 6
    near("top.i1.PLLOUTCORE", 83.333 * 32.0 / 67.0);  // master clk_a x (DIVR+1) 2^DIVQ / (DIVF+1)
    near("top.g1.CLKOUTD", 1000.0 / 27.0 * 4.0 / 55.0 * 2.0);
}

TEST_CASE("Constraint checks: generator input period and constrained generator outputs", "[primitives][checks]") {
    const char* v = "module top(input clk_a, input d, output reg q);\n"
                    "  wire c0;\n"
                    "  MMCME2_BASE #(.CLKIN1_PERIOD(8.0), .CLKFBOUT_MULT_F(10.0), .CLKOUT0_DIVIDE_F(5.0))\n"
                    "    m (.CLKIN1(clk_a), .CLKOUT0(c0), .RST(1'b0), .PWRDWN(1'b0));\n"
                    "  always @(posedge c0) q <= d;\n"
                    "endmodule\n";
    // the input clock is 10 ns, the MMCM was configured for 8 ns; c0 is constrained to 4 ns but is 5 ns
    auto bad = analyze("t.v", v, "top",
                       "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                       "create_clock -name fast -period 4 [get_nets c0]\n");
    CHECK(bad->count("CON-INPUT-PERIOD") == 0); // c0's own constraint stops the derived clock
    CHECK(bad->count("CON-PERIOD-MISMATCH") == 1);
    auto in = analyze("t.v", v, "top", "create_clock -name clk_a -period 10 [get_ports clk_a]\n");
    CHECK(in->count("CON-INPUT-PERIOD") == 1);
    // negative: matching constraints are silent
    auto good = analyze("t.v", v, "top",
                        "create_clock -name clk_a -period 8 [get_ports clk_a]\n"
                        "create_clock -name fast -period 4 [get_nets c0]\n");
    CHECK(good->count("CON-INPUT-PERIOD") == 0);
    CHECK(good->count("CON-PERIOD-MISMATCH") == 0);
}

TEST_CASE("Primitives: Lattice and Gowin block RAMs and DDR cells", "[primitives][lattice]") {
    std::string dp16 = "  DP16KD #(.DATA_WIDTH_A(18), .DATA_WIDTH_B(18)) ram (.CLKA(clk_a), .CEA(1'b1), .WEA(1'b1),\n"
                       "    .CLKB(clk_b), .CEB(1'b1), .WEB(1'b0), .OCEA(1'b1), .OCEB(1'b1), .RSTA(1'b0), .RSTB(1'b0),\n"
                       "    .CSA0(1'b0), .CSA1(1'b0), .CSA2(1'b0), .CSB0(1'b0), .CSB1(1'b0), .CSB2(1'b0),\n"
                       "    .DIA0(dr[0]), .DIA1(dr[1]), .ADA4(ar[0]), .ADA5(ar[1]), .ADB4(br[0]), .ADB5(br[1]),\n"
                       "    .DOB0(q[0]), .DOB1(q[1]));\n";
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input d, input [15:0] din, input [10:0] a,\n"
        "           output [1:0] q, output [15:0] q2, output [31:0] q3, output [15:0] q4, output o, output reg r);\n"
        "  reg [15:0] dr; reg [10:0] ar, br;\n"
        "  always @(posedge clk_a) begin dr <= din; ar <= a; end\n"
        "  always @(posedge clk_b) br <= a;\n" +
            dp16 +
            "  SB_RAM40_4K ice (.WCLK(clk_a), .WCLKE(1'b1), .WE(1'b1), .WADDR(ar), .WDATA(dr), .MASK(16'h0),\n"
            "    .RCLK(clk_b), .RCLKE(1'b1), .RE(1'b1), .RADDR(br), .RDATA(q2));\n"
            "  SDPB #(.BIT_WIDTH_0(32), .BIT_WIDTH_1(32)) sdp (.CLKA(clk_a), .CEA(1'b1), .ADA({ar, 3'b0}),\n"
            "    .DI({dr, dr}), .CLKB(clk_b), .CEB(1'b1), .OCE(1'b1), .ADB({br, 3'b0}), .DO(q3), .RESETA(1'b0),\n"
            "    .RESETB(1'b0), .BLKSELA(3'b0), .BLKSELB(3'b0));\n"
            "  DPB dpb (.CLKA(clk_a), .CEA(1'b1), .WREA(1'b1), .ADA({ar, 3'b0}), .DIA(dr), .CLKB(clk_b),\n"
            "    .CEB(1'b1), .WREB(1'b0), .ADB({br, 3'b0}), .DOB(q4), .DIB(16'h0));\n"
            "  wire q0, q1;\n"
            "  IDDRX1F iddr (.D(d), .SCLK(clk_a), .RST(1'b0), .Q0(q0), .Q1(q1));\n"
            "  ODDRX1F oddr (.D0(q0), .D1(q1), .SCLK(clk_a), .RST(1'b0), .Q(o));\n"
            "  always @(posedge clk_b) r <= q1;\n"
            "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(run->count("FE-PRIMITIVE-PORT") == 0);
    CHECK(crosses(*run, "top.ram.mem_a", "top.ram.dob"));       // written on CLKA, read on CLKB
    CHECK_FALSE(crosses(*run, "top.ram.mem_b", "top.ram.doa")); // WEB = 0: port B never writes
    CHECK(crosses(*run, "top.ice.mem", "top.ice.q"));
    CHECK(crosses(*run, "top.sdp.mem", "top.sdp.q"));
    CHECK(crosses(*run, "top.dpb.mem_a", "top.dpb.qb"));
    CHECK_FALSE(crosses(*run, "top.dpb.mem_b", "top.dpb.qa"));
    CHECK_FALSE(crosses(*run, "top.iddr.", "top.oddr."));  // same clock
    CHECK(run->path("top.iddr.Q1", "top.r") != nullptr);   // clk_a DDR output -> clk_b register
}

TEST_CASE("Primitives: Lattice and Gowin cells from VHDL component libraries", "[primitives][lattice][vhdl]") {
    for (const auto& [lib, cells] :
         std::vector<std::pair<std::string, std::string>>{
             {"ecp5u",
              "  p : EHXPLLL generic map (CLKI_DIV => 1, CLKFB_DIV => 4, CLKOP_DIV => 6, CLKOS_DIV => 12)\n"
              "    port map (CLKI => clk_a, CLKFB => op, CLKOP => op, CLKOS => os, RST => '0', STDBY => '0');\n"
              "  m : DP16KD port map (CLKA => os, CEA => '1', WEA => '1', DIA0 => d, CLKB => clk_b, CEB => '1',\n"
              "    WEB => '0', DOB0 => q);\n"},
             {"sb_ice40_components_syn",
              "  p : SB_PLL40_CORE generic map (DIVR => \"0000\", DIVF => \"1000010\", DIVQ => \"101\")\n"
              "    port map (REFERENCECLK => clk_a, PLLOUTCORE => os, RESETB => '1', BYPASS => '0');\n"
              "  m : SB_RAM40_4K port map (WCLK => os, WCLKE => '1', WE => '1', WDATA(0) => d, RCLK => clk_b,\n"
              "    RCLKE => '1', RE => '1', RDATA(0) => q);\n"},
             {"gw1n",
              "  p : rPLL generic map (FCLKIN => \"100\", IDIV_SEL => 0, FBDIV_SEL => 1)\n"
              "    port map (CLKIN => clk_a, CLKOUT => os, RESET => '0', RESET_P => '0');\n"
              "  m : SDPB port map (CLKA => os, CEA => '1', DI(0) => d, CLKB => clk_b, CEB => '1', OCE => '1',\n"
              "    DO(0) => q);\n"}}) {
        INFO(lib);
        auto run = analyze("t.vhd",
                           "library ieee; use ieee.std_logic_1164.all;\n"
                           "library " + lib + "; use " + lib + ".components.all;\n"
                           "entity top is port (clk_a, clk_b, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of top is\n"
                           "  signal op, os : std_logic;\n"
                           "begin\n" + cells + "end;\n",
                           "top");
        REQUIRE(run->r->cdc);
        dumpPaths(*run);
        CHECK(run->count("VHDL-UNKNOWN-UNIT") == 0);
        CHECK(run->r->diagnostics.end() == std::ranges::find_if(run->r->diagnostics, [](const Diagnostic& d) {
                  return d.severity == Severity::Error;
              }));
        // the RAM is written in the PLL output domain and read on clk_b (asynchronous)
        CHECK(run->hits("CDC-001") + run->hits("CDC-007") + run->hits("CDC-003") > 0);
        bool derived = false;
        for (const auto& c : run->r->clocks->clocks)
            derived = derived || (c.kind == clock::ClockKind::Divider && c.name.starts_with("top.p."));
        CHECK(derived);
    }
}

TEST_CASE("Primitives negative: a VHDL library the project provides is not replaced", "[primitives][lattice][vhdl]") {
    // the project brings its own gw1n library with its own components package
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "own";
    cfg.top = "top";
    cfg.sources = {run->dir.write("pkg.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                                             "package components is\n"
                                             "  component MYCELL port (c, d : in std_logic; q : out std_logic); end component;\n"
                                             "end package;\n"),
                   run->dir.write("cell.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                                              "entity MYCELL is port (c, d : in std_logic; q : out std_logic); end;\n"
                                              "architecture rtl of MYCELL is begin\n"
                                              "  process (c) begin if rising_edge(c) then q <= d; end if; end process;\n"
                                              "end;\n"),
                   run->dir.write("t.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                                           "library gw1n; use gw1n.components.all;\n"
                                           "entity top is port (clk, d : in std_logic; q : out std_logic); end;\n"
                                           "architecture rtl of top is begin\n"
                                           "  u : MYCELL port map (c => clk, d => d, q => q);\n"
                                           "end;\n")};
    cfg.sourceLibraries = {"gw1n", "gw1n", ""};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    run->r = session.run(cfg, progress, cancel);
    for (const auto& d : run->r->diagnostics)
        UNSCOPED_INFO(format(d));
    REQUIRE(run->r->cdc);
    CHECK_FALSE(run->r->hasErrors());
    REQUIRE(run->r->elaboration);
    for (const auto& f : run->r->elaboration->parsedFiles) // no vendor package was added
        CHECK(f.filename().string().find("gowin_components") == std::string::npos);
}

TEST_CASE("Primitives: VHDL component declared locally for a vendor primitive", "[primitives][lattice][vhdl]") {
    // microwatt clk_gen_ecp5: no vendor library clause, the component is declared
    // in the architecture; the ECP5 PLL feeds back from CLKOS
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "entity top is port (clk_a, d : in std_logic; q, q2 : out std_logic); end;\n"
                       "architecture rtl of top is\n"
                       "  component EHXPLLL is\n"
                       "    generic (CLKI_DIV : integer := 1; CLKFB_DIV : integer := 1; CLKOP_DIV : integer := 8;\n"
                       "             CLKOS_DIV : integer := 8; CLKOS_ENABLE : string := \"DISABLED\";\n"
                       "             FEEDBK_PATH : string := \"CLKOP\");\n"
                       "    port (CLKI, CLKFB, RST, STDBY : in std_logic; CLKOP, CLKOS, LOCK : out std_logic);\n"
                       "  end component;\n"
                       "  signal clkop, clkos, lock : std_logic;\n"
                       "begin\n"
                       "  pll : EHXPLLL generic map (CLKOP_DIV => 9, CLKOS_ENABLE => \"ENABLED\", CLKOS_DIV => 2,\n"
                       "                             CLKFB_DIV => 20, CLKI_DIV => 1, FEEDBK_PATH => \"CLKOS\")\n"
                       "    port map (CLKI => clk_a, CLKFB => clkos, RST => '0', STDBY => '0', CLKOP => clkop,\n"
                       "              CLKOS => clkos, LOCK => lock);\n"
                       "  process (clkop) begin if rising_edge(clkop) then q <= d; q2 <= lock; end if; end process;\n"
                       "end;\n",
                       "top", "create_clock -name clk_a -period 83.333 [get_ports clk_a]\n");
    REQUIRE(run->r->clocks);
    CHECK(run->count("IR-UNRESOLVED-INSTANCE") == 0);
    CHECK(run->count("FE-PRIMITIVES") == 1);
    bool found = false;
    for (const auto& c : run->r->clocks->clocks)
        if (c.name == "top.pll.CLKOP") {
            found = true;
            REQUIRE(c.periodNs);
            // VCO = 12 MHz x 20 x 2 = 480 MHz, CLKOP = VCO / 9
            CHECK(std::abs(*c.periodNs - 83.333 * 9.0 / 40.0) < 1e-3);
        }
    CHECK(found);
}

TEST_CASE("Primitives negative: a local component that is no vendor primitive stays a black box", "[primitives][vhdl]") {
    auto run = analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "entity top is port (clk_a, d : in std_logic; q : out std_logic); end;\n"
                       "architecture rtl of top is\n"
                       "  component my_ip port (clk, d : in std_logic; q : out std_logic); end component;\n"
                       "begin\n"
                       "  u : my_ip port map (clk => clk_a, d => d, q => q);\n"
                       "end;\n",
                       "top");
    CHECK(run->count("IR-UNRESOLVED-INSTANCE") == 1);
    CHECK(run->count("FE-PRIMITIVES") == 0);
}

TEST_CASE("Primitives: DSP48 slices, BSCANE2 JTAG clock, differential I/O buffers", "[primitives][dsp]") {
    auto run = analyze(
        "t.v",
        "module top(input clk_a, input clk_b, input [24:0] a, input [17:0] b, output [47:0] p, output [47:0] p2,\n"
        "           output reg [47:0] q, output reg sel_q, inout dq_p, inout dq_n, output dq_o);\n"
        "  reg [24:0] ar; always @(posedge clk_a) ar <= a;\n"
        "  DSP48E1 #(.AREG(0), .BREG(0), .MREG(0), .PREG(1)) m (.CLK(clk_b), .A({5'd0, ar}), .B(b), .C(48'd0),\n"
        "    .CEP(1'b1), .RSTP(1'b0), .OPMODE(7'b0000101), .ALUMODE(4'd0), .P(p));\n"
        "  DSP48E2 #(.AREG(0), .BREG(0), .CREG(0), .DREG(0), .ADREG(0), .MREG(0), .PREG(0)) c (.CLK(clk_b),\n"
        "    .A({5'd0, ar}), .B(b), .P(p2));\n"
        "  always @(posedge clk_b) q <= p2;\n"
        "  wire tck, sel;\n"
        "  BSCANE2 #(.JTAG_CHAIN(2)) bs (.TCK(tck), .SEL(sel), .TDO(1'b0));\n"
        "  always @(posedge clk_a) sel_q <= sel;\n"
        "  IOBUFDS io (.I(ar[0]), .T(1'b0), .IO(dq_p), .IOB(dq_n), .O(dq_o));\n"
        "endmodule\n",
        "top");
    REQUIRE(run->r->cdc);
    dumpPaths(*run);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 0);
    CHECK(crosses(*run, "top.ar", "top.m.")); // PREG: registered on CLK = clk_b
    CHECK(run->path("top.ar", "top.q") != nullptr); // all registers off: combinational to q
    bool tck = false;
    for (const auto& c : run->r->clocks->clocks)
        tck = tck || (c.name == "top.bs.TCK" && c.kind == clock::ClockKind::BlackBox);
    CHECK(tck);
    CHECK(crosses(*run, "top.bs.", "top.sel_q")); // TAP state in the TCK domain
}

TEST_CASE("Sources with `timescale and the primitive linker", "[primitives][timescale]") {
    // verilog-ethernet: every file starts with `timescale; the generated
    // wrapper has none, which slang rejected silently (0 errors, no design)
    auto run = analyze("t.v",
                       "`timescale 1ns / 1ps\n"
                       "module sub(input c, input d, output reg q); always @(posedge c) q <= d; endmodule\n"
                       "module top(input clk, input d, output q);\n"
                       "  wire c; BUFG b (.I(clk), .O(c));\n"
                       "  sub s (.c(c), .d(d), .q(q));\n"
                       "endmodule\n",
                       "top");
    CHECK_FALSE(run->r->hasErrors());
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-SLANG-MissingTimeScale") == 0);
}

TEST_CASE("Unconnected ports are reported once per instance", "[primitives][unconnected]") {
    auto run = analyze("t.v",
                       "module sub(input a, input b, input c, output x, output y); assign x = a & b & c; assign y = a;\n"
                       "endmodule\n"
                       "module top(input clk_a, input d, output q, output q2);\n"
                       "  sub u1 (.a(d), .x(q));\n"           // b, c open inputs, y open output
                       "  sub u2 (.a(d), .b(d), .c(d), .x(q2));\n" // only y open
                       "endmodule\n",
                       "top");
    REQUIRE(run->count("FE-UNCONNECTED-PORTS") == 2);
    CHECK(run->count("FE-SLANG-UnconnectedInputPort") == 0);
    CHECK(run->count("FE-SLANG-UnconnectedOutputPort") == 0);
    for (const auto& d : run->r->diagnostics) {
        if (d.code != "FE-UNCONNECTED-PORTS")
            continue;
        if (d.message.find("inputs: b, c") != std::string::npos) {
            CHECK(d.severity == Severity::Warning);
            CHECK(d.message.find("3 port(s)") == 0);
            CHECK(d.message.find("outputs: y") != std::string::npos);
        }
        else {
            CHECK(d.severity == Severity::Info); // open outputs only
            CHECK(d.message.find("1 port(s)") == 0);
        }
    }
}

TEST_CASE("Intel synchronizer cells with Intel's own SDC idioms (entity:instance names)", "[primitives][constraints]") {
    constexpr const char* v = "module top(input clk_a, input clk_b, input rst, input s, output so, output ro);\n"
                              "  reg sr;\n"
                              "  always @(posedge clk_a) sr <= s;\n"
                              "  altera_std_synchronizer #(.depth(3)) u_sync (.clk(clk_b), .reset_n(1'b1), .din(sr), .dout(so));\n"
                              "  altera_reset_synchronizer #(.ASYNC_RESET(1), .DEPTH(2)) u_rst (.reset_in(rst), .clk(clk_b), .reset_out(ro));\n"
                              "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n";
    auto run = analyze("t.v", v, "top",
                       clocks + "set_false_path -to [get_keepers {*altera_std_synchronizer:*|din_s1}]\n"
                                "set_false_path -to [get_registers {altera_reset_synchronizer:*|altera_reset_synchronizer_int_chain[*]}]\n");
    REQUIRE(run->r->cdc);
    CHECK(run->count("CON-UNMATCHED") == 0);
    const auto* p = run->path("top.sr", "top.u_sync.din_s1");
    REQUIRE(p);
    CHECK(p->syncType == SyncType::ThreeFlop);
    CHECK(p->relation == clock::ClockRelation::Asynchronous);
    CHECK(std::ranges::any_of(p->notes, [](const std::string& n) { return n.find("set_false_path") != std::string::npos; }));
    const auto* rs = run->path("top.rst", "top.u_rst.altera_reset_synchronizer_int_chain");
    REQUIRE(rs);
    CHECK(rs->syncType == SyncType::ResetSync);
    // negative: another entity name matches nothing
    auto none = analyze("t.v", v, "top", clocks + "set_false_path -to [get_keepers {*altera_dcfifo_synchronizer:*|din_s1}]\n");
    CHECK(none->count("CON-UNMATCHED") >= 1);
    const auto* q = none->path("top.sr", "top.u_sync.din_s1");
    REQUIRE(q);
    CHECK(q->relation == clock::ClockRelation::Unknown);
}

TEST_CASE("Primitives: a user module whose name differs only in case does not hide the model", "[primitives]") {
    // verilog-ethernet: module 'iddr' wraps Xilinx 'IDDR' (Verilog names are case-sensitive)
    auto run = analyze("t.v",
                       "module iddr(input clk_b, input d, output q1, output q2);\n"
                       "  IDDR #(.DDR_CLK_EDGE(\"SAME_EDGE_PIPELINED\")) iddr_inst (.Q1(q1), .Q2(q2), .C(clk_b), .CE(1'b1),\n"
                       "       .D(d), .R(1'b0), .S(1'b0));\n"
                       "endmodule\n"
                       "module top(input clk_a, input clk_b, input d, output q1, output q2);\n"
                       "  reg a; always @(posedge clk_a) a <= d;\n"
                       "  iddr u (.clk_b(clk_b), .d(a), .q1(q1), .q2(q2));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-UNRESOLVED-INSTANCE") == 0);
    CHECK(std::ranges::any_of(run->r->diagnostics, [](const Diagnostic& d) {
        return d.code == "FE-PRIMITIVES" && d.message.find("IDDR") != std::string::npos;
    }));
    CHECK_FALSE(run->r->cdc->paths.empty()); // a crosses into the IDDR model's registers
    // negative: a user module named exactly like the primitive replaces it
    auto own = analyze("t.v",
                       "module IDDR(output Q1, output Q2, input C, input CE, input D, input R, input S);\n"
                       "  assign Q1 = D; assign Q2 = D;\n"
                       "endmodule\n"
                       "module top(input clk_a, input clk_b, input d, output q1, output q2);\n"
                       "  reg a; always @(posedge clk_a) a <= d;\n"
                       "  IDDR u (.Q1(q1), .Q2(q2), .C(clk_b), .CE(1'b1), .D(a), .R(1'b0), .S(1'b0));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(own->r->cdc);
    CHECK(own->r->cdc->paths.empty());
}

TEST_CASE("Primitives: IDELAYE2 in FIXED mode has no tap counter register", "[primitives]") {
    // verilog-ethernet NexysVideo: FIXED delays with C tied to 0
    const std::string fixed = "module top(input clk_a, input clk_b, input d, output q);\n"
                              "  wire dd;\n"
                              "  IDELAYE2 #(.IDELAY_TYPE(\"FIXED\")) u (.IDATAIN(d), .DATAOUT(dd), .DATAIN(1'b0), .C(1'b0),\n"
                              "     .CE(1'b0), .INC(1'b0), .CINVCTRL(1'b0), .CNTVALUEIN(5'd0), .LD(1'b0), .LDPIPEEN(1'b0),\n"
                              "     .REGRST(1'b0));\n"
                              "  reg r; always @(posedge clk_b) r <= dd;\n"
                              "  assign q = r;\n"
                              "endmodule\n";
    auto run = analyze("t.v", fixed, "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("CLK-CONSTANT") == 0);
    CHECK(run->r->design->stats().registers == 1);
    // negative: a variable delay keeps its counter (clocked by C)
    std::string var = fixed;
    var.replace(var.find("\"FIXED\""), 7, "\"VAR_LOAD\"");
    var.replace(var.find(".C(1'b0)"), 8, ".C(clk_a)");
    auto v = analyze("t.v", var, "top");
    REQUIRE(v->r->cdc);
    CHECK(v->r->design->stats().registers == 2);
}

TEST_CASE("Primitives: Xilinx UNIMACRO multiplier and dual-clock FIFO", "[primitives]") {
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input [7:0] a, input [7:0] b, output [15:0] p,\n"
                       "           input [7:0] d, input we, input re, output [7:0] q);\n"
                       "  MULT_MACRO #(.LATENCY(3), .WIDTH_A(8), .WIDTH_B(8)) m (.P(p), .A(a), .B(b), .CE(1'b1),\n"
                       "     .CLK(clk_a), .RST(1'b0));\n"
                       "  FIFO_DUALCLOCK_MACRO #(.DATA_WIDTH(8), .FIFO_SIZE(\"18Kb\")) f (.DI(d), .WRCLK(clk_a), .WREN(we),\n"
                       "     .RDCLK(clk_b), .RDEN(re), .DO(q), .RST(1'b0));\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-UNRESOLVED-INSTANCE") == 0);
    const auto* f = run->path("top.f.mem", "top.f.do_ff");
    REQUIRE(f);
    CHECK(f->syncType == SyncType::AsyncFifo);
    // the multiplier is three register stages in clk_a: no crossing of its own
    CHECK(std::ranges::none_of(run->r->cdc->paths, [](const CdcPath& p) { return p.dstName.starts_with("top.m."); }));
}

TEST_CASE("Frontend: a net used before its declaration is accepted (as by Vivado and Quartus)", "[primitives]") {
    // ADI's axi_ad9361_tx uses dac_sync before declaring it
    auto run = analyze("t.v",
                       "module top(input clk_a, input clk_b, input d, output reg q);\n"
                       "  assign early = d;\n"
                       "  wire early;\n"
                       "  reg a; always @(posedge clk_a) a <= early;\n"
                       "  always @(posedge clk_b) q <= a;\n"
                       "endmodule\n",
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->count("FE-SLANG-UsedBeforeDeclared") == 0);
    CHECK(run->path("top.a", "top.q") != nullptr);
}

TEST_CASE("Primitives: Intel megafunction parameters in another case", "[primitives][intel]") {
    // Quartus takes .WIDTH for `width` (verilog-ethernet's oddr.v writes it so)
    auto bits = [](const std::string& params) {
        auto run = analyze("t.v",
                           "module top(input clk, input [4:0] a, input [4:0] b, output [4:0] q);\n"
                           "  altddio_out #(" + params + ") u (.aset(1'b0), .datain_h(a), .datain_l(b),\n"
                           "    .outclocken(1'b1), .outclock(clk), .aclr(1'b0), .dataout(q));\n"
                           "endmodule\n",
                           "top", "");
        REQUIRE(run->r->design);
        return run->r->design->stats().registerBits;
    };
    CHECK(bits(".WIDTH(5), .POWER_UP_HIGH(\"OFF\"), .OE_REG(\"UNUSED\")") == 10); // two 5-bit registers
    CHECK(bits(".width(5)") == 10);                                                // the declared spelling
    CHECK(bits(".NOT_A_PARAMETER(5)") == 2);                                       // negative: no such parameter
}

TEST_CASE("Primitives: xpm_fifo_sync from VHDL is a one-clock FIFO", "[primitives][vhdl]") {
    // SURF FifoXpm (GEN_SYNC_FIFO_G): xpm_fifo_sync from the xpm component library
    auto fifo = [](const std::string& readClk) {
        return analyze("t.vhd",
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "library xpm; use xpm.vcomponents.all;\n"
                       "entity top is port (clk_a, clk_b, we, re, rst : in std_logic; d : in std_logic_vector(7 downto 0);\n"
                       "  q : out std_logic_vector(7 downto 0)); end;\n"
                       "architecture rtl of top is\n"
                       "  signal din, dout : std_logic_vector(7 downto 0);\n"
                       "begin\n"
                       "  process (clk_a) begin if rising_edge(clk_a) then din <= d; end if; end process;\n"
                       "  u : xpm_fifo_sync generic map (FIFO_WRITE_DEPTH => 16, WRITE_DATA_WIDTH => 8, READ_DATA_WIDTH => 8)\n"
                       "    port map (rst => rst, wr_clk => clk_a, wr_en => we, din => din, rd_en => re, dout => dout,\n"
                       "      sleep => '0', injectsbiterr => '0', injectdbiterr => '0');\n"
                       "  process (" + readClk + ") begin if rising_edge(" + readClk + ") then q <= dout; end if; end process;\n"
                       "end;\n",
                       "top");
    };
    auto same = fifo("clk_a");
    REQUIRE(same->r->cdc);
    CHECK(same->count("IR-UNRESOLVED-INSTANCE") == 0);
    CHECK(same->count("VHDL-COMPONENT") == 0);
    CHECK(same->r->cdc->stats.paths == 0);
    // negative: the FIFO does not cross clocks, its output read in another clock is a crossing
    auto other = fifo("clk_b");
    REQUIRE(other->r->cdc);
    CHECK(other->path("top.u.dout_ff", "top.q") != nullptr);
    CHECK(other->r->cdc->stats.unsynchronized >= 1);
}

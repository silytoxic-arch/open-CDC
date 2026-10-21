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
                          "altera_std_synchronizer"})
        CHECK(lib.find(n).has_value());
    CHECK(lib.find("fdre") == std::optional<std::string>("FDRE")); // VHDL spelling
    CHECK_FALSE(lib.find("MMCME2_ADV").has_value());
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
                       "  MMCME2_ADV pll ();\n"
                       "endmodule\n",
                       "top");
    CHECK_FALSE(run->r->cdc);
    CHECK(run->count("FE-UNKNOWN-MODULE") == 1); // MMCME2_ADV; FDRE is modelled
    CHECK(run->count("FE-SLANG-UnknownModule") == 0);
}

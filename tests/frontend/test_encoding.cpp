// Non-UTF-8 sources (GBK comments of Chinese Windows editors) and unknown
// modules (vendor IP cores without their HDL), end to end through the engine.

#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

// GBK: "数据位宽", "系统时钟", "头文件"
constexpr const char* kWidth = "\xCA\xFD\xBE\xDD\xCE\xBB\xBF\xED";
constexpr const char* kClock = "\xCF\xB5\xCD\xB3\xCA\xB1\xD6\xD3";
constexpr const char* kHeader = "\xCD\xB7\xCE\xC4\xBC\xFE";

constexpr const char* kSdc = "create_clock -name a -period 10 [get_ports clk_a]\n"
                             "create_clock -name b -period 7 [get_ports clk_b]\n"
                             "set_clock_groups -asynchronous -group a -group b\n";

std::size_t count(const engine::AnalysisResult& r, std::string_view code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(r.diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
}

std::shared_ptr<const engine::AnalysisResult> run(engine::ProjectConfig cfg) {
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

} // namespace

TEST_CASE("GBK comments in Verilog sources and included files are read, not warned about", "[frontend][encoding]") {
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "gbk";
    cfg.top = "top";
    dir.write("defs.vh", std::string("// ") + kHeader + "\n`define W 8\n");
    cfg.sources = {dir.write("top.v", std::string("`include \"defs.vh\"\n"
                                                  "module top(input clk_a, //") + kClock + "\n"
                                      "           input clk_b, input [`W-1:0] d, output reg [`W-1:0] q);\n"
                                      "  reg [`W-1:0] a, s1; // " + kWidth + "\n"
                                      "  always @(posedge clk_a) a <= d;\n"
                                      "  always @(posedge clk_b) begin s1 <= a; q <= s1; end\n"
                                      "endmodule\n")};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", std::string("# ") + kClock + "\n" + kSdc)};
    auto r = run(cfg);
    CHECK(count(*r, "FE-SLANG-InvalidUTF8Seq") == 0);
    CHECK(count(*r, "FE-ENCODING") == 2); // top.v and defs.vh
    CHECK(count(*r, "CON-ENCODING") == 1);
    REQUIRE(r->cdc);
    CHECK(r->cdc->paths.size() == 1);
    // negative: a UTF-8 file is not reported
    test::TempDir utf;
    engine::ProjectConfig u = cfg;
    u.sources = {utf.write("top.v", "module top(input clk_a, input clk_b, input d, output reg q); // \xE6\x95\xB0\n"
                                    "  reg a; always @(posedge clk_a) a <= d; always @(posedge clk_b) q <= a;\n"
                                    "endmodule\n")};
    u.constraints = {utf.write("c.sdc", kSdc)};
    auto ru = run(u);
    CHECK(count(*ru, "FE-ENCODING") == 0);
    CHECK(count(*ru, "CON-ENCODING") == 0);
}

TEST_CASE("GBK comments in VHDL sources", "[frontend][encoding]") {
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "gbk_vhdl";
    cfg.top = "top";
    cfg.sources = {dir.write("top.vhd", std::string("library ieee; use ieee.std_logic_1164.all;\n"
                                                    "entity top is port (clk_a, clk_b, d : in std_logic; q : out std_logic); -- ") +
                                            kClock + "\nend entity;\n"
                                            "architecture rtl of top is\n  signal a, s1 : std_logic; -- " + kWidth + "\n"
                                            "begin\n"
                                            "  process (clk_a) begin if rising_edge(clk_a) then a <= d; end if; end process;\n"
                                            "  process (clk_b) begin if rising_edge(clk_b) then s1 <= a; q <= s1; end if; end process;\n"
                                            "end architecture;\n")};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", kSdc)};
    auto r = run(cfg);
    CHECK(count(*r, "FE-ENCODING") == 1);
    REQUIRE(r->cdc);
    CHECK(r->cdc->paths.size() == 1);
}

TEST_CASE("Unknown modules: an error with a hint, or black boxes on request", "[frontend][encoding]") {
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "ip";
    cfg.top = "top";
    cfg.sources = {dir.write("top.v", "module top(input clk_a, input clk_b, input [7:0] d, output reg [7:0] q,\n"
                                      "           output q1, output q2);\n"
                                      "  wire [7:0] dout;\n"
                                      "  fifo_8X4K u_fifo (.wr_clk(clk_a), .rd_clk(clk_b), .din(d), .wr_en(1'b1), .rd_en(1'b1),\n"
                                      "                    .dout(dout));\n"
                                      "  reg a; always @(posedge clk_a) a <= d[0];\n"
                                      "  IDDR u_iddr (.Q1(q1), .Q2(q2), .C(clk_b), .CE(1'b1), .D(a), .R(1'b0), .S(1'b0));\n"
                                      "  always @(posedge clk_b) q <= dout;\n"
                                      "endmodule\n")};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", kSdc)};
    auto err = run(cfg);
    CHECK_FALSE(err->cdc);
    const auto it = std::ranges::find_if(err->diagnostics, [](const Diagnostic& d) {
        // slang's error, or the linker's when vendor primitives are linked as well
        return (d.code == "FE-SLANG-UnknownModule" || d.code == "FE-UNKNOWN-MODULE") &&
               d.message.find("fifo_8X4K") != std::string::npos;
    });
    REQUIRE(it != err->diagnostics.end());
    REQUIRE_FALSE(it->notes.empty());
    CHECK(it->notes.front().find("unknownModules") != std::string::npos);

    // without vendor primitives: slang's own error carries the hint too
    {
        test::TempDir d2;
        engine::ProjectConfig plain = cfg;
        plain.sources = {d2.write("top.v", "module top(input clk_b, input [7:0] d, output [7:0] q);\n"
                                           "  fifo_8X4K u_fifo (.rd_clk(clk_b), .din(d), .dout(q));\n"
                                           "endmodule\n")};
        auto e2 = run(plain);
        const auto s2 = std::ranges::find(e2->diagnostics, std::string("FE-SLANG-UnknownModule"), &Diagnostic::code);
        REQUIRE(s2 != e2->diagnostics.end());
        REQUIRE_FALSE(s2->notes.empty());
        CHECK(s2->notes.front().find("blackbox") != std::string::npos);
    }

    cfg.options.blackBoxUnknownModules = true;
    auto bb = run(cfg);
    REQUIRE(bb->cdc);
    CHECK(count(*bb, "FE-BLACKBOX-MODULE") == 1);
    CHECK(count(*bb, "FE-UNKNOWN-MODULE") == 0);
    // the modelled primitive is still used: a crosses into the IDDR's registers
    CHECK(std::ranges::any_of(bb->diagnostics, [](const Diagnostic& d) {
        return d.code == "FE-PRIMITIVES" && d.message.find("IDDR") != std::string::npos;
    }));
    CHECK_FALSE(bb->cdc->paths.empty());
}

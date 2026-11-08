// Parameters without a default value that get their value from a defparam
// (NES_MiSTer: 'parameter [9:0] SSREG_INDEX_DMC1' set through two defparams),
// and positional parameters of a VHDL entity after a value OpenCDC cannot represent.
#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

struct Analysis {
    std::shared_ptr<const engine::AnalysisResult> r;
    [[nodiscard]] std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(r->diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
    }
    [[nodiscard]] std::size_t errors() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(r->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    }
    [[nodiscard]] bool has(std::string_view code, std::string_view text) const {
        return std::ranges::any_of(r->diagnostics, [&](const Diagnostic& d) {
            return d.code == code && d.message.find(text) != std::string::npos;
        });
    }
    [[nodiscard]] bool crosses(std::string_view from, std::string_view to) const {
        if (!r->cdc)
            return false;
        return std::ranges::any_of(r->cdc->paths, [&](const auto& p) {
            return p.srcName.find(from) != std::string::npos && p.dstName.find(to) != std::string::npos;
        });
    }
};

Analysis analyze(test::TempDir& dir, const std::vector<std::pair<std::string, std::string>>& files,
                 const std::string& top) {
    std::string sources;
    for (const auto& [n, t] : files) {
        dir.write(n, t);
        sources += (sources.empty() ? "\"" : ", \"") + n + "\"";
    }
    const auto p = dir.write("p.ocdc.json", "{\"format\": \"opencdc-project\", \"version\": 1, \"name\": \"p\", "
                                            "\"top\": \"" + top + "\", \"sources\": [" + sources + "]}");
    auto cfg = engine::loadProjectFile(p);
    REQUIRE(cfg);
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    Analysis a{session.run(cfg.value(), progress, cancel)};
    REQUIRE(a.r);
    for (const auto& d : a.r->diagnostics)
        UNSCOPED_INFO(format(d));
    if (a.r->cdc)
        for (const auto& path : a.r->cdc->paths)
            UNSCOPED_INFO("path: " << path.srcName << " -> " << path.dstName);
    return a;
}

// q captures d in clk_b only when the parameter has the expected value: the
// crossing exists exactly when the value reached the leaf.
const char* const kLeaf = "module leaf #(parameter IDX = 0, parameter integer WANT = 5)\n"
                          "  (input clk_b, input d, output reg q);\n"
                          "  always @(posedge clk_b) if (IDX == WANT) q <= d;\n"
                          "endmodule\n";

} // namespace

TEST_CASE("defparam: implicitly typed parameters without a default value", "[frontend][defparam]") {
    test::TempDir dir;
    const auto a = analyze(
        dir,
        {{"leaf.v", kLeaf},
         {"m.v",
          // range, no type (NES_MiSTer)
          "module m_range #(parameter [9:0] IDX) (input clk_b, input d, output q);\n"
          "  leaf #(IDX) u (.clk_b(clk_b), .d(d), .q(q));\n"
          "endmodule\n"
          // no range and no type: the type of the value
          "module m_plain #(parameter IDX) (input clk_b, input d, output q);\n"
          "  leaf #(IDX) u (.clk_b(clk_b), .d(d), .q(q));\n"
          "endmodule\n"
          // signed, no range: a negative value stays negative
          "module m_signed #(parameter signed IDX) (input clk_b, input d, output q);\n"
          "  leaf #(.IDX(IDX < 0 ? 5 : 0)) u (.clk_b(clk_b), .d(d), .q(q));\n"
          "endmodule\n"
          // two levels of defparams (apu.sv in NES_MiSTer)
          "module m_outer #(parameter [9:0] IDX) (input clk_b, input d, output q);\n"
          "  m_range inner (.clk_b(clk_b), .d(d), .q(q));\n"
          "  defparam inner.IDX = IDX;\n"
          "endmodule\n"},
         {"top.v", "module top (input clk_a, input clk_b, input d, output [5:0] q);\n"
                   "  reg r; always @(posedge clk_a) r <= d;\n"
                   "  m_range  u_range  (.clk_b(clk_b), .d(r), .q(q[0])); defparam u_range.IDX = 5;\n"
                   "  m_plain  u_plain  (.clk_b(clk_b), .d(r), .q(q[1])); defparam u_plain.IDX = 5;\n"
                   "  m_signed u_signed (.clk_b(clk_b), .d(r), .q(q[2])); defparam u_signed.IDX = -3;\n"
                   "  m_outer  u_outer  (.clk_b(clk_b), .d(r), .q(q[3])); defparam u_outer.IDX = 5;\n"
                   // the value is used, not taken as matching: 4 and +3 do not cross
                   "  m_range  u_other  (.clk_b(clk_b), .d(r), .q(q[4])); defparam u_other.IDX = 4;\n"
                   "  m_signed u_pos    (.clk_b(clk_b), .d(r), .q(q[5])); defparam u_pos.IDX = 3;\n"
                   "endmodule\n"}},
        "top");
    CHECK(a.errors() == 0);
    CHECK(a.count("IR-UNSUPPORTED-PROCESS") == 0);
    CHECK(a.count("IR-UNSUPPORTED-PARAM") == 0);
    CHECK(a.crosses("top.r", "u_range.u.q"));
    CHECK(a.crosses("top.r", "u_plain.u.q"));
    CHECK(a.crosses("top.r", "u_signed.u.q"));
    CHECK(a.crosses("top.r", "u_outer.inner.u.q"));
    CHECK_FALSE(a.crosses("top.r", "u_other.u.q"));
    CHECK_FALSE(a.crosses("top.r", "u_pos.u.q"));
}

TEST_CASE("defparam negative: a parameter without a default and without a value is an error",
          "[frontend][defparam]") {
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"leaf.v", kLeaf},
                            {"top.v", "module m #(parameter [9:0] IDX) (input clk_b, input d, output q);\n"
                                      "  leaf #(IDX) u (.clk_b(clk_b), .d(d), .q(q));\n"
                                      "endmodule\n"
                                      "module top (input clk_b, input d, output q);\n"
                                      "  m u_m (.clk_b(clk_b), .d(d), .q(q));\n"
                                      "endmodule\n"}},
                           "top");
    CHECK(a.has("FE-SLANG-ParamHasNoValue", "'IDX'"));
    CHECK(a.errors() > 0);
}

TEST_CASE("Mixed language: ordered parameters of a VHDL entity keep their position", "[frontend][defparam][vhdl]") {
    test::TempDir dir;
    const char* const vhd = "library ieee; use ieee.std_logic_1164.all;\n"
                            "entity vreg is\n"
                            "  generic (A : integer := 0; DEF : std_logic_vector(7 downto 0) := x\"00\");\n"
                            "  port (clk : in std_logic; d : in std_logic_vector(7 downto 0);\n"
                            "        q : out std_logic_vector(7 downto 0));\n"
                            "end entity;\n"
                            "architecture rtl of vreg is\n"
                            "begin\n"
                            "  process(clk) begin if rising_edge(clk) then if d = DEF then q <= d; end if; end if;\n"
                            "  end process;\n"
                            "end architecture;\n";
    SECTION("all values known") {
        const auto a =
            analyze(dir,
                    {{"vreg.vhd", vhd},
                     {"top.sv", "module top (input clk, input [7:0] d, output [7:0] q);\n"
                                "  vreg #(3, 8'h22) u (.clk(clk), .d(d), .q(q));\nendmodule\n"}},
                    "top");
        CHECK(a.errors() == 0);
    }
    SECTION("the first value cannot be represented: the error names it, the second is bound") {
        const auto a =
            analyze(dir,
                    {{"vreg.vhd", vhd},
                     {"top.sv", "module top (input clk, input [7:0] d, output [7:0] q);\n"
                                "  localparam int S [2] = '{1, 2};\n"
                                "  vreg #(S, 8'h22) u (.clk(clk), .d(d), .q(q));\nendmodule\n"}},
                    "top");
        CHECK(a.has("IR-UNSUPPORTED-PARAM", "instance 'u' of 'vreg'"));
        CHECK(a.has("VHDL-GENERIC", "generic 'A'"));
        CHECK_FALSE(a.has("VHDL-GENERIC", "generic 'DEF'"));
    }
}

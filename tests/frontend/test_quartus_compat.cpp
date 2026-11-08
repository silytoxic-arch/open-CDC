// Constructs that are errors in (System)Verilog but that Quartus accepts
// (MiSTer's framework relies on them), the Cyclone V atoms, Quartus' PLL
// clock names, and inout ports driven from inside.
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
        return static_cast<std::size_t>(std::ranges::count_if(r->diagnostics, [](const Diagnostic& d) {
            return d.isError();
        }));
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

/// The design in `files` (name -> text), top `top`, with an SDC.
Analysis analyze(test::TempDir& dir, const std::vector<std::pair<std::string, std::string>>& files,
                 const std::string& top, const std::string& sdc = "") {
    std::string sources;
    for (const auto& [n, t] : files) {
        dir.write(n, t);
        sources += (sources.empty() ? "\"" : ", \"") + n + "\"";
    }
    std::string constraints;
    if (!sdc.empty()) {
        dir.write("c.sdc", sdc);
        constraints = ", \"constraints\": [\"c.sdc\"]";
    }
    const auto p = dir.write("p.ocdc.json", "{\"format\": \"opencdc-project\", \"version\": 1, \"name\": \"p\", "
                                            "\"top\": \"" + top + "\", \"sources\": [" + sources + "]" +
                                                constraints + "}");
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

} // namespace

TEST_CASE("Quartus compatibility: nets assigned in procedural blocks are read as variables", "[frontend][quartus]") {
    test::TempDir dir;
    const auto a = analyze(
        dir,
        {{"m.sv", "module m (input clk_a, input clk_b, input d, output [1:0] o, output     q,\n"
                  "          inout  reg io);\n"
                  "  wire n;\n"
                  "  reg r;\n"
                  "  always @(posedge clk_a) r <= d;\n"
                  "  always_comb begin n = r; o = {r, r}; io = r; end\n"
                  "  reg s; always @(posedge clk_b) s <= n;\n"
                  "  assign q = s;\n"
                  "endmodule\n"},
         {"top.sv", "module top (input clk_a, input clk_b, input d, output [1:0] o, output q, inout io);\n"
                    "  m u (.clk_a(clk_a), .clk_b(clk_b), .d(d), .o(o), .q(q), .io(io));\nendmodule\n"}},
        "top");
    CHECK(a.errors() == 0);
    CHECK(a.has("FE-NET-PROCEDURAL-ASSIGN", "net 'n'"));
    CHECK(a.has("FE-NET-PROCEDURAL-ASSIGN", "net 'o'"));
    CHECK(a.has("FE-NET-PROCEDURAL-ASSIGN", "inout port 'io'"));
    // the variable carries the crossing: r (clk_a) through n into s (clk_b)
    CHECK(a.crosses("u.r", "u.s"));
}

TEST_CASE("Quartus compatibility: names of a generate block used in the module", "[frontend][quartus]") {
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"m.sv", "module m #(parameter EN = 1) (input clk_a, input clk_b, input d, output reg q);\n"
                                     "  always @(posedge clk_a) if (EN) kbd <= d;\n"
                                     "  always @(posedge clk_b) q <= kbd;\n"
                                     "  generate if (EN) begin\n"
                                     "    reg kbd;\n"
                                     "  end endgenerate\n"
                                     "endmodule\n"}},
                           "m");
    CHECK(a.errors() == 0);
    CHECK(a.has("FE-GENERATE-SCOPE", "'kbd'"));
    CHECK(a.crosses("kbd", "m.q"));
}

TEST_CASE("Quartus compatibility negative: real errors stay errors", "[frontend][quartus]") {
    test::TempDir dir;
    // an identifier declared nowhere, and a wire with a continuous assignment and a
    // declaration assignment (not corrected)
    const auto a = analyze(dir,
                           {{"m.sv", "module m (input clk, input d, output reg q);\n"
                                     "  wire w = d;\n"
                                     "  always @(posedge clk) begin q <= nosuch; w = d; end\n"
                                     "endmodule\n"}},
                           "m");
    CHECK(a.count("FE-SLANG-UndeclaredIdentifier") == 1);
    CHECK(a.count("FE-SLANG-AssignToNet") == 1);
    CHECK(a.count("FE-NET-PROCEDURAL-ASSIGN") == 0);
    CHECK(a.count("FE-GENERATE-SCOPE") == 0);
}

TEST_CASE("Verilog files in a SystemVerilog project: initialized procedural declarations", "[frontend][quartus]") {
    test::TempDir dir;
    const char* body = "module %s (input clk, output reg [3:0] q);\n"
                       "  always @(posedge clk) begin reg [3:0] t = 4'd1; q <= t; end\nendmodule\n";
    std::string v(256, '\0'), sv(256, '\0');
    v.resize(static_cast<std::size_t>(std::snprintf(v.data(), v.size(), body, "a")));
    sv.resize(static_cast<std::size_t>(std::snprintf(sv.data(), sv.size(), body, "b")));
    const auto a = analyze(dir,
                           {{"a.v", v},
                            {"b.sv", sv},
                            {"top.sv", "module top (input clk, output [3:0] x, output [3:0] y);\n"
                                       "  a ua (.clk(clk), .q(x)); b ub (.clk(clk), .q(y));\nendmodule\n"}},
                           "top");
    // Verilog-2001 semantics in the .v file; the .sv file is told (as information:
    // read as synthesis tools do, the initializer is the power-up value)
    CHECK(a.count("FE-SLANG-StaticInitializerMustBeExplicit") == 0);
    CHECK(a.count("FE-IMPLICIT-STATIC") == 1);
    CHECK(std::ranges::all_of(a.r->diagnostics, [](const Diagnostic& d) {
        return d.code != "FE-IMPLICIT-STATIC" || (d.location.file.ends_with("b.sv") && d.severity == Severity::Info);
    }));
}

TEST_CASE("Inout ports driven inside a child, connected through a concatenation", "[frontend][quartus]") {
    test::TempDir dir;
    // MiSTer's HPS_BUS: the core drives its clock on a bit of an inout bus
    const auto a = analyze(dir,
                           {{"t.v", "module child (inout [3:0] bus, input clk_in, input d, output reg q);\n"
                                    "  assign bus[2] = clk_in;\n"
                                    "  always @(posedge clk_in) q <= d;\nendmodule\n"
                                    "module mid (inout [3:0] bus, input clk_in, input d, output q);\n"
                                    "  child c (.bus(bus), .clk_in(clk_in), .d(d), .q(q));\nendmodule\n"
                                    "module top (input clk_a, input clk_b, input d, output reg q, output r);\n"
                                    "  wire clk_sys, x, y, z;\n"
                                    "  mid m (.bus({x, clk_sys, y, z}), .clk_in(clk_a), .d(d), .q(r));\n"
                                    "  reg s; always @(posedge clk_b) s <= d;\n"
                                    "  always @(posedge clk_sys) q <= s;\nendmodule\n"}},
                           "top");
    CHECK(a.errors() == 0);
    // clk_sys is clk_a: the register is clocked, and the crossing from clk_b is found
    CHECK(a.count("CLK-UNRESOLVED") == 0);
    CHECK(a.crosses("top.s", "top.q"));
    // negative: a bit the child does not drive stays undriven (no clock)
    const auto b = analyze(dir,
                           {{"t.v", "module child (inout [3:0] bus, input clk_in);\n"
                                    "  assign bus[2] = clk_in;\nendmodule\n"
                                    "module top (input clk_a, input d, output reg q);\n"
                                    "  wire clk_x, w2, w1, w0;\n"
                                    "  child c (.bus({clk_x, w2, w1, w0}), .clk_in(clk_a));\n"
                                    "  always @(posedge clk_x) q <= d;\nendmodule\n"}},
                           "top");
    CHECK(b.count("CLK-UNRESOLVED") == 1);
}

TEST_CASE("Cyclone V atoms and Quartus' PLL clock names", "[frontend][quartus][primitives]") {
    test::TempDir dir;
    const auto a = analyze(
        dir,
        {{"t.v",
          "module top (input clk_50, input clk_b, input sel, input d, output reg q, output reg r, output reg s,\n"
          "            output reg t);\n"
          "  wire [1:0] pclk;\n"
          "  altera_pll #(.reference_clock_frequency(\"50.0 MHz\"), .number_of_clocks(2),\n"
          "               .output_clock_frequency0(\"100.0 MHz\"), .output_clock_frequency1(\"25.0 MHz\"))\n"
          "    pll_i (.refclk(clk_50), .rst(1'b0), .outclk(pclk), .locked(), .fbclk(1'b0), .fboutclk());\n"
          "  wire mux; cyclonev_clkselect cs (.inclk({2'b00, pclk[1], pclk[0]}), .clkselect({1'b0, sel}),\n"
          "                                   .outclk(mux));\n"
          "  wire user0; cyclonev_hps_interface_clocks_resets cr (.h2f_user0_clk(user0), .h2f_rst_n());\n"
          "  wire [31:0] gp; cyclonev_hps_interface_mpu_general_purpose g (.gp_in(32'd0), .gp_out(gp));\n"
          "  wire c1; cyclonev_lcell_comb lc (.dataa(d), .datab(1'b0), .combout(c1));\n"
          "  wire c2; lcell l2 (c1, c2);\n"
          "  reg a0; always @(posedge pclk[0]) a0 <= c2;\n"
          "  always @(posedge pclk[1]) q <= a0;\n"
          "  always @(posedge mux) r <= d;\n"
          "  always @(posedge user0) s <= a0;\n"
          "  always @(posedge clk_b) t <= gp[0];\n"
          "endmodule\n"}},
        "top",
        "create_clock -period 20 [get_ports clk_50]\n"
        "create_clock -period 10 [get_pins -compatibility_mode *|h2f_user0_clk]\n"
        "derive_pll_clocks\n"
        "set_clock_groups -exclusive -group [get_clocks {pll_i|general[1].gpll~PLL_OUTPUT_COUNTER|divclk}] \\\n"
        "  -group [get_clocks {*|h2f_user0_clk}]\n");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-UNKNOWN-MODULE") == 0);
    CHECK(a.count("CON-UNMATCHED") == 0);
    REQUIRE(a.r->clocks);
    bool aliasOk = false, user0 = false;
    for (const auto& c : a.r->clocks->clocks) {
        aliasOk = aliasOk || std::ranges::find(c.aliases, "pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk") !=
                                 c.aliases.end();
        user0 = user0 || (c.periodNs && *c.periodNs == 10.0 && c.name.find("h2f_user0_clk") != std::string::npos);
    }
    CHECK(aliasOk);
    CHECK(user0);
    // the LUT and lcell are combinational; the PLL outputs are related (same reference)
    CHECK_FALSE(a.crosses("top.a0", "top.q"));
    // the HPS user clock is a clock of its own
    CHECK(a.crosses("top.a0", "top.s"));
    // the clock multiplexer output carries the PLL clocks: r is clocked
    CHECK(a.count("CLK-UNRESOLVED") == 0);
    // negative: the HPS's general-purpose register has no clock domain (not a crossing)
    CHECK_FALSE(a.crosses("gp", "top.t"));
}

TEST_CASE("VHDL: a slice of SHL's result (descending, whatever the argument's range)", "[frontend][vhdl]") {
    test::TempDir dir;
    // VM2413's phase generator (NES_MiSTer): SHL("00000000" & x, n)(19 downto 2), where
    // "00000000" & x is ascending (0 to 17) and SHL returns (17 downto 0)
    const auto a = analyze(dir,
                           {{"p.vhd", "library ieee; use ieee.std_logic_1164.all; use ieee.std_logic_unsigned.all;\n"
                                      "entity p is port (clk : in std_logic; x : in std_logic_vector(11 downto 0);\n"
                                      "  n : in std_logic_vector(2 downto 0); q : out std_logic_vector(9 downto 0));\n"
                                      "end entity;\narchitecture rtl of p is begin\n"
                                      "  process (clk) begin if rising_edge(clk) then\n"
                                      "    q <= SHL(\"000000\" & x, n)(11 downto 2);\n"
                                      "  end if; end process;\nend architecture;\n"}},
                           "p");
    CHECK(a.errors() == 0);
    CHECK(a.count("VHDL-SLICE") == 0);
}

TEST_CASE("Mixed: a defparam'd parameter as a VHDL generic", "[frontend][vhdl]") {
    test::TempDir dir;
    // NES_MiSTer's apu.sv: eReg_SavestateV #(SSREG_INDEX_DMC1, ...) where SSREG_INDEX_DMC1 is set by a defparam
    const auto a = analyze(
        dir,
        {{"e.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                   "entity ent is generic (A : integer; def : std_logic_vector(7 downto 0));\n"
                   "  port (clk : in std_logic; d : in std_logic_vector(7 downto 0);\n"
                   "        q : out std_logic_vector(7 downto 0)); end;\n"
                   "architecture rtl of ent is begin\n"
                   "  process (clk) begin if rising_edge(clk) then q <= d or def; end if; end process;\nend;\n"},
         {"b.sv", "module mid #(parameter [9:0] IDX) (input clk, input [7:0] d, output [7:0] q);\n"
                  "  ent #(IDX, 8'h12) u (.clk(clk), .d(d), .q(q));\nendmodule\n"
                  "module top (input clk, input [7:0] d, output [7:0] q);\n"
                  "  mid m (.clk(clk), .d(d), .q(q));\n  defparam m.IDX = 10'd7;\nendmodule\n"}},
        "top");
    CHECK(a.errors() == 0);
    CHECK(a.count("IR-UNSUPPORTED-PARAM") == 0);
    // negative: a generic without a value and without a parameter stays an error
    test::TempDir dir2;
    const auto b = analyze(
        dir2,
        {{"e.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                   "entity ent is generic (A : integer; def : std_logic_vector(7 downto 0));\n"
                   "  port (clk : in std_logic); end;\narchitecture rtl of ent is begin end;\n"},
         {"b.sv", "module top (input clk);\n  ent #(5) u (.clk(clk));\nendmodule\n"}},
        "top");
    CHECK(b.count("VHDL-GENERIC") == 1);
}

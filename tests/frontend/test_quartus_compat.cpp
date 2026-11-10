// Constructs that are errors in (System)Verilog but that Quartus accepts
// (MiSTer's framework relies on them), the Cyclone V atoms, Quartus' PLL
// clock names, and inout ports driven from inside.
#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "engine/Report.h"
#include "ir/Dump.h"

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
                 const std::string& top, const std::string& sdc = "", const std::string& extraJson = "") {
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
                                                constraints + extraJson + "}");
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

TEST_CASE("Quartus compatibility: function ports declared as wire", "[frontend][quartus]") {
    // C64_MiSTer sid_tables.sv: function signed [15:0] tanh_y_mirror(wire x_neg, wire signed [15:0] y)
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"m.sv", "module m (input clk, input signed [15:0] d, input n, output reg signed [15:0] q);\n"
                                     "  function signed [15:0] mirror(wire x_neg, wire signed [15:0] y);\n"
                                     "    mirror = x_neg ? -y : y;\n"
                                     "  endfunction\n"
                                     "  always @(posedge clk) q <= mirror(n, d);\n"
                                     "endmodule\n"}},
                           "m");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-WIRE-FUNCTION-PORT") == 2);
    // negative: 'wire' as a data type elsewhere is still an error
    test::TempDir dir2;
    const auto b = analyze(dir2, {{"m.sv", "module m (input clk, output reg q);\n"
                                           "  logic [wire] x;\n"
                                           "  always @(posedge clk) q <= 1'b0;\n"
                                           "endmodule\n"}},
                           "m");
    CHECK(b.errors() > 0);
    CHECK(b.count("FE-WIRE-FUNCTION-PORT") == 0);
}

TEST_CASE("True dual-port RAM written from two always blocks", "[frontend][quartus]") {
    // C64_MiSTer iecdrv_mem: one array written on clock_a and on clock_b (as the Intel
    // and Xilinx templates infer a true dual-port RAM)
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"m.sv", "module m (input ca, input cb, input [3:0] aa, input [3:0] ab, input [7:0] da,\n"
                                     "          input [7:0] db, input wa, input wb, output reg [7:0] qa, output reg [7:0] qb,\n"
                                     "          output reg [7:0] r1);\n"
                                     "  reg [7:0] ram[16];\n"
                                     "  always @(posedge ca) begin if (wa) ram[aa] <= da; qa <= ram[aa]; end\n"
                                     "  always @(posedge cb) begin if (wb) ram[ab] <= db; qb <= ram[ab]; end\n"
                                     // negative: written in one block only, read in the other: one array
                                     "  reg [7:0] one[16];\n"
                                     "  always @(posedge ca) one[aa] <= da;\n"
                                     "  always @(posedge cb) r1 <= one[ab];\n"
                                     "endmodule\n"}},
                           "m");
    CHECK(a.errors() == 0);
    CHECK(a.count("IR-MULTIDRIVER") == 0);
    // each port writes its own array, each read port reads both
    CHECK(a.crosses("m.ram#2", "m.qa"));
    CHECK(a.crosses("m.ram", "m.qb"));
    CHECK_FALSE(a.crosses("m.one#2", "m.r1"));
    CHECK(a.crosses("m.one", "m.r1"));
}

TEST_CASE("Errors in a branch of 'if (constant)' that no instance takes are warnings", "[frontend]") {
    // corundum pcie_tlp_fifo_raw.v: if (OUT_TLP_SEG_COUNT > 1) ... x[i*W +: W] with W = $clog2(1) = 0
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"m.v", "module m #(parameter N = 1, parameter W = $clog2(N)) (input clk, input [3:0] d,\n"
                                    "  output reg [3:0] q);\n"
                                    "  reg [N*W-1:0] sel;\n"
                                    "  always @* begin\n"
                                    "    sel = 0;\n"
                                    "    if (N > 1) begin\n"
                                    "      sel[0 +: W] = d[0];\n"
                                    "    end else begin\n"
                                    "      sel = 0;\n"
                                    "    end\n"
                                    "  end\n"
                                    "  always @(posedge clk) q <= d ^ sel;\n"
                                    "endmodule\n"
                                    "module top(input clk, input [3:0] d, output [3:0] q);\n"
                                    "  m u (.clk(clk), .d(d), .q(q));\n"
                                    "endmodule\n"}},
                           "top");
    CHECK(a.errors() == 0);
    CHECK(a.has("FE-SLANG-ValueMustBePositive", "positive"));
    REQUIRE(a.r->cdc);
    CHECK(a.count("IR-ALWAYS-NO-EVENT") == 0);
    CHECK(a.count("IR-UNSUPPORTED-STMT") == 0);
    // negative: the same error where the branch is taken stays an error
    test::TempDir dir2;
    const auto b = analyze(dir2,
                           {{"m.v", "module top #(parameter N = 1, parameter W = $clog2(N)) (input clk, input [3:0] d,\n"
                                    "  output reg [3:0] q);\n"
                                    "  reg [3:0] sel;\n"
                                    "  always @* begin\n"
                                    "    sel = 0;\n"
                                    "    if (N < 2) sel[0 +: W] = d[0];\n"
                                    "  end\n"
                                    "  always @(posedge clk) q <= d ^ sel;\n"
                                    "endmodule\n"}},
                           "top");
    CHECK(b.errors() > 0);
}

TEST_CASE("Black boxes: what only an unknown module connects is its output", "[frontend]") {
    // corundum: the PCIe IP (generated from Tcl, no sources) drives user_clk
    test::TempDir dir;
    const auto a = analyze(dir,
                           {{"m.v", "module top(input clk_a, input d, input [1:0] cfg, output reg q, output reg r);\n"
                                    "  wire user_clk, ip_data;\n"
                                    "  reg a_q;\n"
                                    "  always @(posedge clk_a) a_q <= d;\n"
                                    // a_q and cfg are driven here: inputs of the black box, not hijacked
                                    "  pcie_ip u_ip (.user_clk(user_clk), .data_out(ip_data), .data_in(a_q), .cfg(cfg));\n"
                                    "  always @(posedge user_clk) q <= a_q;\n"
                                    "  always @(posedge user_clk) r <= ip_data;\n"
                                    "endmodule\n"}},
                           "top", "create_clock -name ca -period 10 [get_ports clk_a]\n"
                                  "set_max_delay -datapath_only -from [get_cells a_q_reg] -to [get_cells q_reg] 4\n",
                           ", \"options\": {\"unknownModules\": \"blackbox\"}");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-BLACKBOX-MODULE") == 1);
    CHECK(a.count("CLK-UNRESOLVED") == 0);
    REQUIRE(a.r->clocks);
    CHECK(std::ranges::any_of(a.r->clocks->clocks, [](const auto& c) { return c.name.find("user_clk") != std::string::npos; }));
    // the crossing into the black box's clock domain is analyzed, and the designer's
    // set_max_delay -datapath_only is recorded (the clock relation is unknown)
    REQUIRE(a.r->cdc);
    const auto it = std::ranges::find_if(a.r->cdc->paths, [](const auto& p) {
        return p.srcName == "top.a_q" && p.dstName == "top.q";
    });
    REQUIRE(it != a.r->cdc->paths.end());
    CHECK_FALSE(it->timingException.empty());
    // negative: a_q keeps its own register as driver (no multiple drivers from the black box)
    CHECK(a.count("IR-MULTIDRIVER") == 0);
    CHECK(a.crosses("top.a_q", "top.q"));
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

TEST_CASE("Top-level tristate pads: the logic reading them sees the pin", "[frontend][quartus][pad]") {
    test::TempDir dir;
    // MiSTer: assign USER_IO[2] = !(SW[1] ? HDMI_I2S : user_out[2]) ? 1'b0 : 1'bZ; user_in <= USER_IO
    const auto a = analyze(dir,
                           {{"top.v", "module top (input clk_a, input clk_b, input oe, inout [1:0] io,\n"
                                      "            output reg q, output reg q2, inout plain, output reg q3);\n"
                                      "  reg r; always @(posedge clk_a) r <= ~r;\n"
                                      "  assign io[0] = oe ? r : 1'bz;\n"
                                      "  assign io[1] = !r ? 1'b0 : 1'bz;\n"
                                      "  always @(posedge clk_b) begin q <= io[0]; q2 <= io[1]; end\n"
                                      "  assign plain = r;\n" // negative: an inout used as a plain output
                                      "  always @(posedge clk_b) q3 <= plain;\n"
                                      "endmodule\n"}},
                           "top",
                           "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                           "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                           "set_clock_groups -asynchronous -group clk_a -group clk_b\n"
                           "set_output_delay -clock clk_b 1 [get_ports {io[0]}]\n");
    CHECK(a.errors() == 0);
    const auto toExactly = [&](std::string_view dst) {
        return std::ranges::any_of(a.r->cdc->paths, [&](const auto& p) { return p.dstName == dst; });
    };
    REQUIRE(a.r->cdc);
    CHECK_FALSE(toExactly("top.q"));
    CHECK_FALSE(toExactly("top.q2"));
    CHECK(a.has("CDC-NO-DOMAIN", "'top.io'"));
    CHECK(a.crosses("top.r", "top.q3"));
    // what the design drives out of the pad is still checked against its output delay
    CHECK(a.has("CDC-OUTPUT-CLOCK", "output 'top.io[0]'"));
}

TEST_CASE("Quartus SDC: plain names are keeper collections; a false path on a crossing is shown",
          "[frontend][quartus][sdc]") {
    // MiSTer sys_top.sdc: set_false_path -from {cfg[*]}, set_false_path -from {*_osd|rot*}
    const char* const rtl = "module osd (input clk_a, input clk_b, input [1:0] d, output reg [1:0] q);\n"
                            "  reg [1:0] rot; always @(posedge clk_a) rot <= d;\n"
                            "  always @(posedge clk_b) q <= rot;\n"
                            "endmodule\n"
                            "module top (input clk_a, input clk_b, input [3:0] d, output reg [3:0] q, output [1:0] o,\n"
                            "            output reg [3:0] q2);\n"
                            "  reg [3:0] cfg; reg [3:0] other;\n"
                            "  always @(posedge clk_a) begin cfg <= d; other <= ~d; end\n"
                            "  always @(posedge clk_b) begin q <= cfg; q2 <= other; end\n"
                            "  osd hdmi_osd (.clk_a(clk_a), .clk_b(clk_b), .d(d[1:0]), .q(o));\n"
                            "endmodule\n";
    const std::string clocks = "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                               "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                               "set_clock_groups -asynchronous -group clk_a -group clk_b\n";
    auto exceptionOf = [](const Analysis& a, std::string_view src) {
        for (const auto& p : a.r->cdc->paths)
            if (p.srcName.starts_with(src))
                return p.timingException;
        return std::string("<no path>");
    };
    SECTION("Quartus constraints (derive_pll_clocks): {cfg[*]} names the register") {
        test::TempDir dir;
        const auto a = analyze(dir, {{"top.sv", rtl}}, "top",
                               "derive_pll_clocks\n" + clocks + "set_false_path -from {cfg[*]}\n");
        REQUIRE(a.r->cdc);
        CHECK(a.count("CON-UNMATCHED") == 0);
        CHECK(exceptionOf(a, "top.cfg").starts_with("set_false_path ("));
        CHECK(exceptionOf(a, "top.other").empty()); // not covered
        // still a crossing without synchronizer: the exception is shown, not applied
        CHECK(a.crosses("top.cfg", "top.q"));
        // reports: a CSV column, an HTML column and detail row
        const std::string csv = engine::pathsCsv(*a.r);
        CHECK(csv.find("destination_line,timing_exception") != std::string::npos);
        CHECK(csv.find("set_false_path (") != std::string::npos);
        const std::string html = engine::toHtmlReport(*a.r);
        CHECK(html.find("<th>Timing exception</th>") != std::string::npos);
        CHECK(html.find("set_false_path c.sdc:") != std::string::npos);
    }
    SECTION("a Quartus hierarchical name ('|') is read so in any constraints") {
        test::TempDir dir;
        const auto a = analyze(dir, {{"top.sv", rtl}}, "top", clocks + "set_false_path -from {*_osd|rot*}\n");
        REQUIRE(a.r->cdc);
        CHECK(exceptionOf(a, "top.hdmi_osd.rot").starts_with("set_false_path ("));
        CHECK(exceptionOf(a, "top.cfg").empty());
    }
    SECTION("negative: a plain name in other constraints is not evaluated") {
        test::TempDir dir;
        const auto a = analyze(dir, {{"top.sv", rtl}}, "top", clocks + "set_false_path -from {cfg[*]}\n");
        REQUIRE(a.r->cdc);
        CHECK(a.has("CON-IGNORED", "set_false_path x1"));
        CHECK(exceptionOf(a, "top.cfg").empty());
    }
}

TEST_CASE("Waivers selecting crossings by the timing exception that covers them", "[frontend][quartus][sdc][waiver]") {
    test::TempDir dir;
    dir.write("top.sv", "module top (input clk_a, input clk_b, input [3:0] d, output reg [3:0] q, output reg [3:0] q2);\n"
                        "  reg [3:0] cfg; reg [3:0] other;\n"
                        "  always @(posedge clk_a) begin cfg <= d; other <= ~d; end\n"
                        "  always @(posedge clk_b) begin q <= cfg; q2 <= other; end\n"
                        "endmodule\n");
    dir.write("c.sdc", "derive_pll_clocks\n"
                       "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                       "create_clock -name clk_b -period 7 [get_ports clk_b]\n"
                       "set_clock_groups -asynchronous -group clk_a -group clk_b\n"
                       "set_false_path -from {cfg[*]}\n");
    auto run = [&](const std::string& selector) {
        dir.write("w.json", "{\"format\": \"opencdc-waivers\", \"version\": 1, \"waivers\": [{\"id\": \"W-QS\", "
                            "\"timingException\": \"" + selector + "\", \"reason\": \"quasi-static\"}]}");
        const auto p = dir.write("p.ocdc.json",
                                 "{\"format\": \"opencdc-project\", \"version\": 1, \"name\": \"p\", \"top\": \"top\", "
                                 "\"sources\": [\"top.sv\"], \"constraints\": [\"c.sdc\"], \"waivers\": [\"w.json\"]}");
        auto cfg = engine::loadProjectFile(p);
        REQUIRE(cfg);
        engine::AnalysisSession session;
        NullProgressSink progress;
        CancellationToken cancel;
        Analysis a{session.run(cfg.value(), progress, cancel)};
        REQUIRE(a.r);
        REQUIRE(a.r->cdc);
        for (const auto& d : a.r->diagnostics)
            UNSCOPED_INFO(format(d));
        return a;
    };
    auto waived = [](const Analysis& a, std::string_view src) {
        for (const auto& p : a.r->cdc->paths)
            if (p.srcName.starts_with(src))
                return p.waived;
        return false;
    };
    SECTION("any timing exception") {
        const auto a = run("*");
        CHECK(a.count("WAIVER-INVALID") == 0);
        CHECK(waived(a, "top.cfg"));
        CHECK_FALSE(waived(a, "top.other")); // no exception covers it
        REQUIRE(a.r->cdc->waivers.size() == 1);
        CHECK(a.r->cdc->waivers[0].uses > 0);
    }
    SECTION("negative: another constraint file") {
        const auto a = run("*other.sdc*");
        CHECK_FALSE(waived(a, "top.cfg"));
        CHECK(a.r->cdc->waivers[0].uses == 0);
    }
}

TEST_CASE("Output net ports with a value are continuous assignments", "[quartus-compat][port-default]") {
    // SERV's go_board and PolarFire tops: `output wire o_led2 = 1'b0` (Yosys and Vivado accept it)
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module top(input wire clk, input wire d,\n"
                              "           output wire o_led = 1'b1,\n"
                              "           output wire [1:0] o_two =\n"
                              "             {d, 1'b0},\n"
                              "           output reg q = 1'b0);\n"
                              "  always @(posedge clk) q <= o_two[1] & o_led;\n"
                              "endmodule\n"}},
                     "top");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-PORT-NET-DEFAULT") == 2); // not for the variable q, which may have one
    CHECK(a.has("FE-PORT-NET-DEFAULT", "'o_two'"));
    REQUIRE(a.r->design);
    CHECK(a.r->design->stats().registers == 1);
    // the value drives the port
    const auto dump = ir::dumpDesign(*a.r->design);
    UNSCOPED_INFO(dump);
    CHECK(dump.find("assign o_led = 1'b1") != std::string::npos);
    CHECK(dump.find("assign o_two = {d, 1'b0}") != std::string::npos);
    // negative: an inout port with a value stays an error
    test::TempDir dir2;
    auto b = analyze(dir2, {{"t.v", "module top(inout wire io = 1'b0); endmodule\n"}}, "top");
    CHECK(b.errors() >= 1);
    CHECK(b.count("FE-PORT-NET-DEFAULT") == 0);
}

TEST_CASE("Quartus SDC: clock patterns after derive_pll_clocks, net names with entities", "[quartus-compat][sdc]") {
    // BrianHG DDR3 DECA demo: set_input_delay -clock [get_clocks {*DDR3_PLL5*clk[2]}],
    // create_clock [get_nets {I2C_HDMI_Config:u_I2C_HDMI_Config|mI2C_CTRL_CLK}]
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module i2c(input clk, output reg slow, input d, output reg q);\n"
                              "  reg [3:0] n; always @(posedge clk) begin n <= n + 1'b1; slow <= n[3]; end\n"
                              "  always @(posedge slow) q <= d;\n"
                              "endmodule\n"
                              "module top(input clk50, input din, output reg q, output reg q2, output qs);\n"
                              "  wire [4:0] c; wire s;\n"
                              "  altpll #(.clk0_multiply_by(2), .clk0_divide_by(1), .inclk0_input_frequency(20000))\n"
                              "    PLL5 (.inclk({1'b0, clk50}), .clk(c));\n"
                              "  always @(posedge c[0]) q <= din;\n"
                              "  always @(posedge clk50) q2 <= din;\n"
                              "  i2c u_i2c (.clk(clk50), .slow(s), .d(din), .q(qs));\n"
                              "endmodule\n"}},
                     "top",
                     "create_clock -period 20 [get_ports clk50]\n"
                     "create_clock -period \"1.0 MHz\" [get_nets {i2c:u_i2c|slow}]\n"
                     "derive_pll_clocks\n"
                     "set_input_delay -clock [get_clocks {*PLL5*clk[0]}] -max 1 [get_ports din]\n"
                     // negative: a pattern that matches no clock
                     "set_output_delay -clock [get_clocks {*PLL7*clk[0]}] -max 1 [get_ports q]\n");
    CHECK(a.errors() == 0);
    REQUIRE(a.r->clocks);
    CHECK(std::ranges::any_of(a.r->clocks->clocks, [](const auto& c) {
        return c.name == "i2c:u_i2c|slow" && c.periodNs && *c.periodNs == 1000.0;
    }));
    CHECK(a.count("CON-UNMATCHED") == 0);
    // din is in the PLL clock domain: it crosses into clk50 (asynchronous to nothing declared: related)
    CHECK(a.count("CDC-INPUT-CLOCK") == 0);
    CHECK(a.has("CDC-OUTPUT-CLOCK", "unknown clock '*PLL7*clk[0]'"));
}

TEST_CASE("MAX 10 I/O atoms of altera_gpio_lite", "[quartus-compat][max10]") {
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module top(input clk_a, input clk_b, input d0, input d1, input pin, output pad,\n"
                              "           output reg q, output reg h);\n"
                              "  wire o, ddr_hi, ddr_lo, hr;\n"
                              "  fiftyfivenm_ddio_out #(.async_mode(\"none\")) out (.datainhi(d0), .datainlo(d1),\n"
                              "    .clkhi(clk_a), .clklo(clk_a), .muxsel(clk_a), .ena(1'b1), .dataout(o));\n"
                              "  fiftyfivenm_io_obuf ob (.i(o), .oe(1'b1), .o(pad), .obar());\n"
                              "  fiftyfivenm_ddio_in in_ (.datain(pin), .clk(clk_b), .ena(1'b1), .regouthi(ddr_hi),\n"
                              "    .regoutlo(ddr_lo), .clkout());\n"
                              "  fiftyfivenm_io_clock_divider div (.clk(clk_a), .clkout(hr));\n"
                              "  always @(posedge clk_b) q <= ddr_hi ^ ddr_lo ^ pad;\n"
                              "  always @(posedge hr) h <= d0;\n"
                              "endmodule\n"}},
                     "top", "create_clock -period 10 [get_ports clk_a]\ncreate_clock -period 7 [get_ports clk_b]\n");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-UNKNOWN-MODULE") == 0);
    CHECK(a.count("FE-PRIMITIVE-PORT") == 1); // clkout of the DDR input is not modelled
    REQUIRE(a.r->clocks);
    CHECK(std::ranges::any_of(a.r->clocks->clocks, [](const auto& c) {
        return c.name == "top.div.clkout" && c.periodNs && *c.periodNs == 20.0;
    }));
    // the DDR output register (clk_a) is read in clk_b: a crossing
    CHECK(a.crosses("top.out.hi", "top.q"));
    CHECK_FALSE(a.crosses("top.in_.hi", "top.q")); // the DDR input registers are in clk_b
}

TEST_CASE("altddio_bidir and the Cyclone V names of PLL clocks", "[quartus-compat][cyclonev]") {
    // BrianHG DDR3 on a Cyclone V: DQ/DQS through altddio_bidir; the SDC names the clocks of
    // altera_pll as *DDR3_PLL5*counter[2]* and of altpll as ...|auto_generated|generic_pll1~...
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module top(input clk50, input clk_b, input [7:0] w, inout [7:0] dq, output reg [7:0] q,\n"
                              "           output reg [7:0] v, output reg [7:0] x);\n"
                              "  wire [1:0] o; wire [4:0] c; wire [7:0] rh, rl;\n"
                              "  altera_pll #(.reference_clock_frequency(\"50.0 MHz\"), .number_of_clocks(2),\n"
                              "    .output_clock_frequency0(\"100.0 MHz\"), .output_clock_frequency1(\"100.0 MHz\"))\n"
                              "    DDR3_PLL5 (.refclk(clk50), .rst(1'b0), .outclk(o));\n"
                              "  altpll #(.clk0_multiply_by(3), .clk0_divide_by(2), .inclk0_input_frequency(20000))\n"
                              "    VGA (.inclk({1'b0, clk50}), .clk(c));\n"
                              "  altddio_bidir #(.width(8)) io (.outclock(o[0]), .outclocken(1'b1), .oe(1'b1),\n"
                              "    .inclock(o[1]), .inclocken(1'b1), .datain_h(w), .datain_l(w), .padio(dq),\n"
                              "    .dataout_h(rh), .dataout_l(rl), .aclr(1'b0), .aset(1'b0), .sclr(1'b0), .sset(1'b0));\n"
                              "  always @(posedge o[1]) q <= rh ^ rl;\n"
                              "  always @(posedge c[0]) v <= q;\n"
                              "  always @(posedge clk_b) x <= w;\n"
                              "endmodule\n"}},
                     "top",
                     "create_clock -period 20 [get_ports clk50]\n"
                     "create_clock -period 7 [get_ports clk_b]\n"
                     "derive_pll_clocks\n"
                     "set_input_delay -clock [get_clocks {*DDR3_PLL5*counter[1]*}] -max 1 [get_ports {dq[*]}]\n"
                     "set_output_delay -clock [get_clocks {VGA|auto_generated|generic_pll1~PLL_OUTPUT_COUNTER|divclk}] "
                     "-max 1 [get_ports {v[*]}]\n"
                     // negative: an output of another clock domain
                     "set_output_delay -clock [get_clocks {*DDR3_PLL5*counter[1]*}] -max 1 [get_ports {x[*]}]\n");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-UNKNOWN-MODULE") == 0);
    CHECK(a.count("CDC-INPUT-CLOCK") == 0);
    CHECK_FALSE(a.has("CDC-OUTPUT-CLOCK", "'top.v"));
    CHECK(a.has("CDC-OUTPUT-CLOCK", "'top.x"));
    // the pad is read by the input registers of the DDR I/O, in inclock's domain
    CHECK_FALSE(a.crosses("top.io.dataout_h", "top.q"));
}

TEST_CASE("$size of an instance array is its size, as Vivado evaluates it", "[vivado-compat][size]") {
    // taxi (KC705 example): .PORTS($size(xfcp_sw_us)) with taxi_axis_if xfcp_sw_us[2]()
    const std::string lib = "interface bus_if; logic v; endinterface\n"
                            "module sink #(parameter N = 1)(input wire clk, input wire [N-1:0] d, output reg [N-1:0] q);\n"
                            "  always @(posedge clk) q <= d;\n"
                            "endmodule\n";
    SECTION("[N], a parameter expression and a range") {
        test::TempDir dir;
        auto a = analyze(dir,
                         {{"t.sv", lib + "module top #(parameter K = 3)(input wire clk, input wire [15:0] d,\n"
                                         "  output wire [2:0] q1, output wire [3:0] q2, output wire [4:0] q3);\n"
                                         "  bus_if two[3]();\n"
                                         "  bus_if many[K+1]();\n"
                                         "  bus_if span[6:2]();\n"
                                         "  sink #(.N($size(two))) s1(.clk(clk), .d(d[2:0]), .q(q1));\n"
                                         "  sink #(.N($size( many ))) s2(.clk(clk), .d(d[3:0]), .q(q2));\n"
                                         "  sink #(.N($size(span))) s3(.clk(clk), .d(d[4:0]), .q(q3));\n"
                                         "endmodule\n"}},
                         "top");
        CHECK(a.errors() == 0);
        CHECK(a.count("FE-SIZE-INSTANCE-ARRAY") == 3);
        CHECK(a.has("FE-SIZE-INSTANCE-ARRAY", "'two'"));
        CHECK(a.has("FE-SIZE-INSTANCE-ARRAY", "replaced by its size (3)"));
        CHECK(a.has("FE-SIZE-INSTANCE-ARRAY", "replaced by its size (K+1)"));
        // the warning has no empty note
        for (const auto& d : a.r->diagnostics)
            if (d.code == "FE-SIZE-INSTANCE-ARRAY")
                CHECK(d.notes.empty());
        REQUIRE(a.r->design);
        // 3 + 4 + 5 register bits: the sizes became the widths
        const auto dump = ir::dumpDesign(*a.r->design);
        UNSCOPED_INFO(dump);
        CHECK(a.r->design->stats().registerBits == 12);
    }
    SECTION("negative: $size of a single instance is still an error") {
        test::TempDir dir;
        auto a = analyze(dir,
                         {{"t.sv", lib + "module top(input wire clk, input wire [3:0] d, output wire [3:0] q);\n"
                                         "  bus_if one();\n"
                                         "  sink #(.N($size(one))) s(.clk(clk), .d(d), .q(q));\n"
                                         "endmodule\n"}},
                         "top");
        CHECK(a.errors() >= 1);
        CHECK(a.count("FE-SIZE-INSTANCE-ARRAY") == 0);
    }
}

TEST_CASE("Initial blocks: loops bounded by a value computed from a wide string parameter", "[frontend][initial]") {
    // taxi's XFCP modules: the ID ROM is filled from a string parameter of more than 64 bits; the
    // string length j is found by a loop and bounds the next one
    auto design = [](const std::string& str) {
        return "module top #(parameter ID_STR = " + str + ")(input wire clk, input wire [5:0] a, output reg [7:0] q);\n"
               "  logic [7:0] rom[64];\n"
               "  integer j;\n"
               "  initial begin\n"
               "    for (integer i = 0; i < 64; i = i + 1) rom[i] = 0;\n"
               "    j = 0;\n"
               "    for (integer i = 1; i <= 16; i = i + 1)\n"
               "      if (j == i-1 && (ID_STR >> (i*8)) > 0) j = i;\n"
               "    for (integer i = 0; i <= j; i = i + 1) rom[i+16] = ID_STR[8*(j-i) +: 8];\n"
               "  end\n"
               "  always @(posedge clk) q <= rom[a];\n"
               "endmodule\n";
    };
    for (const std::string str : {"\"I2C\"", "\"Statistics\"", "\"0123456789abcdef\""}) {
        test::TempDir dir;
        auto a = analyze(dir, {{"t.sv", design(str)}}, "top");
        INFO(str);
        CHECK(a.errors() == 0);
        CHECK(a.count("IR-UNSUPPORTED-LOOP") == 0);
        CHECK(a.count("IR-INITIAL-IGNORED") == 0);
    }
    // negative: a bound that depends on an input is still not unrolled
    test::TempDir dir;
    auto b = analyze(dir,
                     {{"t.sv", "module top(input wire clk, input wire [3:0] n, output reg [7:0] q);\n"
                               "  always @(posedge clk) for (integer i = 0; i < n; i = i + 1) q[i] <= 1'b1;\n"
                               "endmodule\n"}},
                     "top");
    CHECK(b.count("IR-UNSUPPORTED-LOOP") == 1);
}

TEST_CASE("A parameter of an interface instance in a constant expression, as Vivado accepts it", "[vivado-compat][hier-const]") {
    // taxi's 10G/25G examples: .DATA_W(axis_qsfp_tx[0].DATA_W), .ID_W(axis_stat.ID_W)
    const std::string lib = "interface bus_if #(parameter DATA_W = 8); logic [DATA_W-1:0] d; endinterface\n"
                            "module sink #(parameter N = 1)(input wire clk, input wire [N-1:0] d, output reg [N-1:0] q);\n"
                            "  always @(posedge clk) q <= d;\n"
                            "endmodule\n";
    SECTION("an element of an interface array and a single instance") {
        test::TempDir dir;
        auto a = analyze(dir,
                         {{"t.sv", lib + "module top(input wire clk, input wire [15:0] d, output wire [5:0] q1,\n"
                                         "           output wire [9:0] q2);\n"
                                         "  bus_if #(.DATA_W(6)) arr[2]();\n"
                                         "  bus_if #(.DATA_W(10)) one();\n"
                                         "  sink #(.N(arr[0].DATA_W)) s1(.clk(clk), .d(d[5:0]), .q(q1));\n"
                                         "  sink #(.N(one.DATA_W)) s2(.clk(clk), .d(d[9:0]), .q(q2));\n"
                                         "endmodule\n"}},
                         "top");
        CHECK(a.errors() == 0);
        REQUIRE(a.r->design);
        CHECK(a.r->design->stats().registerBits == 16);
    }
    SECTION("negative: a signal is still not a constant") {
        test::TempDir dir;
        auto a = analyze(dir,
                         {{"t.sv", lib + "module top(input wire clk, input wire [15:0] d, output wire [5:0] q1);\n"
                                         "  bus_if #(.DATA_W(6)) one();\n"
                                         "  sink #(.N(one.d)) s1(.clk(clk), .d(d[5:0]), .q(q1));\n"
                                         "endmodule\n"}},
                         "top");
        CHECK(a.errors() >= 1);
    }
}

TEST_CASE("An assignment pattern on an output port connection is a target", "[frontend][pattern-lvalue]") {
    // taxi's KC705 10G example: .xcvr_txp('{sfp_tx_p}) on `output wire logic xcvr_txp[CNT]`
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.sv", "module leaf(input wire logic clk_a, input wire logic clk_b, input wire logic d,\n"
                               "            output wire logic q[2]);\n"
                               "  logic ra, rb;\n"
                               "  always_ff @(posedge clk_a) ra <= d;\n"
                               "  always_ff @(posedge clk_b) rb <= d;\n"
                               "  assign q[0] = ra;\n"
                               "  assign q[1] = rb;\n"
                               "endmodule\n"
                               "module top(input wire logic clk_a, input wire logic clk_b, input wire logic d,\n"
                               "           output logic ya, output logic yb);\n"
                               "  wire logic a, b;\n"
                               "  leaf u(.clk_a(clk_a), .clk_b(clk_b), .d(d), .q('{a, b}));\n"
                               "  always_ff @(posedge clk_b) begin ya <= a; yb <= b; end\n"
                               "endmodule\n"}},
                     "top", "create_clock -period 10 [get_ports clk_a]\ncreate_clock -period 7 [get_ports clk_b]\n");
    CHECK(a.errors() == 0);
    CHECK(a.count("IR-UNSUPPORTED-LVALUE") == 0);
    // the first element is q[0] (the left index): a comes from clk_a, b from clk_b
    CHECK(a.crosses("top.u.ra", "top.ya"));
    CHECK_FALSE(a.crosses("top.u.rb", "top.yb"));
    CHECK_FALSE(a.crosses("top.u.ra", "top.yb"));
}

TEST_CASE("Yosys compatibility: a unary operator applied to a unary operation", "[yosys-compat][unary]") {
    // the CSI-2 receiver's iCEBreaker top: else if (!&(btn_debounce)) btn_debounce <= btn_debounce + 1;
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module top(input clk, input [3:0] m, output reg [3:0] c, output reg n);\n"
                              "  always @(posedge clk) begin\n"
                              "    if (!&(c)) c <= c + 1'b1;\n"
                              "    n <= ~|m[3:1] ^ !^ m;\n"
                              "  end\n"
                              "endmodule\n"}},
                     "top");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-UNARY-OPERAND") == 2);
    CHECK(a.has("FE-UNARY-OPERAND", "'!&(c)'"));
    // negative: written with parentheses there is nothing to report
    test::TempDir dir2;
    auto b = analyze(dir2,
                     {{"t.v", "module top(input clk, output reg [3:0] c);\n"
                              "  always @(posedge clk) if (!(&c)) c <= c + 1'b1;\n"
                              "endmodule\n"}},
                     "top");
    CHECK(b.errors() == 0);
    CHECK(b.count("FE-UNARY-OPERAND") == 0);
}

TEST_CASE("Yosys compatibility: a part select bounded by a loop variable in a comparison", "[yosys-compat][variable-range]") {
    // the CSI-2 receiver's byte aligner: if (... && (last_byte[i:0] == 0)) inside for (i = 0; i < 8; ...)
    test::TempDir dir;
    auto a = analyze(dir,
                     {{"t.v", "module top(input clk, input [7:0] b, output reg [2:0] o);\n"
                              "  reg [2:0] off; integer i;\n"
                              "  always @(*) begin\n"
                              "    off = 0;\n"
                              "    for (i = 0; i < 8; i = i + 1)\n"
                              "      if (b[i:0] == 0) off = i;\n"
                              "  end\n"
                              "  always @(posedge clk) o <= off;\n"
                              "endmodule\n"}},
                     "top");
    CHECK(a.errors() == 0);
    CHECK(a.count("FE-VARIABLE-RANGE") == 1);
    CHECK(a.has("FE-VARIABLE-RANGE", "'b[i:0]'"));
    // negative: outside a comparison the width matters: still an error
    test::TempDir dir2;
    auto b = analyze(dir2,
                     {{"t.v", "module top(input clk, input [7:0] b, output reg [7:0] o);\n"
                              "  integer i;\n"
                              "  always @(posedge clk)\n"
                              "    for (i = 0; i < 8; i = i + 1) o <= {b[i:0], 1'b0};\n"
                              "endmodule\n"}},
                     "top");
    CHECK(b.errors() >= 1);
    CHECK(b.count("FE-VARIABLE-RANGE") == 0);
}

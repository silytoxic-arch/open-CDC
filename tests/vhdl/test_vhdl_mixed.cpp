// Mixed-language designs (T8.4): Verilog instantiating VHDL and VHDL
// instantiating Verilog, parameters/generics and ports across the border,
// nesting that needs several elaboration rounds, and the error cases.

#include "../TestUtil.h"
#include "core/Diagnostic.h"
#include "frontend/Frontend.h"
#include "ir/Dump.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace opencdc;

namespace {

struct MixedRun {
    std::shared_ptr<const ir::Design> design;
    std::vector<Diagnostic> diags;
    frontend::ElaborationSummary summary;
    std::string dump;

    [[nodiscard]] std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == code; }));
    }
    [[nodiscard]] std::size_t errors() const {
        return static_cast<std::size_t>(std::ranges::count_if(diags, [](const Diagnostic& d) { return d.isError(); }));
    }
    [[nodiscard]] bool has(std::string_view text) const { return dump.find(text) != std::string::npos; }
};

MixedRun run(const std::vector<std::pair<std::string, std::string>>& files, std::string top,
             std::vector<std::pair<std::string, std::string>> params = {}) {
    test::TempDir dir;
    frontend::FrontendInput in;
    for (const auto& [name, text] : files)
        in.sourceFiles.push_back(dir.write(name, text));
    in.top = std::move(top);
    in.topParameters = std::move(params);
    CollectingDiagnosticSink sink;
    auto out = frontend::makeDefaultFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    MixedRun r;
    r.design = out.value().design;
    r.summary = out.value().summary;
    r.diags = sink.take();
    if (r.design)
        r.dump = ir::dumpDesign(*r.design);
    for (const auto& d : r.diags)
        UNSCOPED_INFO(format(d));
    UNSCOPED_INFO(r.dump);
    return r;
}

const char* kVhdlSync = "library ieee; use ieee.std_logic_1164.all;\n"
                        "entity vsync is generic (W : positive := 1; INIT : std_logic := '0');\n"
                        "  port (clk : in std_logic; d : in std_logic_vector(W-1 downto 0);\n"
                        "        q : out std_logic_vector(W-1 downto 0)); end;\n"
                        "architecture rtl of vsync is\n"
                        "  signal s1, s2 : std_logic_vector(W-1 downto 0) := (others => INIT);\n"
                        "begin\n"
                        "  process (clk) begin if rising_edge(clk) then s1 <= d; s2 <= s1; end if; end process;\n"
                        "  q <= s2;\n"
                        "end;\n";

} // namespace

TEST_CASE("Mixed: Verilog top instantiates a VHDL entity with parameters", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk_a, input clk_b, input [7:0] din, output [7:0] dout);\n"
                            "  reg [7:0] a;\n"
                            "  always @(posedge clk_a) a <= din;\n"
                            "  vsync #(.W(8), .INIT(1'b1)) u_sync (.clk(clk_b), .d(a), .q(dout));\n"
                            "endmodule\n"}},
                 "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.design->instances.size() == 2);
    CHECK(r.has("module vsync#(W=8, INIT='1')"));
    CHECK(r.has("inst u_sync : vsync (.clk(clk_b), .d(a), .q(dout))"));
    CHECK(r.has("wire [7:0] s1 init=8'b11111111"));
    REQUIRE(r.summary.topInstances.size() == 1);
    CHECK(r.summary.topInstances.front().children.front().definition == "vsync");
}

TEST_CASE("Mixed: VHDL top instantiates a Verilog module with generics", "[vhdl][mixed]") {
    auto r = run({{"vsync.v", "module vlog_sync #(parameter WIDTH = 1, parameter MODE = \"FAST\") (\n"
                              "  input clk, input [WIDTH-1:0] d, output [WIDTH-1:0] q);\n"
                              "  reg [WIDTH-1:0] s1, s2;\n"
                              "  always @(posedge clk) begin s1 <= d; s2 <= s1; end\n"
                              "  assign q = (MODE == \"FAST\") ? s2 : s1;\n"
                              "endmodule\n"},
                  {"top.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                              "entity top is port (clk : in std_logic; d : in std_logic_vector(3 downto 0);\n"
                              "  q, r : out std_logic_vector(3 downto 0)); end;\n"
                              "architecture rtl of top is\n"
                              "  component vlog_sync is generic (width : natural; mode : string);\n"
                              "    port (clk : in std_logic; d : in std_logic_vector; q : out std_logic_vector);\n"
                              "  end component;\n"
                              "begin\n"
                              "  u0 : vlog_sync generic map (width => 4, mode => \"FAST\") port map (clk, d, q);\n"
                              "  u1 : entity work.vlog_sync generic map (WIDTH => 4, MODE => \"SLOW\")\n"
                              "    port map (clk => clk, d => d, q => r);\n"
                              "end;\n"}},
                 "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.design->instances.size() == 3);
    // two parameterizations (MODE "FAST" / "SLOW"): two bodies
    CHECK(std::ranges::count_if(r.design->bodies, [](const ir::ModuleBody& b) {
              return b.name == "vlog_sync" && b.params.starts_with("#(WIDTH=4, MODE=");
          }) == 2);
    CHECK(r.has("inst u0 : vlog_sync (.clk(clk), .d(d), .q(q))"));
    // the wrapper of the Verilog requests is not part of the design
    CHECK_FALSE(r.has("opencdc_mixed_wrapper"));
}

TEST_CASE("Mixed: VHDL -> Verilog -> VHDL -> Verilog nesting converges", "[vhdl][mixed]") {
    auto r = run({{"leaf.v", "module leaf #(parameter N = 1) (input clk, input [N-1:0] d, output reg [N-1:0] q);\n"
                             "  always @(posedge clk) q <= d;\n"
                             "endmodule\n"},
                  {"mid.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                              "entity mid is generic (N : positive := 2);\n"
                              "  port (clk : in std_logic; d : in std_logic_vector(N-1 downto 0);\n"
                              "        q : out std_logic_vector(N-1 downto 0)); end;\n"
                              "architecture rtl of mid is begin\n"
                              "  u_leaf : entity work.leaf generic map (N => N) port map (clk => clk, d => d, q => q);\n"
                              "end;\n"},
                  {"upper.v", "module upper #(parameter N = 3) (input clk, input [N-1:0] d, output [N-1:0] q);\n"
                              "  mid #(.N(N)) u_mid (.clk(clk), .d(d), .q(q));\n"
                              "endmodule\n"},
                  {"top.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                              "entity top is port (clk : in std_logic; d : in std_logic_vector(4 downto 0);\n"
                              "  q : out std_logic_vector(4 downto 0)); end;\n"
                              "architecture rtl of top is begin\n"
                              "  u_up : entity work.upper generic map (N => 5) port map (clk, d, q);\n"
                              "end;\n"}},
                 "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.design->instances.size() == 4);
    CHECK(r.has("module leaf#(N=5)"));
    CHECK(r.has("reg q <= d @(posedge clk)"));
    CHECK(r.design->stats().registerBits == 5);
}

TEST_CASE("Mixed: top inferred across languages", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk, input [1:0] d, output [1:0] q);\n"
                            "  vsync #(2) u (.clk(clk), .d(d), .q(q));\n"
                            "endmodule\n"}},
                 "");
    REQUIRE(r.design);
    REQUIRE(r.design->tops.size() == 1); // vsync is instantiated from Verilog: not a top
    CHECK(r.design->instance(r.design->tops.front()).hierPath == "top");
}

TEST_CASE("Mixed: narrower Verilog actual is zero-extended at a VHDL input", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk, input [1:0] d, output [3:0] q);\n"
                            "  vsync #(.W(4)) u (.clk(clk), .d(d), .q(q));\n"
                            "endmodule\n"}},
                 "top");
    REQUIRE(r.design);
    CHECK(r.has(".d({2'b00, d})"));
}

TEST_CASE("Mixed negative: unknown module in a mixed design", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk, input d, output q);\n"
                            "  nothing u (.a(d));\n"
                            "  vsync u2 (.clk(clk), .d(d), .q(q));\n"
                            "endmodule\n"}},
                 "top");
    CHECK_FALSE(r.design);
    CHECK(r.count("FE-UNKNOWN-MODULE") == 1);
}

TEST_CASE("Mixed negative: port that the VHDL entity does not have", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk, input d, output q);\n"
                            "  vsync u (.clk(clk), .data(d), .q(q));\n"
                            "endmodule\n"}},
                 "top");
    CHECK_FALSE(r.design);
    CHECK(r.count("FE-MIXED-PORT") == 1);
}

TEST_CASE("Mixed negative: parameter value of the wrong kind for a VHDL generic", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.v", "module top(input clk, input d, output q);\n"
                            "  vsync #(.W(\"WIDE\")) u (.clk(clk), .d(d), .q(q));\n"
                            "endmodule\n"}},
                 "top");
    CHECK_FALSE(r.design);
    CHECK(r.count("VHDL-GENERIC") >= 1);
}

TEST_CASE("Mixed negative: name defined in both languages is reported", "[vhdl][mixed]") {
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"vsync.v", "module vsync #(parameter W = 1) (input clk, input [W-1:0] d, output [W-1:0] q);\n"
                              "  assign q = d;\nendmodule\n"},
                  {"top.v", "module top(input clk, input d, output q);\n"
                            "  vsync u (.clk(clk), .d(d), .q(q));\nendmodule\n"}},
                 "top");
    REQUIRE(r.design);
    CHECK(r.count("FE-MIXED-DUPLICATE") == 1);
    CHECK(r.design->stats().registers == 0); // the Verilog module is used
}

TEST_CASE("Mixed: a top that names nothing suggests the closest units", "[vhdl][mixed]") {
    // Digilent's MIPI_CSI2_RxTop.vhd declares 'mipi_csi2_rx_top'
    auto r = run({{"vsync.vhd", kVhdlSync},
                  {"top.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                              "entity mipi_csi2_rx_top is port (clk : in std_logic; d : in std_logic_vector(0 downto 0);\n"
                              "  q : out std_logic_vector(0 downto 0)); end;\n"
                              "architecture rtl of mipi_csi2_rx_top is begin\n"
                              "  u : entity work.vsync port map (clk => clk, d => d, q => q);\n"
                              "end;\n"},
                  {"m.v", "module other(input a, output b); assign b = a; endmodule\n"}},
                 "MIPI_CSI2_RxTop");
    CHECK_FALSE(r.design);
    const auto it = std::ranges::find(r.diags, std::string("FE-SLANG-InvalidTopModule"), &Diagnostic::code);
    REQUIRE(it != r.diags.end());
    CHECK(it->message.find("neither a VHDL entity nor a Verilog module") != std::string::npos);
    REQUIRE_FALSE(it->notes.empty());
    CHECK(it->notes.front().find("'mipi_csi2_rx_top'") != std::string::npos);
    // negative: an unrelated name gets no suggestion
    auto far = run({{"vsync.vhd", kVhdlSync}, {"m.v", "module other(input a, output b); assign b = a; endmodule\n"}},
                   "completely_different_name");
    const auto f = std::ranges::find(far.diags, std::string("FE-SLANG-InvalidTopModule"), &Diagnostic::code);
    REQUIRE(f != far.diags.end());
    CHECK(f->notes.empty());
}

TEST_CASE("Mixed: vendor library clause after a comment that says 'library'", "[vhdl][mixed]") {
    // Digilent: "-- Uncomment the following library declaration if instantiating\n-- any Xilinx primitives ..."
    auto r = run({{"t.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                            "-- Uncomment the following library declaration if instantiating\n"
                            "-- any Xilinx primitives in this code.\n"
                            "library UNISIM;\nuse UNISIM.VComponents.all;\n"
                            "entity t is port (i : in std_logic; o : out std_logic); end;\n"
                            "architecture rtl of t is begin\n"
                            "  b : BUFG port map (I => i, O => o);\n"
                            "end;\n"}},
                 "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-UNDECLARED") == 0);
}

TEST_CASE("Mixed: outputs of a declared component without an entity are black-box outputs", "[vhdl][mixed]") {
    // Digilent Ram2Ddr: the MIG 'ddr' component drives ui_clk; rstn feeds its sys_rst
    auto r = run({{"t.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                            "entity t is port (clk, rst, d : in std_logic; q : out std_logic); end;\n"
                            "architecture rtl of t is\n"
                            "  component ddr port (sys_clk_i, sys_rst : in std_logic; ui_clk : out std_logic;\n"
                            "    app_rdy : out std_logic); end component;\n"
                            "  signal rstn, ui_clk, rdy, r1 : std_logic;\n"
                            "begin\n"
                            "  process (clk) begin if rising_edge(clk) then rstn <= not rst; end if; end process;\n"
                            "  u : ddr port map (sys_clk_i => clk, sys_rst => rstn, ui_clk => ui_clk, app_rdy => rdy);\n"
                            "  process (ui_clk) begin if rising_edge(ui_clk) then r1 <= rdy and d; end if; end process;\n"
                            "  q <= r1;\n"
                            "end;\n"}},
                 "t");
    REQUIRE(r.design);
    CHECK(r.count("IR-UNRESOLVED-INSTANCE") == 1);
    // rstn is read by the black box: kept; r1 is clocked by the black box's ui_clk
    CHECK(r.has("reg rstn"));
    CHECK(r.has("reg r1"));
    const auto& top = r.design->body(r.design->instance(r.design->tops.front()).body);
    const auto it = std::ranges::find(top.children, std::string("u"), &ir::ChildInstance::name);
    REQUIRE(it != top.children.end());
    CHECK(std::ranges::count_if(it->ports, [](const ir::PortBinding& p) { return p.dir == ir::PortDir::Out; }) == 2);
    CHECK(std::ranges::count_if(it->ports, [](const ir::PortBinding& p) { return p.dir == ir::PortDir::In; }) == 2);
}

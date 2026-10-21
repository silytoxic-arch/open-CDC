// VHDL frontend: elaboration and lowering to the Design IR (T8.2 / T8.3).
// Every construct has a positive case; errors and unsupported constructs
// have negative cases. The IR is checked through its deterministic dump.

#include "../TestUtil.h"
#include "cdc/CdcAnalyzer.h"
#include "core/Diagnostic.h"
#include "engine/AnalysisSession.h"
#include "frontend/Frontend.h"
#include "ir/Dump.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace opencdc;

namespace {

struct VhdlRun {
    std::shared_ptr<const ir::Design> design;
    std::vector<Diagnostic> diags;
    frontend::ElaborationSummary summary;
    std::string dump;

    [[nodiscard]] std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == code; }));
    }
    [[nodiscard]] bool has(std::string_view text) const { return dump.find(text) != std::string::npos; }
    [[nodiscard]] std::size_t errors() const {
        return static_cast<std::size_t>(std::ranges::count_if(diags, [](const Diagnostic& d) { return d.isError(); }));
    }
};

struct Src {
    std::string name;
    std::string text;
    std::string library;
};

VhdlRun elaborate(const std::vector<Src>& files, std::string top,
                  std::vector<std::pair<std::string, std::string>> generics = {}) {
    test::TempDir dir;
    frontend::FrontendInput in;
    for (const auto& f : files) {
        in.sourceFiles.push_back(dir.write(f.name, f.text));
        in.sourceLibraries.push_back(f.library);
    }
    in.top = std::move(top);
    in.topParameters = std::move(generics);
    CollectingDiagnosticSink sink;
    auto out = frontend::makeVhdlFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    VhdlRun r;
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

VhdlRun elaborate(std::string_view text, std::string top,
                  std::vector<std::pair<std::string, std::string>> generics = {}) {
    return elaborate({{"d.vhd", std::string(text), ""}}, std::move(top), std::move(generics));
}

constexpr const char* kLib = "library ieee;\nuse ieee.std_logic_1164.all;\nuse ieee.numeric_std.all;\n";

std::string lib(std::string_view body) {
    return std::string(kLib) + std::string(body);
}

} // namespace

// ===========================================================================
// Registers
// ===========================================================================

TEST_CASE("VHDL: clocked process with asynchronous active-low reset", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, rst_n : in std_logic; d : in std_logic_vector(3 downto 0);\n"
                           "  q : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk, rst_n) begin\n"
                           "    if rst_n = '0' then q <= (others => '0');\n"
                           "    elsif rising_edge(clk) then q <= d; end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("reg q <= d @(posedge clk) arst(!rst_n -> 4'b0000)"));
}

TEST_CASE("VHDL: clock edge forms ('event, falling edge, wait until)", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, a, b, c, rst : in std_logic; x, y, z : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin if clk'event and clk = '1' then x <= a; end if; end process;\n"
                           "  process (clk) begin if falling_edge(clk) then y <= b; end if; end process;\n"
                           "  process begin wait until rising_edge(clk);\n"
                           "    if rst = '1' then z <= '0'; else z <= c; end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.has("reg x <= a @(posedge clk)"));
    CHECK(r.has("reg y <= b @(negedge clk)"));
    CHECK(r.has("reg z <= c @(posedge clk) srst(rst -> 1'b0)"));
}

TEST_CASE("VHDL: clock enable and synchronous reset are extracted", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, rst, en : in std_logic; d : in unsigned(7 downto 0);\n"
                           "  q : out unsigned(7 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then\n"
                           "      if rst = '1' then q <= (others => '0'); elsif en = '1' then q <= d + 1; end if;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.has("srst(rst -> 8'b00000000)"));
    CHECK(r.has("en(en)"));
}

TEST_CASE("VHDL: 'rising_edge(clk) and en' makes an enable", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, en, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin if rising_edge(clk) and en = '1' then q <= d; end if; end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.has("reg q <= d @(posedge clk) en(en)"));
}

TEST_CASE("VHDL: process variables become registers only when read before written", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, d : in std_logic; q1, q2 : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  p : process (clk)\n"
                           "    variable held : std_logic := '0';\n"
                           "    variable tmp  : std_logic;\n"
                           "  begin\n"
                           "    if rising_edge(clk) then\n"
                           "      q1 <= held; held := d;\n"  // read first: storage
                           "      tmp := not d; q2 <= tmp;\n" // written first: wire
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.has("reg p.held <= d @(posedge clk)"));
    CHECK_FALSE(r.has("reg p.tmp"));
    CHECK(r.has("reg q1 <= p.held"));
}

// ===========================================================================
// Combinational logic
// ===========================================================================

TEST_CASE("VHDL: concurrent conditional and selected assignments", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (s : in std_logic_vector(1 downto 0); a, b, c : in std_logic;\n"
                           "  x, y : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  x <= a when s = \"00\" else b when s = \"01\" else c;\n"
                           "  with s select y <= a when \"00\", b when \"01\" | \"10\", c when others;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("assign x = "));
    CHECK(r.has("assign y = "));
    CHECK(r.design->body(r.design->instance(r.design->tops.front()).body).latches.empty());
}

TEST_CASE("VHDL: combinational process with case is latch free", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (s : in std_logic_vector(1 downto 0); y : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (all) begin\n"
                           "    case s is\n"
                           "      when \"00\" => y <= \"0001\";\n"
                           "      when \"01\" => y <= \"0010\";\n"
                           "      when \"10\" => y <= \"0100\";\n"
                           "      when others => y <= \"1000\";\n"
                           "    end case;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("comb  y = "));
}

TEST_CASE("VHDL negative: incomplete combinational assignment infers a latch", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (en, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (en, d) begin if en = '1' then q <= d; end if; end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("IR-LATCH-INFERRED") == 1);
    CHECK(r.design->body(r.design->instance(r.design->tops.front()).body).latches.size() == 1);
}

TEST_CASE("VHDL: two-process style with a record variable has no latches", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, rst, inc : in std_logic; cnt : out unsigned(3 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type two_process_r is record\n"
                           "    Cnt : unsigned(3 downto 0);\n"
                           "    Vld : std_logic;\n"
                           "  end record;\n"
                           "  signal r, r_next : two_process_r;\n"
                           "begin\n"
                           "  p_comb : process (all)\n"
                           "    variable v : two_process_r;\n"
                           "  begin\n"
                           "    v := r;\n"
                           "    if inc = '1' then v.Cnt := r.Cnt + 1; v.Vld := '1'; end if;\n"
                           "    r_next <= v;\n"
                           "  end process;\n"
                           "  cnt <= r.Cnt;\n"
                           "  p_seq : process (clk) begin\n"
                           "    if rising_edge(clk) then\n"
                           "      r <= r_next;\n"
                           "      if rst = '1' then r.Cnt <= (others => '0'); r.Vld <= '0'; end if;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    const auto& body = r.design->body(r.design->instance(r.design->tops.front()).body);
    CHECK(body.latches.empty());
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("wire [3:0] r.Cnt"));
    CHECK(r.has("wire r.Vld"));
    CHECK(r.has("reg r.Cnt <= r_next.Cnt @(posedge clk) srst(rst -> 4'b0000)"));
}

// ===========================================================================
// Hierarchy
// ===========================================================================

TEST_CASE("VHDL: generics, for-generate and component instantiation", "[vhdl][elab]") {
    auto r = elaborate(lib("entity sync is generic (STAGES : positive := 2);\n"
                           "  port (clk, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of sync is\n"
                           "  signal s : std_logic_vector(STAGES-1 downto 0);\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then s <= s(STAGES-2 downto 0) & d; end if; end process;\n"
                           "  q <= s(STAGES-1);\n"
                           "end;\n"
                           "library ieee; use ieee.std_logic_1164.all;\n"
                           "entity top is generic (N : natural := 3);\n"
                           "  port (clk : in std_logic; d : in std_logic_vector(N-1 downto 0); q : out std_logic_vector(N-1 downto 0)); end;\n"
                           "architecture rtl of top is\n"
                           "  component sync is generic (STAGES : positive := 2);\n"
                           "    port (clk, d : in std_logic; q : out std_logic); end component;\n"
                           "begin\n"
                           "  g : for i in 0 to N-1 generate\n"
                           "    u : sync generic map (STAGES => 3) port map (clk, d(i), q(i));\n"
                           "  end generate;\n"
                           "  g_if : if N > 2 generate\n"
                           "    u_extra : entity work.sync port map (clk => clk, d => d(0), q => open);\n"
                           "  end generate;\n"
                           "end;\n"),
                       "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.design->instances.size() == 5);
    CHECK(r.has("inst g[0].u : sync"));
    CHECK(r.has("inst g[2].u : sync"));
    CHECK(r.has("inst g_if.u_extra : sync"));
    CHECK(r.has("module sync#(STAGES=3)"));
    CHECK(r.has("module sync#(STAGES=2)"));
    REQUIRE(r.summary.topInstances.size() == 1);
    CHECK(r.summary.topInstances.front().children.size() == 4);
}

TEST_CASE("VHDL: top-level generic override", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is generic (W : positive);\n"
                           "  port (clk : in std_logic; d : in std_logic_vector(W-1 downto 0);\n"
                           "  q : out std_logic_vector(W-1 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin if rising_edge(clk) then q <= d; end if; end process;\n"
                           "end;\n"),
                       "t", {{"W", "12"}});
    REQUIRE(r.design);
    CHECK(r.has("module t#(W=12)"));
    CHECK(r.has("input wire [11:0] d"));
}

TEST_CASE("VHDL negative: a generic without value is an error", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is generic (W : positive);\n"
                           "  port (d : in std_logic_vector(W-1 downto 0); q : out std_logic_vector(W-1 downto 0)); end;\n"
                           "architecture rtl of t is begin q <= d; end;\n"),
                       "t");
    CHECK_FALSE(r.design);
    CHECK(r.count("VHDL-GENERIC") == 1);
    CHECK(r.errors() == 1); // no follow-up errors
}

TEST_CASE("VHDL: unconstrained ports take the constraint of the actual", "[vhdl][elab]") {
    auto r = elaborate(lib("entity inv is port (a : in std_logic_vector; y : out std_logic_vector); end;\n"
                           "architecture rtl of inv is begin y <= not a; end;\n"
                           "library ieee; use ieee.std_logic_1164.all;\n"
                           "entity top is port (a : in std_logic_vector(5 downto 0); y : out std_logic_vector(5 downto 0)); end;\n"
                           "architecture rtl of top is begin\n"
                           "  u : entity work.inv port map (a => a, y => y);\n"
                           "end;\n"),
                       "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("input wire [5:0] a"));
}

TEST_CASE("VHDL: partial port association and expressions on inputs", "[vhdl][elab]") {
    auto r = elaborate(lib("entity c is port (a : in std_logic_vector(3 downto 0); y : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of c is begin y <= a; end;\n"
                           "library ieee; use ieee.std_logic_1164.all;\n"
                           "entity top is port (p, q : in std_logic_vector(1 downto 0); y : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of top is begin\n"
                           "  u : entity work.c port map (a(3 downto 2) => p, a(1 downto 0) => q and \"11\", y => y);\n"
                           "end;\n"),
                       "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("inst u : c (.a({p, "));
}

TEST_CASE("VHDL negative: instance of an unknown entity is a black box", "[vhdl][elab]") {
    auto r = elaborate(lib("entity top is port (a : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of top is\n"
                           "  component FDRE port (C, D : in std_logic; Q : out std_logic); end component;\n"
                           "begin\n"
                           "  u : FDRE port map (C => a, D => a, Q => y);\n"
                           "end;\n"),
                       "top");
    REQUIRE(r.design);
    CHECK(r.count("IR-UNRESOLVED-INSTANCE") == 1);
    REQUIRE(r.design->unsupported.size() == 1);
    CHECK(r.design->unsupported.front().code == "IR-UNRESOLVED-INSTANCE");
}

TEST_CASE("VHDL: packages in another library, functions and constants", "[vhdl][elab]") {
    const std::string pkg = lib("package util is\n"
                                "  function log2ceil(n : positive) return natural;\n"
                                "  function bin2gray(b : unsigned) return unsigned;\n"
                                "  constant DEPTH : positive := 16;\n"
                                "end package;\n"
                                "package body util is\n"
                                "  function log2ceil(n : positive) return natural is\n"
                                "    variable r : natural := 0;\n"
                                "  begin\n"
                                "    while 2**r < n loop r := r + 1; end loop;\n"
                                "    return r;\n"
                                "  end function;\n"
                                "  function bin2gray(b : unsigned) return unsigned is\n"
                                "  begin return b xor shift_right(b, 1); end function;\n"
                                "end package body;\n");
    const std::string top = lib("library mylib; use mylib.util.all;\n"
                                "entity t is port (clk : in std_logic; b : in unsigned(log2ceil(DEPTH) downto 0);\n"
                                "  g : out unsigned(log2ceil(DEPTH) downto 0)); end;\n"
                                "architecture rtl of t is begin\n"
                                "  process (clk) begin if rising_edge(clk) then g <= bin2gray(b); end if; end process;\n"
                                "end;\n");
    auto r = elaborate({{"util.vhd", pkg, "mylib"}, {"t.vhd", top, ""}}, "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("input wire [4:0] b"));
    CHECK(r.has("xor("));
}

TEST_CASE("VHDL negative: unknown package", "[vhdl][elab]") {
    auto r = elaborate(lib("use work.nothing.all;\n"
                           "entity t is port (a : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of t is begin y <= a; end;\n"),
                       "t");
    CHECK_FALSE(r.design);
    CHECK(r.count("VHDL-UNDECLARED") + r.count("VHDL-UNKNOWN-PACKAGE") >= 1);
}

// ===========================================================================
// Types and storage
// ===========================================================================

TEST_CASE("VHDL: integer ranges, signed types and ASYNC_REG attributes", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk : in std_logic; d : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  signal cnt : integer range 0 to 9;\n"
                           "  signal s : signed(7 downto 0);\n"
                           "  signal meta, sync : std_logic;\n"
                           "  attribute ASYNC_REG : string;\n"
                           "  attribute ASYNC_REG of meta, sync : signal is \"TRUE\";\n"
                           "begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then\n"
                           "      if cnt = 9 then cnt <= 0; else cnt <= cnt + 1; end if;\n"
                           "      s <= s - 1; meta <= d; sync <= meta;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "  y <= sync xor s(7) xor '1' when cnt = 3 else '0';\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("wire [3:0] cnt"));
    CHECK(r.has("wire signed [7:0] s"));
    CHECK(r.has("wire meta (* ASYNC_REG = \"TRUE\" *)"));
}

TEST_CASE("VHDL: array of vectors is a memory; dynamic write and read", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, we : in std_logic; wa, ra : in unsigned(2 downto 0);\n"
                           "  wd : in std_logic_vector(7 downto 0); rd : out std_logic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type mem_t is array (0 to 7) of std_logic_vector(7 downto 0);\n"
                           "  signal mem : mem_t;\n"
                           "begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then if we = '1' then mem(to_integer(wa)) <= wd; end if; end if;\n"
                           "  end process;\n"
                           "  rd <= mem(to_integer(ra));\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("wire [8 x 8] mem"));
    CHECK(r.has("dyn_insert"));
    CHECK(r.has("dyn_select"));
}

TEST_CASE("VHDL: loops with exit and procedures with out parameters", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (v : in std_logic_vector(7 downto 0); idx : out unsigned(2 downto 0);\n"
                           "  par : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  procedure parity(signal x : in std_logic_vector; variable p : out std_logic) is\n"
                           "    variable acc : std_logic := '0';\n"
                           "  begin\n"
                           "    for i in x'range loop acc := acc xor x(i); end loop;\n"
                           "    p := acc;\n"
                           "  end procedure;\n"
                           "begin\n"
                           "  process (all)\n"
                           "    variable p : std_logic;\n"
                           "  begin\n"
                           "    idx <= (others => '0');\n"
                           "    for i in 0 to 7 loop\n"
                           "      if v(i) = '1' then idx <= to_unsigned(i, 3); exit; end if;\n"
                           "    end loop;\n"
                           "    parity(v, p);\n"
                           "    par <= p;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("comb  idx = "));
    CHECK(r.has("comb  par = "));
}

// ===========================================================================
// Errors and unsupported constructs
// ===========================================================================

TEST_CASE("VHDL negative: width mismatch is an error and yields no design", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (a : in std_logic_vector(3 downto 0); y : out std_logic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is begin y <= a; end;\n"),
                       "t");
    CHECK_FALSE(r.design);
    CHECK(r.errors() >= 1);
}

TEST_CASE("VHDL negative: assignment to an input port", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (a : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of t is begin a <= '0'; y <= a; end;\n"),
                       "t");
    CHECK_FALSE(r.design);
    CHECK(r.count("VHDL-ASSIGN") == 1);
}

TEST_CASE("VHDL negative: syntax errors are reported with their location", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (a : in std_logic y : out std_logic); end;\n"), "t");
    CHECK_FALSE(r.design);
    REQUIRE(r.count("VHDL-SYNTAX") >= 1);
    CHECK(r.diags.front().location.line == 4);
}

TEST_CASE("VHDL negative: testbench processes with waits are not analyzed", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk : out std_logic); end;\n"
                           "architecture sim of t is begin\n"
                           "  process begin clk <= '0'; wait for 5 ns; clk <= '1'; wait for 5 ns; end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("VHDL-TESTBENCH") == 1);
    CHECK(r.design->body(r.design->instance(r.design->tops.front()).body).registers.empty());
}

TEST_CASE("VHDL negative: missing top entity", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (a : in std_logic); end;\narchitecture rtl of t is begin end;\n"), "nope");
    CHECK_FALSE(r.design);
    CHECK(r.count("VHDL-TOP") == 1);
}

TEST_CASE("VHDL: top inferred when not given", "[vhdl][elab]") {
    auto r = elaborate(lib("entity leaf is port (a : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of leaf is begin y <= a; end;\n"
                           "library ieee; use ieee.std_logic_1164.all;\n"
                           "entity top is port (a : in std_logic; y : out std_logic); end;\n"
                           "architecture rtl of top is begin u : entity work.leaf port map (a, y); end;\n"),
                       "");
    REQUIRE(r.design);
    REQUIRE(r.design->tops.size() == 1);
    CHECK(r.design->instance(r.design->tops.front()).hierPath == "top");
}

// ===========================================================================
// Language selection and equivalence with the Verilog examples
// ===========================================================================

TEST_CASE("Default frontend selects the language by file extension", "[vhdl][frontend]") {
    CHECK(frontend::isVhdlFile("a/b.vhd"));
    CHECK(frontend::isVhdlFile("B.VHDL"));
    CHECK_FALSE(frontend::isVhdlFile("x.v"));
    CHECK_FALSE(frontend::isVhdlFile("x.sv"));
}

namespace {

using PathKey = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string>;

std::multiset<PathKey> cdcSummary(const std::filesystem::path& project) {
    auto cfg = engine::loadProjectFile(project);
    REQUIRE(cfg.ok());
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg.value(), progress, cancel);
    REQUIRE(r);
    REQUIRE(r->cdc);
    std::multiset<PathKey> out;
    for (const auto& p : r->cdc->paths) {
        std::string rules;
        for (const auto& h : p.hits)
            rules += h.ruleId + ";";
        out.emplace(p.srcName, p.dstName, std::string(cdc::toString(p.status)), std::string(cdc::toString(p.syncType)),
                    std::to_string(p.syncStages), rules);
    }
    return out;
}

} // namespace

TEST_CASE("VHDL examples give the same CDC results as the Verilog examples", "[vhdl][example]") {
    const auto ex = test::examplesDir();
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"smoke/smoke.ocdc.json", "vhdl/smoke/smoke_vhdl.ocdc.json"},
        {"cdc_rules/cdc_rules.ocdc.json", "vhdl/cdc_rules/cdc_rules_vhdl.ocdc.json"},
        {"cdc_advanced/cdc_advanced.ocdc.json", "vhdl/cdc_advanced/cdc_advanced_vhdl.ocdc.json"},
        {"async_fifo/afifo.ocdc.json", "vhdl/async_fifo/afifo_vhdl.ocdc.json"},
        // mixed: Verilog top -> VHDL -> Verilog, and VHDL top -> Verilog -> VHDL
        {"smoke/smoke.ocdc.json", "mixed/smoke/smoke_mixed.ocdc.json"},
        {"cdc_advanced/cdc_advanced.ocdc.json", "mixed/cdc_advanced/cdc_advanced_mixed.ocdc.json"},
    };
    for (const auto& [v, h] : pairs) {
        INFO(v << " vs " << h);
        const auto a = cdcSummary(ex / v);
        const auto b = cdcSummary(ex / h);
        CHECK(!a.empty());
        CHECK(a == b);
    }
}

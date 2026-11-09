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

TEST_CASE("VHDL: asynchronous reset disabled by a generic", "[vhdl][elab]") {
    const std::string src = lib("entity t is generic (RST_ASYNC_G : boolean := false);\n"
                                "  port (clk, rst, d : in std_logic; q : out std_logic); end;\n"
                                "architecture rtl of t is begin\n"
                                "  process (clk, rst) begin\n"
                                "    if RST_ASYNC_G and rst = '1' then q <= '0';\n"
                                "    elsif rising_edge(clk) then q <= d; end if;\n"
                                "  end process;\n"
                                "end;\n");
    auto off = elaborate(src, "t");
    REQUIRE(off.design);
    CHECK(off.has("reg q <= d @(posedge clk)\n"));
    auto on = elaborate(src, "t", {{"RST_ASYNC_G", "true"}});
    REQUIRE(on.design);
    CHECK(on.has("reg q <= d @(posedge clk) arst(rst -> 1'b0)"));
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

TEST_CASE("VHDL: byte-enable write into a part of a dynamically indexed element", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk : in std_logic; we : in std_logic_vector(1 downto 0);\n"
                           "  a : in unsigned(1 downto 0); d : in std_logic_vector(15 downto 0);\n"
                           "  q : out std_logic_vector(15 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type mem_t is array (0 to 3) of std_logic_vector(15 downto 0);\n"
                           "  type rec_t is record lo, hi : std_logic_vector(7 downto 0); end record;\n"
                           "  type rmem_t is array (0 to 3) of rec_t;\n"
                           "begin\n"
                           "  process (clk)\n"
                           "    variable mem : mem_t;\n"
                           "    variable rmem : rmem_t;\n"
                           "  begin\n"
                           "    if rising_edge(clk) then\n"
                           "      for i in 0 to 1 loop\n"
                           "        if we(i) = '1' then mem(to_integer(a))(8*i+7 downto 8*i) := d(8*i+7 downto 8*i); end if;\n"
                           "      end loop;\n"
                           "      rmem(to_integer(a)).hi := d(15 downto 8);\n"
                           "      q <= mem(to_integer(a)) xor (rmem(to_integer(a)).hi & rmem(to_integer(a)).lo);\n"
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.design->unsupported.empty());
    CHECK(r.has("dyn_insert"));
    CHECK(r.has("reg p"));
}

TEST_CASE("VHDL negative: a variable index into a field of a variably indexed element", "[vhdl][elab]") {
    // m(i)(j) is supported (stage 14); m(i).f(j) is not
    auto r = elaborate(lib("entity t is port (clk : in std_logic; i, j : in natural range 0 to 3; d : in std_logic;\n"
                           "  q : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type e_t is record f : std_logic_vector(3 downto 0); g : std_logic; end record;\n"
                           "  type m_t is array (0 to 3) of e_t;\n"
                           "  signal m : m_t;\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then m(i).f(j) <= d; end if; end process;\n"
                           "  q <= m(0).f;\n"
                           "end;\n"),
                       "t");
    CHECK(r.count("IR-UNSUPPORTED-TARGET") == 1);
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

// ===========================================================================
// Stage 9 fixes found on real code (surf)
// ===========================================================================

TEST_CASE("VHDL: reset 'if' after the clocked 'if' is an asynchronous reset", "[vhdl][elab]") {
    // surf Synchronizer: the later statement wins, so rst overrides the clock
    auto r = elaborate(lib("entity t is port (clk, rst, d : in std_logic; q : out std_logic_vector(1 downto 0)); end;\n"
                           "architecture rtl of t is signal s : std_logic_vector(1 downto 0) := \"11\"; begin\n"
                           "  process (clk, rst) begin\n"
                           "    if rising_edge(clk) then s <= s(0) & d; end if;\n"
                           "    if rst = '0' then s <= \"11\"; end if;\n"
                           "  end process;\n"
                           "  q <= s;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-CLOCKED-TAIL") == 0);
    CHECK(r.has("arst(!rst -> 2'b11)"));
}

TEST_CASE("VHDL negative: other statements after the clocked 'if' are reported", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then q <= d; end if;\n"
                           "    q <= '0';\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("VHDL-CLOCKED-TAIL") == 1);
    CHECK(r.has("reg q <= d @(posedge clk)\n"));
}

TEST_CASE("VHDL: package constant calls a function whose body is in the package body", "[vhdl][elab]") {
    // surf AxiLitePkg: accepted by synthesis tools; only that body is bound early
    auto r = elaborate(lib("package p is\n"
                           "  type r_t is record a : std_logic; b : std_logic_vector(1 downto 0); end record;\n"
                           "  function mk (b : std_logic_vector(1 downto 0) := \"00\") return r_t;\n"
                           "  constant C1 : r_t := mk(b => \"01\");\n"
                           "  function mk2 return r_t;\n"
                           "  constant C2 : r_t := mk2;\n"
                           "end package;\n"
                           "package body p is\n"
                           "  function mk (b : std_logic_vector(1 downto 0) := \"00\") return r_t is\n"
                           "    variable r : r_t; begin r.a := '1'; r.b := b; return r; end function;\n"
                           "  function mk2 return r_t is begin return mk(\"10\"); end function;\n"
                           "end package body;\n"
                           "library ieee; use ieee.std_logic_1164.all; use work.p.all;\n"
                           "entity t is port (q1, q2 : out std_logic_vector(1 downto 0)); end;\n"
                           "architecture a of t is begin q1 <= C1.b; q2 <= C2.b; end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("q1 = 2'b01"));
    CHECK(r.has("q2 = 2'b10"));
}

TEST_CASE("VHDL: record fields untouched by the branches bypass the multiplexers", "[vhdl][elab]") {
    // two-process style: 'rdy' is set to '1' before the if chain; it must stay
    // a constant-data flop (reset bridge), not logic selected by 'en'
    auto r = elaborate(lib("entity t is port (clk, rst, en : in std_logic; rdy : out std_logic;\n"
                           "  c : out unsigned(3 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type reg_t is record rdy : std_logic; cnt : unsigned(3 downto 0); end record;\n"
                           "  signal r, rin : reg_t := ('0', (others => '0'));\n"
                           "begin\n"
                           "  comb : process (r, en) variable v : reg_t; begin\n"
                           "    v := r; v.rdy := '1';\n"
                           "    if en = '1' then v.cnt := r.cnt + 1; end if;\n"
                           "    if r.cnt = 15 then v.cnt := (others => '0'); end if;\n"
                           "    rin <= v;\n"
                           "  end process;\n"
                           "  seq : process (clk) begin if rising_edge(clk) then\n"
                           "    if rst = '1' then r <= ('0', (others => '0')); else r <= rin; end if;\n"
                           "  end if; end process;\n"
                           "  rdy <= r.rdy; c <= r.cnt;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("rin.rdy = 1'b1"));
}

// ===========================================================================
// Stage 14 fixes found on real code (microwatt)
// ===========================================================================

TEST_CASE("VHDL: slices with variable bounds of static length", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (d : in std_ulogic_vector(255 downto 0); a : in std_ulogic_vector(1 downto 0);\n"
                           "  b : in std_ulogic_vector(2 downto 0); q : out std_ulogic_vector(63 downto 0);\n"
                           "  q2 : out std_ulogic_vector(3 downto 0); q3 : out std_ulogic_vector(0 to 1)); end;\n"
                           "architecture rtl of t is\n"
                           "  function select_dword(data : std_ulogic_vector(255 downto 0); addr : std_ulogic_vector(1 downto 0))\n"
                           "    return std_ulogic_vector is\n"
                           "    variable firstbit : integer;\n"
                           "  begin\n"
                           "    firstbit := to_integer(unsigned(addr)) * 64;\n"
                           "    return data(firstbit + 63 downto firstbit);\n"
                           "  end;\n"
                           "  signal asc : std_ulogic_vector(0 to 7);\n"
                           "begin\n"
                           "  asc <= d(7 downto 0);\n"
                           "  q <= select_dword(d, a);\n"
                           "  process (all) variable j : integer; begin\n"
                           "    j := (7 - to_integer(unsigned(b))) * 4;\n"
                           "    q2 <= d(j + 3 downto j);\n"
                           "    q3 <= asc(to_integer(unsigned(b)) to to_integer(unsigned(b)) + 1);\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-NOT-STATIC") == 0);
    CHECK(r.has("[64] = dyn_select<1@255:0>(A=d"));
    CHECK(r.has("[4] = dyn_select<1@255:0>(A=d"));
    CHECK(r.has("[2] = dyn_select<1@0:7>(A=asc, S=$add")); // right bound: lowest bit offset
}

TEST_CASE("VHDL negative: slice bounds of variable length are still rejected", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (d : in std_ulogic_vector(15 downto 0); a, b : in natural range 0 to 15;\n"
                           "  q : out std_ulogic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (all) begin q <= (others => '0'); q(0) <= d(a downto b)(b); end process;\n"
                           "end;\n"),
                       "t");
    CHECK(r.count("VHDL-NOT-STATIC") >= 1);
}

TEST_CASE("VHDL: numeric_std std_match and is_x overloads, slices in range choices", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (v : in std_ulogic_vector(3 downto 0); u : in unsigned(3 downto 0);\n"
                           "  s : in std_ulogic; m1, m2, m3 : out boolean; info : in std_ulogic_vector(3 downto 0);\n"
                           "  w : out std_ulogic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  m1 <= std_match(v, \"01--\");\n"
                           "  m2 <= std_match(s, '1') and not is_x(u);\n"
                           "  m3 <= not is_X(signed(v));\n"
                           "  w <= (3 downto 0 => info, others => '0');\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("w = {4'b0000, info}"));
}

TEST_CASE("VHDL negative: a slice in a range choice must have the choice's length", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (info : in std_ulogic_vector(3 downto 0); w : out std_ulogic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is begin w <= (4 downto 0 => info, others => '0'); end;\n"),
                       "t");
    CHECK(r.count("VHDL-LENGTH") == 1);
}

TEST_CASE("VHDL: two variable indexes in one target", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, d : in std_ulogic; i : in natural range 0 to 3; j : in natural range 0 to 1;\n"
                           "  q : out std_ulogic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type row_t is array (0 to 1) of std_ulogic;\n"
                           "  type tab_t is array (0 to 3) of row_t;\n"
                           "  signal tab : tab_t := (others => (others => '0'));\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then tab(i)(j) <= d; end if; end process;\n"
                           "  g : for k in 0 to 3 generate q(2 * k + 1) <= tab(k)(0); q(2 * k) <= tab(k)(1); end generate;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-TARGET") == 0);
    CHECK(r.has("dyn_insert"));
}

TEST_CASE("VHDL: a case over every literal of an enumeration infers no latch", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (a, b : in std_ulogic; sel : in std_ulogic_vector(1 downto 0); q : out std_ulogic); end;\n"
                           "architecture rtl of t is\n"
                           "  type kind_t is (K_A, K_B, K_C);\n"
                           "begin\n"
                           "  process (all) variable k : kind_t; begin\n"
                           "    k := K_A;\n"
                           "    if sel = \"01\" then k := K_B; elsif sel = \"10\" then k := K_C; end if;\n"
                           "    case k is\n"
                           "      when K_A => q <= a;\n"
                           "      when K_B => q <= b;\n"
                           "      when K_C => q <= a and b;\n"
                           "    end case;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
}

TEST_CASE("VHDL: textio routines with arguments are accepted in initial value functions", "[vhdl][elab]") {
    auto r = elaborate(lib("use std.textio.all;\n"
                           "entity t is generic (F : string := \"init.hex\"); port (clk : in std_ulogic; a : in natural range 0 to 3;\n"
                           "  q : out std_ulogic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type ram_t is array (0 to 3) of std_ulogic_vector(7 downto 0);\n"
                           "  impure function init_ram(name : string) return ram_t is\n"
                           "    file f : text open read_mode is name; variable l : line; variable w : std_ulogic_vector(7 downto 0);\n"
                           "    variable r : ram_t := (others => (others => '0'));\n"
                           "  begin\n"
                           "    for i in 0 to 3 loop exit when endfile(f); readline(f, l); hread(l, w); r(i) := w; end loop;\n"
                           "    return r;\n"
                           "  end function;\n"
                           "  signal mem : ram_t := init_ram(F);\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then q <= mem(a); end if; end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-NO-OVERLOAD") == 0);
}

// ===========================================================================
// Stage 15 fixes found on real code (microwatt Arty, LiteX)
// ===========================================================================

TEST_CASE("VHDL: a function that reads a file has an unknown result", "[vhdl][elab]") {
    // litedram-initmem: the loop bound exceeds the RAM, endfile() ends it in simulation
    auto r = elaborate(lib("use std.textio.all;\n"
                           "entity t is port (clk : in std_ulogic; a : in natural range 0 to 3;\n"
                           "  q : out std_ulogic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type ram_t is array (0 to 3) of std_ulogic_vector(7 downto 0);\n"
                           "  procedure load(ram : inout ram_t; name : string) is\n"
                           "    file f : text open read_mode is name; variable l : line; variable w : std_ulogic_vector(7 downto 0);\n"
                           "  begin\n"
                           "    for i in 0 to 99 loop exit when endfile(f); readline(f, l); hread(l, w); ram(i) := w; end loop;\n"
                           "  end procedure;\n"
                           "  impure function init_ram(name : string) return ram_t is\n"
                           "    variable r : ram_t := (others => (others => '0'));\n"
                           "  begin load(r, name); return r; end function;\n"
                           "  signal mem : ram_t := init_ram(\"x.hex\");\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then q <= mem(a); end if; end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-INDEX") == 0);
    CHECK(r.count("IR-UNSUPPORTED-INIT") == 1);
}

TEST_CASE("VHDL: null slice targets and slices with multiplied bounds", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is generic (N : natural := 8); port (d : in std_ulogic_vector(127 downto 0);\n"
                           "  s : in std_ulogic; w : in natural range 0 to 1; q : out std_ulogic_vector(7 downto 0);\n"
                           "  r : out std_ulogic_vector(63 downto 0); a, b : in std_ulogic; k : out std_ulogic_vector(1 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  q(q'left downto N) <= (others => '0');\n" // null range
                           "  q(N - 1 downto 0) <= d(7 downto 0);\n"
                           "  r <= d((w + 1) * 64 - 1 downto w * 64);\n"
                           "  with a & b select k <= \"01\" when \"10\", \"10\" when \"01\", \"00\" when others;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-AMBIGUOUS") == 0);
    CHECK(r.has("[64] = dyn_select<1@127:0>(A=d"));
}

TEST_CASE("VHDL: slice target with a variable offset and a fixed width", "[vhdl][elab]") {
    // Digilent MIPI CSI-2 lane merger: rbTdataInt((i+cnt+1)*8-1 downto (i+cnt)*8) <= ...
    auto r = elaborate(lib("entity t is port (clk : in std_logic; cnt : in natural range 0 to 2;\n"
                           "  d : in std_logic_vector(15 downto 0); q : out std_logic_vector(31 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  signal acc : std_logic_vector(31 downto 0);\n"
                           "begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then\n"
                           "      for i in 0 to 1 loop\n"
                           "        acc((i+cnt+1)*8-1 downto (i+cnt)*8) <= d((i+1)*8-1 downto i*8);\n"
                           "      end loop;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "  q <= acc;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-NOT-STATIC") == 0);
    CHECK(r.has("dyn_insert"));
    // negative: bounds with different variable parts have no fixed width
    auto bad = elaborate(lib("entity t is port (clk : in std_logic; a, b : in natural range 0 to 2;\n"
                             "  d : in std_logic_vector(7 downto 0); q : out std_logic_vector(31 downto 0)); end;\n"
                             "architecture rtl of t is begin\n"
                             "  process (clk) begin\n"
                             "    if rising_edge(clk) then q(a*8+7 downto b*8) <= d; end if;\n"
                             "  end process;\n"
                             "end;\n"),
                         "t");
    CHECK(bad.count("VHDL-NOT-STATIC") == 1);
}

TEST_CASE("VHDL: slice target with a variable offset and a width of one element", "[vhdl][elab]") {
    // SURF AxiStreamResize with one-byte slave lanes:
    // v.obMaster.tKeep((SLV_BYTES_C*idx)+(SLV_BYTES_C-1) downto (SLV_BYTES_C*idx)) := ibM.tKeep(SLV_BYTES_C-1 downto 0)
    auto one = [](const std::string& value) {
        return elaborate(lib("entity t is port (clk : in std_logic; n : in natural range 0 to 7;\n"
                             "  d : in std_logic_vector(1 downto 0); q : out std_logic_vector(7 downto 0)); end;\n"
                             "architecture rtl of t is\n"
                             "  constant W : natural := 1;\n"
                             "  signal r : std_logic_vector(7 downto 0) := (others => '0');\n"
                             "begin\n"
                             "  process (clk)\n"
                             "    variable v : std_logic_vector(7 downto 0);\n"
                             "  begin\n"
                             "    if rising_edge(clk) then\n"
                             "      v := r;\n"
                             "      v((W*n)+(W-1) downto (W*n)) := " + value + ";\n"
                             "      r <= v;\n"
                             "    end if;\n"
                             "  end process;\n"
                             "  q <= r;\n"
                             "end;\n"),
                         "t");
    };
    auto r = one("d(W-1 downto 0)");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-TYPE") == 0);
    CHECK(r.has("dyn_insert"));
    // negative: a value of two elements does not fit the one-element slice
    auto bad = one("d");
    CHECK(bad.errors() == 1);
    CHECK(bad.count("VHDL-LENGTH") == 1);
}

TEST_CASE("VHDL: a case choice of another length than the selector never matches", "[vhdl][elab]") {
    // Digilent usb2device Control_Registers: 6-bit choices among 7-bit ones (loc_addr is 7 bits)
    auto run = [](const std::string& choice) {
        return elaborate(lib("entity t is port (clk : in std_logic; a : in std_logic_vector(6 downto 0);\n"
                             "  d : in std_logic; q1, q2 : out std_logic); end;\n"
                             "architecture rtl of t is begin\n"
                             "  process (clk) begin\n"
                             "    if rising_edge(clk) then\n"
                             "      case a is\n"
                             "        when b\"0000001\" => q1 <= d;\n"
                             "        when b\"" + choice + "\" => q2 <= d;\n"
                             "        when others => null;\n"
                             "      end case;\n"
                             "    end if;\n"
                             "  end process;\n"
                             "end;\n"),
                         "t");
    };
    auto shortChoice = run("011100");
    REQUIRE(shortChoice.design);
    CHECK(shortChoice.errors() == 0);
    CHECK(shortChoice.count("VHDL-LENGTH") == 1);
    // q2 is never written: only q1 is a register
    CHECK(shortChoice.design->stats().registers == 1);
    // negative: a choice of the selector's length matches, no warning
    auto same = run("0011100");
    REQUIRE(same.design);
    CHECK(same.count("VHDL-LENGTH") == 0);
    CHECK(same.design->stats().registers == 2);
}

TEST_CASE("VHDL: 'use lib.pkg;' makes the package name visible for expanded names", "[vhdl][elab]") {
    const std::string pkg = lib("package dbg is\n"
                                "  type dbg_t is record a : std_logic; b : std_logic_vector(1 downto 0); end record;\n"
                                "end package;\n");
    const std::string body = "library work;\nuse work.dbg;\n" +
                             lib("entity t is port (x : in std_logic; q : out dbg.dbg_t); end;\n"
                                 "architecture rtl of t is begin\n"
                                 "  q.a <= x; q.b <= x & x;\n"
                                 "end;\n");
    auto r = elaborate({{"dbg.vhd", pkg, ""}, {"t.vhd", body, ""}}, "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    // negative: without the use clause the package name is not visible
    std::string noUse = body;
    noUse.replace(noUse.find("use work.dbg;\n"), 14, "");
    auto bad = elaborate({{"dbg.vhd", pkg, ""}, {"t.vhd", noUse, ""}}, "t");
    CHECK(bad.count("VHDL-UNDECLARED") >= 1);
}

TEST_CASE("VHDL: 'x <= v when rising_edge(clk);' is a register", "[vhdl][elab]") {
    // Digilent Ps2Interface: ps2_clk_s <= ps2_clk_clean when rising_edge(clk);
    auto r = elaborate(lib("entity t is port (clk, en, d : in std_logic; q, q2, l : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  signal s, s2 : std_logic;\n"
                           "begin\n"
                           "  s <= d when rising_edge(clk);\n"
                           "  s2 <= d when rising_edge(clk) and en = '1' else unaffected;\n"
                           "  q <= s; q2 <= s2;\n"
                           "  l <= d when en = '1';\n" // negative: no edge, a latch as before
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-EDGE") == 0);
    CHECK(r.has("reg s <= d @(posedge clk)"));
    CHECK(r.has("reg s2 <= d @(posedge clk) en("));
    CHECK(r.count("IR-LATCH-INFERRED") == 1); // only 'l'
}

TEST_CASE("VHDL: 'x <= r when rst else v when rising_edge(clk);' is a register with asynchronous reset",
          "[vhdl][elab]") {
    // MiSTer ascal.vhd: i_reset_na <= '0' WHEN reset_na = '0' ELSE '1' WHEN rising_edge(i_clk);
    auto r = elaborate(lib("entity t is port (clk, rst_n, set, d : in std_logic; q, q2, c : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  signal s, s2 : std_logic;\n"
                           "begin\n"
                           "  s <= '0' when rst_n = '0' else '1' when rising_edge(clk);\n"
                           "  s2 <= '0' when rst_n = '0' else '1' when set = '1' else d when rising_edge(clk)\n"
                           "        else unaffected;\n"
                           "  q <= s; q2 <= s2;\n"
                           "  c <= '0' when rst_n = '0' else d;\n" // negative: no edge, combinational
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-EDGE") == 0);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("reg s <= 1'b1 @(posedge clk) arst(!rst_n -> 1'b0)"));
    CHECK(r.has("reg s2 <= d @(posedge clk) arst("));
    CHECK_FALSE(r.has("reg c "));
}

TEST_CASE("VHDL: a selected assignment listing every value of an enumeration is complete", "[vhdl][elab]") {
    // T65: with Set_BusA_To select BusA <= ... when ..., 11 literals (4 bits, 5 codes unused)
    auto r = elaborate(lib("entity t is port (s : in std_logic_vector(1 downto 0); a, b, c : in std_logic;\n"
                           "  y, n : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  type e_t is (e0, e1, e2);\n"
                           "  signal e : e_t;\n"
                           "begin\n"
                           "  e <= e0 when s = \"00\" else e1 when s = \"01\" else e2;\n"
                           "  with e select y <= a when e0, b when e1, c when e2;\n"
                           // negative: 'unaffected' keeps the value: a latch
                           "  with e select n <= a when e0, b when e1, unaffected when others;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK_FALSE(r.has("latch y "));
    CHECK(r.has("latch n "));
    CHECK(r.count("IR-LATCH-INFERRED") == 1);
}

TEST_CASE("VHDL: a logical operator on arrays of different lengths is a warning (Quartus compiles it)",
          "[vhdl][elab]") {
    // Genesis_MiSTer T80: ioq := (ioq and x"7") xor ('0' & BusA);   (9 and 4 bits)
    auto r = elaborate(lib("entity t is port (a : in std_logic_vector(8 downto 0); b : in std_logic_vector(7 downto 0);\n"
                           "  q : out std_logic_vector(8 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (all) variable v : std_logic_vector(8 downto 0); begin\n"
                           "    v := (a and x\"7\") xor ('0' & b);\n"
                           "    q <= v;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-LENGTH-LOGIC") == 1);
    // the result is as long as the longer operand, as Quartus reads it (SNES CX4Map:
    // a 22-bit and a 23-bit operand into a 23-bit port)
    auto l = elaborate(lib("entity t is port (a : in std_logic_vector(21 downto 0); m : in std_logic_vector(22 downto 0);\n"
                           "  q : out std_logic_vector(22 downto 0)); end;\n"
                           "architecture rtl of t is begin\n  q <= a and m;\nend;\n"),
                       "t");
    CHECK(l.errors() == 0);
    CHECK(l.count("VHDL-LENGTH-LOGIC") == 1);
    // negative: an assignment of the wrong length is still an error
    auto n = elaborate(lib("entity t is port (a : in std_logic_vector(8 downto 0); q : out std_logic_vector(3 downto 0));\n"
                           "end;\narchitecture rtl of t is begin\n  q <= a;\nend;\n"),
                       "t");
    CHECK(n.errors() > 0);
}

TEST_CASE("VHDL: a concurrent procedure call with signal out parameters", "[vhdl][elab]") {
    // Genesis_MiSTer T80_ALU: AddSub(BusA(3 downto 0), BusB(3 downto 0), ..., Q_v(3 downto 0), HalfCarry_v);
    auto r = elaborate(lib("entity t is port (clk : in std_logic; a, b : in std_logic_vector(3 downto 0); sub : in std_logic;\n"
                           "  q : out std_logic_vector(3 downto 0); co : out std_logic); end;\n"
                           "architecture rtl of t is\n"
                           "  procedure AddSub(A : std_logic_vector; B : std_logic_vector; Sub : std_logic;\n"
                           "                   signal Res : out std_logic_vector; signal Carry : out std_logic) is\n"
                           "    variable r : unsigned(A'length downto 0);\n"
                           "  begin\n"
                           "    if Sub = '1' then r := ('0' & unsigned(A)) - ('0' & unsigned(B));\n"
                           "    else r := ('0' & unsigned(A)) + ('0' & unsigned(B)); end if;\n"
                           "    Res <= std_logic_vector(r(A'length - 1 downto 0)); Carry <= r(A'length);\n"
                           "  end;\n"
                           "  signal s : std_logic_vector(3 downto 0); signal c : std_logic;\n"
                           "begin\n"
                           "  AddSub(a, b, sub, s, c);\n"
                           "  q <= s when rising_edge(clk);\n"
                           "  co <= c when rising_edge(clk);\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-STATEMENT") == 0);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("comb  s = "));
    CHECK(r.has("comb  c = "));
    CHECK(r.has("reg q <= s @(posedge clk)"));
}

TEST_CASE("VHDL: conditions that together cover every two-state value need no final else", "[vhdl][elab]") {
    // Genesis_MiSTer vdp.vhd: OBJ_MAX_LINE <= ... when H40 = '1' and E = '0' else ... when H40 = '0' and E = '1';
    auto r = elaborate(lib("entity t is port (h, e : in std_logic; a, b, c, d : in std_logic_vector(5 downto 0);\n"
                           "  y, n : out std_logic_vector(5 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  y <= a when h = '1' and e = '0' else b when h = '1' and e = '1' else\n"
                           "       c when h = '0' and e = '0' else d when h = '0' and e = '1';\n"
                           // negative: h = '0' and e = '1' is missing
                           "  n <= a when h = '1' and e = '0' else b when h = '1' and e = '1' else c when h = '0' and e = '0';\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK_FALSE(r.has("latch y "));
    CHECK(r.has("latch n "));
    CHECK(r.count("IR-LATCH-INFERRED") == 1);
}

TEST_CASE("VHDL: 'when others' after choices covering every two-state value is not a latch", "[vhdl][elab]") {
    // SNES_MiSTer 65C816 ALU: case CTRL.secOp is (all 8 codes) ... when others => null;
    auto r = elaborate(lib("entity t is port (s : in std_logic_vector(1 downto 0); a, b : in std_logic_vector(3 downto 0);\n"
                           "  y, n : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (all) begin\n"
                           "    case s is when \"00\" | \"01\" => y <= a; when \"10\" | \"11\" => y <= b;\n"
                           "      when others => null; end case;\n"
                           "  end process;\n"
                           // negative: "11" is missing, others really holds
                           "  process (all) begin\n"
                           "    case s is when \"00\" | \"01\" => n <= a; when \"10\" => n <= b;\n"
                           "      when others => null; end case;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK_FALSE(r.has("latch y "));
    CHECK(r.has("latch n "));
}

TEST_CASE("VHDL: a type conversion in the formal part of a port association", "[vhdl][elab]") {
    // SNES_MiSTer DSP.vhd: signed(q_a) => BRR_BUF_DO
    auto r = elaborate(lib("entity child is port (clk : in std_logic; d : in std_logic_vector(7 downto 0);\n"
                           "  q : out std_logic_vector(7 downto 0)); end;\n"
                           "architecture rtl of child is begin q <= d when rising_edge(clk); end;\n") +
                           lib("entity t is port (clk : in std_logic; d : in std_logic_vector(7 downto 0);\n"
                               "  o, o2 : out signed(7 downto 0)); end;\n"
                               "architecture rtl of t is\n"
                               "  function inv(x : std_logic_vector) return signed is begin return signed(not x); end;\n"
                               "  signal s, s2 : signed(7 downto 0);\n"
                               "begin\n"
                               "  u : entity work.child port map (clk => clk, d => d, signed(q) => s);\n"
                               // negative: a conversion function is still reported
                               "  v : entity work.child port map (clk => clk, d => d, inv(q) => s2);\n"
                               "  o <= s; o2 <= s2;\n"
                               "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-PORT") == 1);
    CHECK(r.has("inst u : child (.clk(clk), .d(d), .q(s))"));
}

TEST_CASE("VHDL: '&' on a constrained array type works on its unconstrained base", "[vhdl][elab]") {
    // C64_MiSTer video_vicII_656x.vhd: charStore <= charStore(37 downto 0) & nextChar;
    // with type charStoreDef is array(38 downto 0) of unsigned(11 downto 0)
    auto r = elaborate(lib("entity t is port (clk : in std_logic; d : in unsigned(3 downto 0); q : out unsigned(3 downto 0));\n"
                           "end;\n"
                           "architecture rtl of t is\n"
                           "  type st is array(2 downto 0) of unsigned(3 downto 0);\n"
                           "  signal s : st;\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then s <= s(1 downto 0) & d; end if; end process;\n"
                           "  q <= s(2);\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.has("reg s "));
    // negative: a result of the wrong length is still an error
    auto n = elaborate(lib("entity t is port (clk : in std_logic; d : in unsigned(3 downto 0)); end;\n"
                           "architecture rtl of t is\n"
                           "  type st is array(2 downto 0) of unsigned(3 downto 0);\n"
                           "  signal s : st;\n"
                           "begin\n"
                           "  process (clk) begin if rising_edge(clk) then s <= s & d; end if; end process;\n"
                           "end;\n"),
                       "t");
    CHECK(n.count("VHDL-LENGTH") == 1);
}

TEST_CASE("VHDL: a signal driven twice with the same value is one driver", "[vhdl][elab]") {
    // Atari800_MiSTer address_decoder.vhdl: SDRAM_WRITE_EN <= write_enable_next; concurrently
    // and as the default in a combinational process
    auto r = elaborate(lib("entity t is port (a, b, sel : in std_logic; y, n : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  y <= a;\n"
                           "  process (a, b, sel) begin y <= a; n <= a; if sel = '1' then n <= b; end if; end process;\n"
                           // negative: a different value is a real second driver
                           "  n <= b;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("IR-SAME-DRIVERS") == 1);
    CHECK(r.count("IR-MULTIDRIVER") == 1);
    CHECK(std::ranges::any_of(r.diags, [](const Diagnostic& d) {
        return d.code == "IR-MULTIDRIVER" && d.message.find("'n'") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(r.diags, [](const Diagnostic& d) {
        return d.code == "IR-SAME-DRIVERS" && d.message.find("'y'") != std::string::npos;
    }));
}

TEST_CASE("VHDL negative: a clock edge that is not the last condition stays unsupported", "[vhdl][elab]") {
    auto r = elaborate(lib("entity t is port (clk, rst_n, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  q <= d when rising_edge(clk) else '0' when rst_n = '0';\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.count("IR-UNSUPPORTED-EDGE") == 1);
}

// ===========================================================================
// Constructs found in SURF's Ethernet MAC (stage 44)
// ===========================================================================

TEST_CASE("VHDL: a slice with variable bounds assigned (others => x)", "[vhdl][elab]") {
    // SURF EthMacRxImportXgmii: varMaster.tKeep(conv_integer(macSize) downto 0) := (others => '1')
    auto r = elaborate(lib("entity t is port (clk : in std_logic; n : in unsigned(2 downto 0);\n"
                           "  q : out std_logic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) variable v : std_logic_vector(7 downto 0); begin\n"
                           "    if rising_edge(clk) then\n"
                           "      v := (others => '0');\n"
                           "      v(to_integer(n) downto 0) := (others => '1');\n"
                           "      q <= v;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    // every bit compares its index with the bound: bit 7 is set for n >= 7, bit 0 always
    CHECK(r.has("le.s(A=32'b00000000000000000000000000000111, B={29'b00000000000000000000000000000, n})"));
    CHECK(r.has("reg q <= {$mux$"));
    const auto& b = r.design->body(r.design->instance(r.design->tops.front()).body);
    REQUIRE(b.registers.size() == 1);
    CHECK(b.registers.front().q.width() == 8);

    // negative: a value that is not (others => x) still needs static bounds
    auto n = elaborate(lib("entity t is port (clk : in std_logic; n : in unsigned(2 downto 0);\n"
                           "  d : in std_logic_vector(7 downto 0); q : out std_logic_vector(7 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) variable v : std_logic_vector(7 downto 0); begin\n"
                           "    if rising_edge(clk) then\n"
                           "      v := (others => '0');\n"
                           "      v(to_integer(n) downto 0) := d(to_integer(n) downto 0);\n"
                           "      q <= v;\n"
                           "    end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    CHECK(n.count("VHDL-NOT-STATIC") >= 1);
}

TEST_CASE("VHDL: a signal assigned from a process variable after the clocked if", "[vhdl][elab]") {
    // SURF EthMacRxImportXgmii: the variable is computed on the clock edge and copied
    // to the signal after the clocked if: the signal is the variable's register
    auto r = elaborate(lib("entity t is port (clk : in std_logic; d : in std_logic_vector(3 downto 0);\n"
                           "  q : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) variable v : std_logic_vector(3 downto 0); begin\n"
                           "    if rising_edge(clk) then\n"
                           "      v := (others => '0');\n"
                           "      v := d;\n"
                           "    end if;\n"
                           "    q <= v;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-CLOCKED-TAIL") == 0);
    const auto& b = r.design->body(r.design->instance(r.design->tops.front()).body);
    REQUIRE(b.registers.size() == 1); // the variable
    CHECK(r.has("reg proc0.v <= d @(posedge clk)"));
    CHECK(r.has("comb  q = proc0.v"));

    // negative: a signal read after the clocked if is sampled on both edges: still ignored
    auto n = elaborate(lib("entity t is port (clk : in std_logic; d : in std_logic_vector(3 downto 0);\n"
                           "  q : out std_logic_vector(3 downto 0)); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) variable v : std_logic_vector(3 downto 0); begin\n"
                           "    if rising_edge(clk) then v := d; end if;\n"
                           "    q <= d;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    CHECK(n.count("VHDL-CLOCKED-TAIL") == 1);
}

TEST_CASE("VHDL: 'if TRUE_GENERIC then comb elsif rising_edge' is combinational", "[vhdl][elab]") {
    // SURF AxiStreamFifoV2: if FIFO_FIXED_THRESH_G then pause <= ...; elsif rising_edge(clk) ...
    const std::string text = lib("entity t is generic (FIXED_G : boolean := true);\n"
                                 "  port (clk, a, b : in std_logic; q : out std_logic); end;\n"
                                 "architecture rtl of t is begin\n"
                                 "  process (clk, a, b) begin\n"
                                 "    if FIXED_G then q <= a or b;\n"
                                 "    elsif rising_edge(clk) then q <= a; end if;\n"
                                 "  end process;\n"
                                 "end;\n");
    auto r = elaborate(text, "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-ASYNC-LOAD") == 0);
    CHECK(r.design->body(r.design->instance(r.design->tops.front()).body).registers.empty());
    // negative: with the generic false, the clocked branch: a register, no asynchronous branch
    auto f = elaborate(text, "t", {{"FIXED_G", "false"}});
    REQUIRE(f.design);
    REQUIRE(f.design->body(f.design->instance(f.design->tops.front()).body).registers.size() == 1);
    CHECK(f.has("reg q <= a @(posedge clk)"));
}

// ===========================================================================
// Stage 78: f32c (Lattice Diamond, ECP5)
// ===========================================================================

TEST_CASE("VHDL: a clock edge with several enables", "[vhdl][elab][f32c]") {
    // f32c_core.vhd: if rising_edge(clk) and clk_enable = '1' and (not C_cache or dmem_cache_wait = '0') then
    auto r = elaborate(lib("entity t is generic (C_cache : boolean := true);\n"
                           "  port (clk, ce, w, d : in std_logic; q : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk, ce, w) begin\n"
                           "    if rising_edge(clk) and ce = '1' and (not C_cache or w = '0') then q <= d; end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-EDGE") == 0);
    CHECK(r.count("IR-LATCH-INFERRED") == 0);
    CHECK(r.has("reg q <= d @(posedge clk) en("));
}

TEST_CASE("VHDL: several clocked ifs in one process", "[vhdl][elab][f32c]") {
    // f32c_cache.vhd: if falling_edge(clk) ... end if; if rising_edge(clk) ... end if;
    // f32c_core.vhd: two ifs on the same edge with different enables, a later assignment wins
    auto r = elaborate(lib("entity t is port (clk, en1, en2, a, b, c : in std_logic; x, y, z : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin\n"
                           "    if falling_edge(clk) then x <= a; end if;\n"
                           "    if rising_edge(clk) then y <= b; end if;\n"
                           "  end process;\n"
                           "  process (clk, en1, en2) begin\n"
                           "    if rising_edge(clk) and en1 = '1' then z <= a; end if;\n"
                           "    if rising_edge(clk) and en2 = '1' then z <= c; end if;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("VHDL-CLOCKED-TAIL") == 0);
    CHECK(r.has("reg x <= a @(negedge clk)"));
    CHECK(r.has("reg y <= b @(posedge clk)"));
    // one register for z: en2 wins over en1
    CHECK(r.count("IR-MULTIDRIVER") == 0);
    CHECK(r.has("reg z <= "));

    // negative: a statement that is not a clocked if keeps the process as it was
    auto n = elaborate(lib("entity t is port (clk, a, b : in std_logic; x, y : out std_logic); end;\n"
                           "architecture rtl of t is begin\n"
                           "  process (clk) begin\n"
                           "    if rising_edge(clk) then x <= a; end if;\n"
                           "    y <= b;\n"
                           "  end process;\n"
                           "end;\n"),
                       "t");
    CHECK(n.count("VHDL-CLOCKED-TAIL") == 1);
}

TEST_CASE("VHDL: a shared variable RAM written by two processes", "[vhdl][elab][f32c]") {
    // f32c bram_true2p_1clk.vhd: one process per port, the ports in generate branches
    const std::string src = lib("entity t is generic (dual : boolean := true);\n"
                                "  port (clk, ka, kb, we_a, we_b : in std_logic; aa, ab : in unsigned(1 downto 0);\n"
                                "  da, db : in std_logic_vector(7 downto 0); qa, qb : out std_logic_vector(7 downto 0)); end;\n"
                                "architecture rtl of t is\n"
                                "  type mem_t is array (0 to 3) of std_logic_vector(7 downto 0);\n"
                                "  shared variable ram : mem_t;\n"
                                "begin\n"
                                "  g_a: if true generate\n"
                                "  process (clk) begin if rising_edge(clk) then\n"
                                "    if we_a = '1' then ram(to_integer(aa)) := da; end if;\n"
                                "    qa <= ram(to_integer(aa)); end if; end process;\n"
                                "  end generate;\n"
                                "  g_b: if dual generate\n"
                                "  process (kb) begin if rising_edge(kb) then\n"
                                "    if we_b = '1' then ram(to_integer(ab)) := db; end if;\n"
                                "    qb <= ram(to_integer(ab)); end if; end process;\n"
                                "  end generate;\n"
                                "  g_b2: if not dual generate\n"
                                "  process (kb) begin if rising_edge(kb) then qb <= ram(to_integer(ab)); end if; end process;\n"
                                "  end generate;\n"
                                "end;\n");
    auto r = elaborate(src, "t");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-MULTIDRIVER") == 0);
    CHECK(r.has("reg ram <= "));
    CHECK(r.has("reg ram#2 <= "));
    CHECK(r.has("xor")); // every read sees both copies

    // only one port elaborated: the second writer's copy is tied to 0
    auto single = elaborate(src, "t", {{"dual", "false"}});
    REQUIRE(single.design);
    CHECK(single.errors() == 0);
    CHECK_FALSE(single.has("reg ram#2 <= "));

    // negative: a signal driven by two processes is still a multiple driver
    auto bad = elaborate(lib("entity t is port (clk, kb, a, b : in std_logic; q : out std_logic); end;\n"
                             "architecture rtl of t is signal s : std_logic; begin\n"
                             "  process (clk) begin if rising_edge(clk) then s <= a; end if; end process;\n"
                             "  process (kb) begin if rising_edge(kb) then s <= b; end if; end process;\n"
                             "  q <= s;\n"
                             "end;\n"),
                         "t");
    CHECK(bad.count("IR-MULTIDRIVER") >= 1);
}

TEST_CASE("VHDL: a ROM initialized from a file, a partly open output", "[vhdl][elab][f32c]") {
    // f32c rom.vhd: while not endfile(f) loop readline(f, l); ... (the file is not read);
    // glue_sdram_dv.vhd: spi_cen(0) => spi_ss(i), spi_cen(3 downto 1) => open
    auto r = elaborate("library ieee; use std.textio.all; use ieee.std_logic_1164.all;\n"
                       "entity c is port (clk : in std_logic; o : out std_logic_vector(3 downto 0)); end;\n"
                       "architecture x of c is\n"
                       "  type T_rom is array(0 to 3) of std_logic_vector(3 downto 0);\n"
                       "  impure function f(constant fname : string) return T_rom is\n"
                       "    file fh : text open read_mode is fname;\n"
                       "    variable m : T_rom; variable l : line;\n"
                       "  begin\n"
                       "    m := (others => (others => '0'));\n"
                       "    while not endfile(fh) loop readline(fh, l); end loop;\n"
                       "    return m;\n"
                       "  end f;\n"
                       "  signal rom : T_rom := f(\"boot.srec\");\n"
                       "begin\n"
                       "  o <= rom(0) when rising_edge(clk);\n"
                       "end x;\n"
                       "library ieee; use ieee.std_logic_1164.all;\n"
                       "entity top is port (clk : in std_logic; s : out std_logic); end;\n"
                       "architecture x of top is begin\n"
                       "  u : entity work.c port map (clk => clk, o(0) => s, o(3 downto 1) => open);\n"
                       "end x;\n",
                       "top");
    REQUIRE(r.design);
    CHECK(r.errors() == 0);
    CHECK(r.count("IR-UNSUPPORTED-LOOP") == 0);
    CHECK(r.count("IR-UNSUPPORTED-PORT") == 0);
    CHECK(r.count("IR-UNSUPPORTED-INIT") == 1); // the file is not read: the initial value is unknown
    CHECK(r.has("inst u : c (.clk(clk), .o({3'bxxx, s}))"));
}

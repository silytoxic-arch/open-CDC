// Records and packed structs across the language border (stage 9): whole
// record ports in both directions, VHDL element associations (p.f, p.f.g,
// p(i), p(h downto l)) on Verilog ports, and the error cases.

#include "../TestUtil.h"
#include "cdc/CdcModel.h"
#include "engine/AnalysisSession.h"

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

constexpr const char* kPkg = "library ieee; use ieee.std_logic_1164.all;\n"
                             "package types is\n"
                             "  type bus_t is record valid : std_logic; data : std_logic_vector(7 downto 0); end record;\n"
                             "end package;\n";

constexpr const char* kSvTypes = "typedef struct packed { logic [3:0] len; logic flag; } hdr_t;\n"
                                 "typedef struct packed { logic valid; hdr_t hdr; logic [7:0] data; } bus_t;\n";

struct Run {
    std::shared_ptr<const engine::AnalysisResult> r;
    test::TempDir dir;

    [[nodiscard]] std::vector<std::string> sources(std::string_view src) const {
        std::vector<std::string> out;
        for (const auto& p : r->cdc->paths)
            if (p.dstName.find(src) == 0)
                out.push_back(p.srcName + " " + std::to_string(p.width));
        std::ranges::sort(out);
        return out;
    }
    [[nodiscard]] std::size_t count(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(r->diagnostics, [&](const Diagnostic& d) { return d.code == code; }));
    }
};

std::unique_ptr<Run> analyze(const std::vector<std::pair<std::string, std::string>>& files, const std::string& top) {
    auto run = std::make_unique<Run>();
    engine::ProjectConfig cfg;
    cfg.name = "rec";
    cfg.top = top;
    cfg.language = frontend::LanguageStd::SystemVerilog2017;
    for (const auto& [name, text] : files) {
        cfg.sources.push_back(run->dir.write(name, text));
        cfg.sourceLibraries.emplace_back();
    }
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

TEST_CASE("Records: Verilog packed struct to a VHDL record port, field order kept", "[vhdl][mixed][records]") {
    auto run = analyze({{"pkg.vhd", kPkg},
                        {"sink.vhd", "library ieee; use ieee.std_logic_1164.all; use work.types.all;\n"
                                     "entity sink is port (clk : in std_logic; i : in bus_t; o : out bus_t); end;\n"
                                     "architecture rtl of sink is begin\n"
                                     "  process (clk) begin if rising_edge(clk) then o <= i; end if; end process;\n"
                                     "end;\n"},
                        {"top.sv", "typedef struct packed { logic valid; logic [7:0] data; } bus_t;\n"
                                   "module top(input clk_a, input clk_b, input [8:0] din, output [8:0] dout,\n"
                                   "           output logic [7:0] dd);\n"
                                   "  bus_t a, b;\n"
                                   "  always_ff @(posedge clk_a) a <= din;\n"
                                   "  sink u (.clk(clk_b), .i(a), .o(b));\n"
                                   "  assign dout = b;\n"
                                   "  always_ff @(posedge clk_b) dd <= b.data;\n"
                                   "endmodule\n"}},
                       "top");
    REQUIRE(run->r->cdc);
    // b.data is re-registered (two flops), b.valid is used directly (one flop):
    // only true if the struct MSB (valid) lands on the record's first field
    const auto& paths = run->r->cdc->paths;
    REQUIRE(paths.size() == 2);
    for (const auto& p : paths) {
        if (p.dstName == "top.u.o.data") {
            CHECK(p.width == 8);
            CHECK(p.syncType == SyncType::TwoFlop);
        }
        else {
            CHECK(p.dstName == "top.u.o.valid");
            CHECK(p.syncType == SyncType::SingleFlop);
        }
    }
}

TEST_CASE("Records: VHDL record signal to a Verilog packed struct port", "[vhdl][mixed][records]") {
    auto run = analyze({{"pkg.vhd", kPkg},
                        {"vsink.sv", "typedef struct packed { logic valid; logic [7:0] data; } bus_t;\n"
                                     "module vsink(input clk, input bus_t i, output bus_t o, output logic v);\n"
                                     "  always_ff @(posedge clk) begin o <= i; v <= i.valid; end\n"
                                     "endmodule\n"},
                        {"top.vhd", "library ieee; use ieee.std_logic_1164.all; use work.types.all;\n"
                                    "entity top is port (clk_a, clk_b : in std_logic; din : in bus_t;\n"
                                    "  dout : out bus_t; v : out std_logic); end;\n"
                                    "architecture rtl of top is signal a : bus_t; begin\n"
                                    "  process (clk_a) begin if rising_edge(clk_a) then a <= din; end if; end process;\n"
                                    "  u : entity work.vsink port map (clk => clk_b, i => a, o => dout, v => v);\n"
                                    "end;\n"}},
                       "top");
    REQUIRE(run->r->cdc);
    CHECK(run->sources("top.u.v") == std::vector<std::string>{"top.a.valid 1"});
    CHECK(run->sources("top.u.o") == std::vector<std::string>{"top.a.data 8", "top.a.valid 1"});
}

TEST_CASE("Records: VHDL element associations on Verilog struct and vector ports", "[vhdl][mixed][records]") {
    auto run = analyze(
        {{"vsink.sv", std::string(kSvTypes) +
                          "module vsink(input clk, input bus_t i, output bus_t o, output logic v, output logic [3:0] l);\n"
                          "  always_ff @(posedge clk) begin o <= i; v <= i.valid; l <= i.hdr.len; end\n"
                          "endmodule\n"
                          "module vflat(input clk, input [0:7] d, output reg [0:7] q);\n"
                          "  always @(posedge clk) q <= d;\n"
                          "endmodule\n"},
         {"top.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                     "entity top is port (clk_a, clk_b : in std_logic; d8 : in std_logic_vector(7 downto 0);\n"
                     "  v, fl : out std_logic; l : out std_logic_vector(3 downto 0); q0, q7 : out std_logic;\n"
                     "  dd : out std_logic_vector(7 downto 0)); end;\n"
                     "architecture rtl of top is\n"
                     "  signal av, af, ax : std_logic; signal ad : std_logic_vector(7 downto 0);\n"
                     "  signal al : std_logic_vector(3 downto 0);\n"
                     "begin\n"
                     "  process (clk_a) begin if rising_edge(clk_a) then\n"
                     "    av <= d8(0); ad <= d8; al <= d8(3 downto 0); af <= d8(2); ax <= d8(5);\n"
                     "  end if; end process;\n"
                     "  u : entity work.vsink port map (clk => clk_b, i.valid => av, i.hdr.len => al,\n"
                     "    i.hdr.flag => af, i.data => ad, o.data => dd, o.hdr.flag => fl, v => v, l => l);\n"
                     "  f : entity work.vflat port map (clk => clk_b, d(0) => ax, d(1 to 7) => \"0000000\",\n"
                     "    q(0) => q0, q(7) => q7);\n"
                     "end;\n"}},
        "top");
    REQUIRE(run->r->cdc);
    CHECK(std::ranges::none_of(run->r->diagnostics, [](const Diagnostic& d) { return d.isError(); }));
    CHECK(run->sources("top.u.v") == std::vector<std::string>{"top.av 1"});
    CHECK(run->sources("top.u.l") == std::vector<std::string>{"top.al 4"});
    CHECK(run->sources("top.u.o") == std::vector<std::string>{"top.ad 8", "top.af 1", "top.al 4", "top.av 1"});
    // d is [0:7]: d(0) is the MSB, so ax reaches q's MSB only
    CHECK(run->sources("top.f.q") == std::vector<std::string>{"top.ax 1"});
}

TEST_CASE("Records negative: unknown field and out-of-range index on a Verilog port", "[vhdl][mixed][records]") {
    auto run = analyze({{"vsink.sv", std::string(kSvTypes) +
                                         "module vsink(input clk, input bus_t i, input [3:0] n, output bus_t o);\n"
                                         "  always_ff @(posedge clk) o <= i;\n"
                                         "endmodule\n"},
                        {"top.vhd", "library ieee; use ieee.std_logic_1164.all;\n"
                                    "entity top is port (clk_a : in std_logic; a, b : in std_logic); end;\n"
                                    "architecture rtl of top is begin\n"
                                    "  u : entity work.vsink port map (clk => clk_a, i.nosuch => a, n(4) => b,\n"
                                    "    i.data(9) => b);\n"
                                    "end;\n"}},
                       "top");
    CHECK_FALSE(run->r->cdc);
    CHECK(run->count("VHDL-PORT-MAP") == 3);
}

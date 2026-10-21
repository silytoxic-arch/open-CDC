// Regression tests for RTL extraction (slang AST -> Design IR).
// Each test elaborates a small Verilog snippet and checks the extracted
// structure through the stable IR dump format (one line per object).

#include "frontend/Frontend.h"
#include "ir/Dump.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opencdc;
using namespace opencdc::frontend;

namespace {

struct Extract {
    std::shared_ptr<const ir::Design> design;
    std::vector<Diagnostic> diags;
    std::string dump;

    [[nodiscard]] bool has(std::string_view line) const {
        std::istringstream in(dump);
        std::string l;
        while (std::getline(in, l)) {
            const auto first = l.find_first_not_of(' ');
            if (first != std::string::npos && std::string_view(l).substr(first) == line)
                return true;
        }
        return false;
    }
    [[nodiscard]] bool contains(std::string_view text) const { return dump.find(text) != std::string::npos; }
    [[nodiscard]] std::size_t countCode(std::string_view code) const {
        return static_cast<std::size_t>(
            std::ranges::count_if(diags, [&](const Diagnostic& d) { return d.code == code; }));
    }
    [[nodiscard]] bool hasErrors() const {
        return std::ranges::any_of(diags, [](const Diagnostic& d) { return d.isError(); });
    }
    [[nodiscard]] const ir::ModuleBody& body(std::string_view name) const {
        for (const auto& b : design->bodies)
            if (b.name == name)
                return b;
        FAIL("no module body " << name);
        return design->bodies.front();
    }
};

Extract extract(std::string_view verilog, LanguageStd lang = LanguageStd::Verilog2001, std::string top = "") {
    test::TempDir dir;
    FrontendInput in;
    in.sourceFiles = {dir.write(lang == LanguageStd::SystemVerilog2017 ? "t.sv" : "t.v", verilog)};
    in.language = lang;
    in.top = std::move(top);
    CollectingDiagnosticSink sink;
    auto out = makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    Extract e;
    e.design = out.value().design;
    e.diags = sink.take();
    if (e.design)
        e.dump = ir::dumpDesign(*e.design);
    UNSCOPED_INFO(e.dump);
    for (const auto& d : e.diags)
        UNSCOPED_INFO(format(d));
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// Flip-flops, clocks and resets
// ---------------------------------------------------------------------------

TEST_CASE("Plain D flip-flop", "[extract][ff]") {
    auto e = extract("module m(input clk, input d, output reg q);\n"
                     "  always @(posedge clk) q <= d;\n"
                     "endmodule\n");
    REQUIRE(e.design);
    CHECK(e.has("reg q <= d @(posedge clk)"));
    CHECK(e.design->stats().registers == 1);
    CHECK_FALSE(e.hasErrors());
}

TEST_CASE("Negative-edge clock", "[extract][ff]") {
    auto e = extract("module m(input clk, input d, output reg q);\n"
                     "  always @(negedge clk) q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(negedge clk)"));
}

TEST_CASE("Asynchronous active-low reset", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst_n, input [3:0] d, output reg [3:0] q);\n"
                     "  always @(posedge clk or negedge rst_n)\n"
                     "    if (!rst_n) q <= 4'h5; else q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) arst(!rst_n -> 4'b0101)"));
    CHECK(e.countCode("IR-RESET-POLARITY") == 0);
}

TEST_CASE("Asynchronous active-high reset", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst, input d, output reg q);\n"
                     "  always @(posedge clk or posedge rst)\n"
                     "    if (rst) q <= 1'b1; else q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) arst(rst -> 1'b1)"));
}

TEST_CASE("Asynchronous reset written as comparison, event order reversed", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst_n, input d, output reg q);\n"
                     "  always @(negedge rst_n or posedge clk) begin\n"
                     "    if (rst_n == 1'b0) begin\n"
                     "      q <= 1'b0;\n"
                     "    end else begin\n"
                     "      q <= d;\n"
                     "    end\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) arst(!rst_n -> 1'b0)"));
}

TEST_CASE("Asynchronous set and reset with priority", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst, input set, input d, output reg q);\n"
                     "  always @(posedge clk or posedge rst or posedge set)\n"
                     "    if (rst) q <= 1'b0;\n"
                     "    else if (set) q <= 1'b1;\n"
                     "    else q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) arst(rst -> 1'b0) arst(set -> 1'b1)"));
}

TEST_CASE("Reset polarity mismatch is reported", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst, input d, output reg q);\n"
                     "  always @(posedge clk or posedge rst)\n"
                     "    if (!rst) q <= 1'b0; else q <= d;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-RESET-POLARITY") == 1);
    CHECK(e.has("reg q <= d @(posedge clk) arst(!rst -> 1'b0)"));
}

TEST_CASE("Two edges without reset structure are not guessed", "[extract][reset]") {
    auto e = extract("module m(input a, input b, input d, output reg q);\n"
                     "  always @(posedge a or posedge b) q <= d;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-AMBIGUOUS-CLOCK") == 1);
    CHECK(e.design->stats().registers == 0);
    CHECK(e.design->unsupported.size() == 1);
}

TEST_CASE("Registers without reset keep the reset branch out of D", "[extract][reset]") {
    // 'r2' is not reset: it must still be a register clocked by clk, without arst.
    auto e = extract("module m(input clk, input rst_n, input d, output reg r1, output reg r2);\n"
                     "  always @(posedge clk or negedge rst_n)\n"
                     "    if (!rst_n) r1 <= 1'b0;\n"
                     "    else begin r1 <= d; r2 <= r1; end\n"
                     "endmodule\n");
    CHECK(e.has("reg r1 <= d @(posedge clk) arst(!rst_n -> 1'b0)"));
    CHECK(e.has("reg r2 <= r1 @(posedge clk)"));
}

TEST_CASE("Bits with different reset behaviour become separate registers", "[extract][reset]") {
    auto e = extract("module m(input clk, input rst, input en, input [7:0] d, output reg [7:0] p);\n"
                     "  always @(posedge clk or posedge rst)\n"
                     "    if (rst) p[3:0] <= 4'd0;\n"
                     "    else if (en) p <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg p[3:0] <= d[3:0] @(posedge clk) arst(rst -> 4'b0000) en(en)"));
    CHECK(e.has("reg p[7:4] <= d[7:4] @(posedge clk) en(en)"));
}

TEST_CASE("Synchronous reset (active high and active low)", "[extract][srst]") {
    auto e = extract("module m(input clk, input srst, input srst_n, input d, output reg q1, output reg q2);\n"
                     "  always @(posedge clk) if (srst) q1 <= 1'b0; else q1 <= d;\n"
                     "  always @(posedge clk) if (!srst_n) q2 <= 1'b1; else q2 <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q1 <= d @(posedge clk) srst(srst -> 1'b0)"));
    CHECK(e.has("reg q2 <= d @(posedge clk) srst(!srst_n -> 1'b1)"));
}

TEST_CASE("Clock enable", "[extract][enable]") {
    auto e = extract("module m(input clk, input en, input [7:0] d, output reg [7:0] q);\n"
                     "  always @(posedge clk) if (en) q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) en(en)"));
}

TEST_CASE("Synchronous reset with clock enable", "[extract][enable][srst]") {
    auto e = extract("module m(input clk, input srst, input en, input [3:0] d, output reg [3:0] q);\n"
                     "  always @(posedge clk)\n"
                     "    if (srst) q <= 4'd0;\n"
                     "    else if (en) q <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= d @(posedge clk) srst(srst -> 4'b0000) en(en)"));
}

TEST_CASE("Constants selected by decoded logic stay data, not reset", "[extract][srst]") {
    auto e = extract("module m(input clk, input [1:0] s, output reg [3:0] q);\n"
                     "  always @(posedge clk)\n"
                     "    case (s)\n"
                     "      2'd0: q <= 4'b0001;\n"
                     "      2'd1: q <= 4'b0010;\n"
                     "      default: q <= 4'b1000;\n"
                     "    endcase\n"
                     "endmodule\n");
    CHECK(e.contains("reg q <= $mux$"));
    CHECK_FALSE(e.contains("srst("));
}

TEST_CASE("Enable through an always-false hold is recognized from the else branch", "[extract][enable]") {
    auto e = extract("module m(input clk, input hold, input d, output reg q);\n"
                     "  always @(posedge clk) if (hold) q <= q; else q <= d;\n"
                     "endmodule\n");
    CHECK(e.contains("reg q <= d @(posedge clk) en($not$"));
}

// ---------------------------------------------------------------------------
// Partial assignment, multiple drivers
// ---------------------------------------------------------------------------

TEST_CASE("Different bits of a vector in different always blocks", "[extract][drivers]") {
    auto e = extract("module m(input clka, input clkb, input [7:0] d, output reg [7:0] q);\n"
                     "  always @(posedge clka) q[3:0] <= d[3:0];\n"
                     "  always @(posedge clkb) q[7:4] <= d[7:4];\n"
                     "endmodule\n");
    CHECK(e.has("reg q[3:0] <= d[3:0] @(posedge clka)"));
    CHECK(e.has("reg q[7:4] <= d[7:4] @(posedge clkb)"));
    CHECK(e.countCode("IR-MULTIDRIVER") == 0);
}

TEST_CASE("Two always blocks driving the same register", "[extract][drivers]") {
    auto e = extract("module m(input clk, input a, input b, output reg q);\n"
                     "  always @(posedge clk) q <= a;\n"
                     "  always @(posedge clk) q <= b;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-MULTIDRIVER") == 1);
    CHECK(e.hasErrors());
}

TEST_CASE("Continuous assignment and always block on the same net", "[extract][drivers]") {
    auto e = extract("module m(input clk, input a, input b, output reg [1:0] q);\n"
                     "  always @(posedge clk) q <= {a, a};\n"
                     "  initial q = 2'b00;\n"
                     "endmodule\n"
                     "module n(input a, input b, output [1:0] y);\n"
                     "  assign y = {a, b};\n"
                     "  assign y[0] = b;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-MULTIDRIVER") == 1); // only module n
}

// ---------------------------------------------------------------------------
// Combinational processes and latches
// ---------------------------------------------------------------------------

TEST_CASE("Combinational always @* fully assigned", "[extract][comb]") {
    auto e = extract("module m(input s, input [3:0] a, input [3:0] b, output reg [3:0] y);\n"
                     "  always @* begin\n"
                     "    y = a;\n"
                     "    if (s) y = b;\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.contains("comb  y = $mux$"));
    CHECK(e.contains("= mux(A=a, B=b, S=s)"));
    CHECK(e.design->stats().latches == 0);
    CHECK(e.countCode("IR-LATCH-INFERRED") == 0);
}

TEST_CASE("Incomplete combinational assignment infers a latch", "[extract][latch]") {
    auto e = extract("module m(input en, input d, output reg q);\n"
                     "  always @(*) if (en) q = d;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-LATCH-INFERRED") == 1);
    CHECK(e.design->stats().latches == 1);
    CHECK(e.contains("latch q <= $mux$"));
}

TEST_CASE("always_latch infers a latch without warning", "[extract][latch]") {
    auto e = extract("module m(input logic en, input logic d, output logic q);\n"
                     "  always_latch if (en) q = d;\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.design->stats().latches == 1);
    CHECK(e.countCode("IR-LATCH-INFERRED") == 0);
}

TEST_CASE("Unread temporaries are not latches or registers", "[extract][latch][dead]") {
    // Module-level integers used only as temporaries inside one branch are
    // latches in strict Verilog semantics, but nothing reads them: no hardware.
    auto e = extract("module m(input clk, input v, input [3:0] k, output reg [7:0] len);\n"
                     "  integer i, cnt;\n"
                     "  reg [7:0] len_next;\n"
                     "  always @* begin\n"
                     "    len_next = len;\n"
                     "    if (v) begin\n"
                     "      cnt = 0;\n"
                     "      for (i = 0; i < 4; i = i + 1) if (k[i]) cnt = i;\n"
                     "      len_next = len + cnt;\n"
                     "    end\n"
                     "  end\n"
                     "  always @(posedge clk) len <= len_next;\n"
                     "endmodule\n");
    CHECK(e.design->stats().latches == 0);
    CHECK(e.countCode("IR-LATCH-INFERRED") == 0);
    CHECK(e.countCode("IR-DEAD-STORAGE") == 1);
    CHECK(e.has("reg len <= len_next @(posedge clk)"));
}

TEST_CASE("Explicit level-sensitive event list is combinational", "[extract][comb]") {
    auto e = extract("module m(input a, input b, output reg y);\n"
                     "  always @(a or b) y = a & b;\n"
                     "endmodule\n");
    CHECK(e.contains("comb  y = $and$"));
    CHECK(e.design->stats().registers == 0);
}

TEST_CASE("always_comb and always_ff", "[extract][sv]") {
    auto e = extract("module m(input logic clk, input logic [1:0] a, output logic [1:0] q);\n"
                     "  logic [1:0] n;\n"
                     "  always_comb n = ~a;\n"
                     "  always_ff @(posedge clk) q <= n;\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.contains("comb  n = $not$"));
    CHECK(e.has("reg q <= n @(posedge clk)"));
}

// ---------------------------------------------------------------------------
// Blocking assignments, locals, loops
// ---------------------------------------------------------------------------

TEST_CASE("Blocking temporaries inside a clocked block", "[extract][blocking]") {
    auto e = extract("module m(input clk, input a, input b, output reg q);\n"
                     "  always @(posedge clk) begin : blk\n"
                     "    reg t;\n"
                     "    t = a & b;\n"
                     "    q <= t;\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.contains("reg q <= $and$"));
    // The block-local temporary is not a register of the design.
    CHECK(e.design->stats().registers == 1);
}

TEST_CASE("Blocking assignment chain in a clocked block forms a shift register", "[extract][blocking]") {
    auto e = extract("module m(input clk, input d, output reg q);\n"
                     "  reg a;\n"
                     "  always @(posedge clk) begin\n"
                     "    q = a;\n"
                     "    a = d;\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.has("reg q <= a @(posedge clk)"));
    CHECK(e.has("reg a <= d @(posedge clk)"));
}

TEST_CASE("For loop with module-level integer index is unrolled", "[extract][loop]") {
    auto e = extract("module m(input clk, input [3:0] d, output reg [3:0] r);\n"
                     "  integer i;\n"
                     "  always @(posedge clk)\n"
                     "    for (i = 0; i < 4; i = i + 1)\n"
                     "      r[i] <= d[3-i];\n"
                     "endmodule\n");
    CHECK(e.has("reg r <= {d[0], d[1], d[2], d[3]} @(posedge clk)"));
    CHECK(e.design->stats().registers == 1); // no register for i
    CHECK(e.design->unsupported.empty());
}

TEST_CASE("For loop in a combinational block with a local loop variable", "[extract][loop][sv]") {
    auto e = extract("module m(input logic [7:0] d, output logic [3:0] cnt);\n"
                     "  always_comb begin\n"
                     "    cnt = '0;\n"
                     "    for (int i = 0; i < 8; i++) cnt = cnt + d[i];\n"
                     "  end\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.contains("comb  cnt = $add$"));
    CHECK(e.design->unsupported.empty());
}

TEST_CASE("Loop with non-constant bound is reported", "[extract][loop][unsupported]") {
    auto e = extract("module m(input [3:0] n, input [7:0] d, output reg [7:0] y);\n"
                     "  integer i;\n"
                     "  always @* begin\n"
                     "    y = 8'd0;\n"
                     "    for (i = 0; i < n; i = i + 1) y = y + d;\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-LOOP") == 1);
}

TEST_CASE("while loops are reported as unsupported", "[extract][unsupported]") {
    auto e = extract("module m(input [3:0] a, output reg [3:0] y);\n"
                     "  always @* begin y = a; while (y > 4'd8) y = y - 4'd1; end\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-STMT") == 1);
}

// ---------------------------------------------------------------------------
// Memories and dynamic indices
// ---------------------------------------------------------------------------

TEST_CASE("Memory write and read with dynamic addresses", "[extract][memory]") {
    auto e = extract("module m(input clk, input we, input [3:0] wa, input [3:0] ra, input [7:0] wd,\n"
                     "         output [7:0] rd);\n"
                     "  reg [7:0] mem [0:15];\n"
                     "  always @(posedge clk) if (we) mem[wa] <= wd;\n"
                     "  assign rd = mem[ra];\n"
                     "endmodule\n");
    CHECK(e.contains("var [8 x 16] mem"));
    CHECK(e.contains("= dyn_insert<8@0:15>(A=mem, S=wa, D=wd)"));
    CHECK(e.contains("= dyn_select<8@0:15>(A=mem, S=ra)"));
    CHECK(e.contains("reg mem <= $dyn_insert$"));
    CHECK(e.contains(" en(we)"));
}

TEST_CASE("Constant memory index is a static slice", "[extract][memory]") {
    auto e = extract("module m(input clk, input [7:0] wd, output [7:0] first);\n"
                     "  reg [7:0] mem [0:3];\n"
                     "  always @(posedge clk) mem[0] <= wd;\n"
                     "  assign first = mem[0];\n"
                     "endmodule\n");
    // [0:3] ascending: element 0 is the most significant byte (offset 24).
    CHECK(e.has("reg mem{24+:8} <= wd @(posedge clk)"));
    CHECK(e.has("assign first = mem{24+:8}"));
}

TEST_CASE("Dynamic bit select and indexed part-select", "[extract][select]") {
    auto e = extract("module m(input [15:0] v, input [3:0] i, input [1:0] k, output b, output [3:0] n);\n"
                     "  assign b = v[i];\n"
                     "  assign n = v[k*4 +: 4];\n"
                     "endmodule\n");
    CHECK(e.contains("= dyn_select<1@15:0>(A=v, S=i)"));
    CHECK(e.contains("= dyn_select<1@15:0>(A=v, S=$"));
    CHECK(e.design->unsupported.empty());
}

TEST_CASE("Indexed part-select with a variable base as assignment target", "[extract][select]") {
    auto e = extract("module m(input clk, input [1:0] seg, input [7:0] d, output reg [31:0] q);\n"
                     "  always @(posedge clk) q[seg*8 +: 8] <= d;\n"
                     "endmodule\n");
    CHECK(e.design->unsupported.empty());
    CHECK(e.contains("= dyn_insert<1@31:0>(A=q, S=$mul$"));
    CHECK(e.contains(", D=d)"));
    CHECK(e.contains("reg q <= $dyn_insert$"));
}

TEST_CASE("Ascending declared range keeps declared indices in the dump", "[extract][select]") {
    auto e = extract("module m(input [0:7] a, output [0:3] y);\n"
                     "  assign y = a[2:5];\n"
                     "endmodule\n");
    CHECK(e.has("assign y = a[2:5]"));
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

TEST_CASE("Conditional operator, concatenation and replication", "[extract][expr]") {
    auto e = extract("module m(input s, input [1:0] a, input [1:0] b, output [5:0] y);\n"
                     "  assign y = {2{s ? a : b}} ^ {a, b, 2'b11};\n"
                     "endmodule\n");
    CHECK(e.contains("= mux(A=b, B=a, S=s)"));
    // {2{...}} is 4 bits wide and zero-extended to the 6-bit context.
    CHECK(e.contains("= xor(A={2'b00, $mux$0, $mux$0}, B={a, b, 2'b11})"));
}

TEST_CASE("Concatenated assignment target", "[extract][expr]") {
    auto e = extract("module m(input [3:0] a, input [3:0] b, output c, output [3:0] s);\n"
                     "  assign {c, s} = a + b;\n"
                     "endmodule\n");
    // One connection per target; the 5-bit sum is split between them.
    CHECK(e.has("assign s = $add$0[3:0]"));
    CHECK(e.has("assign c = $add$0[4]"));
    CHECK(e.contains("= add(A={1'b0, a}, B={1'b0, b})"));
}

TEST_CASE("Parameters and localparams fold to constants", "[extract][expr]") {
    auto e = extract("module m #(parameter W = 3) (output [7:0] y);\n"
                     "  localparam K = W * 2 + 1;\n"
                     "  assign y = K << 1;\n"
                     "endmodule\n");
    CHECK(e.has("assign y = 8'b00001110"));
    CHECK(e.design->bodies[0].cells.empty());
}

TEST_CASE("Signed arithmetic shift and comparison", "[extract][expr]") {
    auto e = extract("module m(input signed [7:0] a, input signed [7:0] b, output signed [7:0] y, output lt);\n"
                     "  assign y = a >>> 2;\n"
                     "  assign lt = a < b;\n"
                     "endmodule\n");
    CHECK(e.contains("= sshr.s(A=a, B="));
    CHECK(e.contains("= lt.s(A=a, B=b)"));
}

TEST_CASE("Reduction and logical operators", "[extract][expr]") {
    auto e = extract("module m(input [3:0] a, input b, output x, output y, output z);\n"
                     "  assign x = ^a;\n"
                     "  assign y = ~|a && b;\n"
                     "  assign z = !b;\n"
                     "endmodule\n");
    CHECK(e.contains("= reduce_xor(A=a)"));
    CHECK(e.contains("= reduce_or(A=a)"));
    CHECK(e.contains("= logic_and(A=$not$"));
    CHECK(e.contains("= logic_not(A=b)"));
}

TEST_CASE("casez items become wildcard comparisons", "[extract][case]") {
    auto e = extract("module m(input [2:0] r, output reg [1:0] g);\n"
                     "  always @* begin\n"
                     "    casez (r)\n"
                     "      3'b1??: g = 2'd2;\n"
                     "      3'b01?: g = 2'd1;\n"
                     "      default: g = 2'd0;\n"
                     "    endcase\n"
                     "  end\n"
                     "endmodule\n");
    CHECK(e.contains("= eq_wild(A=r, B=3'b1zz)"));
    CHECK(e.contains("= eq_wild(A=r, B=3'b01z)"));
    CHECK(e.contains("comb  g = $mux$"));
}

TEST_CASE("User functions are inlined", "[extract][function]") {
    auto e = extract("module m(input clk, input [3:0] b, output reg [3:0] g);\n"
                     "  function [3:0] bin2gray;\n"
                     "    input [3:0] v;\n"
                     "    begin\n"
                     "      bin2gray = v ^ (v >> 1);\n"
                     "    end\n"
                     "  endfunction\n"
                     "  always @(posedge clk) g <= bin2gray(b);\n"
                     "endmodule\n");
    CHECK(e.contains("= shr(A=b, B="));
    CHECK(e.contains("= xor(A=b, B=$shr$"));
    CHECK(e.contains("reg g <= $xor$"));
    CHECK(e.design->unsupported.empty());
}

TEST_CASE("Constant function calls fold", "[extract][function]") {
    auto e = extract("module m #(parameter DEPTH = 16) (output [7:0] y);\n"
                     "  function integer clog2(input integer v);\n"
                     "    integer i;\n"
                     "    begin clog2 = 0; for (i = v - 1; i > 0; i = i >> 1) clog2 = clog2 + 1; end\n"
                     "  endfunction\n"
                     "  assign y = clog2(DEPTH);\n"
                     "endmodule\n");
    CHECK(e.has("assign y = 8'b00000100"));
}

TEST_CASE("Gate primitives", "[extract][primitive]") {
    auto e = extract("module m(input a, input b, input c, output y, output z);\n"
                     "  nand g1 (y, a, b, c);\n"
                     "  not  g2 (z, a);\n"
                     "endmodule\n");
    CHECK(e.contains("= and(A=$and$"));
    CHECK(e.contains("= not(A=a)"));
    CHECK(e.contains("assign y = $not$"));
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

TEST_CASE("Port connections with expressions and concatenated outputs", "[extract][hierarchy]") {
    auto e = extract("module leaf(input [1:0] i, output [1:0] o); assign o = ~i; endmodule\n"
                     "module top(input a, input b, input c, output x, output y);\n"
                     "  leaf u0 (.i({a & b, c}), .o({x, y}));\n"
                     "endmodule\n");
    CHECK(e.contains("inst u0 : leaf (.i({$and$"));
    CHECK(e.contains(", c}), .o({x, y}))"));
    CHECK(e.countCode("IR-MULTIDRIVER") == 0);
}

TEST_CASE("Parameterized modules produce one body per parameter set", "[extract][hierarchy]") {
    auto e = extract("module r #(parameter W = 1) (input clk, input [W-1:0] d, output reg [W-1:0] q);\n"
                     "  always @(posedge clk) q <= d;\n"
                     "endmodule\n"
                     "module top(input clk, input [7:0] d, output [7:0] q1, output [3:0] q2, output [3:0] q3);\n"
                     "  r #(.W(8)) u1 (clk, d, q1);\n"
                     "  r #(.W(4)) u2 (clk, d[3:0], q2);\n"
                     "  r #(.W(4)) u3 (clk, d[7:4], q3);\n"
                     "endmodule\n",
                     LanguageStd::Verilog2001, "top");
    CHECK(e.contains("module r#(W=8)"));
    CHECK(e.contains("module r#(W=4)"));
    const auto st = e.design->stats();
    CHECK(st.instances == 4);
    CHECK(st.registers == 3);
    CHECK(st.registerBits == 16);
    // Instance tree references the right bodies.
    const auto u2 = e.design->findInstance("top.u2");
    REQUIRE(u2);
    CHECK(e.design->body(e.design->instance(*u2).body).params == "#(W=4)");
}

TEST_CASE("Generate if/for select instances and logic", "[extract][generate]") {
    auto e = extract("module top #(parameter USE_REG = 1, parameter N = 2) (input clk, input [N-1:0] d,\n"
                     "                                                     output [N-1:0] q);\n"
                     "  genvar k;\n"
                     "  generate\n"
                     "    for (k = 0; k < N; k = k + 1) begin : g\n"
                     "      if (USE_REG) begin : r\n"
                     "        reg t;\n"
                     "        always @(posedge clk) t <= d[k];\n"
                     "        assign q[k] = t;\n"
                     "      end else begin : w\n"
                     "        assign q[k] = d[k];\n"
                     "      end\n"
                     "    end\n"
                     "  endgenerate\n"
                     "endmodule\n");
    CHECK(e.has("reg g[0].r.t <= d[0] @(posedge clk)"));
    CHECK(e.has("reg g[1].r.t <= d[1] @(posedge clk)"));
    CHECK(e.has("assign q[1] = g[1].r.t"));
    CHECK_FALSE(e.contains("g[0].w"));
}

TEST_CASE("Hierarchical references are reported and kept conservatively", "[extract][unsupported]") {
    auto e = extract("module leaf(input a); wire inner = a; endmodule\n"
                     "module top(input a, output y);\n"
                     "  leaf u (.a(a));\n"
                     "  assign y = u.inner;\n"
                     "endmodule\n",
                     LanguageStd::Verilog2001, "top");
    CHECK(e.countCode("IR-UNSUPPORTED-EXPR") == 1);
    CHECK(e.contains("= opaque() \"hierarchical reference 'inner'\""));
    REQUIRE(e.design->unsupported.size() == 1);
    CHECK(e.design->unsupported[0].module == "top");
}

TEST_CASE("Task calls inside always blocks are reported", "[extract][unsupported]") {
    auto e = extract("module m(input clk, input d, output reg q);\n"
                     "  task set_q(input v); q = v; endtask\n"
                     "  always @(posedge clk) set_q(d);\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-STMT") == 1);
}

// ---------------------------------------------------------------------------
// Initial values and error handling
// ---------------------------------------------------------------------------

TEST_CASE("Initial values from initializers and initial blocks", "[extract][init]") {
    auto e = extract("module m(input clk, input d, output reg q, output reg p, output x);\n"
                     "  reg r = 1'b1;\n"
                     "  initial begin q = 1'b0; p = 1'b1; end\n"
                     "  always @(posedge clk) begin q <= d; p <= q; r <= p; end\n"
                     "  assign x = r;\n"
                     "endmodule\n");
    CHECK(e.contains("var r init=1'b1"));
    CHECK(e.contains("output var q init=1'b0"));
    CHECK(e.contains("output var p init=1'b1"));
    CHECK(e.design->stats().registers == 3);
}

TEST_CASE("Non-constant asynchronous load is flagged", "[extract][reset]") {
    auto e = extract("module m(input clk, input load, input [1:0] v, input [1:0] d, output reg [1:0] q);\n"
                     "  always @(posedge clk or posedge load) if (load) q <= v; else q <= d;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-ASYNC-LOAD") == 1);
    CHECK(e.has("reg q <= d @(posedge clk) arst(load -> v)"));
}

TEST_CASE("Design IR is not built when the HDL has errors", "[extract][errors]") {
    test::TempDir dir;
    FrontendInput in;
    in.sourceFiles = {dir.write("e.v", "module m(input a, output y); assign y = b; endmodule\n")};
    CollectingDiagnosticSink sink;
    auto out = makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    CHECK_FALSE(out.value().design);
    CHECK(std::ranges::any_of(sink.diagnostics(), [](const Diagnostic& d) { return d.code == "IR-SKIPPED"; }));
}

namespace {

std::string goldenText(const char* name) {
    std::ifstream golden(std::filesystem::path(OPENCDC_TESTS_DIR) / "ir_extract" / "golden" / name, std::ios::binary);
    REQUIRE(golden);
    std::stringstream text;
    text << golden.rdbuf();
    std::string s = text.str();
    std::erase(s, '\r'); // tolerate CRLF checkouts on Windows
    return s;
}

std::string extractExample(const std::filesystem::path& dir, std::initializer_list<const char*> files,
                           std::vector<std::filesystem::path> includeDirs, const char* top) {
    FrontendInput in;
    for (const char* f : files)
        in.sourceFiles.push_back(dir / f);
    in.includeDirs = std::move(includeDirs);
    in.top = top;
    CollectingDiagnosticSink sink;
    auto out = makeSlangFrontend()->elaborate(in, sink);
    REQUIRE(out.ok());
    REQUIRE(out.value().design);
    CHECK(sink.diagnostics().empty());
    return ir::dumpDesign(*out.value().design);
}

} // namespace

// The golden files were reviewed line by line against the RTL; a change in
// them must be reviewed again, not just regenerated.
TEST_CASE("Smoke example IR matches the reviewed golden dump", "[extract][golden]") {
    const auto dir = test::examplesDir() / "smoke";
    CHECK(extractExample(dir, {"rtl/sync_2ff.v", "rtl/tx_domain.v", "rtl/rx_domain.v", "rtl/top.v"},
                         {dir / "rtl" / "include"}, "top") == goldenText("smoke.ir"));
}

TEST_CASE("Async FIFO example IR matches the reviewed golden dump", "[extract][golden]") {
    const auto dir = test::examplesDir() / "async_fifo";
    CHECK(extractExample(dir,
                         {"rtl/fifomem.v", "rtl/sync_r2w.v", "rtl/sync_w2r.v", "rtl/rptr_empty.v", "rtl/wptr_full.v",
                          "rtl/afifo.v"},
                         {}, "afifo") == goldenText("afifo.ir"));
}

// Regression tests for RTL extraction (slang AST -> Design IR).
// Each test elaborates a small Verilog snippet and checks the extracted
// structure through the stable IR dump format (one line per object).

#include "cdc/SeqGraph.h"
#include "frontend/Frontend.h"
#include "graph/DesignView.h"
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
        const auto it = std::ranges::find(design->bodies, name, &ir::ModuleBody::name);
        INFO("module body " << name);
        REQUIRE(it != design->bodies.end()); // single exit: no unreachable return after FAIL (MSVC C4702)
        return *it;
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

TEST_CASE("Synchronous reset written as a comparison with a constant", "[extract][srst]") {
    // Analog Devices style: if (rstn == 1'b0); also == 1, != 0 (32-bit) and != 1'b1
    auto e = extract("module m(input clk, input r1, input r2, input r3, input r4, input d,\n"
                     "         output reg q1, output reg q2, output reg q3, output reg q4);\n"
                     "  always @(posedge clk) if (r1 == 1'b0) q1 <= 1'b0; else q1 <= d;\n"
                     "  always @(posedge clk) if (r2 == 1) q2 <= 1'b1; else q2 <= d;\n"
                     "  always @(posedge clk) if (r3 != 0) q3 <= 1'b0; else q3 <= d;\n"
                     "  always @(posedge clk) if (r4 != 1'b1) q4 <= 1'b0; else q4 <= d;\n"
                     "endmodule\n");
    CHECK(e.has("reg q1 <= d @(posedge clk) srst(!r1 -> 1'b0)"));
    CHECK(e.has("reg q2 <= d @(posedge clk) srst(r2 -> 1'b1)"));
    CHECK(e.has("reg q3 <= d @(posedge clk) srst(r3 -> 1'b0)"));
    CHECK(e.has("reg q4 <= d @(posedge clk) srst(!r4 -> 1'b0)"));
}

TEST_CASE("Comparisons that are not a reset stay in D", "[extract][srst]") {
    // negative: a multi-bit compare (decoded state) and a compare of two
    // signals select data, not a reset
    auto e = extract("module m(input clk, input [1:0] s, input a, input b, input d,\n"
                     "         output reg q1, output reg q2);\n"
                     "  always @(posedge clk) if (s == 2'd0) q1 <= 1'b0; else q1 <= d;\n"
                     "  always @(posedge clk) if (a == b) q2 <= 1'b0; else q2 <= d;\n"
                     "endmodule\n");
    CHECK_FALSE(e.contains("srst("));
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
    CHECK(e.contains("reg q <= {$mux$")); // bit 2 is 0 in every branch: a constant, not selected
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

TEST_CASE("Blocking and non-blocking assignments mixed in a combinational block", "[extract][comb]") {
    // Digilent PWM_AXI: the default branch of a read multiplexer uses '<='
    auto e = extract("module m(input clk, input [1:0] s, input [7:0] a, input [7:0] b, output reg [7:0] q);\n"
                     "  reg [7:0] y;\n"
                     "  always @(posedge clk) q <= y;\n"
                     "  always @(*)\n"
                     "    case (s)\n"
                     "      2'd0: y = a;\n"
                     "      2'd1: begin y = 0; if (b[0]) y = b; end\n"
                     "      default: y <= 0;\n"
                     "    endcase\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-MIXED-ASSIGN") == 1);
    CHECK(e.design->stats().latches == 0);       // every path assigns y
    CHECK(e.countCode("IR-LATCH-INFERRED") == 0);
    graph::DesignView view(*e.design);
    CollectingDiagnosticSink sink;
    cdc::SeqGraph sg(view, sink);
    CHECK(sg.combLoops() == 0);                  // not y depending on itself
    // negative: only non-blocking assignments in a combinational block keep their meaning
    auto n = extract("module m(input s, input [7:0] a, output reg [7:0] y);\n"
                     "  always @(*) if (s) y <= a; else y <= 0;\n"
                     "endmodule\n");
    CHECK(n.countCode("IR-MIXED-ASSIGN") == 0);
    CHECK(n.design->stats().latches == 0);
    // negative: a clocked block mixing both keeps its register behaviour (and the warning)
    auto c = extract("module m(input clk, input [7:0] a, output reg [7:0] y);\n"
                     "  always @(posedge clk) begin y = 0; y <= a; end\n"
                     "endmodule\n");
    CHECK(c.countCode("IR-MIXED-ASSIGN") >= 1);
}

TEST_CASE("Incomplete combinational assignment infers a latch", "[extract][latch]") {
    auto e = extract("module m(input en, input d, output reg q);\n"
                     "  always @(*) if (en) q = d;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-LATCH-INFERRED") == 1);
    CHECK(e.design->stats().latches == 1);
    CHECK(e.contains("latch q <= $mux$"));
}

TEST_CASE("A case listing every selector value needs no default", "[extract][latch]") {
    // uart16550 uart_receiver: 16 constant items for a 4-bit selector, some grouped
    auto e = extract("module m(input [3:0] lcr, output reg [9:0] toc);\n"
                     "  always @(lcr)\n"
                     "    case (lcr[3:0])\n"
                     "      4'b0000: toc = 447; 4'b0100: toc = 479; 4'b0001, 4'b1000: toc = 511; 4'b1100: toc = 543;\n"
                     "      4'b0010, 4'b0101, 4'b1001: toc = 575; 4'b0011, 4'b0110, 4'b1010, 4'b1101: toc = 639;\n"
                     "      4'b0111, 4'b1011, 4'b1110: toc = 703; 4'b1111: toc = 767;\n"
                     "    endcase\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-LATCH-INFERRED") == 0);
    CHECK(e.design->stats().latches == 0);
}

TEST_CASE("A case missing a selector value without default infers a latch", "[extract][latch]") {
    auto e = extract("module m(input [1:0] s, input a, b, c, output reg y);\n"
                     "  always @* case (s) 2'b00: y = a; 2'b01: y = b; 2'b10: y = c; endcase\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-LATCH-INFERRED") == 1);
    // casez: a wildcard item covers both values of its '?' bits
    auto w = extract("module m(input [1:0] s, input a, b, output reg y);\n"
                     "  always @* casez (s) 2'b0?: y = a; 2'b10: y = b; 2'b11: y = a; endcase\n"
                     "endmodule\n");
    CHECK(w.countCode("IR-LATCH-INFERRED") == 0);
    auto w2 = extract("module m(input [1:0] s, input a, b, output reg y);\n"
                      "  always @* casez (s) 2'b0?: y = a; 2'b10: y = b; endcase\n"
                      "endmodule\n");
    CHECK(w2.countCode("IR-LATCH-INFERRED") == 1);
    // plain case: an x bit never matches, so it covers nothing
    auto x = extract("module m(input [1:0] s, input a, b, output reg y);\n"
                     "  always @* case (s) 2'b0x: y = a; 2'b00: y = b; 2'b10: y = b; 2'b11: y = a; endcase\n"
                     "endmodule\n");
    CHECK(x.countCode("IR-LATCH-INFERRED") == 1);
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

TEST_CASE("Identical duplicate assignments are one driver", "[extract][multidriver]") {
    // LiteX emits the same continuous assignment several times
    auto e = extract("module m(input [3:0] a, output [1:0] x, output [1:0] y);\n"
                     "  assign {x, y} = a;\n"
                     "  assign {x, y} = a;\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-MULTIDRIVER") == 0);
    auto n = extract("module m(input [3:0] a, input [3:0] b, output [1:0] x, output [1:0] y);\n"
                     "  assign {x, y} = a;\n"
                     "  assign {x, y} = b;\n"
                     "endmodule\n");
    CHECK(n.countCode("IR-MULTIDRIVER") >= 1);
}

TEST_CASE("Static part of a dynamically indexed memory element as target", "[extract][memory]") {
    // LiteX byte-enable write: mem[adr][i*8 +: 8] <= dat[i*8 +: 8]
    auto e = extract("module m(input clk, input [3:0] we, input [1:0] adr, input [31:0] dat, output [31:0] q);\n"
                     "  reg [31:0] mem [0:3];\n"
                     "  always @(posedge clk) begin : wr\n"
                     "    integer i;\n"
                     "    for (i = 0; i < 4; i = i + 1) if (we[i]) mem[adr][i*8 +: 8] <= dat[i*8 +: 8];\n"
                     "  end\n"
                     "  assign q = mem[adr];\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-LVALUE") == 0);
    CHECK(e.contains("dyn_insert"));
    CHECK(e.contains("dyn_select"));
}

TEST_CASE("Compound assignments read the target's current value", "[extract][sv]") {
    // nontrivial-mips icache: rddata_n |= {W{hit[i]}} & data[i] in a for loop
    auto e = extract("module t(input logic [3:0] hit, input logic [7:0] d0, d1, d2, d3, input logic [3:0] q,\n"
                     "         output logic [7:0] y, output logic [2:0] cnt);\n"
                     "  logic [7:0] d [4];\n"
                     "  assign d[0] = d0; assign d[1] = d1; assign d[2] = d2; assign d[3] = d3;\n"
                     "  always_comb begin\n"
                     "    y = '0;\n"
                     "    cnt = '0;\n"
                     "    for (int i = 0; i < 4; ++i) begin\n"
                     "      y |= {8{hit[i]}} & d[i];\n"
                     "      cnt += q[i];\n"
                     "    end\n"
                     "  end\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    REQUIRE(e.design);
    CHECK_FALSE(e.hasErrors());
    CHECK(e.countCode("IR-UNSUPPORTED-EXPR") == 0);
    // y depends on every d and hit, cnt on every q: no opaque value
    CHECK_FALSE(e.contains("opaque"));
    CHECK(e.has("comb  y = $or$9"));
    CHECK(e.has("comb  cnt = $add$10"));
    for (const char* n : {"d0", "d1", "d2", "d3", "hit"})
        CHECK(e.contains(n));
}

TEST_CASE("Asynchronous reset in a named block with a local declaration", "[extract][reset]") {
    // secworks aes: always @(posedge clk or negedge reset_n) begin: reg_update integer i; if (!reset_n) ...
    auto e = extract("module t(input clk, input reset_n, input we, input [7:0] d, output reg [7:0] q);\n"
                     "  reg [7:0] mem [0:3];\n"
                     "  always @(posedge clk or negedge reset_n)\n"
                     "    begin: reg_update\n"
                     "      integer i;\n"
                     "      if (!reset_n) begin\n"
                     "        for (i = 0; i < 4; i = i + 1) mem[i] <= 8'h0;\n"
                     "        q <= 8'h0;\n"
                     "      end else if (we) q <= d;\n"
                     "    end\n"
                     "endmodule\n");
    REQUIRE(e.design);
    CHECK(e.countCode("IR-AMBIGUOUS-CLOCK") == 0);
    CHECK(e.contains("@(posedge clk) arst(!reset_n -> 8'b00000000)"));
    // a static declaration with an initializer applies once, at time 0 (stage 63):
    // the block is still 'if (reset) ... else ...'
    auto stat = extract("module t(input clk, input reset_n, input [7:0] d, output reg [7:0] q);\n"
                        "  always @(posedge clk or negedge reset_n)\n"
                        "    begin: b\n"
                        "      reg [7:0] t = 8'h0;\n"
                        "      if (!reset_n) q <= 8'h0; else q <= d;\n"
                        "    end\n"
                        "endmodule\n");
    CHECK(stat.countCode("IR-AMBIGUOUS-CLOCK") == 0);
    CHECK(stat.contains("@(posedge clk) arst(!reset_n -> 8'b00000000)"));
    // negative: an automatic declaration with an initializer is executed every time,
    // a statement before the reset test: the block stays ambiguous
    auto init = extract("module t(input clk, input reset_n, input [7:0] d, output reg [7:0] q);\n"
                        "  always @(posedge clk or negedge reset_n)\n"
                        "    begin: b\n"
                        "      automatic logic [7:0] t = 8'h0;\n"
                        "      if (!reset_n) q <= 8'h0; else q <= d;\n"
                        "    end\n"
                        "endmodule\n",
                        LanguageStd::SystemVerilog2017);
    CHECK(init.countCode("IR-AMBIGUOUS-CLOCK") == 1);
}

TEST_CASE("Multi-dimensional unpacked arrays are flattened", "[extract][array]") {
    // open-la500 icache: wire [7:0] way_bank_addra [1:0][3:0], assigned per [i][j] in nested generate loops
    auto e = extract("module t(input clk, input [7:0] a, input s, output [7:0] y, output reg [7:0] q);\n"
                     "  wire [7:0] b [1:0][3:0];\n"
                     "  reg  [7:0] m [1:0][3:0];\n"
                     "  genvar i, j;\n"
                     "  for (i = 0; i < 2; i = i + 1) begin: gi\n"
                     "    for (j = 0; j < 4; j = j + 1) begin: gj\n"
                     "      assign b[i][j] = a + i * 4 + j;\n"
                     "    end\n"
                     "  end\n"
                     "  always @(posedge clk) begin\n"
                     "    m[s][2] <= b[1][3];\n"
                     "    q <= m[1][2];\n"
                     "  end\n"
                     "  assign y = b[s][1];\n"
                     "endmodule\n");
    REQUIRE(e.design);
    CHECK(e.countCode("IR-UNSUPPORTED-TYPE") == 0);
    CHECK(e.countCode("IR-UNSUPPORTED-LVALUE") == 0);
    // b[1][3]: outer element 1 (32 bits up), inner element 3 (24 bits up)
    CHECK(e.has("wire [32 x 2] b"));
    CHECK(e.contains("b{56+:8} = $add$9[7:0]"));
    CHECK(e.has("reg q <= m{48+:8} @(posedge clk)"));
    CHECK(e.contains("dyn_select<32@1:0>(A=b, S=s)"));
    // negative: an array of a non-integral element type is still reported
    auto r = extract("module t2(input clk, input real d, output o);\n  real r [1:0];\n  assign o = 1'b0;\nendmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(r.countCode("IR-UNSUPPORTED-TYPE") >= 1);
}

TEST_CASE("IR: always without an event control is combinational logic, as synthesis tools read it",
          "[ir][process]") {
    auto e = extract("module t(input [1:0] sel, input r, output reg m);\n"
                     "  always begin\n"
                     "    case (sel) 2'b00: m = r; default: m = 1'b0; endcase\n"
                     "  end\nendmodule\n");
    CHECK(e.countCode("IR-ALWAYS-NO-EVENT") == 1);
    CHECK(e.countCode("IR-UNSUPPORTED-PROCESS") == 0);
    CHECK(e.has("cell $mux$1 [1] = mux(A=1'b0, B=r, S=$eq$0)"));
    CHECK(e.has("comb  m = $mux$1"));
}

TEST_CASE("IR negative: always blocks that wait stay unsupported", "[ir][process]") {
    auto e = extract("module t(input x, output reg a, output reg b, output reg c, output reg d);\n"
                     "  task automatic pulse; begin #1 c = 1; end endtask\n"
                     "  always begin #5 a = ~a; end\n"
                     "  always begin wait (x); b = 1; end\n"
                     "  always begin pulse; end\n"
                     "  always begin d = #2 x; end\n"
                     "endmodule\n");
    CHECK(e.countCode("IR-ALWAYS-NO-EVENT") == 0);
    CHECK(e.countCode("IR-UNSUPPORTED-PROCESS") == 4);
}

TEST_CASE("IR: assignment patterns", "[ir][pattern]") {
    auto e = extract("module t(input [7:0] r, output [7:0] o);\n"
                     "  wire [7:0] rom [0:3] = '{8'h11, 8'h22, 8'h33, 8'h44};\n" // ascending: rom[0] on the left
                     "  wire [7:0] arr [3:0] = '{r, 8'h00, 8'h00, 8'h00};\n"     // descending: arr[3] = r
                     "  wire [7:0] one [1] = '{r};\n"
                     "  wire [7:0] dflt [0:1] = '{default: 8'hA5};\n"
                     "  wire [3:0][3:0] rep = '{4{4'h3}};\n"
                     "  assign o = rom[1] ^ arr[3] ^ one[0] ^ dflt[0] ^ rep[3];\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.countCode("IR-UNSUPPORTED-EXPR") == 0);
    CHECK(e.has("assign rom = 32'b00010001001000100011001101000100"));
    CHECK(e.has("assign arr = {r, 24'b000000000000000000000000}"));
    CHECK(e.has("assign one = r"));
    CHECK(e.has("assign dflt = 16'b1010010110100101"));
    CHECK(e.has("assign rep = 16'b0011001100110011"));
}

TEST_CASE("IR: unpacked structs and their arrays", "[ir][struct]") {
    auto e = extract("typedef struct { logic [7:0] y; logic [3:0] c; logic b; } ph_t;\n"
                     "module t(input clk, input [7:0] d, output reg [7:0] q, output reg [3:0] qc, output reg qb);\n"
                     "  ph_t p[3];\n"
                     "  ph_t s;\n"
                     "  localparam ph_t K = '{8'h01, 4'h2, 1'b1};\n"
                     "  localparam logic [7:0] LUT [4] = '{8'h10, 8'h20, 8'h30, 8'h40};\n"
                     "  always @(posedge clk) begin\n"
                     "    p[0].y <= d; p[0].c <= 4'h5; p[1] <= p[0];\n"
                     "    s <= '{8'h00, 4'h9, 1'b1};\n"
                     "    q <= p[1].y ^ K.y ^ LUT[d[1:0]]; qc <= s.c; qb <= s.b;\n"
                     "  end\nendmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.countCode("IR-UNSUPPORTED-TYPE") == 0);
    CHECK(e.countCode("IR-UNSUPPORTED-EXPR") == 0);
    CHECK(e.countCode("IR-UNSUPPORTED-LVALUE") == 0);
    // first field at bit 0: y [7:0], c [11:8], b [12]
    CHECK(e.has("var [13 x 3] p"));
    CHECK(e.has("reg s <= 13'b1100100000000 @(posedge clk)"));
    CHECK(e.has("reg p{13+:25} <= {4'b0101, d, p{26+:13}} @(posedge clk)"));
    CHECK(e.has("reg qc <= s[11:8] @(posedge clk)"));
    CHECK(e.has("reg qb <= s[12] @(posedge clk)"));
    CHECK(e.contains("8'b00000001")); // K.y
    CHECK(e.contains("A=32'b00010000001000000011000001000000")); // the table, LUT[0] on the left
}

TEST_CASE("IR: a case listing every value of its selector with integer labels is complete", "[ir][case]") {
    auto e = extract("module t(input [1:0] s, input [3:0] a, b, c, d,\n"
                     "         output reg [3:0] y, output reg [3:0] x, output reg [3:0] v,\n"
                     "         output reg [3:0] n1, output reg [3:0] n2, output reg [3:0] n3);\n"
                     "  always @* case (s) 0: y = a; 1: y = b; 2: y = c; 3: y = d; endcase\n"
                     "  always @* case (s) 2'd0: x = a; 32'h1: x = b; 2: x = c; 3'd3: x = d; endcase\n"
                     "  always @* case ($signed(s)) -2: v = a; -1: v = b; 0: v = c; 1: v = d; endcase\n"
                     // negative: a value missing, a label the selector never reaches, signed too
                     "  always @* case (s) 0: n1 = a; 1: n1 = b; 2: n1 = c; endcase\n"
                     "  always @* case (s) 0: n2 = a; 1: n2 = b; 2: n2 = c; 4: n2 = d; endcase\n"
                     "  always @* case ($signed(s)) -2: n3 = a; -1: n3 = b; 0: n3 = c; 2: n3 = d; endcase\n"
                     "endmodule\n");
    CHECK_FALSE(e.contains("latch y "));
    CHECK_FALSE(e.contains("latch x "));
    CHECK_FALSE(e.contains("latch v "));
    CHECK(e.contains("latch n1 "));
    CHECK(e.contains("latch n2 "));
    CHECK(e.contains("latch n3 "));
    CHECK(e.countCode("IR-LATCH-INFERRED") == 3);
}

TEST_CASE("IR: a for loop whose condition also tests a flag set in its body", "[ir][loop]") {
    // Intel's altera_pll_reconfig_core: for (i = 0; (i < N) & !break_loop; i = i + 1)
    auto e = extract("module t(input clk, input [3:0] req, output reg [1:0] sel, output reg any);\n"
                     "  integer i; reg found;\n"
                     "  always @(posedge clk) begin\n"
                     "    found = 1'b0; sel <= 2'd0;\n"
                     "    for (i = 0; (i < 4) & !found; i = i + 1)\n"
                     "      if (req[i]) begin sel <= i[1:0]; found = 1'b1; end\n"
                     "    any <= found;\n"
                     "  end\nendmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-LOOP") == 0);
    // the lowest request wins: request 2 is only taken when 0 and 1 are not set
    CHECK(e.has("cell $and$7 [1] = and(A=$logic_not$1, B=$logic_not$6)"));
    CHECK(e.has("cell $mux$11 [2] = mux(A={1'b0, $mux$5[0]}, B=$mux$9, S=$and$7)"));
    CHECK(e.has("reg sel <= $mux$17 @(posedge clk)"));
}

TEST_CASE("IR negative: a for loop without a constant bound stays unsupported", "[ir][loop]") {
    auto e = extract("module t(input clk, input [3:0] req, output reg [1:0] sel);\n"
                     "  integer i; reg found;\n"
                     "  always @(posedge clk) begin\n"
                     "    found = 1'b0; sel <= 2'd0;\n"
                     "    for (i = 0; !found; i = i + 1) if (req[i]) begin sel <= i[1:0]; found = 1'b1; end\n"
                     "  end\nendmodule\n");
    CHECK(e.countCode("IR-UNSUPPORTED-LOOP") == 1);
}

TEST_CASE("IR: static variables declared in a block keep their value; automatic ones are re-initialized",
          "[ir][process]") {
    // MiSTer audio_out.sv: always @(posedge clk, posedge reset) begin reg [1:0] dly1 = 0; if (reset) ...
    auto e = extract("module t(input clk, input reset, input ce, output reg en, output reg [1:0] o);\n"
                     "  always @(posedge clk, posedge reset) begin\n"
                     "    reg [1:0] dly = 0;\n"
                     "    if (reset) begin dly <= 0; en <= 0; end\n"
                     "    else if (ce) begin if (~&dly) dly <= dly + 1'd1; else en <= 1; end\n"
                     "  end\n"
                     "  always @(posedge clk) begin\n"
                     "    automatic logic [1:0] t = 2'd1;\n"
                     "    t = t + 1'd1;\n"
                     "    o <= t;\n"
                     "  end\nendmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK(e.countCode("IR-AMBIGUOUS-CLOCK") == 0);
    CHECK(e.countCode("FE-IMPLICIT-STATIC") == 1); // 'reg dly = 0' without 'static': information
    CHECK_FALSE(e.hasErrors());
    CHECK(e.contains("reg dly <= $mux$"));
    CHECK(e.contains("arst(reset -> 2'b00) en(ce)"));
    CHECK(e.has("reg o <= 2'b10 @(posedge clk)"));
}

TEST_CASE("IR: a case whose selector has constant bits needs no item for the values it cannot take", "[ir][case]") {
    // NES_MiSTer Mapper162: case ({state[3][2], 1'b0, state[3][0]}) 0: 1: 4: 5:
    auto e = extract("module t(input [3:0] st, input [7:0] a, b, c, d, output logic [7:0] p, output logic [7:0] n);\n"
                     "  always_comb case ({st[2], 1'b0, st[0]}) 0: p = a; 1: p = b; 4: p = c; 5: p = d; endcase\n"
                     // negative: the middle bit is 1, the items never match
                     "  always_comb case ({st[2], 1'b1, st[0]}) 0: n = a; 1: n = b; 4: n = c; 5: n = d; endcase\n"
                     "endmodule\n",
                     LanguageStd::SystemVerilog2017);
    CHECK_FALSE(e.contains("latch p "));
    CHECK(e.contains("latch n "));
}

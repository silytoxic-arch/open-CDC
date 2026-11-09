#include "graph/DesignView.h"

#include "../DesignUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;
using namespace opencdc::graph;

namespace {

Driver driverOfRegisterD(const DesignView& v, std::string_view hierQ, std::uint32_t bit = 0) {
    auto [inst, r] = test::findRegister(v.design(), hierQ);
    const auto& reg = v.bodyOf(inst).registers[r];
    auto b = v.bitOf(inst, reg.d, bit);
    REQUIRE(b);
    return v.driver(*b);
}

Driver driverOfClock(const DesignView& v, std::string_view hierQ) {
    auto [inst, r] = test::findRegister(v.design(), hierQ);
    auto b = v.bitOf(inst, v.bodyOf(inst).registers[r].clock.signal, 0);
    REQUIRE(b);
    return v.driver(*b);
}

} // namespace

TEST_CASE("Driver lookup crosses ports up and down the hierarchy", "[graph]") {
    auto b = test::buildDesign("module src(input clk, output reg q); always @(posedge clk) q <= ~q; endmodule\n"
                               "module dst(input clk, input d, output reg q); always @(posedge clk) q <= d; endmodule\n"
                               "module top(input c1, input c2, output y);\n"
                               "  wire w1, w2;\n"
                               "  assign w2 = w1;\n"
                               "  src u_s (.clk(c1), .q(w1));\n"
                               "  dst u_d (.clk(c2), .d(w2), .q(y));\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    const Driver d = driverOfRegisterD(v, "top.u_d.q");
    CHECK(d.kind == DriverKind::Register);
    CHECK(v.name(d.bit) == "top.u_s.q");

    const Driver c = driverOfClock(v, "top.u_d.q");
    CHECK(c.kind == DriverKind::PrimaryInput);
    CHECK(v.name(c.bit) == "top.c2");
}

TEST_CASE("Driver lookup reports constants, cells and undriven nets", "[graph]") {
    auto b = test::buildDesign("module top(input clk, input [1:0] a, output reg q0, output reg q1, output reg q2);\n"
                               "  wire tie = 1'b1;\n"
                               "  wire floating;\n"
                               "  always @(posedge clk) begin q0 <= tie; q1 <= a[0] & a[1]; q2 <= floating; end\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    const Driver k = driverOfRegisterD(v, "top.q0");
    CHECK(k.kind == DriverKind::Constant);
    CHECK(k.constant == ir::Logic::L1);
    CHECK(driverOfRegisterD(v, "top.q1").kind == DriverKind::Cell);
    const Driver u = driverOfRegisterD(v, "top.q2");
    CHECK(u.kind == DriverKind::Undriven);
    CHECK(v.name(u.bit) == "top.floating");
}

TEST_CASE("Unconnected input port and bit-level aliasing", "[graph]") {
    auto b = test::buildDesign("module leaf(input [3:0] i, input en, output reg [3:0] q);\n"
                               "  always @(posedge en) q <= i;\n"
                               "endmodule\n"
                               "module top(input [7:0] bus, output [3:0] y);\n"
                               "  leaf u (.i(bus[7:4]), .en(), .q(y));\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    const Driver d = driverOfRegisterD(v, "top.u.q", 2);
    CHECK(d.kind == DriverKind::PrimaryInput);
    CHECK(v.name(d.bit) == "top.bus[6]");
    CHECK(driverOfClock(v, "top.u.q").kind == DriverKind::Undriven);
}

TEST_CASE("Alias loops terminate", "[graph]") {
    auto b = test::buildDesign("module top(input clk, output reg q);\n"
                               "  wire a, c;\n"
                               "  assign a = c;\n"
                               "  assign c = a;\n"
                               "  always @(posedge clk) q <= a;\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    CHECK(driverOfRegisterD(v, "top.q").kind == DriverKind::Loop);
}

TEST_CASE("The stop callback ends the walk at a given bit", "[graph]") {
    auto b = test::buildDesign("module top(input clk_in, input d, output reg q);\n"
                               "  wire clk_buf = clk_in;\n"
                               "  always @(posedge clk_buf) q <= d;\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    auto [inst, r] = test::findRegister(*b.design, "top.q");
    auto clk = v.bitOf(inst, v.bodyOf(inst).registers[r].clock.signal, 0);
    REQUIRE(clk);
    const Driver d = v.driver(*clk, [&](const BitRef& x) { return v.name(x) == "top.clk_buf"; });
    CHECK(d.kind == DriverKind::Undriven);
    CHECK(v.name(d.bit) == "top.clk_buf");
    CHECK(v.registerCount() == 1);
}

TEST_CASE("Driver runs: one walk for consecutive bits, the same answer as bit by bit", "[graph]") {
    // the CDC graph resolves the inputs of very wide cells (a RAM's read and write logic,
    // millions of bits) run by run
    auto b = test::buildDesign("module src(input clk, output reg [7:0] q); always @(posedge clk) q <= q + 8'd1; endmodule\n"
                               "module top(input clk, input [3:0] a, inout [1:0] pad, input oe, output reg [15:0] y, output reg [1:0] z);\n"
                               "  wire [7:0] w;\n"
                               "  src u (.clk(clk), .q(w));\n"
                               "  wire [15:0] mix = {4'b1010, a, w};\n"
                               "  assign pad = oe ? a[1:0] : 2'bzz;\n"
                               "  always @(posedge clk) begin y <= mix; z <= pad; end\n"
                               "endmodule\n",
                               "top");
    DesignView v(*b.design);
    auto [inst, r] = test::findRegister(v.design(), "top.y");
    const auto& reg = v.bodyOf(inst).registers[r];
    std::vector<std::uint32_t> lengths;
    for (std::uint32_t i = 0; i < 16;) {
        auto first = v.bitOf(inst, reg.d, i);
        REQUIRE(first);
        const auto [d, n] = v.driverRun(*first, 16 - i);
        REQUIRE(n >= 1);
        for (std::uint32_t j = 0; j < n; ++j) {
            const Driver one = v.driver(BitRef{first->inst, first->net, first->bit + j});
            CHECK(one.kind == d.kind);
            if (d.kind != DriverKind::Constant) {
                CHECK(one.index == d.index);
                CHECK(one.bit.inst == d.bit.inst);
                CHECK(one.bit.net == d.bit.net);
                CHECK(one.bit.bit == d.bit.bit + j);
            }
        }
        lengths.push_back(n);
        i += n;
    }
    // w (the register in u, through its output port), a (a primary input), the constant
    REQUIRE(lengths.size() == 3);
    CHECK(lengths[0] == 8);
    CHECK(lengths[1] == 4);
    CHECK(lengths[2] == 4);
    CHECK(v.driverRun(*v.bitOf(inst, reg.d, 0), 8).first.kind == DriverKind::Register);
    // the run never exceeds the length asked for
    CHECK(v.driverRun(*v.bitOf(inst, reg.d, 0), 3).second == 3);
    // negative: a net with a tristate pad is walked bit by bit (the logic reading it sees the pin)
    auto [pinst, pr] = test::findRegister(v.design(), "top.z");
    const auto pb = v.bitOf(pinst, v.bodyOf(pinst).registers[pr].d, 0);
    REQUIRE(pb);
    REQUIRE(v.isPad(v.driver(*pb).bit));
    const auto [pd, pn] = v.driverRun(*pb, 2);
    CHECK(pn == 1);
    CHECK(pd.kind == v.driver(*pb).kind);
}

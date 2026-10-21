#include "ir/ConstFold.h"
#include "ir/Design.h"
#include "ir/Dump.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;
using namespace opencdc::ir;

namespace {

Net makeNet(std::string name, std::uint32_t width, NetKind kind = NetKind::Wire, PortDir dir = PortDir::None) {
    Net n;
    n.name = std::move(name);
    n.width = width;
    n.msb = static_cast<std::int32_t>(width) - 1;
    n.lsb = 0;
    n.kind = kind;
    n.dir = dir;
    return n;
}

} // namespace

TEST_CASE("ConstBits conversions and formatting", "[ir][const]") {
    auto c = ConstBits::fromUint(0b1010, 4);
    CHECK(c.toString() == "4'b1010");
    CHECK(c.toUint() == 10u);
    CHECK(c.toInt() == -6);
    CHECK_FALSE(c.isZero());

    ConstBits x(3, Logic::X);
    x[0] = Logic::L1;
    x[2] = Logic::Z;
    CHECK(x.toString() == "3'bzx1");
    CHECK_FALSE(x.isFullyDefined());
    CHECK_FALSE(x.toUint().has_value());

    auto big = ConstBits::fromUint(0xAB, 72);
    CHECK(big.toString() == "72'h0000000000000000ab");
    CHECK(big.extract(4, 4).toUint() == 0xAu);
}

TEST_CASE("SigSpec normalizes, extracts and replaces", "[ir][sigspec]") {
    const NetId a{0};
    const NetId b{1};
    SigSpec s = SigSpec::net(a, 4);                 // a[3:0]
    s.append(SigSpec::net(b, 2, 2));                // b[3:2]
    s.append(SigSpec::constant(0b1, 1));
    s.append(SigSpec::constant(0b0, 1));
    REQUIRE(s.width() == 8);
    REQUIRE(s.chunks().size() == 3);                // the two constants merged
    CHECK(s.chunks()[2].constant.toString() == "2'b01");

    // Adjacent bits of the same net merge back into one chunk.
    SigSpec m = SigSpec::net(a, 2, 0);
    m.append(SigSpec::net(a, 2, 2));
    CHECK(m == SigSpec::net(a, 4));

    auto mid = s.extract(3, 3);                     // a[3], b[3:2]
    REQUIRE(mid.chunks().size() == 2);
    CHECK(mid.chunks()[0] == SigChunk{a, 3, 1, {}});
    CHECK(mid.chunks()[1] == SigChunk{b, 2, 2, {}});

    auto rep = s.replaced(0, SigSpec::constant(0xF, 4));
    CHECK(rep.extract(0, 4).asConst()->toUint() == 0xFu);
    CHECK(rep.extract(4, 4) == s.extract(4, 4));

    CHECK(SigSpec::net(a, 1).resized(3, true) == SigSpec::net(a, 1).repeated(3));
    CHECK(SigSpec::net(a, 4).resized(6, false).extract(4, 2).asConst()->isZero());
    CHECK(SigSpec::net(a, 4).resized(2, false) == SigSpec::net(a, 2));
    CHECK(s.references(b));
    CHECK_FALSE(SigSpec::constant(1, 1).references(a));
    CHECK(SigSpec::net(a, 4).onlyReferences(a));
}

TEST_CASE("Constant folding of arithmetic, comparison and logic", "[ir][fold]") {
    auto fold = [](CellKind k, std::uint64_t a, std::uint64_t b, std::uint32_t w, std::uint32_t outW,
                   bool isSigned = false) {
        Cell c;
        c.kind = k;
        c.width = outW;
        c.isSigned = isSigned;
        c.inputs = {SigSpec::constant(a, w), SigSpec::constant(b, w)};
        auto r = simplifyCell(c);
        REQUIRE(r.has_value());
        REQUIRE(r->asConst());
        return *r->asConst()->toUint();
    };
    CHECK(fold(CellKind::Add, 15, 1, 4, 4) == 0);   // wraps
    CHECK(fold(CellKind::Sub, 0, 1, 4, 4) == 15);
    CHECK(fold(CellKind::Mul, 3, 5, 8, 8) == 15);
    CHECK(fold(CellKind::Shl, 1, 3, 8, 8) == 8);
    CHECK(fold(CellKind::Lt, 3, 5, 4, 1) == 1);
    CHECK(fold(CellKind::Lt, 0xF, 1, 4, 1) == 0);               // unsigned 15 < 1
    CHECK(fold(CellKind::Lt, 0xF, 1, 4, 1, true) == 1);         // signed -1 < 1
    CHECK(fold(CellKind::Eq, 7, 7, 3, 1) == 1);
    CHECK(fold(CellKind::LogicAnd, 2, 0, 2, 1) == 0);
    CHECK(fold(CellKind::Xnor, 0b1100, 0b1010, 4, 4) == 0b1001);

    Cell div;
    div.kind = CellKind::Div;
    div.width = 4;
    div.inputs = {SigSpec::constant(5, 4), SigSpec::constant(0, 4)};
    CHECK_FALSE(simplifyCell(div)->asConst()->isFullyDefined()); // x/0 = x

    Cell red;
    red.kind = CellKind::ReduceXor;
    red.width = 1;
    red.inputs = {SigSpec::constant(0b0111, 4)};
    CHECK(simplifyCell(red)->asConst()->toUint() == 1u);
}

TEST_CASE("Mux and dynamic select simplification", "[ir][fold]") {
    const NetId a{0};
    const NetId b{1};
    const NetId s{2};
    Cell mux;
    mux.kind = CellKind::Mux;
    mux.width = 4;
    mux.inputs = {SigSpec::net(a, 4), SigSpec::net(b, 4), SigSpec::constant(1, 1)};
    CHECK(simplifyCell(mux) == SigSpec::net(b, 4));
    mux.inputs[2] = SigSpec::constant(0, 1);
    CHECK(simplifyCell(mux) == SigSpec::net(a, 4));
    mux.inputs[2] = SigSpec::net(s, 1);
    CHECK_FALSE(simplifyCell(mux).has_value());
    mux.inputs[1] = SigSpec::net(a, 4);                  // A == B
    CHECK(simplifyCell(mux) == SigSpec::net(a, 4));

    // a[7:0] viewed as 4 elements of 2 bits, declared [3:0]; select element 2.
    Cell sel;
    sel.kind = CellKind::DynSelect;
    sel.width = 2;
    sel.elemWidth = 2;
    sel.rangeLeft = 3;
    sel.rangeRight = 0;
    sel.inputs = {SigSpec::net(a, 8), SigSpec::constant(2, 2)};
    CHECK(simplifyCell(sel) == SigSpec::net(a, 2, 4));
    // Ascending range [0:3]: element 0 is the most significant.
    sel.rangeLeft = 0;
    sel.rangeRight = 3;
    sel.inputs[1] = SigSpec::constant(0, 2);
    CHECK(simplifyCell(sel) == SigSpec::net(a, 2, 6));

    Cell ins;
    ins.kind = CellKind::DynInsert;
    ins.width = 8;
    ins.elemWidth = 2;
    ins.rangeLeft = 3;
    ins.rangeRight = 0;
    ins.inputs = {SigSpec::net(a, 8), SigSpec::constant(1, 2), SigSpec::net(b, 2)};
    CHECK(simplifyCell(ins) == SigSpec::net(a, 8).replaced(2, SigSpec::net(b, 2)));

    CHECK(elementOffset(5, 7, 0) == 5u);
    CHECK(elementOffset(5, 0, 7) == 2u);
    CHECK_FALSE(elementOffset(8, 7, 0).has_value());
}

TEST_CASE("Design flattening offsets and instance tree", "[ir][design]") {
    Design d;
    ModuleBody leaf;
    leaf.name = "leaf";
    const NetId lclk = leaf.addNet(makeNet("clk", 1, NetKind::Wire, PortDir::In));
    const NetId lq = leaf.addNet(makeNet("q", 2, NetKind::Variable, PortDir::Out));
    leaf.ports = {{"clk", PortDir::In, lclk}, {"q", PortDir::Out, lq}};
    Register r;
    r.q = leaf.netSig(lq);
    r.d = SigSpec::constant(0, 2);
    r.clock = {leaf.netSig(lclk), Edge::Pos};
    leaf.registers.push_back(r);
    const ModuleId leafId = d.addBody(std::move(leaf));

    ModuleBody top;
    top.name = "top";
    const NetId tclk = top.addNet(makeNet("clk", 1, NetKind::Wire, PortDir::In));
    const NetId tw = top.addNet(makeNet("w", 4));
    top.children.push_back({"u0", leafId, "leaf", {{"clk", PortDir::In, lclk, top.netSig(tclk)}, {"q", PortDir::Out, lq, SigSpec::net(tw, 2)}}, {}});
    top.children.push_back({"g[1].u1", leafId, "leaf", {{"clk", PortDir::In, lclk, top.netSig(tclk)}, {"q", PortDir::Out, lq, SigSpec::net(tw, 2, 2)}}, {}});
    top.children.push_back({"u_bb", ModuleId{}, "missing", {}, {}});
    const ModuleId topId = d.addBody(std::move(top));

    d.addTop(topId);
    REQUIRE(d.instances.size() == 3); // unresolved child has no node
    const auto& t = d.instance(d.tops.at(0));
    CHECK(t.hierPath == "top");
    REQUIRE(t.children.size() == 2);
    const auto& u1 = d.instance(t.children[1]);
    CHECK(u1.hierPath == "top.g[1].u1");
    CHECK(u1.childIndex == 1);
    CHECK(d.findInstance("top.u0").has_value());

    // Flattened Id spaces do not overlap.
    const auto& u0 = d.instance(t.children[0]);
    CHECK(u0.netBase != u1.netBase);
    CHECK((u0.registerBase + 1 == u1.registerBase || u1.registerBase + 1 == u0.registerBase));

    auto st = d.stats();
    CHECK(st.instances == 3);
    CHECK(st.uniqueBodies == 2);
    CHECK(st.registers == 2);
    CHECK(st.registerBits == 4);
    CHECK(st.nets == 2 + 2 + 2);
}

TEST_CASE("Dump renders declared bit indices and all object kinds", "[ir][dump]") {
    ModuleBody b;
    b.name = "m";
    b.params = "#(W=4)";
    Net down = makeNet("a", 8, NetKind::Wire, PortDir::In);
    down.msb = 8;
    down.lsb = 1; // a[8:1]
    const NetId a = b.addNet(down);
    Net up = makeNet("c", 4, NetKind::Variable);
    up.msb = 0;
    up.lsb = 3;   // c[0:3]
    up.attrs.push_back({"ASYNC_REG", "\"TRUE\""});
    const NetId c = b.addNet(up);
    const NetId clk = b.addNet(makeNet("clk", 1, NetKind::Wire, PortDir::In));

    Cell add;
    add.kind = CellKind::Add;
    add.width = 4;
    add.inputs = {SigSpec::net(a, 4, 4), SigSpec::constant(1, 4)};
    const SigSpec sum = b.addCell(add);

    b.connections.push_back({SigSpec::net(c, 2, 0), SigSpec::net(a, 2, 0), ConnKind::Assign, {}});
    Register r;
    r.q = b.netSig(c);
    r.d = sum;
    r.clock = {b.netSig(clk), Edge::Neg};
    r.asyncResets.push_back({b.netSig(clk), false, SigSpec::constant(0, 4)});
    b.registers.push_back(r);

    const auto text = dumpBody(b);
    INFO(text);
    CHECK(text.find("module m#(W=4)") != std::string::npos);
    CHECK(text.find("input wire [8:1] a") != std::string::npos);
    CHECK(text.find("var [0:3] c (* ASYNC_REG = \"TRUE\" *)") != std::string::npos);
    CHECK(text.find("cell $add$0 [4] = add(A=a[8:5], B=4'b0001)") != std::string::npos);
    CHECK(text.find("assign c[2:3] = a[2:1]") != std::string::npos);
    CHECK(text.find("reg c <= $add$0 @(negedge clk) arst(!clk -> 4'b0000)") != std::string::npos);
    CHECK(formatSig(b, [&] {
              SigSpec s = SigSpec::net(a, 1);
              s.append(SigSpec::constant(2, 2));
              return s;
          }()) == "{2'b10, a[1]}");
}

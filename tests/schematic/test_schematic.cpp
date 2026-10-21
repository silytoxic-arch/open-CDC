// Stage 6: CDC schematic model and layered layout (Qt-free).

#include "schematic/Schematic.h"

#include "../cdc/CdcTestUtil.h"

#include <cmath>

using namespace opencdc;
using namespace opencdc::cdc;
using namespace opencdc::schematic;
using namespace opencdc::test::cdc;

namespace {

bool near(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

/// Layout invariants: real nodes do not overlap, edges start and end on
/// their nodes' borders, everything is inside the drawing area.
void checkLayout(const Schematic& s) {
    std::vector<const Node*> real;
    for (const auto& n : s.nodes)
        if (n.kind != NodeKind::Dummy)
            real.push_back(&n);
    for (std::size_t i = 0; i < real.size(); ++i) {
        const Node& a = *real[i];
        CHECK(a.w > 0);
        CHECK(a.h > 0);
        CHECK(a.x >= 0);
        CHECK(a.y >= 0);
        CHECK(a.x + a.w <= s.width + 1e-6);
        CHECK(a.y + a.h <= s.height + 1e-6);
        for (std::size_t j = i + 1; j < real.size(); ++j) {
            const Node& b = *real[j];
            const bool apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
            INFO(a.name << " / " << b.name);
            CHECK(apart);
        }
    }
    for (const auto& e : s.edges) {
        REQUIRE(e.points.size() >= 2);
        const Node& f = s.nodes[e.from];
        const Node& t = s.nodes[e.to];
        const auto& p0 = e.points.front();
        const auto& p1 = e.points.back();
        // Forward edges leave on the right and enter on the left; reversed
        // (cycle) edges the other way round.
        const bool forward = near(p0.x, f.x + f.w) && near(p1.x, t.x);
        const bool backward = near(p0.x, f.x) && near(p1.x, t.x + t.w);
        CHECK((forward || backward));
        CHECK((p0.y > f.y && p0.y < f.y + f.h));
        CHECK((p1.y > t.y && p1.y < t.y + t.h));
    }
}

const Node& nodeNamed(const Schematic& s, std::string_view name) {
    const Node* n = s.find(name);
    if (!n) {
        for (const auto& m : s.nodes)
            UNSCOPED_INFO("node " << m.name);
        FAIL("no node " << name);
    }
    return *n;
}

bool hasEdge(const Schematic& s, std::string_view from, std::string_view to, EdgeKind kind) {
    const Node* f = s.find(from);
    const Node* t = s.find(to);
    return f && t && std::ranges::any_of(s.edges, [&](const Edge& e) {
               return e.from == f->id && e.to == t->id && e.kind == kind;
           });
}

} // namespace

TEST_CASE("Schematic of a two-flop synchronizer", "[schematic]") {
    auto r = analyze("module top(input clk_a, input clk_b, input d, output y);\n"
                     "  reg a, s1, s2;\n"
                     "  always @(posedge clk_a) a <= d;\n"
                     "  always @(posedge clk_b) begin s1 <= a; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n");
    auto s = buildForPath(*r.cdc, r.path("top.a", "top.s1").id);
    REQUIRE(s.nodes.size() == 3);
    CHECK(nodeNamed(s, "top.a").roles == kSource);
    CHECK(nodeNamed(s, "top.a").clock == "clk_a");
    CHECK(nodeNamed(s, "top.s1").roles == kDestination);
    CHECK(nodeNamed(s, "top.s2").roles == kSyncStage);
    CHECK(nodeNamed(s, "top.s2").clock == "clk_b");
    REQUIRE(hasEdge(s, "top.a", "top.s1", EdgeKind::Path));
    CHECK(hasEdge(s, "top.s1", "top.s2", EdgeKind::Chain));
    const auto& crossing = *std::ranges::find_if(s.edges, [](const Edge& e) { return e.kind == EdgeKind::Path; });
    CHECK(crossing.crossesClocks);
    CHECK(s.title.find("clk_a") != std::string::npos);
    CHECK(nodeNamed(s, "top.s1").detail.find("clock: clk_b") != std::string::npos);

    layout(s);
    checkLayout(s);
    CHECK(nodeNamed(s, "top.a").layer == 0);
    CHECK(nodeNamed(s, "top.s1").layer == 1);
    CHECK(nodeNamed(s, "top.s2").layer == 2);
    CHECK(nodeNamed(s, "top.a").x < nodeNamed(s, "top.s1").x);
    CHECK(countCrossings(s) == 0);
    // A single edge per side uses the middle of the box.
    const auto& e0 = s.edges.front();
    CHECK(near(e0.points.front().y, nodeNamed(s, "top.a").y + nodeNamed(s, "top.a").h / 2));
}

TEST_CASE("Schematic merges every source of a destination's logic", "[schematic]") {
    auto r = analyze("module top(input clk_a, input clk_b, input clk_c, input x, input z, output y);\n"
                     "  reg a, c, s1, s2;\n"
                     "  always @(posedge clk_a) a <= x;\n"
                     "  always @(posedge clk_c) c <= z;\n"
                     "  always @(posedge clk_b) begin s1 <= a | c; s2 <= s1; end\n"
                     "  assign y = s2;\n"
                     "endmodule\n",
                     "set_clock_groups -asynchronous -group clk_a -group clk_b -group clk_c\n");
    auto s = buildForPath(*r.cdc, r.path("top.a", "top.s1").id);
    CHECK(s.paths.size() == 2); // a -> s1 and c -> s1
    std::size_t logic = 0;
    for (const auto& n : s.nodes)
        logic += n.kind == NodeKind::Logic ? 1U : 0U;
    REQUIRE(logic == 1);
    const auto& gate = *std::ranges::find(s.nodes, NodeKind::Logic, &Node::kind);
    CHECK(gate.label == "or");
    CHECK(std::ranges::count(s.edges, gate.id, &Edge::to) == 2);
    // The two inputs of the gate enter at different ports.
    CHECK(nodeNamed(s, "top.c").clock == "clk_c");
    layout(s);
    checkLayout(s);
    {
        std::vector<double> ports;
        for (const auto& e : s.edges)
            if (e.to == gate.id)
                ports.push_back(e.points.back().y);
        REQUIRE(ports.size() == 2);
        CHECK_FALSE(near(ports[0], ports[1]));
    }
    CHECK(nodeNamed(s, "top.a").layer == nodeNamed(s, "top.c").layer);
    CHECK(gate.layer < nodeNamed(s, "top.s1").layer);
}

TEST_CASE("Schematic of a qualified data path shows the qualifier", "[schematic]") {
    auto r = analyze("module top(input clk_a, input clk_b, input [7:0] d, input v, output reg [7:0] q);\n"
                     "  reg [7:0] data; reg valid, s1, s2;\n"
                     "  always @(posedge clk_a) begin data <= d; valid <= v; end\n"
                     "  always @(posedge clk_b) begin s1 <= valid; s2 <= s1; if (s2) q <= data; end\n"
                     "endmodule\n");
    auto s = buildForPath(*r.cdc, r.path("top.data", "top.q").id);
    CHECK((nodeNamed(s, "top.s2").roles & kQualifier) != 0);
    CHECK(hasEdge(s, "top.s2", "top.q", EdgeKind::Qualifier));
    layout(s);
    checkLayout(s);
}

TEST_CASE("Schematic of a reconvergence shows the meeting register", "[schematic][CDC-005]") {
    auto r = analyze("module top(input clk_a, input clk_b, input x, input z, output reg y);\n"
                     "  reg a1, a2, sa1, sa2, sb1, sb2;\n"
                     "  always @(posedge clk_a) begin a1 <= x; a2 <= z; end\n"
                     "  always @(posedge clk_b) begin sa1 <= a1; sa2 <= sa1; sb1 <= a2; sb2 <= sb1; y <= sa2 & sb2; "
                     "end\n"
                     "endmodule\n");
    auto s = buildForPath(*r.cdc, r.path("top.a1", "top.sa1").id);
    const Node& y = nodeNamed(s, "top.y");
    CHECK((y.roles & kReconvergence) != 0);
    CHECK(y.status == AnalysisStatus::PotentialRisk);
    CHECK(hasEdge(s, "top.sa2", "top.y", EdgeKind::Reconvergence));
    layout(s);
    checkLayout(s);
}

TEST_CASE("Layout breaks cycles and routes long edges through dummies", "[schematic][layout]") {
    Schematic s;
    for (const char* name : {"a", "b", "c", "d"}) {
        Node n;
        n.id = s.nodes.size();
        n.name = name;
        n.label = name;
        s.nodes.push_back(n);
    }
    auto edge = [&](std::size_t f, std::size_t t) {
        Edge e;
        e.from = f;
        e.to = t;
        s.edges.push_back(e);
    };
    edge(0, 1);
    edge(1, 2);
    edge(2, 0); // cycle
    edge(0, 3);
    edge(2, 3); // long edge from a's layer: a -> d spans layers
    layout(s);
    checkLayout(s);
    CHECK(s.nodes[0].layer < s.nodes[1].layer);
    CHECK(s.nodes[1].layer < s.nodes[2].layer);
    const auto dummies = std::ranges::count(s.nodes, NodeKind::Dummy, &Node::kind);
    CHECK(dummies >= 1);
    // The reversed edge c -> a still starts at c and ends at a.
    const Edge& back = s.edges[2];
    CHECK(near(back.points.front().x, s.nodes[2].x));
    CHECK(near(back.points.back().x, s.nodes[0].x + s.nodes[0].w));
}

TEST_CASE("Schematics of every example path are well formed and deterministic", "[schematic][example]") {
    struct Example {
        const char* dir;
        std::vector<const char*> files;
        const char* top;
        const char* sdc;
    };
    const std::vector<Example> examples = {
        {"cdc_advanced",
         {"rtl/sync_2ff.v", "rtl/sync_vec.v", "rtl/rst_sync.v", "rtl/pulse_sync.v", "rtl/gray_fifo.v",
          "rtl/handshake.v", "rtl/cdc_advanced.v"},
         "cdc_advanced",
         "constraints/cdc_advanced.sdc"},
        {"cdc_rules", {"rtl/cdc_rules.v"}, "cdc_rules", "constraints/cdc_rules.sdc"}};
    for (const auto& ex : examples) {
        const char* dir = ex.dir;
        auto r = analyzeExample(ex.dir, ex.files, ex.top, ex.sdc);
        for (const auto& p : r.cdc->paths) {
            INFO(dir << " path #" << p.id);
            auto s = buildForPath(*r.cdc, p.id);
            REQUIRE_FALSE(s.nodes.empty());
            CHECK(std::ranges::find(s.paths, p.id) != s.paths.end());
            CHECK(s.find(p.srcName));
            CHECK(s.find(p.dstName));
            layout(s);
            checkLayout(s);
            auto again = buildForPath(*r.cdc, p.id);
            layout(again);
            REQUIRE(again.nodes.size() == s.nodes.size());
            for (std::size_t i = 0; i < s.nodes.size(); ++i) {
                CHECK(again.nodes[i].x == s.nodes[i].x);
                CHECK(again.nodes[i].y == s.nodes[i].y);
            }
        }
    }
}

TEST_CASE("Schematic of an asynchronous FIFO draws the whole structure", "[schematic][fifo]") {
    auto r = analyzeExample("cdc_advanced",
                            {"rtl/sync_2ff.v", "rtl/sync_vec.v", "rtl/rst_sync.v", "rtl/pulse_sync.v",
                             "rtl/gray_fifo.v", "rtl/handshake.v", "rtl/cdc_advanced.v"},
                            "cdc_advanced", "constraints/cdc_advanced.sdc");
    const auto& mem = r.path("cdc_advanced.u_fifo.mem", "cdc_advanced.u_fifo.rdata");
    auto s = buildForPath(*r.cdc, mem.id);
    CHECK(s.paths.size() == 3);
    CHECK(s.title.find("async-fifo structure") != std::string::npos);
    CHECK(nodeNamed(s, "cdc_advanced.u_fifo.mem").kind == NodeKind::Memory);
    CHECK(s.find("cdc_advanced.u_fifo.wptr"));
    CHECK(s.find("cdc_advanced.u_fifo.rptr"));
    CHECK(hasEdge(s, "cdc_advanced.u_fifo.u_w2r.s2", "cdc_advanced.u_fifo.rdata", EdgeKind::Qualifier));
    // Equal short names in different instances are told apart.
    CHECK(nodeNamed(s, "cdc_advanced.u_fifo.u_w2r.s1").label == "u_w2r.s1");
    CHECK(nodeNamed(s, "cdc_advanced.u_fifo.u_r2w.s1").label == "u_r2w.s1");
    CHECK(nodeNamed(s, "cdc_advanced.u_fifo.mem").label == "mem");
    layout(s);
    checkLayout(s);
}

TEST_CASE("SVG output draws every node and edge", "[schematic][svg]") {
    auto r = analyzeExample("cdc_advanced",
                            {"rtl/sync_2ff.v", "rtl/sync_vec.v", "rtl/rst_sync.v", "rtl/pulse_sync.v",
                             "rtl/gray_fifo.v", "rtl/handshake.v", "rtl/cdc_advanced.v"},
                            "cdc_advanced", "constraints/cdc_advanced.sdc");
    auto s = buildForPath(*r.cdc, r.path("cdc_advanced.a_x", "cdc_advanced.u_mix.meta").id);
    layout(s);
    const std::string svg = toSvg(s, *r.cdc);
    auto count = [&](std::string_view needle) {
        std::size_t n = 0;
        for (auto pos = svg.find(needle); pos != std::string::npos; pos = svg.find(needle, pos + 1))
            ++n;
        return n;
    };
    const auto real = static_cast<std::size_t>(
        std::ranges::count_if(s.nodes, [](const Node& n) { return n.kind != NodeKind::Dummy; }));
    CHECK(svg.starts_with("<svg "));
    CHECK(svg.ends_with("</svg>\n"));
    CHECK(count("<g class=\"node\">") == real);
    CHECK(count("class=\"edge\"") == s.edges.size());
    CHECK(count("<g") == count("</g>"));
    CHECK(svg.find(">a_x<") != std::string::npos);
    CHECK(svg.find("#d02020") != std::string::npos); // violation color (CDC-004)
    CHECK(svg.find("&lt;") == std::string::npos);    // nothing needed escaping here...
    // ...but names with markup characters are escaped.
    s.nodes.front().label = "a<b&c";
    const std::string escaped = toSvg(s, *r.cdc);
    CHECK(escaped.find("a&lt;b&amp;c") != std::string::npos);
}

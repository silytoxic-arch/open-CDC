// Layered (Sugiyama-style) layout of a CDC schematic, left to right.

#include "schematic/Schematic.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace opencdc::schematic {

namespace {

struct Segment {
    std::size_t from = 0; ///< node (real or dummy), in layout direction (left to right)
    std::size_t to = 0;
};

double labelWidth(const Node& n, const LayoutOptions& o) {
    if (n.kind == NodeKind::Dummy)
        return 0.0;
    const double text = static_cast<double>(n.label.size()) * o.charWidth + 28.0;
    return std::max(n.kind == NodeKind::Logic ? 48.0 : o.minNodeWidth, text);
}

} // namespace

void layout(Schematic& s, const LayoutOptions& o) {
    const std::size_t real = s.nodes.size();
    if (real == 0) {
        s.width = s.height = 0;
        return;
    }

    // --- 1. Cycle breaking: DFS in node order; back edges are reversed. ---
    std::vector<std::vector<std::size_t>> out(real);
    for (std::size_t e = 0; e < s.edges.size(); ++e)
        out[s.edges[e].from].push_back(e);
    std::vector<bool> reversed(s.edges.size(), false);
    std::vector<int> state(real, 0); // 0 new, 1 on stack, 2 done
    for (std::size_t root = 0; root < real; ++root) {
        if (state[root] != 0)
            continue;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
        state[root] = 1;
        while (!stack.empty()) {
            auto& [n, next] = stack.back();
            if (next < out[n].size()) {
                const std::size_t e = out[n][next++];
                const std::size_t t = s.edges[e].to;
                if (state[t] == 1)
                    reversed[e] = true;
                else if (state[t] == 0) {
                    state[t] = 1;
                    stack.emplace_back(t, 0);
                }
            }
            else {
                state[n] = 2;
                stack.pop_back();
            }
        }
    }
    auto tail = [&](std::size_t e) { return reversed[e] ? s.edges[e].to : s.edges[e].from; };
    auto head = [&](std::size_t e) { return reversed[e] ? s.edges[e].from : s.edges[e].to; };

    // --- 2. Longest-path layering, then sources pulled next to their readers. ---
    std::vector<std::vector<std::size_t>> succ(real), pred(real);
    for (std::size_t e = 0; e < s.edges.size(); ++e) {
        if (tail(e) == head(e))
            continue;
        succ[tail(e)].push_back(head(e));
        pred[head(e)].push_back(tail(e));
    }
    std::vector<int> indeg(real, 0);
    for (std::size_t n = 0; n < real; ++n)
        indeg[n] = static_cast<int>(pred[n].size());
    std::vector<std::size_t> topo;
    for (std::size_t n = 0; n < real; ++n)
        if (indeg[n] == 0)
            topo.push_back(n);
    for (std::size_t i = 0; i < topo.size(); ++i)
        for (const auto t : succ[topo[i]])
            if (--indeg[t] == 0)
                topo.push_back(t);
    std::vector<int> layer(real, 0);
    for (const auto n : topo)
        for (const auto t : succ[n])
            layer[t] = std::max(layer[t], layer[n] + 1);
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        const auto n = *it;
        if (!pred[n].empty() || succ[n].empty())
            continue;
        int minSucc = layer[succ[n].front()];
        for (const auto t : succ[n])
            minSucc = std::min(minSucc, layer[t]);
        layer[n] = std::max(layer[n], minSucc - 1);
    }
    int layers = 0;
    for (std::size_t n = 0; n < real; ++n) {
        s.nodes[n].layer = layer[n];
        layers = std::max(layers, layer[n] + 1);
    }

    // --- 3. Dummy nodes on edges spanning several layers. ---
    std::vector<Segment> segments;
    std::vector<std::vector<std::size_t>> routeNodes(s.edges.size()); // per edge, in layout direction
    for (std::size_t e = 0; e < s.edges.size(); ++e) {
        std::size_t prev = tail(e);
        routeNodes[e].push_back(prev);
        for (int l = s.nodes[tail(e)].layer + 1; l < s.nodes[head(e)].layer; ++l) {
            Node d;
            d.id = s.nodes.size();
            d.kind = NodeKind::Dummy;
            d.layer = l;
            s.nodes.push_back(std::move(d));
            segments.push_back({prev, s.nodes.size() - 1});
            prev = s.nodes.size() - 1;
            routeNodes[e].push_back(prev);
        }
        if (prev != head(e))
            segments.push_back({prev, head(e)});
        routeNodes[e].push_back(head(e));
    }

    // --- 4. Ordering: barycenter sweeps. ---
    std::vector<std::vector<std::size_t>> byLayer(static_cast<std::size_t>(layers));
    for (const auto& n : s.nodes)
        byLayer[static_cast<std::size_t>(n.layer)].push_back(n.id);
    std::vector<std::vector<std::size_t>> up(s.nodes.size()), down(s.nodes.size());
    for (const auto& sg : segments) {
        down[sg.from].push_back(sg.to);
        up[sg.to].push_back(sg.from);
    }
    auto setOrder = [&](std::size_t l) {
        for (std::size_t i = 0; i < byLayer[l].size(); ++i)
            s.nodes[byLayer[l][i]].order = static_cast<int>(i);
    };
    for (std::size_t l = 0; l < byLayer.size(); ++l)
        setOrder(l);
    auto sweep = [&](std::size_t l, const std::vector<std::vector<std::size_t>>& neighbours) {
        std::vector<std::pair<double, std::size_t>> keyed;
        for (const auto n : byLayer[l]) {
            const auto& nb = neighbours[n];
            double key = s.nodes[n].order;
            if (!nb.empty()) {
                double sum = 0;
                for (const auto m : nb)
                    sum += s.nodes[m].order;
                key = sum / static_cast<double>(nb.size());
            }
            keyed.emplace_back(key, n);
        }
        std::ranges::stable_sort(keyed, {}, &std::pair<double, std::size_t>::first);
        for (std::size_t i = 0; i < keyed.size(); ++i)
            byLayer[l][i] = keyed[i].second;
        setOrder(l);
    };
    for (int it = 0; it < o.orderingSweeps; ++it) {
        if (it % 2 == 0)
            for (std::size_t l = 1; l < byLayer.size(); ++l)
                sweep(l, up);
        else
            for (std::size_t l = byLayer.size() - 1; l-- > 0;)
                sweep(l, down);
    }

    // --- 5. Coordinates: layers as columns, nodes stacked and centered. ---
    for (auto& n : s.nodes) {
        n.w = labelWidth(n, o);
        n.h = n.kind == NodeKind::Dummy ? 0.0 : o.nodeHeight;
    }
    std::vector<double> colWidth(byLayer.size(), 0.0), colHeight(byLayer.size(), 0.0);
    for (std::size_t l = 0; l < byLayer.size(); ++l) {
        for (const auto n : byLayer[l]) {
            colWidth[l] = std::max(colWidth[l], s.nodes[n].w);
            colHeight[l] += (s.nodes[n].kind == NodeKind::Dummy ? o.nodeGap * 0.5 : s.nodes[n].h + o.nodeGap);
        }
        colWidth[l] = std::max(colWidth[l], 16.0);
    }
    const double maxHeight = *std::ranges::max_element(colHeight);
    double x = o.layerGap * 0.5;
    for (std::size_t l = 0; l < byLayer.size(); ++l) {
        double y = (maxHeight - colHeight[l]) * 0.5 + o.nodeGap * 0.5;
        for (const auto n : byLayer[l]) {
            Node& node = s.nodes[n];
            node.x = x + (colWidth[l] - node.w) * 0.5;
            node.y = y;
            y += node.kind == NodeKind::Dummy ? o.nodeGap * 0.5 : node.h + o.nodeGap;
        }
        x += colWidth[l] + o.layerGap;
    }
    s.width = x - o.layerGap * 0.5;
    s.height = maxHeight + o.nodeGap * 0.5;

    // --- 6. Edge routes through the dummy nodes, in the edge's direction. ---
    // Several edges at one side of a node get separate ports, ordered by the
    // position of the node at their other end (fewer crossings at the box).
    std::vector<std::vector<std::size_t>> inPorts(real), outPorts(real); // edges, in layout direction
    for (std::size_t e = 0; e < s.edges.size(); ++e) {
        const auto& rn = routeNodes[e];
        outPorts[rn.front()].push_back(e);
        inPorts[rn.back()].push_back(e);
    }
    auto otherY = [&](std::size_t e, bool atStart) {
        const auto& rn = routeNodes[e];
        const Node& n = s.nodes[atStart ? rn[1] : rn[rn.size() - 2]];
        return n.y + n.h * 0.5;
    };
    std::vector<double> startY(s.edges.size()), endY(s.edges.size());
    auto assign = [&](std::vector<std::size_t>& edges, std::size_t node, bool atStart, std::vector<double>& ports) {
        std::ranges::stable_sort(edges, {}, [&](std::size_t e) { return otherY(e, atStart); });
        const Node& n = s.nodes[node];
        for (std::size_t i = 0; i < edges.size(); ++i)
            ports[edges[i]] = n.y + n.h * static_cast<double>(i + 1) / static_cast<double>(edges.size() + 1);
    };
    for (std::size_t n = 0; n < real; ++n) {
        assign(outPorts[n], n, true, startY);
        assign(inPorts[n], n, false, endY);
    }
    for (std::size_t e = 0; e < s.edges.size(); ++e) {
        std::vector<Point> pts;
        const auto& rn = routeNodes[e];
        for (std::size_t i = 0; i < rn.size(); ++i) {
            const Node& n = s.nodes[rn[i]];
            if (n.kind == NodeKind::Dummy)
                pts.push_back({n.x, n.y + n.h * 0.5});
            else if (i == 0)
                pts.push_back({n.x + n.w, startY[e]}); // leaves on the right
            else
                pts.push_back({n.x, endY[e]});         // enters on the left
        }
        if (reversed[e])
            std::ranges::reverse(pts);
        s.edges[e].points = std::move(pts);
    }
}

std::size_t countCrossings(const Schematic& s) {
    // Segments between adjacent layers from the routes.
    struct Seg {
        int layer;
        double y0, y1;
    };
    std::vector<Seg> segs;
    for (const auto& e : s.edges) {
        for (std::size_t i = 0; i + 1 < e.points.size(); ++i) {
            auto a = e.points[i];
            auto b = e.points[i + 1];
            if (a.x > b.x)
                std::swap(a, b);
            segs.push_back({static_cast<int>(std::lround(a.x)), a.y, b.y});
        }
    }
    std::size_t n = 0;
    for (std::size_t i = 0; i < segs.size(); ++i)
        for (std::size_t j = i + 1; j < segs.size(); ++j)
            if (segs[i].layer == segs[j].layer &&
                ((segs[i].y0 < segs[j].y0 && segs[i].y1 > segs[j].y1) ||
                 (segs[i].y0 > segs[j].y0 && segs[i].y1 < segs[j].y1)))
                ++n;
    return n;
}

} // namespace opencdc::schematic

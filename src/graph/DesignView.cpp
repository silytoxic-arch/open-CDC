#include "graph/DesignView.h"

#include <algorithm>

namespace opencdc::graph {

using namespace opencdc::ir;

const char* toString(DriverKind k) noexcept {
    switch (k) {
        case DriverKind::Undriven: return "undriven";
        case DriverKind::Constant: return "constant";
        case DriverKind::PrimaryInput: return "primary input";
        case DriverKind::Register: return "register";
        case DriverKind::Latch: return "latch";
        case DriverKind::Cell: return "combinational cell";
        case DriverKind::UnresolvedInstance: return "unresolved instance";
        case DriverKind::Loop: return "alias loop";
    }
    return "?";
}

DesignView::DesignView(const Design& design) : design_(design) {
    bodies_.resize(design.bodies.size());
    for (std::size_t b = 0; b < design.bodies.size(); ++b) {
        const ModuleBody& body = design.bodies[b];
        auto& idx = bodies_[b].drivers;
        idx.resize(body.nets.size());

        auto addSig = [&](const SigSpec& s, EntryKind kind, std::uint32_t index, std::uint32_t sub) {
            std::uint32_t pos = 0;
            for (const auto& c : s.chunks()) {
                if (!c.isConst())
                    idx[c.net.index()].push_back({c.offset, c.offset + c.width, kind, index, sub, pos, 0});
                pos += c.width;
            }
        };
        for (std::size_t i = 0; i < body.connections.size(); ++i)
            addSig(body.connections[i].lhs, EntryKind::Conn, static_cast<std::uint32_t>(i), 0);
        for (std::size_t i = 0; i < body.registers.size(); ++i)
            addSig(body.registers[i].q, EntryKind::Reg, static_cast<std::uint32_t>(i), 0);
        for (std::size_t i = 0; i < body.latches.size(); ++i)
            addSig(body.latches[i].q, EntryKind::Latch, static_cast<std::uint32_t>(i), 0);
        for (std::size_t i = 0; i < body.cells.size(); ++i)
            addSig(body.netSig(body.cells[i].output), EntryKind::Cell, static_cast<std::uint32_t>(i), 0);
        for (const auto& p : body.ports)
            if (p.dir == PortDir::In || p.dir == PortDir::InOut)
                addSig(body.netSig(p.net), EntryKind::InPort, 0, 0);
        for (std::size_t c = 0; c < body.children.size(); ++c) {
            const auto& ch = body.children[c];
            for (std::size_t p = 0; p < ch.ports.size(); ++p)
                if (ch.ports[p].dir == PortDir::Out)
                    addSig(ch.ports[p].actual, EntryKind::ChildOut, static_cast<std::uint32_t>(c),
                           static_cast<std::uint32_t>(p));
        }
        for (auto& v : idx) {
            std::ranges::sort(v, {}, &Entry::lo);
            std::uint32_t running = 0;
            for (auto& e : v) {
                running = std::max(running, e.hi);
                e.maxHi = running;
            }
        }
    }

    children_.resize(design.instances.size());
    for (std::size_t i = 0; i < design.instances.size(); ++i) {
        const auto& node = design.instances[i];
        children_[i].assign(design.body(node.body).children.size(), InstanceId{});
        totalRegisters_ += design.body(node.body).registers.size();
    }
    latchBase_.resize(design.instances.size());
    for (std::size_t i = 0; i < design.instances.size(); ++i) {
        latchBase_[i] = totalLatches_;
        totalLatches_ += design.body(design.instances[i].body).latches.size();
    }
    for (std::size_t i = 0; i < design.instances.size(); ++i) {
        const auto& node = design.instances[i];
        if (node.parent.valid())
            children_[node.parent.index()][node.childIndex] = InstanceId{static_cast<std::uint32_t>(i)};
    }
}

const DesignView::Entry* DesignView::find(const BodyIndex& idx, NetId net, std::uint32_t bit) const {
    const auto& v = idx.drivers.at(net.index());
    if (v.size() <= 4) {
        for (const auto& e : v)
            if (bit >= e.lo && bit < e.hi)
                return &e;
        return nullptr;
    }
    // Wide nets (e.g. wire arrays connecting thousands of instances) have many
    // driver ranges: binary search for the last range starting at or before
    // `bit`, then walk back while an earlier range can still cover it.
    auto it = std::upper_bound(v.begin(), v.end(), bit, [](std::uint32_t b, const Entry& e) { return b < e.lo; });
    while (it != v.begin()) {
        --it;
        if (bit < it->hi)
            return &*it;
        if (it->maxHi <= bit)
            break;
    }
    return nullptr;
}

InstanceId DesignView::child(InstanceId parent, std::size_t childIndex) const {
    const auto& v = children_.at(parent.index());
    return childIndex < v.size() ? v[childIndex] : InstanceId{};
}

Logic DesignView::constBit(const SigSpec& sig, std::uint32_t index) {
    std::uint32_t pos = 0;
    for (const auto& c : sig.chunks()) {
        if (index < pos + c.width)
            return c.isConst() ? c.constant[index - pos] : Logic::X;
        pos += c.width;
    }
    return Logic::X;
}

std::optional<BitRef> DesignView::bitOf(InstanceId inst, const SigSpec& sig, std::uint32_t index) const {
    std::uint32_t pos = 0;
    for (const auto& c : sig.chunks()) {
        if (index < pos + c.width) {
            if (c.isConst())
                return std::nullopt;
            return BitRef{inst, c.net, c.offset + (index - pos)};
        }
        pos += c.width;
    }
    return std::nullopt;
}

Driver DesignView::driver(const BitRef& start, const std::function<bool(const BitRef&)>& stop,
                          std::vector<BitRef>* hops) const {
    BitRef cur = start;
    for (int steps = 0; steps < 100000; ++steps) {
        if (hops)
            hops->push_back(cur);
        if (stop && stop(cur))
            return {DriverKind::Undriven, cur, 0, Logic::X};

        const auto& node = design_.instance(cur.inst);
        const ModuleBody& body = design_.body(node.body);
        const Entry* e = find(bodies_[node.body.index()], cur.net, cur.bit);
        if (!e)
            return {DriverKind::Undriven, cur, 0, Logic::X};
        const std::uint32_t pos = e->pos + (cur.bit - e->lo);

        switch (e->kind) {
            case EntryKind::Reg: return {DriverKind::Register, cur, e->index, Logic::X};
            case EntryKind::Latch: return {DriverKind::Latch, cur, e->index, Logic::X};
            case EntryKind::Cell: return {DriverKind::Cell, cur, e->index, Logic::X};
            case EntryKind::Conn: {
                const SigSpec& rhs = body.connections[e->index].rhs;
                if (pos >= rhs.width())
                    return {DriverKind::Undriven, cur, 0, Logic::X};
                auto next = bitOf(cur.inst, rhs, pos);
                if (!next)
                    return {DriverKind::Constant, cur, 0, constBit(rhs, pos)};
                cur = *next;
                continue;
            }
            case EntryKind::InPort: {
                if (!node.parent.valid())
                    return {DriverKind::PrimaryInput, cur, 0, Logic::X};
                const ModuleBody& parentBody = design_.body(design_.instance(node.parent).body);
                const auto& ci = parentBody.children.at(node.childIndex);
                const PortBinding* pb = nullptr;
                for (const auto& p : ci.ports)
                    if (p.childNet == cur.net)
                        pb = &p;
                if (!pb || cur.bit >= pb->actual.width())
                    return {DriverKind::Undriven, cur, 0, Logic::X}; // unconnected input
                auto next = bitOf(node.parent, pb->actual, cur.bit);
                if (!next)
                    return {DriverKind::Constant, cur, 0, constBit(pb->actual, cur.bit)};
                cur = *next;
                continue;
            }
            case EntryKind::ChildOut: {
                const InstanceId childInst = child(cur.inst, e->index);
                if (!childInst.valid())
                    return {DriverKind::UnresolvedInstance, cur, e->index, Logic::X};
                const auto& pb = body.children[e->index].ports[e->sub];
                if (!pb.childNet.valid() || pos >= bodyOf(childInst).net(pb.childNet).width)
                    return {DriverKind::Undriven, cur, 0, Logic::X};
                cur = BitRef{childInst, pb.childNet, pos};
                continue;
            }
        }
    }
    return {DriverKind::Loop, cur, 0, Logic::X};
}

std::string DesignView::name(const BitRef& b) const {
    const auto& node = design_.instance(b.inst);
    const Net& n = design_.body(node.body).net(b.net);
    std::string out = node.hierPath + "." + n.name;
    if (n.arrayDepth > 0)
        out += "{" + std::to_string(b.bit) + "}";
    else if (n.width > 1 || n.msb != n.lsb)
        out += "[" + std::to_string(n.indexOf(b.bit)) + "]";
    return out;
}

} // namespace opencdc::graph

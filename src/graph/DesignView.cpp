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

namespace {
std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> padKey(InstanceId inst, NetId net, std::uint32_t bit) {
    return {static_cast<std::uint32_t>(inst.index()), static_cast<std::uint32_t>(net.index()), bit};
}
} // namespace

DesignView::DesignView(const Design& design) : design_(design) {
    bodies_.resize(design.bodies.size());
    // inout ports: driven from inside where the body (or a child, through its own
    // inout port) drives the bit, from outside otherwise (resolved below)
    struct InoutBinding {
        std::size_t body, child, port;
    };
    std::vector<InoutBinding> inoutBindings;
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
            if (p.dir == PortDir::In)
                addSig(body.netSig(p.net), EntryKind::InPort, 0, 0);
        for (std::size_t c = 0; c < body.children.size(); ++c) {
            const auto& ch = body.children[c];
            for (std::size_t p = 0; p < ch.ports.size(); ++p) {
                if (ch.ports[p].dir == PortDir::Out)
                    addSig(ch.ports[p].actual, EntryKind::ChildOut, static_cast<std::uint32_t>(c),
                           static_cast<std::uint32_t>(p));
                else if (ch.ports[p].dir == PortDir::InOut && ch.body.valid() && ch.ports[p].childNet.valid())
                    inoutBindings.push_back({b, c, p});
            }
        }
        // A Verilog instance of an unknown module (a black box) has ports of unknown
        // direction: the bits connected to one that nothing else drives are its
        // outputs (corundum: the PCIe IP's user_clk, the transceivers' user clocks)
        for (std::size_t c = 0; c < body.children.size(); ++c) {
            const auto& ch = body.children[c];
            if (ch.body.valid())
                continue;
            for (std::size_t p = 0; p < ch.ports.size(); ++p) {
                if (ch.ports[p].dir != PortDir::None)
                    continue;
                std::uint32_t pos = 0;
                for (const auto& k : ch.ports[p].actual.chunks()) {
                    if (!k.isConst()) {
                        auto& list = idx[k.net.index()];
                        std::vector<bool> covered(k.width, false);
                        for (const auto& e : list)
                            for (std::uint32_t i = std::max(e.lo, k.offset); i < std::min(e.hi, k.offset + k.width); ++i)
                                covered[i - k.offset] = true;
                        for (std::uint32_t i = 0; i < k.width;) {
                            if (covered[i]) {
                                ++i;
                                continue;
                            }
                            std::uint32_t j = i;
                            while (j < k.width && !covered[j])
                                ++j;
                            list.push_back({k.offset + i, k.offset + j, EntryKind::ChildOut, static_cast<std::uint32_t>(c),
                                            static_cast<std::uint32_t>(p), pos + i, 0});
                            i = j;
                        }
                    }
                    pos += k.width;
                }
            }
        }
    }
    // is bit `bit` of `net` in body `b` driven from inside (no InPort entry for inout ports yet)?
    const auto drivenInside = [&](std::size_t b, NetId net, std::uint32_t bit) {
        for (const auto& e : bodies_[b].drivers[net.index()])
            if (bit >= e.lo && bit < e.hi)
                return true;
        return false;
    };
    // a child's inout port drives the parent's bits that the child drives inside; up the
    // hierarchy until nothing changes (one level per round)
    std::vector<std::vector<bool>> added(inoutBindings.size());
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t k = 0; k < inoutBindings.size(); ++k) {
            const auto& ib = inoutBindings[k];
            const auto& ch = design.bodies[ib.body].children[ib.child];
            const auto& pb = ch.ports[ib.port];
            auto& done = added[k];
            done.resize(pb.actual.width(), false);
            std::uint32_t pos = 0;
            for (const auto& c : pb.actual.chunks()) {
                for (std::uint32_t i = 0; i < c.width; ++i, ++pos) {
                    if (c.isConst() || done[pos] || !drivenInside(ch.body.index(), pb.childNet, pos))
                        continue;
                    done[pos] = true;
                    changed = true;
                    bodies_[ib.body].drivers[c.net.index()].push_back({c.offset + i, c.offset + i + 1,
                                                                       EntryKind::ChildOut,
                                                                       static_cast<std::uint32_t>(ib.child),
                                                                       static_cast<std::uint32_t>(ib.port), pos, 0});
                }
            }
        }
    }
    // the other bits of inout ports come from outside
    for (std::size_t b = 0; b < design.bodies.size(); ++b) {
        const ModuleBody& body = design.bodies[b];
        for (const auto& p : body.ports) {
            if (p.dir != PortDir::InOut)
                continue;
            const auto width = body.net(p.net).width;
            std::vector<Entry> outside;
            for (std::uint32_t bit = 0; bit < width; ++bit) {
                if (drivenInside(b, p.net, bit))
                    continue;
                if (!outside.empty() && outside.back().hi == bit)
                    ++outside.back().hi;
                else
                    outside.push_back({bit, bit + 1, EntryKind::InPort, 0, 0, bit, 0});
            }
            auto& v = bodies_[b].drivers[p.net.index()];
            v.insert(v.end(), outside.begin(), outside.end());
        }
    }
    for (auto& body : bodies_)
        for (auto& v : body.drivers) {
            std::ranges::sort(v, {}, &Entry::lo);
            std::uint32_t running = 0;
            for (auto& e : v) {
                running = std::max(running, e.hi);
                e.maxHi = running;
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
    // tristate pads: bits of a top instance's inout ports assigned 'z or (oe ? d : 'z)
    for (std::size_t i = 0; i < design.instances.size(); ++i) {
        const auto& node = design.instances[i];
        if (node.parent.valid())
            continue;
        const InstanceId inst{static_cast<std::uint32_t>(i)};
        const ModuleBody& body = design.body(node.body);
        for (const auto& p : body.ports) {
            if (p.dir != PortDir::InOut)
                continue;
            for (std::uint32_t bit = 0; bit < body.net(p.net).width; ++bit) {
                const Entry* e = find(bodies_[node.body.index()], p.net, bit);
                if (!e || e->kind != EntryKind::Conn)
                    continue;
                const SigSpec& rhs = body.connections[e->index].rhs;
                const std::uint32_t pos = e->pos + (bit - e->lo);
                if (pos >= rhs.width())
                    continue;
                const auto inner = bitOf(inst, rhs, pos);
                bool tristate = !inner && constBit(rhs, pos) == Logic::Z;
                if (inner) {
                    const Entry* ce = find(bodies_[node.body.index()], inner->net, inner->bit);
                    if (ce && ce->kind == EntryKind::Cell && body.cells[ce->index].kind == CellKind::Mux) {
                        const auto& cell = body.cells[ce->index];
                        const std::uint32_t cpos = ce->pos + (inner->bit - ce->lo);
                        for (std::size_t k = 0; k < 2 && k < cell.inputs.size(); ++k)
                            if (cpos < cell.inputs[k].width() && !bitOf(inst, cell.inputs[k], cpos) &&
                                constBit(cell.inputs[k], cpos) == Logic::Z)
                                tristate = true;
                    }
                }
                if (tristate)
                    pads_.emplace(padKey(inst, p.net, bit),
                                  inner ? *inner : BitRef{inst, p.net, bit});
            }
        }
    }
}

bool DesignView::isPad(const BitRef& b) const {
    return pads_.contains(padKey(b.inst, b.net, b.bit));
}

std::optional<BitRef> DesignView::padInternal(const BitRef& b) const {
    const auto it = pads_.find(padKey(b.inst, b.net, b.bit));
    if (it == pads_.end() || (it->second.inst == b.inst && it->second.net == b.net && it->second.bit == b.bit))
        return std::nullopt; // not a pad, or driven by a constant 'z only
    return it->second;
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
        // a tristate pad: the logic reading it sees the pin
        if (!pads_.empty() && isPad(cur))
            return {DriverKind::PrimaryInput, cur, 0, Logic::X};

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

std::pair<Driver, std::uint32_t> DesignView::driverRun(const BitRef& start, std::uint32_t maxLen) const {
    if (maxLen <= 1)
        return {driver(start), 1};
    // a net with a tristate pad (a top-level inout): bit by bit, as driver() checks pads
    auto padNet = [&](const BitRef& b) {
        if (pads_.empty())
            return false;
        const auto it = pads_.lower_bound(padKey(b.inst, b.net, 0));
        return it != pads_.end() && std::get<0>(it->first) == static_cast<std::uint32_t>(b.inst.index()) &&
               std::get<1>(it->first) == static_cast<std::uint32_t>(b.net.index());
    };
    // bits left in the chunk of `sig` that holds bit `index` (and whether it is constant)
    auto chunkLeft = [](const SigSpec& sig, std::uint32_t index) -> std::pair<std::uint32_t, bool> {
        std::uint32_t pos = 0;
        for (const auto& c : sig.chunks()) {
            if (index < pos + c.width)
                return {pos + c.width - index, c.isConst()};
            pos += c.width;
        }
        return {1, true};
    };
    std::uint32_t run = maxLen;
    BitRef cur = start;
    Driver d{DriverKind::Loop, cur, 0, Logic::X};
    for (int steps = 0; steps < 100000; ++steps) {
        if (padNet(cur))
            return {driver(start), 1};
        const auto& node = design_.instance(cur.inst);
        const ModuleBody& body = design_.body(node.body);
        const Entry* e = find(bodies_[node.body.index()], cur.net, cur.bit);
        if (!e) {
            d = {DriverKind::Undriven, cur, 0, Logic::X};
            run = 1; // the next bits may have a driver
            break;
        }
        run = std::min(run, e->hi - cur.bit);
        const std::uint32_t pos = e->pos + (cur.bit - e->lo);
        if (e->kind == EntryKind::Reg || e->kind == EntryKind::Latch || e->kind == EntryKind::Cell) {
            d = {e->kind == EntryKind::Reg     ? DriverKind::Register
                 : e->kind == EntryKind::Latch ? DriverKind::Latch
                                               : DriverKind::Cell,
                 cur, e->index, Logic::X};
            break;
        }
        const SigSpec* via = nullptr;
        std::uint32_t at = 0;
        InstanceId viaInst = cur.inst;
        if (e->kind == EntryKind::Conn) {
            via = &body.connections[e->index].rhs;
            at = pos;
        }
        else if (e->kind == EntryKind::InPort) {
            if (!node.parent.valid()) {
                d = {DriverKind::PrimaryInput, cur, 0, Logic::X};
                break;
            }
            const ModuleBody& parentBody = design_.body(design_.instance(node.parent).body);
            const auto& ci = parentBody.children.at(node.childIndex);
            for (const auto& p : ci.ports)
                if (p.childNet == cur.net)
                    via = &p.actual;
            at = cur.bit;
            viaInst = node.parent;
        }
        else { // ChildOut
            const InstanceId childInst = child(cur.inst, e->index);
            if (!childInst.valid()) {
                d = {DriverKind::UnresolvedInstance, cur, e->index, Logic::X};
                break;
            }
            const auto& pb = body.children[e->index].ports[e->sub];
            if (!pb.childNet.valid() || pos >= bodyOf(childInst).net(pb.childNet).width) {
                d = {DriverKind::Undriven, cur, 0, Logic::X};
                run = 1;
                break;
            }
            run = std::min(run, bodyOf(childInst).net(pb.childNet).width - pos);
            cur = BitRef{childInst, pb.childNet, pos};
            continue;
        }
        if (!via || at >= via->width()) {
            d = {DriverKind::Undriven, cur, 0, Logic::X};
            run = 1;
            break;
        }
        const auto [left, isConst] = chunkLeft(*via, at);
        run = std::min(run, left);
        if (isConst) {
            d = {DriverKind::Constant, cur, 0, constBit(*via, at)};
            break;
        }
        cur = *bitOf(viaInst, *via, at);
    }
    if (d.kind == DriverKind::Loop)
        return {driver(start), 1};
    // the walk follows the first bit; overlapping entries (several drivers of one bit)
    // could send a later bit elsewhere: the last bit must agree, otherwise one bit
    if (run > 1) {
        const Driver last = driver(BitRef{start.inst, start.net, start.bit + run - 1});
        const bool same = last.kind == d.kind && last.index == d.index && last.bit.inst == d.bit.inst &&
                          (d.kind == DriverKind::Constant || last.bit.net == d.bit.net) &&
                          (d.kind == DriverKind::Constant || last.bit.bit == d.bit.bit + run - 1);
        if (!same)
            return {driver(start), 1};
    }
    return {d, run};
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

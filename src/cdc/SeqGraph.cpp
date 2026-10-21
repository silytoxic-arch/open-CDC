#include "cdc/SeqGraph.h"

#include "ir/Dump.h"

#include <algorithm>
#include <unordered_set>

namespace opencdc::cdc {

using graph::BitRef;
using graph::Driver;
using graph::DriverKind;
using namespace opencdc::ir;

namespace {

std::size_t hashEntries(const std::vector<ConeEntry>& v) {
    std::size_t h = v.size();
    for (const auto& e : v) {
        h ^= std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(e.node) << 32) ^ (static_cast<std::uint64_t>(e.lo) << 16) ^
                                        e.hi ^ (static_cast<std::uint64_t>(e.flags) << 60)) +
             0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

bool sameEntries(std::span<const ConeEntry> a, const std::vector<ConeEntry>& b) {
    return std::ranges::equal(a, b, [](const ConeEntry& x, const ConeEntry& y) {
        return x.node == y.node && x.lo == y.lo && x.hi == y.hi && x.flags == y.flags;
    });
}

bool isPerBit(CellKind k) {
    return bitDependency(k) == BitDep::Bitwise || k == CellKind::DynInsert;
}

} // namespace

SeqGraph::SeqGraph(const graph::DesignView& view, DiagnosticSink& sink) : view_(view), sink_(sink) {
    offsets_ = {0, 0}; // id 0 = empty set
    nextInBucket_ = {0};
    const auto& d = view.design();

    // Register nodes first (node id == global register index), then latches.
    nodes_.resize(view.registerCount());
    regLocal_.resize(view.registerCount());
    latchNode_.resize(view.latchCount());
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (std::size_t r = 0; r < body.registers.size(); ++r) {
            const auto& reg = body.registers[r];
            const auto g = view.globalRegister(inst, r);
            const auto& ch = reg.q.chunks().front();
            SourceNode n;
            n.kind = SourceKind::Register;
            n.index = g;
            n.first = BitRef{inst, ch.net, ch.offset};
            n.width = ch.width;
            if (const Net& net = body.net(ch.net); net.arrayDepth > 0)
                n.elemWidth = net.elemWidth;
            n.name = node.hierPath + "." + formatSig(body, reg.q);
            n.loc = reg.origin;
            nodes_[g] = std::move(n);
            regLocal_[g] = {inst, r};
        }
    }
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (std::size_t l = 0; l < body.latches.size(); ++l) {
            const auto& lat = body.latches[l];
            const auto& ch = lat.q.chunks().front();
            SourceNode n;
            n.kind = SourceKind::Latch;
            n.index = view.globalLatch(inst, l);
            n.first = BitRef{inst, ch.net, ch.offset};
            n.width = ch.width;
            n.name = node.hierPath + "." + formatSig(body, lat.q);
            n.loc = lat.origin;
            latchNode_[n.index] = static_cast<std::uint32_t>(nodes_.size());
            nodes_.push_back(std::move(n));
        }
    }

    // Register input pins.
    pins_.resize(view.registerCount());
    for (std::size_t g = 0; g < regLocal_.size(); ++g) {
        const auto [inst, r] = regLocal_[g];
        const auto& reg = view.bodyOf(inst).registers[r];
        PinRange& pr = pins_[g];
        auto add = [&](const SigSpec& sig, std::uint32_t index) {
            BitInput bi;
            if (auto b = view.bitOf(inst, sig, index))
                bi = resolve(view.driver(*b));
            bits_.push_back(bi);
        };
        pr.data = static_cast<std::uint32_t>(bits_.size());
        pr.dataLen = reg.d.width();
        for (std::uint32_t i = 0; i < reg.d.width(); ++i)
            add(reg.d, i);
        pr.en = static_cast<std::uint32_t>(bits_.size());
        if (reg.enable) {
            pr.enLen = 1;
            add(*reg.enable, 0);
        }
        pr.srst = static_cast<std::uint32_t>(bits_.size());
        if (reg.syncReset) {
            pr.srstLen = 1;
            add(reg.syncReset->signal, 0);
        }
        pr.arst = static_cast<std::uint32_t>(bits_.size());
        pr.arstLen = static_cast<std::uint32_t>(reg.asyncResets.size());
        for (const auto& a : reg.asyncResets)
            add(a.signal, 0);
    }

    if (combLoops_ > 0)
        sink_.report({"GR-COMB-LOOP", Severity::Warning,
                      std::to_string(combLoops_) +
                          " combinational loop(s) found; the cones of the affected cells are cut at the loop",
                      {}, {}});
    memo_.clear(); // only needed while building
}

std::span<const BitInput> SeqGraph::pinBits(std::size_t globalReg, Pin pin) const {
    const PinRange& pr = pins_.at(globalReg);
    switch (pin) {
        case Pin::Data: return {bits_.data() + pr.data, pr.dataLen};
        case Pin::Enable: return {bits_.data() + pr.en, pr.enLen};
        case Pin::SyncReset: return {bits_.data() + pr.srst, pr.srstLen};
        case Pin::AsyncReset: return {bits_.data() + pr.arst, pr.arstLen};
    }
    return {};
}

std::uint32_t SeqGraph::nodeFor(const Driver& d) {
    const BitRef key{d.bit.inst, d.bit.net, 0};
    if (auto it = extraNodes_.find(key); it != extraNodes_.end())
        return it->second;
    const Net& net = view_.bodyOf(key.inst).net(key.net);
    SourceNode n;
    n.kind = d.kind == DriverKind::PrimaryInput ? SourceKind::PrimaryInput : SourceKind::BlackBox;
    n.first = key;
    n.width = net.width;
    n.name = view_.design().instance(key.inst).hierPath + "." + net.name;
    n.loc = net.loc;
    const auto id = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(std::move(n));
    extraNodes_.emplace(key, id);
    return id;
}

void SeqGraph::accumulate(std::vector<ConeEntry>& acc, ConeEntry e) {
    // Few distinct nodes per cone in practice: merge in place, no sorting.
    if (acc.size() <= 16) {
        for (auto& a : acc) {
            if (a.node == e.node) {
                a.lo = std::min(a.lo, e.lo);
                a.hi = std::max(a.hi, e.hi);
                a.flags |= e.flags;
                return;
            }
        }
    }
    acc.push_back(e);
}

std::uint32_t SeqGraph::intern(std::vector<ConeEntry>& entries) {
    if (entries.empty())
        return 0;
    std::ranges::sort(entries, {}, &ConeEntry::node);
    std::size_t w = 0;
    for (std::size_t r = 0; r < entries.size(); ++r) {
        if (w > 0 && entries[w - 1].node == entries[r].node) {
            auto& m = entries[w - 1];
            m.lo = std::min(m.lo, entries[r].lo);
            m.hi = std::max(m.hi, entries[r].hi);
            m.flags |= entries[r].flags;
        }
        else {
            entries[w++] = entries[r];
        }
    }
    entries.resize(w);
    const std::size_t h = hashEntries(entries);
    auto [it, inserted] = coneIndex_.try_emplace(h, 0);
    if (!inserted) {
        for (std::uint32_t id = it->second; id != 0; id = nextInBucket_[id])
            if (sameEntries(cone(id), entries))
                return id;
    }
    const auto id = static_cast<std::uint32_t>(offsets_.size() - 1);
    pool_.insert(pool_.end(), entries.begin(), entries.end());
    offsets_.push_back(static_cast<std::uint32_t>(pool_.size()));
    nextInBucket_.push_back(inserted ? 0 : it->second);
    it->second = id;
    return id;
}

SeqGraph::CellKey SeqGraph::keyFor(InstanceId inst, std::size_t cell, std::uint32_t bit) const {
    const auto kind = view_.bodyOf(inst).cells[cell].kind;
    return {inst.value, static_cast<std::uint32_t>(cell), isPerBit(kind) ? bit : kAll};
}

std::vector<SeqGraph::CellInput> SeqGraph::cellInputs(InstanceId inst, std::size_t cellIndex, std::uint32_t outBit) const {
    const Cell& c = view_.bodyOf(inst).cells[cellIndex];
    std::vector<CellInput> out;
    out.reserve(c.inputs.size() * 2);
    auto bitOfInput = [&](std::size_t input, std::uint32_t bit, std::uint8_t flags) {
        if (input >= c.inputs.size() || bit >= c.inputs[input].width())
            return;
        if (auto b = view_.bitOf(inst, c.inputs[input], bit))
            out.push_back({*b, flags});
    };
    auto allOfInput = [&](std::size_t input, std::uint8_t flags) {
        if (input >= c.inputs.size())
            return;
        for (std::uint32_t i = 0; i < c.inputs[input].width(); ++i)
            bitOfInput(input, i, flags);
    };
    constexpr std::uint8_t L = ConeEntry::kLogic;
    constexpr std::uint8_t S = ConeEntry::kLogic | ConeEntry::kSelect;
    switch (c.kind) {
        case CellKind::Not: bitOfInput(0, outBit, 0); break;
        case CellKind::And:
        case CellKind::Or:
        case CellKind::Xor:
        case CellKind::Xnor:
            bitOfInput(0, outBit, L);
            bitOfInput(1, outBit, L);
            break;
        case CellKind::Mux:
            bitOfInput(0, outBit, L);
            bitOfInput(1, outBit, L);
            allOfInput(2, S);
            break;
        case CellKind::DynInsert:
            bitOfInput(0, outBit, L);
            allOfInput(2, L);
            allOfInput(1, S);
            break;
        case CellKind::DynSelect:
            allOfInput(0, L);
            allOfInput(1, S);
            break;
        default:
            for (std::size_t i = 0; i < c.inputs.size(); ++i)
                allOfInput(i, L);
            break;
    }
    return out;
}

BitInput SeqGraph::resolve(const Driver& d) {
    BitInput bi;
    auto direct = [&](std::uint32_t node, std::uint32_t bit) {
        bi.cone = BitInput::kDirectCone;
        bi.directNode = node;
        bi.directBit = bit;
        return bi;
    };
    switch (d.kind) {
        case DriverKind::Register: {
            const auto g = view_.globalRegister(d.bit.inst, d.index);
            return direct(registerNode(g), d.bit.bit - nodes_[g].first.bit);
        }
        case DriverKind::Latch: {
            const auto node = latchNode_.at(view_.globalLatch(d.bit.inst, d.index));
            return direct(node, d.bit.bit - nodes_[node].first.bit);
        }
        case DriverKind::PrimaryInput:
        case DriverKind::UnresolvedInstance: return direct(nodeFor(d), d.bit.bit);
        case DriverKind::Cell: return computeCell(keyFor(d.bit.inst, d.index, d.bit.bit));
        default: return bi; // constant, undriven, alias loop: no source
    }
}

BitInput SeqGraph::computeCell(const CellKey& root) {
    if (auto it = memo_.find(root); it != memo_.end() && it->second.state == 2)
        return it->second.value;

    // Iterative post-order DFS over cells (deep logic must not overflow the
    // stack). Each cell's inputs and their drivers are resolved once.
    std::vector<CellKey> stack{root};
    while (!stack.empty()) {
        const CellKey k = stack.back();
        Memo& m = memo_[k];
        if (m.state == 2) {
            stack.pop_back();
            continue;
        }
        if (m.state == 0) {
            m.state = 1;
            std::vector<ResolvedInput> inputs;
            bool pushed = false;
            for (const auto& ci : cellInputs(InstanceId{k.inst}, k.cell, k.bit == kAll ? 0 : k.bit)) {
                const Driver d = view_.driver(ci.bit);
                if (d.kind == DriverKind::Cell) {
                    const CellKey dk = keyFor(d.bit.inst, d.index, d.bit.bit);
                    if (memo_[dk].state == 0) { // may rehash: `m` is not used below
                        stack.push_back(dk);
                        pushed = true;
                    }
                }
                inputs.push_back({ci, d});
            }
            if (pushed) {
                pending_.emplace(k, std::move(inputs));
                continue;
            }
            const BitInput v = combine(k, inputs);
            Memo& done = memo_[k];
            done.value = v;
            done.state = 2;
            stack.pop_back();
            continue;
        }
        // Revisit after the dependencies: combine with the stored inputs.
        auto it = pending_.find(k);
        const BitInput v = combine(k, it->second);
        pending_.erase(it);
        Memo& done = memo_[k];
        done.value = v;
        done.state = 2;
        stack.pop_back();
    }
    return memo_[root].value;
}

BitInput SeqGraph::combine(const CellKey& k, const std::vector<ResolvedInput>& inputs) {
    auto valueOf = [&](const Driver& d) -> BitInput {
        if (d.kind != DriverKind::Cell)
            return resolve(d);
        const Memo& m = memo_[keyFor(d.bit.inst, d.index, d.bit.bit)];
        if (m.state != 2) {
            ++combLoops_; // back edge: the dependency is still being computed
            return {};
        }
        return m.value;
    };

    const Cell& c = view_.bodyOf(InstanceId{k.inst}).cells[k.cell];
    if (c.kind == CellKind::Not) {
        if (inputs.empty())
            return {};
        BitInput v = valueOf(inputs.front().driver);
        v.directInverted = !v.directInverted; // an inverter keeps a direct connection
        return v;
    }
    std::vector<ConeEntry> acc;
    ConeEntry scratch;
    for (const auto& ri : inputs) {
        const BitInput v = valueOf(ri.driver);
        for (auto e : entries(v, scratch)) {
            e.flags |= ri.in.flags;
            accumulate(acc, e);
        }
    }
    BitInput out;
    out.cone = intern(acc);
    return out;
}

BitInput SeqGraph::inputOf(const graph::BitRef& b) {
    return resolve(view_.driver(b));
}

std::unordered_map<std::uint32_t, std::vector<Consumer>>
SeqGraph::consumersOf(const std::vector<std::uint32_t>& wanted) const {
    std::unordered_map<std::uint32_t, std::vector<Consumer>> out;
    if (wanted.empty())
        return out;
    const std::unordered_set<std::uint32_t> set(wanted.begin(), wanted.end());
    for (std::size_t g = 0; g < pins_.size(); ++g) {
        for (const Pin pin : {Pin::Data, Pin::Enable, Pin::SyncReset, Pin::AsyncReset}) {
            const auto bits = pinBits(g, pin);
            ConeEntry scratch;
            for (std::uint32_t i = 0; i < bits.size(); ++i) {
                const BitInput& bi = bits[i];
                for (const auto& e : entries(bi, scratch)) {
                    if (!set.contains(e.node))
                        continue;
                    const bool direct = bi.directNode == e.node;
                    auto& list = out[e.node];
                    if (!direct && !list.empty()) {
                        Consumer& last = list.back();
                        if (last.reg == g && last.pin == pin && !last.direct && last.bitEnd + 1 == i &&
                            last.lo == e.lo && last.hi == e.hi && last.flags == e.flags) {
                            last.bitEnd = i;
                            continue;
                        }
                    }
                    Consumer c;
                    c.reg = g;
                    c.bit = i;
                    c.bitEnd = i;
                    c.pin = pin;
                    c.lo = e.lo;
                    c.hi = e.hi;
                    c.flags = e.flags;
                    c.direct = direct;
                    c.directBit = bi.directBit;
                    list.push_back(c);
                }
            }
        }
    }
    return out;
}

} // namespace opencdc::cdc

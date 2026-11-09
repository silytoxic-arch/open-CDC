#include "cdc/SeqGraph.h"

#include "ir/Dump.h"

#include <algorithm>
#include <map>
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
    neverLoads_.assign(view.registerCount(), false);
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
        // The data input chunk by chunk, run by run: a RAM's array register has
        // millions of bits (corundum: 11.6M register bits). Bits of a run fed by one
        // wide cell share its cone; other drivers give each bit directly.
        for (const auto& ch : reg.d.chunks()) {
            if (ch.isConst()) {
                bits_.insert(bits_.end(), ch.width, BitInput{});
                continue;
            }
            for (std::uint32_t at = 0; at < ch.width;) {
                const BitRef first{inst, ch.net, ch.offset + at};
                const auto [d0, n] = view.driverRun(first, ch.width - at);
                if (d0.kind == DriverKind::Cell && keyFor(d0.bit.inst, d0.index, d0.bit.bit).bit == kAll) {
                    bits_.insert(bits_.end(), n, resolve(d0));
                }
                else {
                    for (std::uint32_t q = 0; q < n; ++q) {
                        Driver dq = d0;
                        dq.bit.bit += q;
                        bits_.push_back(resolve(n == 1                           ? d0
                                                : d0.kind == DriverKind::Constant ? view.driver(BitRef{inst, ch.net, ch.offset + at + q})
                                                                                  : dq));
                    }
                }
                at += n;
            }
        }
        // Controls tied to their inactive level through the hierarchy (a
        // primitive FDRE with CE = 1 and R = 0) are no controls at all.
        auto constantAt = [&](const SigSpec& sig, Logic level) { return constantBit(inst, sig, 0) == level; };
        pr.en = static_cast<std::uint32_t>(bits_.size());
        if (reg.enable && constantAt(*reg.enable, Logic::L0)) {
            // never loads (a block RAM port whose write enable is tied off):
            // it keeps its initial value, its data inputs are no sources
            for (std::uint32_t i = 0; i < pr.dataLen; ++i)
                bits_[pr.data + i] = BitInput{};
            neverLoads_[g] = true;
        }
        else if (reg.enable && !constantAt(*reg.enable, Logic::L1)) {
            pr.enLen = 1;
            add(*reg.enable, 0);
        }
        pr.srst = static_cast<std::uint32_t>(bits_.size());
        if (reg.syncReset &&
            !constantAt(reg.syncReset->signal, reg.syncReset->activeHigh ? Logic::L0 : Logic::L1)) {
            pr.srstLen = 1;
            add(reg.syncReset->signal, 0);
        }
        // (asynchronous resets are indexed by position: dropped only all together)
        pr.arst = static_cast<std::uint32_t>(bits_.size());
        const bool arstTied = std::ranges::all_of(reg.asyncResets, [&](const ResetSpec& a) {
            return constantAt(a.signal, a.activeHigh ? Logic::L0 : Logic::L1);
        });
        if (!arstTied) {
            pr.arstLen = static_cast<std::uint32_t>(reg.asyncResets.size());
            for (const auto& a : reg.asyncResets)
                add(a.signal, 0);
        }
    }

    if (combLoops_ > 0) {
        std::string eg;
        for (const auto& l : loopSamples_)
            eg += (eg.empty() ? "e.g. " : ", ") + l;
        sink_.report({"GR-COMB-LOOP", Severity::Warning,
                      std::to_string(combLoops_) +
                          " combinational loop(s) found; the cones of the affected cells are cut at the loop" +
                          (eg.empty() ? std::string() : " (" + eg + ")"),
                      {}, {}});
    }
    memo_.clear(); // only needed while building
}

std::optional<Logic> SeqGraph::constantBit(InstanceId inst, const SigSpec& sig, std::uint32_t index,
                                           int depth) const {
    // the value of a bit that is constant through the hierarchy and through
    // simple logic (FDRE CE = 1'b1; RAMB WEA = 4'b0 into ENARDEN && |WEA)
    if (index >= sig.width())
        return std::nullopt;
    auto b = view_.bitOf(inst, sig, index);
    if (!b) {
        const Logic c = graph::DesignView::constBit(sig, index);
        return c == Logic::L0 || c == Logic::L1 ? std::optional(c) : std::nullopt;
    }
    const auto drv = view_.driver(*b);
    if (drv.kind == DriverKind::Constant)
        return drv.constant == Logic::L0 || drv.constant == Logic::L1 ? std::optional(drv.constant) : std::nullopt;
    if (drv.kind != DriverKind::Cell || depth > 8)
        return std::nullopt;
    const auto& c = view_.bodyOf(drv.bit.inst).cells[drv.index];
    const auto cinst = drv.bit.inst;
    auto in = [&](std::size_t k, std::uint32_t bit) {
        return k < c.inputs.size() ? constantBit(cinst, c.inputs[k], bit, depth + 1) : std::nullopt;
    };
    auto all = [&](std::size_t k, Logic v) { // every bit of input k is v
        if (k >= c.inputs.size())
            return false;
        for (std::uint32_t i = 0; i < c.inputs[k].width(); ++i)
            if (in(k, i) != v)
                return false;
        return true;
    };
    auto any = [&](std::size_t k, Logic v) {
        if (k >= c.inputs.size())
            return false;
        for (std::uint32_t i = 0; i < c.inputs[k].width(); ++i)
            if (in(k, i) == v)
                return true;
        return false;
    };
    const std::uint32_t ob = drv.bit.bit; // bit of the cell output
    using L = Logic;
    switch (c.kind) {
        case CellKind::Not: {
            auto v = in(0, ob);
            return v ? std::optional(*v == L::L0 ? L::L1 : L::L0) : std::nullopt;
        }
        case CellKind::And: {
            auto a = in(0, ob), bb = in(1, ob);
            if (a == L::L0 || bb == L::L0)
                return L::L0;
            if (a == L::L1 && bb == L::L1)
                return L::L1;
            return std::nullopt;
        }
        case CellKind::Or: {
            auto a = in(0, ob), bb = in(1, ob);
            if (a == L::L1 || bb == L::L1)
                return L::L1;
            if (a == L::L0 && bb == L::L0)
                return L::L0;
            return std::nullopt;
        }
        case CellKind::ReduceOr:
        case CellKind::LogicNot:
            if (ob != 0)
                return L::L0;
            if (any(0, L::L1))
                return c.kind == CellKind::ReduceOr ? L::L1 : L::L0;
            if (all(0, L::L0))
                return c.kind == CellKind::ReduceOr ? L::L0 : L::L1;
            return std::nullopt;
        case CellKind::ReduceAnd:
            if (ob != 0)
                return L::L0;
            if (any(0, L::L0))
                return L::L0;
            if (all(0, L::L1))
                return L::L1;
            return std::nullopt;
        case CellKind::LogicAnd:
        case CellKind::LogicOr: {
            if (ob != 0)
                return L::L0;
            const bool a1 = any(0, L::L1), b1 = any(1, L::L1);
            const bool a0 = all(0, L::L0), b0 = all(1, L::L0);
            if (c.kind == CellKind::LogicAnd) {
                if (a0 || b0)
                    return L::L0;
                if (a1 && b1)
                    return L::L1;
            }
            else {
                if (a1 || b1)
                    return L::L1;
                if (a0 && b0)
                    return L::L0;
            }
            return std::nullopt;
        }
        case CellKind::Mux: {
            auto sel = in(2, 0);
            if (c.inputs.size() == 3 && c.inputs[2].width() == 1 && sel)
                return in(*sel == L::L1 ? 1 : 0, ob);
            auto a = in(0, ob), bb = in(1, ob);
            return a && a == bb ? a : std::nullopt;
        }
        default: return std::nullopt;
    }
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

void SeqGraph::accumulate(std::vector<ConeEntry>& acc, ConeEntry e,
                          std::unordered_map<std::uint32_t, std::size_t>& where) {
    // Few distinct nodes per cone in practice: merge in place, no sorting;
    // large cones (wide cells) use an index.
    auto merge = [&](ConeEntry& a) {
        a.lo = std::min(a.lo, e.lo);
        a.hi = std::max(a.hi, e.hi);
        a.flags |= e.flags;
    };
    if (acc.size() <= 16 && where.empty()) {
        for (auto& a : acc) {
            if (a.node == e.node) {
                merge(a);
                return;
            }
        }
        acc.push_back(e);
        if (acc.size() > 16)
            for (std::size_t i = 0; i < acc.size(); ++i)
                where.emplace(acc[i].node, i);
        return;
    }
    if (auto it = where.find(e.node); it != where.end()) {
        merge(acc[it->second]);
        return;
    }
    where.emplace(e.node, acc.size());
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
    const auto& c = view_.bodyOf(inst).cells[cell];
    // Very wide bitwise cells (whole-memory write logic) are tracked as one
    // unit: one cone for all their bits instead of one per bit.
    return {inst.value, static_cast<std::uint32_t>(cell), isPerBit(kind) && c.width <= kWideCell ? bit : kAll};
}

std::vector<SeqGraph::CellInput> SeqGraph::allCellInputs(InstanceId inst, std::size_t cellIndex) const {
    const Cell& c = view_.bodyOf(inst).cells[cellIndex];
    if (!isPerBit(c.kind))
        return cellInputs(inst, cellIndex, 0);
    std::vector<CellInput> out;
    constexpr std::uint8_t L = ConeEntry::kLogic;
    constexpr std::uint8_t S = ConeEntry::kLogic | ConeEntry::kSelect;
    for (std::size_t i = 0; i < c.inputs.size(); ++i) {
        std::uint8_t flags = c.kind == CellKind::Not ? 0 : L;
        if ((c.kind == CellKind::Mux && i == 2) || (c.kind == CellKind::DynInsert && i == 1))
            flags = S;
        for (std::uint32_t b = 0; b < c.inputs[i].width(); ++b)
            if (auto bit = view_.bitOf(inst, c.inputs[i], b))
                out.push_back({*bit, flags});
    }
    return out;
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
            // Both data inputs carry the same bit (a record field the
            // branches leave alone): the select does not matter for it.
            if (c.inputs.size() == 3 && outBit < c.inputs[0].width() && outBit < c.inputs[1].width() &&
                c.inputs[0].bit(outBit) == c.inputs[1].bit(outBit)) {
                bitOfInput(0, outBit, 0);
                break;
            }
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
            // A cell evaluated as a whole (very wide: the read and write logic of a
            // 2^14 x 128 bit RAM, corundum cpl_queue_manager) has millions of input
            // bits; its cone keeps one entry per source with a bit hull, so its
            // inputs are reduced first: one per wide driver cell, and the lowest and
            // highest bit of each other source (kept until its dependencies are done)
            struct InKey {
                std::uint64_t a = 0, b = 0;
                bool operator==(const InKey&) const = default;
            };
            struct InKeyHash {
                std::size_t operator()(const InKey& x) const noexcept { return x.a * 0x9E3779B97F4A7C15ULL ^ x.b; }
            };
            std::unordered_map<InKey, std::pair<std::size_t, std::size_t>, InKeyHash> seen; // lowest, highest
            const bool whole = k.bit == kAll;
            auto take = [&](const CellInput& ci, const Driver& d) {
                std::optional<CellKey> dk;
                if (d.kind == DriverKind::Cell) {
                    dk = keyFor(d.bit.inst, d.index, d.bit.bit);
                    if (memo_[*dk].state == 0) { // may rehash: `m` is not used below
                        stack.push_back(*dk);
                        pushed = true;
                    }
                }
                if (whole) {
                    const std::uint64_t head = (static_cast<std::uint64_t>(d.kind) << 56) |
                                               (static_cast<std::uint64_t>(ci.flags) << 48) | d.bit.inst.value;
                    const InKey key = dk ? InKey{head, (static_cast<std::uint64_t>(dk->cell) << 32) | dk->bit}
                                         : InKey{head, (static_cast<std::uint64_t>(d.index) << 32) | d.bit.net.value};
                    const auto [it, fresh] = seen.try_emplace(key, inputs.size(), inputs.size());
                    if (!fresh) {
                        if (dk)
                            return; // the same cone again
                        auto& [lo, hi] = it->second;
                        if (d.bit.bit < inputs[lo].driver.bit.bit) {
                            if (lo == hi) {
                                lo = inputs.size();
                                inputs.push_back({ci, d});
                            }
                            else
                                inputs[lo] = {ci, d};
                        }
                        else if (d.bit.bit > inputs[hi].driver.bit.bit) {
                            if (lo == hi) {
                                hi = inputs.size();
                                inputs.push_back({ci, d});
                            }
                            else
                                inputs[hi] = {ci, d};
                        }
                        return;
                    }
                }
                inputs.push_back({ci, d});
            };
            if (!whole) {
                for (const auto& ci : cellInputs(InstanceId{k.inst}, k.cell, k.bit))
                    take(ci, view_.driver(ci.bit));
            }
            else {
                // the whole cell (very wide, or not bitwise): its inputs chunk by chunk,
                // resolved run by run (one walk for a run of consecutive bits), and of a
                // run only what the cone needs: a wide driver cell once, other sources by
                // their first and last bit, a narrow driver cell bit by bit
                const InstanceId inst{k.inst};
                const Cell& c = view_.bodyOf(inst).cells[k.cell];
                constexpr std::uint8_t L = ConeEntry::kLogic;
                constexpr std::uint8_t S = ConeEntry::kLogic | ConeEntry::kSelect;
                for (std::size_t i = 0; i < c.inputs.size(); ++i) {
                    std::uint8_t flags = c.kind == CellKind::Not ? 0 : L;
                    if ((c.kind == CellKind::Mux && i == 2) || ((c.kind == CellKind::DynInsert || c.kind == CellKind::DynSelect) && i == 1))
                        flags = S;
                    for (const auto& ch : c.inputs[i].chunks()) {
                        if (ch.isConst())
                            continue;
                        for (std::uint32_t at = 0; at < ch.width;) {
                            const BitRef first{inst, ch.net, ch.offset + at};
                            const auto [d, n] = view_.driverRun(first, ch.width - at);
                            const bool perBitCell =
                                d.kind == DriverKind::Cell && keyFor(d.bit.inst, d.index, d.bit.bit).bit != kAll;
                            if (n == 1 || perBitCell) {
                                for (std::uint32_t q = 0; q < n; ++q) {
                                    const BitRef bq{inst, ch.net, ch.offset + at + q};
                                    Driver dq = d;
                                    dq.bit.bit += q;
                                    take({bq, flags}, d.kind == DriverKind::Constant ? view_.driver(bq) : dq);
                                }
                            }
                            else if (d.kind != DriverKind::Constant) { // a constant adds nothing to the cone
                                take({first, flags}, d);
                                Driver last = d;
                                last.bit.bit += n - 1;
                                take({BitRef{inst, ch.net, ch.offset + at + n - 1}, flags}, last);
                            }
                            at += n;
                        }
                    }
                }
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
            if (loopSamples_.size() < 3) {
                const auto& node = view_.design().instance(d.bit.inst);
                const auto& b = view_.bodyOf(d.bit.inst);
                const auto& cell = b.cells[d.index];
                loopSamples_.push_back(node.hierPath + "." + b.net(cell.output).name + " (" +
                                       toString(cell.origin) + ")");
            }
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
    std::unordered_map<std::uint32_t, std::size_t> where;
    ConeEntry scratch;
    for (const auto& ri : inputs) {
        const BitInput v = valueOf(ri.driver);
        for (auto e : entries(v, scratch)) {
            e.flags |= ri.in.flags;
            accumulate(acc, e, where);
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
            for (std::uint32_t i = 0; i < bits.size();) {
                const BitInput& bi = bits[i];
                // a run of bits with the same set of sources, none direct (the array
                // register of a RAM: millions of bits fed by one wide cell): one pass,
                // the records the bit-by-bit merge below would give
                std::uint32_t end = i + 1;
                if (!bi.isDirect() && bi.cone != BitInput::kDirectCone)
                    while (end < bits.size() && bits[end].cone == bi.cone && !bits[end].isDirect())
                        ++end;
                const std::uint32_t lastBit = end - 1;
                const std::uint32_t first = i;
                i = end;
                for (const auto& e : entries(bi, scratch)) {
                    if (!set.contains(e.node))
                        continue;
                    const bool direct = bi.directNode == e.node;
                    auto& list = out[e.node];
                    if (!direct && !list.empty()) {
                        Consumer& last = list.back();
                        if (last.reg == g && last.pin == pin && !last.direct && last.bitEnd + 1 == first &&
                            last.lo == e.lo && last.hi == e.hi && last.flags == e.flags) {
                            last.bitEnd = lastBit;
                            continue;
                        }
                    }
                    Consumer c;
                    c.reg = g;
                    c.bit = first;
                    c.bitEnd = direct ? first : lastBit;
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

namespace opencdc::cdc {

namespace {

using Family = std::vector<std::uint32_t>;

void normalize(Family& f) {
    std::erase(f, SeqGraph::kInfeasible);
    std::ranges::sort(f);
    f.erase(std::unique(f.begin(), f.end()), f.end());
}

} // namespace

std::vector<SeqGraph::Fact> SeqGraph::implied(const BitRef& b, bool value, int depth) const {
    if (depth > 16)
        return {};
    const Driver d = view_.driver(b);
    if (d.kind == DriverKind::Register) {
        const auto g = view_.globalRegister(d.bit.inst, d.index);
        return {Fact{registerNode(g), d.bit.bit - nodes_[g].first.bit, value}};
    }
    if (d.kind != DriverKind::Cell)
        return {};
    const InstanceId inst = d.bit.inst;
    const Cell& c = view_.bodyOf(inst).cells[d.index];
    auto in = [&](std::size_t input, std::uint32_t bit) -> std::optional<BitRef> {
        if (input >= c.inputs.size() || bit >= c.inputs[input].width())
            return std::nullopt;
        return view_.bitOf(inst, c.inputs[input], bit);
    };
    auto constBit = [&](std::size_t input, std::uint32_t bit) -> std::optional<bool> {
        if (input >= c.inputs.size() || bit >= c.inputs[input].width())
            return std::nullopt;
        const auto k = c.inputs[input].bit(bit).asConst();
        if (!k || (*k)[0] == Logic::X || (*k)[0] == Logic::Z)
            return std::nullopt;
        return (*k)[0] == Logic::L1;
    };
    std::vector<Fact> out;
    auto add = [&](std::size_t input, std::uint32_t bit, bool v) {
        if (auto br = in(input, bit)) {
            auto f = implied(*br, v, depth + 1);
            out.insert(out.end(), f.begin(), f.end());
        }
    };
    const std::uint32_t outBit = d.bit.bit; // the cell drives its whole output net
    switch (c.kind) {
        case CellKind::Not: add(0, outBit, !value); break;
        case CellKind::LogicNot:
            if (c.inputs.size() == 1 && c.inputs[0].width() == 1)
                add(0, 0, !value);
            break;
        case CellKind::And:
            if (value) {
                add(0, outBit, true);
                add(1, outBit, true);
            }
            break;
        case CellKind::Or:
            if (!value) {
                add(0, outBit, false);
                add(1, outBit, false);
            }
            break;
        case CellKind::LogicAnd:
            if (value && c.inputs.size() == 2 && c.inputs[0].width() == 1 && c.inputs[1].width() == 1) {
                add(0, 0, true);
                add(1, 0, true);
            }
            break;
        case CellKind::LogicOr:
            if (!value && c.inputs.size() == 2 && c.inputs[0].width() == 1 && c.inputs[1].width() == 1) {
                add(0, 0, false);
                add(1, 0, false);
            }
            break;
        case CellKind::ReduceAnd:
            if (value && !c.inputs.empty())
                for (std::uint32_t i = 0; i < c.inputs[0].width(); ++i)
                    add(0, i, true);
            break;
        case CellKind::ReduceOr:
            if (!value && !c.inputs.empty())
                for (std::uint32_t i = 0; i < c.inputs[0].width(); ++i)
                    add(0, i, false);
            break;
        case CellKind::Eq:
        case CellKind::Ne:
            // x == K is true (or x != K false): every bit of x equals K
            if (c.inputs.size() == 2 && c.inputs[0].width() == c.inputs[1].width() &&
                (c.kind == CellKind::Eq) == value)
                for (std::size_t side = 0; side < 2; ++side) {
                    const auto& k = c.inputs[1 - side];
                    if (!k.isFullyConst())
                        continue;
                    for (std::uint32_t i = 0; i < k.width(); ++i)
                        if (const auto kb = constBit(1 - side, i); kb && !c.inputs[side].bit(i).isFullyConst())
                            add(side, i, *kb);
                    break;
                }
            break;
        case CellKind::Mux: {
            // Y = S ? B : A; a constant input that differs from `value` fixes S
            const auto a = constBit(0, outBit);
            const auto bb = constBit(1, outBit);
            if (a && *a != value) {
                add(2, 0, true);
                add(1, outBit, value);
            }
            else if (bb && *bb != value) {
                add(2, 0, false);
                add(0, outBit, value);
            }
            break;
        }
        default: break;
    }
    return out;
}

bool SeqGraph::atMostOneHot(std::size_t globalReg) const {
    // Registers being checked (a copy from a register that is itself being
    // checked is assumed one-hot: the induction over clock cycles holds when
    // every register on the cycle keeps the property).
    std::vector<std::size_t> active;
    std::function<bool(std::size_t)> regOneHot;
    auto oneHotConst = [](const ConstBits& k) {
        unsigned ones = 0;
        for (const auto b : k.bits()) {
            if (b == Logic::X || b == Logic::Z)
                return false;
            ones += b == Logic::L1 ? 1U : 0U;
        }
        return ones <= 1;
    };
    auto regWidth = [&](std::size_t g) {
        const auto [inst, r] = regLocal_[g];
        return view_.bodyOf(inst).registers[r].q.width();
    };
    // Same clock net and edge: a register sampled in another clock can be
    // caught between two one-hot values (old and new bit both set).
    auto sameClock = [&](std::size_t a, std::size_t b) {
        auto key = [&](std::size_t g) -> std::optional<std::pair<graph::BitRef, Edge>> {
            const auto [inst, r] = regLocal_[g];
            const auto& ck = view_.bodyOf(inst).registers[r].clock;
            const auto bit = view_.bitOf(inst, ck.signal, 0);
            if (!bit)
                return std::nullopt;
            return std::pair{view_.driver(*bit).bit, ck.edge};
        };
        const auto ka = key(a);
        const auto kb = key(b);
        return ka && kb && ka->first == kb->first && ka->second == kb->second;
    };
    regOneHot = [&](std::size_t g) -> bool {
        if (g >= regLocal_.size())
            return false;
        if (std::find(active.begin(), active.end(), g) != active.end())
            return true;
        if (active.size() > 8)
            return false;
        const auto [inst0, r] = regLocal_[g];
        const auto& reg = view_.bodyOf(inst0).registers[r];
        const auto w = reg.q.width();
        if (w < 2)
            return false;
        for (const auto& rs : reg.asyncResets) {
            const auto k = rs.value.asConst();
            if (!k || !oneHotConst(*k))
                return false;
        }
        if (reg.syncReset) {
            const auto k = reg.syncReset->value.asConst();
            if (!k || !oneHotConst(*k))
                return false;
        }
        // the power-up value (declaration initializer; bits without one start at 0)
        {
            unsigned ones = 0;
            for (std::uint32_t i = 0; i < w; ++i) {
                const auto b = view_.bitOf(inst0, reg.q, i);
                if (!b)
                    return false;
                const auto& net = view_.bodyOf(b->inst).net(b->net);
                if (net.init && b->bit < net.init->bits().size() && net.init->bits()[b->bit] == Logic::L1)
                    ++ones;
            }
            if (ones > 1)
                return false;
        }
        active.push_back(g);
        std::function<bool(InstanceId, const SigSpec&, int)> values = [&](InstanceId inst, const SigSpec& sig,
                                                                          int depth) -> bool {
            if (depth > 32 || sig.width() == 0)
                return false;
            const auto sw = sig.width();
            if (const auto k = sig.asConst())
                return oneHotConst(*k);
            // Every bit is a constant 0 or a bit of one register, no register
            // bit used twice: a copy, rotation or shift of a one-hot register.
            {
                std::optional<std::size_t> src;
                std::vector<bool> used;
                bool copy = true;
                for (std::uint32_t i = 0; i < sw && copy; ++i) {
                    const auto b = view_.bitOf(inst, sig, i);
                    if (!b) {
                        copy = graph::DesignView::constBit(sig, i) == Logic::L0;
                        continue;
                    }
                    const Driver d = view_.driver(*b);
                    if (d.kind == DriverKind::Constant) {
                        copy = d.constant == Logic::L0;
                        continue;
                    }
                    if (d.kind != DriverKind::Register) {
                        copy = false;
                        break;
                    }
                    const auto rg = view_.globalRegister(d.bit.inst, d.index);
                    if (src && *src != rg) {
                        copy = false;
                        break;
                    }
                    if (!src) {
                        src = rg;
                        used.assign(regWidth(rg), false);
                    }
                    const auto pos = d.bit.bit - nodes_[rg].first.bit;
                    if (pos >= used.size() || used[pos]) {
                        copy = false;
                        break;
                    }
                    used[pos] = true;
                }
                if (copy)
                    return !src || *src == g || (sameClock(*src, g) && regOneHot(*src));
            }
            // one cell over the whole width
            const auto b0 = view_.bitOf(inst, sig, 0);
            if (!b0)
                return false;
            const Driver d0 = view_.driver(*b0);
            if (d0.kind != DriverKind::Cell)
                return false;
            const Cell& c = view_.bodyOf(d0.bit.inst).cells[d0.index];
            const std::uint32_t off = d0.bit.bit;
            if (off + sw > c.width)
                return false;
            for (std::uint32_t i = 1; i < sw; ++i) {
                const auto bi = view_.bitOf(inst, sig, i);
                if (!bi)
                    return false;
                const Driver di = view_.driver(*bi);
                if (di.kind != DriverKind::Cell || di.bit.inst != d0.bit.inst || di.index != d0.index ||
                    di.bit.bit != off + i)
                    return false;
            }
            switch (c.kind) {
                case CellKind::Mux:
                    return c.inputs.size() == 3 && values(d0.bit.inst, c.inputs[0].extract(off, sw), depth + 1) &&
                           values(d0.bit.inst, c.inputs[1].extract(off, sw), depth + 1);
                case CellKind::Shl:
                case CellKind::Shr:
                    // a one-hot value shifted by any amount (decoder: 1 << idx)
                    return c.inputs.size() == 2 &&
                           values(d0.bit.inst, c.inputs[0], depth + 1);
                case CellKind::Sshr: {
                    // arithmetic shift: sign copies are set only for a set MSB
                    if (c.inputs.size() != 2)
                        return false;
                    const auto& a = c.inputs[0];
                    if (c.isSigned && graph::DesignView::constBit(a, a.width() - 1) != Logic::L0)
                        return false;
                    return values(d0.bit.inst, a, depth + 1);
                }
                case CellKind::DynInsert: {
                    // zeros with one element replaced: x = 0; x[idx] = d
                    if (c.inputs.size() != 3)
                        return false;
                    const auto base = c.inputs[0].asConst();
                    if (!base || !oneHotConst(*base))
                        return false;
                    for (const auto bit : base->bits())
                        if (bit != Logic::L0)
                            return false;
                    return values(d0.bit.inst, c.inputs[2], depth + 1);
                }
                default: return false;
            }
        };
        const bool ok = values(inst0, reg.d, 0);
        active.pop_back();
        return ok;
    };
    return regOneHot(globalReg);
}

std::optional<std::vector<SeqGraph::Fact>> SeqGraph::syncResetFacts(std::size_t globalReg, bool active) const {
    if (globalReg >= regLocal_.size() || pins_[globalReg].srstLen == 0)
        return std::nullopt;
    const auto [inst, r] = regLocal_[globalReg];
    const auto& rs = view_.bodyOf(inst).registers[r].syncReset;
    if (!rs)
        return std::nullopt;
    const auto b = view_.bitOf(inst, rs->signal, 0);
    if (!b)
        return std::vector<Fact>{};
    return implied(*b, active == rs->activeHigh);
}

std::optional<std::vector<std::uint32_t>>
SeqGraph::dataAlternatives(std::size_t globalReg, std::uint32_t bit, const FamilyOps& ops, FamilyCache& cache) const {
    if (globalReg >= regLocal_.size() || neverLoads(globalReg))
        return std::nullopt;
    const auto [inst0, r] = regLocal_[globalReg];
    const auto& reg = view_.bodyOf(inst0).registers[r];
    const auto root = view_.bitOf(inst0, reg.d, bit);
    if (!root)
        return Family{0};

    constexpr int kMaxDepth = 48; // logic levels; deeper cones fall back to the label union
    if (cache.used > cache.budget)
        return std::nullopt;
    bool aborted = false;
    auto& memo = cache.cells; // only complete results are stored
    std::unordered_set<CellKey, CellKeyHash> active; // combinational loops

    // any of the members: their labels under the conditions they all share
    auto flatten = [&](const Family& f) {
        std::uint32_t u = SeqGraph::kInfeasible;
        for (const auto id : f)
            u = u == SeqGraph::kInfeasible ? id : ops.either(u, id);
        return u == SeqGraph::kInfeasible ? 0U : u;
    };
    auto prune = [&](Family& f) {
        if (f.size() < 2 || f.size() > 512)
            return;
        std::vector<bool> drop(f.size(), false);
        for (std::size_t i = 0; i < f.size(); ++i)
            for (std::size_t j = 0; j < f.size() && !drop[i]; ++j)
                if (i != j && !drop[j] && ops.subsumes(f[j], f[i]))
                    drop[i] = true;
        Family kept;
        for (std::size_t i = 0; i < f.size(); ++i)
            if (!drop[i])
                kept.push_back(f[i]);
        f = std::move(kept);
    };
    auto capped = [&](Family f) {
        normalize(f);
        prune(f);
        if (f.size() > kMaxFamily) {
            // first merge the members that share their labels
            std::map<std::uint32_t, std::uint32_t> byLabels;
            for (const auto m : f) {
                auto [it, inserted] = byLabels.emplace(ops.labelsOnly(m), m);
                if (!inserted)
                    it->second = ops.either(it->second, m);
            }
            f.clear();
            for (const auto& [l, m] : byLabels)
                f.push_back(m);
            normalize(f);
        }
        if (f.size() > kMaxFamily)
            f = {flatten(f)};
        if (f.empty())
            f = {0}; // every combination contradicts: unreachable, nothing read
        return f;
    };
    auto cross = [&](const Family& a, const Family& b) {
        Family out;
        out.reserve(a.size() * b.size());
        for (const auto x : a)
            for (const auto y : b)
                out.push_back(ops.unite(x, y));
        return capped(std::move(out));
    };
    auto restrictAll = [&](const Family& f, const std::vector<Fact>& facts) {
        if (facts.empty())
            return f;
        Family out;
        for (const auto m : f)
            out.push_back(ops.restrict(m, facts));
        normalize(out);
        return out; // may be empty: this input is never selected
    };

    // g & x is 0 whenever g is 0, whatever x is (g | x is 1 whenever g is 1): the
    // evaluations are partitioned by the value of a gate input; where the gate
    // decides alone, only the gate is read. The gate is preferably the operand
    // without labels (a select), else one whose value implies facts.
    auto gated = [&](const Family& fa, const std::optional<BitRef>& ra, const Family& fb,
                     const std::optional<BitRef>& rb, bool isAnd) -> Family {
        const bool decides = !isAnd; // 0 decides an and, 1 an or
        auto unlabeled = [&](const Family& f) {
            return std::ranges::all_of(f, [&](std::uint32_t m) { return ops.labelsOnly(m) == 0; });
        };
        auto factsOf = [&](const std::optional<BitRef>& ref, bool v) {
            return ref ? implied(*ref, v) : std::vector<Fact>{};
        };
        if (!ra && !rb)
            return cross(fa, fb);
        bool gateIsA = ra.has_value(); // only one operand is a single bit we can look at
        if (ra && rb) {
            if (unlabeled(fa) != unlabeled(fb))
                gateIsA = unlabeled(fa);
            else
                gateIsA = !(factsOf(ra, decides).empty() && !factsOf(rb, decides).empty());
        }
        const auto& rg = gateIsA ? ra : rb;
        const Family& fg = gateIsA ? fa : fb;
        const Family& fx = gateIsA ? fb : fa;
        const auto gd = factsOf(rg, decides);
        const auto gn = factsOf(rg, !decides);
        if (gd.empty() && gn.empty())
            return cross(fa, fb); // nothing tells the evaluations apart
        Family alt = restrictAll(fg, gd);                   // the gate decides alone
        const Family both = restrictAll(cross(fg, fx), gn); // the other input passes
        alt.insert(alt.end(), both.begin(), both.end());
        return capped(std::move(alt));
    };

    std::function<Family(const BitRef&, int)> ofBit;
    std::function<Family(InstanceId, std::size_t, std::uint32_t, int)> ofCell;
    ofBit = [&](const BitRef& b, int depth) -> Family {
        const Driver d = view_.driver(b);
        switch (d.kind) {
            case DriverKind::Register: {
                const auto g = view_.globalRegister(d.bit.inst, d.index);
                return ops.leaf(registerNode(g), d.bit.bit - nodes_[g].first.bit);
            }
            case DriverKind::Latch: {
                const auto node = latchNode_.at(view_.globalLatch(d.bit.inst, d.index));
                return ops.leaf(node, d.bit.bit - nodes_[node].first.bit);
            }
            case DriverKind::Cell: return ofCell(d.bit.inst, d.index, d.bit.bit, depth + 1);
            default: return Family{0}; // inputs, black boxes, constants: no labels
        }
    };
    ofCell = [&](InstanceId inst, std::size_t cellIndex, std::uint32_t outBit, int depth) -> Family {
        const Cell& c = view_.bodyOf(inst).cells[cellIndex];
        const CellKey key{inst.value, static_cast<std::uint32_t>(cellIndex), isPerBit(c.kind) ? outBit : kAll};
        if (auto it = memo.find(key); it != memo.end())
            return it->second;
        if (aborted || depth > kMaxDepth || ++cache.used > cache.budget) {
            aborted = true;
            return Family{0};
        }
        if (!active.insert(key).second)
            return Family{0}; // loop: already counted by the cone analysis
        auto inputRef = [&](std::size_t input, std::uint32_t b) -> std::optional<BitRef> {
            if (input >= c.inputs.size() || b >= c.inputs[input].width())
                return std::nullopt;
            return view_.bitOf(inst, c.inputs[input], b);
        };
        auto inputBit = [&](std::size_t input, std::uint32_t b) -> Family {
            if (auto br = inputRef(input, b))
                return ofBit(*br, depth);
            return Family{0};
        };
        auto wholeInput = [&](std::size_t input) {
            std::uint32_t u = 0;
            if (input < c.inputs.size())
                for (std::uint32_t b = 0; b < c.inputs[input].width() && !aborted; ++b)
                    u = ops.unite(u, flatten(inputBit(input, b)));
            return u;
        };
        Family out;
        switch (c.kind) {
            case CellKind::Not: out = inputBit(0, outBit); break;
            case CellKind::Mux: {
                if (c.inputs.size() == 3 && outBit < c.inputs[0].width() && outBit < c.inputs[1].width() &&
                    c.inputs[0].bit(outBit) == c.inputs[1].bit(outBit)) {
                    out = inputBit(0, outBit);
                    break;
                }
                // one data input at a time, under the select value that picks it
                const auto sel = inputRef(2, 0);
                Family alt = restrictAll(inputBit(0, outBit), sel ? implied(*sel, false) : std::vector<Fact>{});
                const Family b1 = restrictAll(inputBit(1, outBit), sel ? implied(*sel, true) : std::vector<Fact>{});
                alt.insert(alt.end(), b1.begin(), b1.end());
                out = cross(capped(std::move(alt)), Family{wholeInput(2)});
                break;
            }
            case CellKind::DynSelect: {
                // one element at a time, the index always
                Family alt;
                if (!c.inputs.empty())
                    for (std::uint32_t b = 0; b < c.inputs[0].width() && !aborted; ++b) {
                        const Family e = inputBit(0, b);
                        alt.insert(alt.end(), e.begin(), e.end());
                        if (alt.size() > 4 * kMaxFamily)
                            alt = capped(std::move(alt));
                    }
                out = cross(capped(std::move(alt)), Family{wholeInput(1)});
                break;
            }
            case CellKind::And:
            case CellKind::Or:
            case CellKind::LogicAnd:
            case CellKind::LogicOr: {
                // g & x is 0 whenever g is 0, whatever x is (and g | x is 1 whenever g is 1):
                // the evaluations where one input decides the output read only that input
                const bool logic = c.kind == CellKind::LogicAnd || c.kind == CellKind::LogicOr;
                if (c.inputs.size() != 2 || (logic && (c.inputs[0].width() != 1 || c.inputs[1].width() != 1))) {
                    out = {0};
                    for (const auto& ci : cellInputs(inst, cellIndex, outBit))
                        out = cross(out, ofBit(ci.bit, depth));
                    break;
                }
                const std::uint32_t ib = logic ? 0 : outBit;
                const bool isAnd = c.kind == CellKind::And || c.kind == CellKind::LogicAnd;
                out = gated(inputBit(0, ib), inputRef(0, ib), inputBit(1, ib), inputRef(1, ib), isAnd);
                break;
            }
            case CellKind::ReduceOr:
            case CellKind::ReduceAnd: {
                // a chain of two-input ors (ands) over the bits
                if (c.inputs.empty() || c.inputs[0].width() == 0) {
                    out = {0};
                    break;
                }
                const bool isAnd = c.kind == CellKind::ReduceAnd;
                Family acc = inputBit(0, 0);
                for (std::uint32_t b = 1; b < c.inputs[0].width() && !aborted; ++b)
                    acc = gated(inputBit(0, b), inputRef(0, b), acc, std::nullopt, isAnd);
                out = std::move(acc);
                break;
            }
            default: {
                out = {0};
                for (const auto& ci : cellInputs(inst, cellIndex, outBit)) {
                    if (aborted)
                        break;
                    out = cross(out, ofBit(ci.bit, depth));
                }
                break;
            }
        }
        active.erase(key);
        if (!aborted)
            memo.emplace(key, out);
        return out;
    };

    Family f = ofBit(*root, 0);
    if (aborted)
        return std::nullopt;
    return capped(std::move(f));
}

} // namespace opencdc::cdc

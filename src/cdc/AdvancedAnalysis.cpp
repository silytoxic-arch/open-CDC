// Stage-5 CDC analysis passes: reset synchronizers, protocol synchronizers
// (toggle, Gray, handshake, asynchronous FIFO), reconvergence and the other
// structural facts the stage-5 rules judge. See docs/architecture.md §6/§7.

#include "cdc/AnalyzerState.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace opencdc::cdc::detail {

using clock::ClockEdge;
using clock::ClockRelation;
using graph::BitRef;
using graph::DriverKind;
using namespace opencdc::ir;

namespace {

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxListed = 16; ///< related paths listed per finding (the total is counted)

bool isChainType(SyncType t) {
    return t == SyncType::TwoFlop || t == SyncType::ThreeFlop || t == SyncType::MultiFlop || t == SyncType::Toggle ||
           t == SyncType::Gray;
}

std::vector<std::uint32_t> sourceNodes(const SeqGraph& sg, std::span<const BitInput> bits) {
    std::vector<std::uint32_t> v;
    ConeEntry scratch;
    for (const auto& bi : bits)
        for (const auto& e : sg.entries(bi, scratch))
            v.push_back(e.node);
    std::ranges::sort(v);
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool coneContains(const SeqGraph& sg, std::span<const BitInput> bits, std::uint32_t node) {
    ConeEntry scratch;
    for (const auto& bi : bits)
        for (const auto& e : sg.entries(bi, scratch))
            if (e.node == node)
                return true;
    return false;
}

std::optional<Logic> constantBit(const SigSpec& sig, std::uint32_t index) {
    std::uint32_t pos = 0;
    for (const auto& c : sig.chunks()) {
        if (index < pos + c.width)
            return c.isConst() ? std::optional<Logic>(c.constant[index - pos]) : std::nullopt;
        pos += c.width;
    }
    return std::nullopt;
}

/// Constant value of a bit, propagated through simple logic (Verilog width
/// extension turns `a + (x & ~y)` into a wide AND whose upper bits are 0).
std::optional<Logic> constValue(const graph::DesignView& view, const BitRef& b, int depth = 0) {
    const auto d = view.driver(b);
    if (d.kind == DriverKind::Constant)
        return d.constant == Logic::L0 || d.constant == Logic::L1 ? std::optional<Logic>(d.constant) : std::nullopt;
    if (d.kind != DriverKind::Cell || depth > 8)
        return std::nullopt;
    const Cell& c = view.bodyOf(d.bit.inst).cells[d.index];
    const std::uint32_t ob = d.bit.bit;
    auto in = [&](std::size_t k) -> std::optional<Logic> {
        if (k >= c.inputs.size() || ob >= c.inputs[k].width())
            return std::nullopt;
        if (auto v = constantBit(c.inputs[k], ob))
            return *v == Logic::L0 || *v == Logic::L1 ? v : std::nullopt;
        auto bit = view.bitOf(d.bit.inst, c.inputs[k], ob);
        return bit ? constValue(view, *bit, depth + 1) : std::nullopt;
    };
    auto inv = [](std::optional<Logic> v) -> std::optional<Logic> {
        if (!v)
            return std::nullopt;
        return *v == Logic::L0 ? Logic::L1 : Logic::L0;
    };
    // Truth value of a whole input (logical operators).
    auto truth = [&](std::size_t k) -> std::optional<bool> {
        if (k >= c.inputs.size() || c.inputs[k].width() > 64)
            return std::nullopt;
        bool unknown = false;
        for (std::uint32_t i = 0; i < c.inputs[k].width(); ++i) {
            std::optional<Logic> v = constantBit(c.inputs[k], i);
            if (!v) {
                auto bit = view.bitOf(d.bit.inst, c.inputs[k], i);
                v = bit ? constValue(view, *bit, depth + 1) : std::nullopt;
            }
            if (v == Logic::L1)
                return true;
            if (v != Logic::L0)
                unknown = true;
        }
        return unknown ? std::nullopt : std::optional<bool>(false);
    };
    auto boolean = [&](std::optional<bool> v) -> std::optional<Logic> {
        if (!v || ob > 0)
            return ob > 0 ? std::optional<Logic>(Logic::L0) : std::nullopt;
        return *v ? Logic::L1 : Logic::L0;
    };
    switch (c.kind) {
        case CellKind::LogicNot: {
            const auto t = truth(0);
            return boolean(t ? std::optional<bool>(!*t) : std::nullopt);
        }
        case CellKind::LogicAnd: {
            const auto a = truth(0), b2 = truth(1);
            if (a == false || b2 == false)
                return boolean(false);
            return boolean(a == true && b2 == true ? std::optional<bool>(true) : std::nullopt);
        }
        case CellKind::LogicOr: {
            const auto a = truth(0), b2 = truth(1);
            if (a == true || b2 == true)
                return boolean(true);
            return boolean(a == false && b2 == false ? std::optional<bool>(false) : std::nullopt);
        }
        case CellKind::Not: return inv(in(0));
        case CellKind::And: {
            const auto a = in(0), b2 = in(1);
            if (a == Logic::L0 || b2 == Logic::L0)
                return Logic::L0;
            if (a == Logic::L1 && b2 == Logic::L1)
                return Logic::L1;
            return std::nullopt;
        }
        case CellKind::Or: {
            const auto a = in(0), b2 = in(1);
            if (a == Logic::L1 || b2 == Logic::L1)
                return Logic::L1;
            if (a == Logic::L0 && b2 == Logic::L0)
                return Logic::L0;
            return std::nullopt;
        }
        default: return std::nullopt;
    }
}

// ----------------------------------------------------------------------
// XOR-tree tracer (Gray encoders, toggle flops)
// ----------------------------------------------------------------------

/// A bit driven by something other than XOR/XNOR/NOT, a constant shift or a
/// mux with a constant arm: identified by its driver.
struct Leaf {
    DriverKind kind = DriverKind::Undriven;
    std::uint32_t inst = 0;
    std::uint32_t net = 0;
    std::uint32_t bit = 0;
    std::uint32_t index = 0; ///< register / cell index in the body of `inst`
    friend bool operator==(const Leaf& a, const Leaf& b) {
        return a.kind == b.kind && a.inst == b.inst && a.net == b.net && a.bit == b.bit;
    }
    [[nodiscard]] bool sameEntity(const Leaf& o) const { return kind == o.kind && inst == o.inst && net == o.net; }
};

class XorTracer {
public:
    explicit XorTracer(const graph::DesignView& view) : view_(view) {}

    /// XOR leaves of a bit (an even number of occurrences cancels out).
    /// `hold`: the Q bit of the register whose D is traced; a mux arm that
    /// holds it (enable not extracted as a pin) is skipped like a constant arm.
    std::vector<Leaf> leaves(const BitRef& b, std::optional<Leaf> hold = std::nullopt) {
        hold_ = hold;
        memo_.clear();
        auto r = collect(b, 0);
        hold_.reset();
        // A held value (only constants and the register itself, no XOR/NOT
        // of it) does not change: no leaves, in particular not a toggle.
        if (r.kind != Kind::Value)
            return {};
        return std::move(r.leaves);
    }

    /// Leaf of a bit through wires only (no logic).
    Leaf leafOf(const BitRef& b) const { return toLeaf(view_.driver(b)); }

private:
    enum class Kind : std::uint8_t { Value, Const, Hold };
    struct Result {
        Kind kind = Kind::Value;
        std::vector<Leaf> leaves; ///< Hold: {held bit}; Const: empty
        [[nodiscard]] bool passive() const { return kind != Kind::Value; }
    };

    static Leaf toLeaf(const graph::Driver& d) {
        return Leaf{d.kind, d.bit.inst.value, d.bit.net.value, d.bit.bit, d.index};
    }

    static void toggle(std::vector<Leaf>& out, const Leaf& l) {
        if (auto it = std::ranges::find(out, l); it != out.end())
            out.erase(it);
        else
            out.push_back(l);
    }

    static bool sameSet(const std::vector<Leaf>& a, const std::vector<Leaf>& b) {
        return a.size() == b.size() &&
               std::ranges::all_of(a, [&](const Leaf& l) { return std::ranges::find(b, l) != b.end(); });
    }

    Result collect(const BitRef& b, int depth) {
        if (auto it = memo_.find(b); it != memo_.end())
            return it->second;
        Result r = compute(b, depth);
        memo_.emplace(b, r);
        return r;
    }

    Result compute(const BitRef& b, int depth) {
        const auto d = view_.driver(b);
        if (d.kind == DriverKind::Constant || d.kind == DriverKind::Undriven)
            return {Kind::Const, {}};
        const Leaf self = toLeaf(d);
        if (hold_ && self == *hold_)
            return {Kind::Hold, {self}};
        if (d.kind != DriverKind::Cell || depth > 48)
            return {Kind::Value, {self}};
        const auto inst = d.bit.inst;
        const Cell& c = view_.bodyOf(inst).cells[d.index];
        const std::uint32_t ob = d.bit.bit;
        auto arm = [&](std::size_t input, std::uint32_t i) -> Result {
            if (input >= c.inputs.size() || i >= c.inputs[input].width())
                return {Kind::Const, {}};
            if (constantBit(c.inputs[input], i))
                return {Kind::Const, {}};
            auto bit = view_.bitOf(inst, c.inputs[input], i);
            return bit ? collect(*bit, depth + 1) : Result{Kind::Const, {}};
        };
        switch (c.kind) {
            case CellKind::Xor:
            case CellKind::Xnor: {
                const Result x = arm(0, ob), y = arm(1, ob);
                Result out{Kind::Value, {}};
                for (const auto& l : x.leaves)
                    toggle(out.leaves, l);
                for (const auto& l : y.leaves)
                    toggle(out.leaves, l);
                if (x.kind == Kind::Const && y.kind == Kind::Const)
                    out.kind = Kind::Const;
                return out;
            }
            case CellKind::Not: {
                Result x = arm(0, ob);
                if (x.kind == Kind::Hold)
                    x.kind = Kind::Value; // the inverted current value changes it
                return x;
            }
            case CellKind::Shr:
            case CellKind::Shl:
            case CellKind::Sshr: {
                const auto amount = c.inputs.size() > 1 ? c.inputs[1].asConst() : std::nullopt;
                const auto kv = amount ? amount->toUint() : std::nullopt;
                if (!kv)
                    break;
                const std::uint64_t k = *kv;
                const std::uint32_t w = c.inputs[0].width();
                if (c.kind == CellKind::Shl)
                    return ob >= k ? arm(0, static_cast<std::uint32_t>(ob - k)) : Result{Kind::Const, {}};
                if (ob + k < w)
                    return arm(0, static_cast<std::uint32_t>(ob + k));
                if (c.kind == CellKind::Sshr && w > 0)
                    return arm(0, w - 1);
                return {Kind::Const, {}};
            }
            case CellKind::Mux: {
                // Constant select: only one arm matters.
                if (c.inputs.size() > 2) {
                    std::optional<Logic> sel = constantBit(c.inputs[2], 0);
                    if (!sel) {
                        auto sb = view_.bitOf(inst, c.inputs[2], 0);
                        sel = sb ? constValue(view_, *sb) : std::nullopt;
                    }
                    if (sel == Logic::L0)
                        return arm(0, ob);
                    if (sel == Logic::L1)
                        return arm(1, ob);
                }
                // A constant arm (clear/preset) or a hold arm brings no new
                // value: follow the other arm; equal arms merge.
                const Result a = arm(0, ob), bb = arm(1, ob);
                if (a.passive() && bb.passive())
                    return a.kind == Kind::Hold ? a : bb;
                if (a.passive())
                    return bb;
                if (bb.passive())
                    return a;
                if (sameSet(a.leaves, bb.leaves))
                    return a;
                break;
            }
            default: break;
        }
        return {Kind::Value, {self}};
    }

    const graph::DesignView& view_;
    std::optional<Leaf> hold_;
    std::unordered_map<BitRef, Result, graph::BitRefHash> memo_;
};

/// True if every bit of `sig` above bit 0 is 0 (an increment of at most one).
bool atMostOne(const graph::DesignView& view, InstanceId inst, const SigSpec& sig) {
    for (std::uint32_t i = 1; i < sig.width(); ++i) {
        auto v = constantBit(sig, i);
        if (!v) {
            auto bit = view.bitOf(inst, sig, i);
            v = bit ? constValue(view, *bit) : std::nullopt;
        }
        if (!v || *v != Logic::L0)
            return false;
    }
    return sig.width() > 0;
}

// ----------------------------------------------------------------------
// Reset synchronizers
// ----------------------------------------------------------------------

struct ResetWalk {
    std::vector<StageRef> stages;
    std::vector<std::size_t> others; ///< per stage: readers other than the next stage
    std::vector<std::uint32_t> arst; ///< asynchronous reset sources of the first stage
    bool done = false;
};

} // namespace

void findResetSynchronizers(State& st) {
    const auto& sg = st.sg;
    std::vector<ResetWalk> walks;
    for (std::size_t g = 0; g < st.view.registerCount(); ++g) {
        if (st.domain[g].empty() || sg.pinBits(g, Pin::AsyncReset).empty())
            continue;
        if (!sg.pinBits(g, Pin::Enable).empty() || !sg.pinBits(g, Pin::SyncReset).empty())
            continue;
        const auto data = sg.pinBits(g, Pin::Data);
        for (std::uint32_t i = 0; i < data.size(); ++i)
            if (data[i].cone == 0 && !data[i].isDirect()) {
                ResetWalk w;
                w.stages.emplace_back(g, i);
                w.arst = sourceNodes(sg, sg.pinBits(g, Pin::AsyncReset));
                walks.push_back(std::move(w));
            }
    }
    if (walks.empty())
        return;

    for (std::uint32_t depth = 0; depth < kMaxStages; ++depth) {
        std::vector<std::uint32_t> level;
        for (const auto& w : walks)
            if (!w.done)
                level.push_back(SeqGraph::registerNode(w.stages.back().first));
        if (level.empty())
            break;
        std::ranges::sort(level);
        level.erase(std::unique(level.begin(), level.end()), level.end());
        st.queryConsumers(level);
        for (auto& w : walks) {
            if (w.done)
                continue;
            const auto [curReg, curBit] = w.stages.back();
            const Consumer* next = nullptr;
            std::size_t others = 0;
            for (const auto& c : st.consumersOf(curReg)) {
                if (curBit < c.lo || curBit > c.hi)
                    continue;
                const bool link = c.pin == Pin::Data && c.direct && c.directBit == curBit &&
                                  st.domain[c.reg] == st.domain[curReg] && !(c.reg == curReg && c.bit == curBit) &&
                                  std::ranges::none_of(w.stages, [&](const StageRef& s) {
                                      return s.first == c.reg && s.second == c.bit;
                                  }) &&
                                  sg.pinBits(c.reg, Pin::Enable).empty() &&
                                  sourceNodes(sg, sg.pinBits(c.reg, Pin::AsyncReset)) == w.arst;
                if (link && !next)
                    next = &c;
                else
                    others += c.bitEnd - c.bit + 1;
            }
            w.others.push_back(others);
            if (!next || w.stages.size() >= kMaxStages) {
                w.done = true;
                continue;
            }
            w.stages.emplace_back(next->reg, next->bit);
        }
    }

    // Readers of the last stages: registers whose asynchronous reset they drive.
    std::vector<std::uint32_t> lasts;
    for (const auto& w : walks)
        lasts.push_back(SeqGraph::registerNode(w.stages.back().first));
    std::ranges::sort(lasts);
    lasts.erase(std::unique(lasts.begin(), lasts.end()), lasts.end());
    st.queryConsumers(lasts);

    for (auto& w : walks) {
        const auto [lastReg, lastBit] = w.stages.back();
        std::set<std::size_t> protectedRegs;
        for (const auto& c : st.consumersOf(lastReg))
            if (c.pin == Pin::AsyncReset && lastBit >= c.lo && lastBit <= c.hi)
                protectedRegs.insert(c.reg);
        // A lone constant flop is a reset synchronizer only if it drives resets.
        if (w.stages.size() < 2 && protectedRegs.empty())
            continue;
        bool middleFanout = false;
        for (std::size_t s = 0; s + 1 < w.stages.size(); ++s)
            if (s < w.others.size() && w.others[s] > 0)
                middleFanout = true;
        ResetChain rc;
        rc.stages = w.stages;
        rc.protectedRegisters = protectedRegs.size();
        rc.conditions.push_back({"the first stage has a constant data input", true});
        rc.conditions.push_back({"all stages share the same asynchronous reset", true});
        rc.conditions.push_back({"at least " + std::to_string(st.opt.minSyncStages) + " stages (found " +
                                     std::to_string(w.stages.size()) + ")",
                                 w.stages.size() >= st.opt.minSyncStages});
        rc.conditions.push_back({"stages before the last drive only the next stage", !middleFanout});
        rc.conditions.push_back({"all stages are clocked by the same clock and edge", true});
        const std::size_t idx = st.resetChains.size();
        for (const auto& [reg, bit] : rc.stages)
            st.resetChainOf.try_emplace(reg, idx);
        st.resetChains.push_back(std::move(rc));
    }
    st.result->stats.resetSynchronizers = st.resetChains.size();
}

void classifyResetPaths(State& st) {
    const auto& nodes = st.sg.nodes();
    for (std::size_t i = 0; i < st.result->paths.size(); ++i) {
        CdcPath& p = st.result->paths[i];
        if (p.pin != Pin::AsyncReset)
            continue;
        if (p.groupSize > 1) {
            std::string list;
            for (const auto& m : p.groupMembers)
                list += (list.empty() ? "" : ", ") + m;
            if (p.groupSize > p.groupMembers.size())
                list += ", ...";
            p.notes.push_back(std::to_string(p.groupSize) + " registers of clock " + p.dstClockName +
                              " use this asynchronous reset: " + list);
        }
        if (p.throughLogic)
            p.notes.push_back("the reset passes through combinational logic: a glitch can assert the reset");
        if (auto it = st.resetChainOf.find(p.dstRegister); it != st.resetChainOf.end()) {
            const ResetChain& rc = st.resetChains[it->second];
            p.syncType = SyncType::ResetSync;
            p.syncStages = static_cast<std::uint32_t>(rc.stages.size());
            p.conditions = rc.conditions;
            for (const auto& [reg, bit] : rc.stages)
                p.syncRegisters.push_back(st.stageName(reg, bit));
            p.syncStatus = rc.ok() ? SyncStatus::NecessaryConditionsVerified : SyncStatus::StructureRecognized;
            p.notes.push_back(rc.protectedRegisters > 0
                                  ? "reset synchronizer (asynchronous assertion, synchronous release); its output "
                                    "drives the asynchronous reset of " +
                                        std::to_string(rc.protectedRegisters) + " register(s)"
                                  : std::string("reset synchronizer (asynchronous assertion, synchronous release); "
                                                "its output does not drive an asynchronous reset inside the design"));
            continue;
        }
        p.syncStatus = SyncStatus::NotSynchronized;
        if (st.isRegister(p.srcNode)) {
            if (auto it = st.resetChainOf.find(p.srcNode); it != st.resetChainOf.end())
                p.notes.push_back("'" + p.srcName + "' is the output of a reset synchronizer for clock " +
                                  p.srcClockName + "; registers of clock " + p.dstClockName +
                                  " need their own reset synchronizer");
        }
        else if (nodes[p.srcNode].kind == SourceKind::PrimaryInput && p.srcUnclocked) {
            p.notes.push_back("the input has no clock domain; if it is already synchronized to " + p.dstClockName +
                              " outside the design, declare it with set_input_delay -clock");
        }
    }
}

// ----------------------------------------------------------------------
// Protocol synchronizers
// ----------------------------------------------------------------------

namespace {

class ProtocolRecognizer {
public:
    explicit ProtocolRecognizer(State& st) : st_(st), xt_(st.view) {}

    void run() {
        auto& paths = st_.result->paths;
        std::vector<std::uint32_t> toggleLasts;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            CdcPath& p = paths[i];
            if (p.pin != Pin::Data || !isChainType(p.syncType) || !st_.isRegister(p.srcNode) ||
                st_.isConstantDataFlop(p.srcNode))
                continue;
            if (p.width > 1 && (checkGray(i), p.syncType == SyncType::Gray))
                continue;
            if (checkToggle(i))
                toggleLasts.push_back(SeqGraph::registerNode(st_.chains[i].front().stages.back().first));
            else if (p.width == 1)
                checkPulse(p);
        }
        if (!toggleLasts.empty()) {
            st_.queryConsumers(toggleLasts);
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (paths[i].syncType == SyncType::Toggle)
                    findEdgeDetector(i);
        }
        indexBackPaths();
        prefetchDownstream(2);
        for (std::size_t i = 0; i < paths.size(); ++i)
            if (paths[i].syncType == SyncType::QualifiedData)
                checkHandshake(i);
    }

private:
    std::optional<BitRef> dataBit(std::size_t reg, std::uint32_t bit) const { return st_.pinBit(reg, Pin::Data, bit); }

    /// Gray encoder: D[k] = X[k] ^ X[k+1] for k < N-1, D[N-1] = X[N-1].
    /// Returns X (its bit 0) if the bits `d` have that structure.
    std::optional<Leaf> grayPattern(const std::vector<BitRef>& d, const std::vector<Leaf>& holds) {
        const auto n = static_cast<std::uint32_t>(d.size());
        std::vector<std::vector<Leaf>> lv(n);
        for (std::uint32_t k = 0; k < n; ++k)
            lv[k] = xt_.leaves(d[k], holds[k]);
        if (lv[n - 1].size() != 1)
            return std::nullopt;
        const Leaf top = lv[n - 1].front();
        if (top.bit < n - 1)
            return std::nullopt;
        const std::uint32_t base = top.bit - (n - 1);
        for (std::uint32_t k = 0; k + 1 < n; ++k) {
            if (lv[k].size() != 2)
                return std::nullopt;
            Leaf a = top, b = top;
            a.bit = base + k;
            b.bit = base + k + 1;
            const bool match = (lv[k][0] == a && lv[k][1] == b) || (lv[k][0] == b && lv[k][1] == a);
            if (!match)
                return std::nullopt;
        }
        Leaf result = top;
        result.bit = base;
        return result;
    }

    enum class GrayResult : std::uint8_t { Fail, Passive, Gray };

    /// The bits `d` are Gray-encoded: directly, or through a word-wide mux
    /// whose arms are each Gray-encoded, constant, or the held value
    /// (g <= c ? bin2gray(x) : bin2gray(y)). Passive: only constants and the
    /// held value. The binary values are added to `bases`.
    GrayResult grayOf(const std::vector<BitRef>& d, const std::vector<Leaf>& holds, std::vector<Leaf>& bases,
                      int depth) {
        if (auto b = grayPattern(d, holds)) {
            bases.push_back(*b);
            return GrayResult::Gray;
        }
        if (depth > 8)
            return GrayResult::Fail;
        const auto d0 = st_.view.driver(d[0]);
        if (d0.kind != DriverKind::Cell)
            return GrayResult::Fail;
        const Cell& c = st_.view.bodyOf(d0.bit.inst).cells[d0.index];
        if (c.kind != CellKind::Mux || c.inputs.size() != 3)
            return GrayResult::Fail;
        for (std::size_t k = 1; k < d.size(); ++k) {
            const auto dk = st_.view.driver(d[k]);
            if (dk.kind != DriverKind::Cell || dk.bit.inst != d0.bit.inst || dk.index != d0.index ||
                dk.bit.bit != d0.bit.bit + k)
                return GrayResult::Fail;
        }
        std::optional<Logic> sel = constantBit(c.inputs[2], 0);
        if (!sel)
            if (auto sb = st_.view.bitOf(d0.bit.inst, c.inputs[2], 0))
                sel = constValue(st_.view, *sb);
        bool gray = false;
        for (std::size_t arm = 0; arm < 2; ++arm) {
            if ((sel == Logic::L0 && arm == 1) || (sel == Logic::L1 && arm == 0))
                continue;
            std::vector<BitRef> bits;
            bool passive = true;
            for (std::size_t k = 0; k < d.size(); ++k) {
                const auto ob = static_cast<std::uint32_t>(d0.bit.bit + k);
                if (constantBit(c.inputs[arm], ob))
                    continue;
                auto bit = st_.view.bitOf(d0.bit.inst, c.inputs[arm], ob);
                if (!bit)
                    return GrayResult::Fail;
                if (!(xt_.leafOf(*bit) == holds[k]))
                    passive = false;
                bits.push_back(*bit);
            }
            if (passive)
                continue; // constant (clear) or hold
            if (bits.size() != d.size())
                return GrayResult::Fail;
            const auto r = grayOf(bits, holds, bases, depth + 1);
            if (r == GrayResult::Fail)
                return GrayResult::Fail;
            gray = gray || r == GrayResult::Gray;
        }
        return gray ? GrayResult::Gray : GrayResult::Passive;
    }

    /// True if the binary value `x` provably changes by at most one per step:
    /// an adder/subtractor with a one-bit increment, a register loaded from
    /// one, or a mux choosing between such a value and a held register.
    bool stepsByOne(const Leaf& x, int depth = 0) {
        if (depth > 4)
            return false;
        if (x.kind == DriverKind::Register) {
            // A binary register: look at what it is loaded with.
            const std::size_t g = st_.view.globalRegister(InstanceId{x.inst}, x.index);
            auto b = dataBit(g, 0);
            if (!b)
                return false;
            const Leaf l = xt_.leafOf(*b);
            return l.kind == DriverKind::Cell && stepsByOne(l, depth + 1);
        }
        if (x.kind != DriverKind::Cell)
            return false;
        const Cell& c = st_.view.bodyOf(InstanceId{x.inst}).cells[x.index];
        const InstanceId inst{x.inst};
        if (c.kind == CellKind::Add && c.inputs.size() == 2)
            return atMostOne(st_.view, inst, c.inputs[0]) || atMostOne(st_.view, inst, c.inputs[1]);
        if (c.kind == CellKind::Sub && c.inputs.size() == 2)
            return atMostOne(st_.view, inst, c.inputs[1]);
        if (c.kind == CellKind::Mux && c.inputs.size() == 3) {
            // Each arm either steps by one or holds: it is a register that is
            // loaded with this very value (bin <= bin_next; bin_next = c ? bin + 1 : bin).
            for (std::size_t arm = 0; arm < 2; ++arm) {
                auto b = st_.view.bitOf(InstanceId{x.inst}, c.inputs[arm], 0);
                if (!b)
                    return false;
                const Leaf l = xt_.leafOf(*b);
                if (l.kind == DriverKind::Register) {
                    const std::size_t g = st_.view.globalRegister(InstanceId{l.inst}, l.index);
                    auto d = dataBit(g, l.bit - qLeaf(g, 0).bit);
                    if (d && xt_.leafOf(*d).sameEntity(x))
                        continue; // hold
                    if (d) {
                        const Leaf through = xt_.leaves(*d, l).size() == 1 ? xt_.leaves(*d, l).front() : Leaf{};
                        if (through.sameEntity(x))
                            continue; // hold behind an enable mux
                    }
                    return false;
                }
                if (!(l.kind == DriverKind::Cell && stepsByOne(l, depth + 1)))
                    return false;
            }
            return true;
        }
        return false;
    }

    void checkGray(std::size_t i) {
        CdcPath& p = st_.result->paths[i];
        const std::uint32_t n = p.srcBitHi - p.srcBitLo + 1;
        if (n < 2 || n > 64)
            return;
        std::vector<BitRef> d;
        std::vector<Leaf> holds;
        for (std::uint32_t k = 0; k < n; ++k) {
            auto b = dataBit(p.srcNode, p.srcBitLo + k);
            if (!b)
                return;
            d.push_back(*b);
            holds.push_back(qLeaf(p.srcNode, p.srcBitLo + k));
        }
        std::vector<Leaf> bases;
        if (grayOf(d, holds, bases, 0) != GrayResult::Gray)
            return;
        const auto& chains = st_.chains[i];
        const bool equalStages = std::ranges::all_of(
            chains, [&](const Chain& c) { return c.stages.size() == chains.front().stages.size(); });
        // Switching between two different binary values (g <= c ? gray(x) : gray(y))
        // can change several bits at once even if x and y each step by one.
        const bool oneBase = std::ranges::all_of(bases, [&](const Leaf& x) { return x.sameEntity(bases.front()); });
        const bool step = oneBase && stepsByOne(bases.front());
        if (!oneBase)
            p.notes.push_back("the Gray register is loaded from " + std::to_string(bases.size()) +
                              " different binary values; switching between them can change several bits at once");
        p.syncType = SyncType::Gray;
        p.conditions.push_back({"the source register is Gray-encoded (g = b ^ (b >> 1))", true});
        p.conditions.push_back({"every bit has a synchronizer chain of the same length", equalStages});
        std::erase_if(p.unverified, [](const std::string& s) { return s.find("(CDC-008)") != std::string::npos; });
        const bool ok = std::ranges::all_of(p.conditions, [](const Condition& c) { return c.satisfied; });
        if (!ok) {
            p.syncStatus = SyncStatus::StructureRecognized;
        }
        else if (step) {
            p.syncStatus = SyncStatus::NecessaryConditionsVerified;
            p.notes.push_back("the binary value is produced by an increment/decrement by at most one, so the Gray "
                              "value changes by at most one bit per source clock");
        }
        else {
            p.syncStatus = SyncStatus::ProtocolUnverified;
            p.unverified.push_back("the binary value encoded in Gray must change by at most one per source clock "
                                   "(could not be proven from its logic)");
        }
    }

    Leaf qLeaf(std::size_t reg, std::uint32_t bit) const {
        const auto& n = st_.sg.nodes()[reg];
        return xt_.leafOf(BitRef{n.first.inst, n.first.net, n.first.bit + bit});
    }

    /// Every source bit toggles: its next value is its current value
    /// inverted (t <= t ^ event, or t <= ~t under an enable). A vector of
    /// toggle bits carries one independent event per bit.
    bool checkToggle(std::size_t i) {
        CdcPath& p = st_.result->paths[i];
        if (p.srcBitHi - p.srcBitLo >= 64)
            return false;
        for (std::uint32_t bit = p.srcBitLo; bit <= p.srcBitHi; ++bit) {
            auto b = dataBit(p.srcNode, bit);
            if (!b)
                return false;
            const Leaf self = qLeaf(p.srcNode, bit);
            const auto leaves = xt_.leaves(*b, self);
            if (std::ranges::find(leaves, self) == leaves.end())
                return false;
        }
        p.syncType = SyncType::Toggle;
        p.conditions.push_back({"the source register toggles: its next value is its current value inverted", true});
        if (p.width > 1) {
            std::erase_if(p.unverified, [](const std::string& s) { return s.find("(CDC-008)") != std::string::npos; });
            p.notes.push_back("each of the " + std::to_string(p.width) +
                              " bits toggles on its own event: the bits are independent toggle synchronizers");
            if (p.syncStatus == SyncStatus::ProtocolUnverified && p.unverified.empty())
                p.syncStatus = SyncStatus::NecessaryConditionsVerified;
        }
        return true;
    }

    /// A register that can be high for a single cycle: no hold path (no
    /// enable, no feedback from itself) and logic in front of it. A plain
    /// copy of another register of the same clock pulses if that one does;
    /// a copy of an input is not judged (its waveform is unknown).
    bool mayPulse(std::uint32_t reg, int depth = 0) const {
        if (depth > 4 || !st_.isRegister(reg) || st_.isConstantDataFlop(reg))
            return false;
        if (!st_.sg.pinBits(reg, Pin::Enable).empty())
            return false; // holds its value while not enabled
        const auto data = st_.sg.pinBits(reg, Pin::Data);
        if (data.size() != 1 || coneContains(st_.sg, data, SeqGraph::registerNode(reg)))
            return false; // feedback: the register holds its value
        const BitInput& bi = data.front();
        if (bi.isDirect())
            return st_.isRegister(bi.directNode) && st_.domain[bi.directNode] == st_.domain[reg] &&
                   mayPulse(bi.directNode, depth + 1);
        ConeEntry scratch;
        const auto entries = st_.sg.entries(bi, scratch);
        return std::ranges::any_of(entries, [](const ConeEntry& e) { return (e.flags & ConeEntry::kLogic) != 0; });
    }

    void checkPulse(CdcPath& p) {
        if (!mayPulse(p.srcNode))
            return;
        p.sourceMayPulse = true;
        p.notes.push_back("the source register has no hold path (no enable, no feedback) and is computed by logic: "
                          "it can be high for a single source clock cycle");
    }

    void findEdgeDetector(std::size_t i) {
        CdcPath& p = st_.result->paths[i];
        const Chain& ch = st_.chains[i].front();
        const auto last = ch.stages.back();
        std::unordered_set<std::uint32_t> stages;
        for (const auto& [reg, bit] : ch.stages)
            stages.insert(SeqGraph::registerNode(reg));
        // Registers that copy the last stage (s3 <= s2).
        for (const auto& c : st_.consumersOf(last.first))
            if (c.pin == Pin::Data && c.direct && c.directBit == last.second)
                stages.insert(SeqGraph::registerNode(c.reg));
        stages.erase(SeqGraph::registerNode(last.first));
        for (const auto& c : st_.consumersOf(last.first)) {
            if (c.direct || last.second < c.lo || last.second > c.hi)
                continue;
            for (const auto pin : {Pin::Data, Pin::Enable}) {
                ConeEntry scratch;
                for (const auto& bi : st_.sg.pinBits(c.reg, pin))
                    for (const auto& e : st_.sg.entries(bi, scratch))
                        if (stages.contains(e.node)) {
                            p.notes.push_back("the destination detects toggles in '" + st_.sg.nodes()[c.reg].name +
                                              "' (it compares two consecutive samples)");
                            return;
                        }
            }
        }
        p.notes.push_back("no register after the synchronizer compares two consecutive samples (the edge "
                          "detection may be combinational or outside the design): a level reader sees one change per "
                          "event, not a pulse");
    }

    /// Indexes for handshake / FIFO matching (built once).
    void indexBackPaths() {
        const auto& paths = st_.result->paths;
        for (std::size_t k = 0; k < paths.size(); ++k) {
            const CdcPath& p = paths[k];
            if (p.pin != Pin::Data || !isChainType(p.syncType) || p.srcUnclocked)
                continue;
            if (p.width == 1)
                ackBySource_[p.srcNode].push_back(k);
            if (p.syncType == SyncType::Gray)
                grayBySource_[p.srcNode].push_back(k);
        }
    }

    /// Fills the consumer cache for every request-synchronizer stage and the
    /// registers up to `depth` levels after them: one graph scan per level
    /// instead of one per qualified path.
    void prefetchDownstream(std::uint32_t depth) {
        std::unordered_set<std::uint32_t> seen;
        std::vector<std::uint32_t> level;
        for (std::size_t i = 0; i < st_.result->paths.size(); ++i) {
            if (st_.result->paths[i].syncType != SyncType::QualifiedData)
                continue;
            if (auto q = st_.qualifierPath.find(i); q != st_.qualifierPath.end())
                for (const auto& c : st_.chains[q->second])
                    for (const auto& [reg, bit] : c.stages)
                        if (seen.insert(SeqGraph::registerNode(reg)).second)
                            level.push_back(SeqGraph::registerNode(reg));
        }
        for (std::uint32_t d = 0; d < depth && !level.empty(); ++d) {
            st_.queryConsumers(level);
            std::vector<std::uint32_t> next;
            for (const auto n : level)
                for (const auto& c : st_.consumersOf(n))
                    if ((c.pin == Pin::Data || c.pin == Pin::Enable) &&
                        seen.insert(SeqGraph::registerNode(c.reg)).second)
                        next.push_back(SeqGraph::registerNode(c.reg));
            level = std::move(next);
        }
    }

    /// Registers within `depth` destination-domain register levels downstream
    /// of `from` (through data and enable pins), using the consumer cache.
    std::unordered_set<std::uint32_t> downstream(const std::unordered_set<std::uint32_t>& from, std::uint32_t depth) {
        std::unordered_set<std::uint32_t> seen(from.begin(), from.end());
        std::vector<std::uint32_t> level(from.begin(), from.end());
        for (std::uint32_t d = 0; d < depth && !level.empty(); ++d) {
            st_.queryConsumers(level);
            std::vector<std::uint32_t> next;
            for (const auto n : level)
                for (const auto& c : st_.consumersOf(n))
                    if ((c.pin == Pin::Data || c.pin == Pin::Enable) && st_.domain[c.reg] == st_.domain[n] &&
                        seen.insert(SeqGraph::registerNode(c.reg)).second)
                        next.push_back(SeqGraph::registerNode(c.reg));
            level = std::move(next);
        }
        return seen;
    }

    /// Registers in the control cone (enable, select) of register `g`, up
    /// to `depth` register levels upstream in its clock domain.
    std::unordered_set<std::uint32_t> controlCone(std::size_t g, std::uint32_t depth) const {
        std::unordered_set<std::uint32_t> seen;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> work{{SeqGraph::registerNode(g), 0}};
        for (std::size_t w = 0; w < work.size(); ++w) {
            const auto [node, d] = work[w];
            if (!st_.isRegister(node) || d > depth)
                continue;
            for (const Pin pin : {Pin::Enable, Pin::Data}) {
                ConeEntry scratch;
                for (const auto& bi : st_.sg.pinBits(node, pin))
                    for (const auto& e : st_.sg.entries(bi, scratch)) {
                        const bool control = pin == Pin::Enable || (e.flags & ConeEntry::kSelect) != 0 || d > 0;
                        if (control && seen.insert(e.node).second && st_.domain[e.node] == st_.domain[g])
                            work.emplace_back(e.node, d + 1);
                    }
            }
        }
        return seen;
    }

    void checkHandshake(std::size_t i) {
        auto& paths = st_.result->paths;
        auto qit = st_.qualifierPath.find(i);
        if (qit == st_.qualifierPath.end())
            return;
        const std::size_t r = qit->second;
        CdcPath& d = paths[i];
        const CdcPath& req = paths[r];
        auto backMatches = [&](std::size_t k) {
            const CdcPath& back = paths[k];
            return k != r && st_.related(back.srcClock, d.dstClock) && st_.related(back.dstClock, d.srcClock);
        };

        const bool memory = st_.sg.nodes()[d.srcNode].elemWidth > 0;
        if (!memory) {
            // Acknowledge: a single-bit synchronized path back to the source
            // clock that starts at a request-synchronizer stage (ack =
            // synchronized req), or at a register up to two levels after it.
            std::unordered_set<std::uint32_t> reqStages;
            for (const auto& c : st_.chains[r])
                for (const auto& [reg, bit] : c.stages)
                    reqStages.insert(SeqGraph::registerNode(reg));
            std::optional<std::size_t> best;
            auto pick = [&](const std::unordered_set<std::uint32_t>& sources) {
                std::vector<std::uint32_t> sorted(sources.begin(), sources.end());
                std::ranges::sort(sorted); // deterministic choice
                for (const auto n : sorted)
                    if (auto it = ackBySource_.find(n); it != ackBySource_.end())
                        for (const auto k : it->second)
                            if (!best && backMatches(k))
                                best = k;
            };
            pick(reqStages);
            if (!best)
                pick(downstream(reqStages, 2));
            if (best) {
                d.syncType = SyncType::Handshake;
                d.conditions.push_back({"the acknowledge '" + paths[*best].srcName + "' returns to clock " +
                                            d.srcClockName + " through a synchronizer",
                                        true});
                d.unverified.clear();
                d.unverified.push_back("the source must hold the data stable from the request until the acknowledge "
                                       "returns");
                st_.ackPath.emplace(i, *best);
            }
            return;
        }
        // Asynchronous FIFO: the write pointer (qualifier) is Gray, and the
        // read pointer of this FIFO - a register in the read data's control
        // cone (empty = rgray != wptr_sync) - returns to the write clock as Gray.
        if (req.syncType == SyncType::Gray) {
            const auto cone = controlCone(d.dstRegister, 2);
            std::vector<std::uint32_t> sorted(cone.begin(), cone.end());
            std::ranges::sort(sorted);
            for (const auto n : sorted)
                if (auto it = grayBySource_.find(n); it != grayBySource_.end())
                    for (const auto k : it->second)
                        if (backMatches(k)) {
                            const CdcPath& back = paths[k];
                            d.syncType = SyncType::AsyncFifo;
                            d.conditions.push_back(
                                {"the write pointer '" + req.srcName + "' reaches the read clock as Gray code", true});
                            d.conditions.push_back({"the read pointer '" + back.srcName +
                                                        "' returns to the write clock as Gray code",
                                                    true});
                            d.unverified.clear();
                            d.unverified.push_back("the full/empty comparison of the pointers and a power-of-two "
                                                   "depth are not checked");
                            st_.ackPath.emplace(i, k);
                            return;
                        }
        }
        d.notes.push_back("memory read qualified by a synchronized signal; no Gray pointer pair in both "
                          "directions was found, so this is not recognized as an asynchronous FIFO");
    }

    std::unordered_map<std::uint32_t, std::vector<std::size_t>> ackBySource_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> grayBySource_;
    State& st_;
    XorTracer xt_;
};

} // namespace

void recognizeProtocols(State& st) {
    ProtocolRecognizer(st).run();
}

// ----------------------------------------------------------------------
// Reconvergence, combined bits, mixed clocks
// ----------------------------------------------------------------------

namespace {

/// Interned sets of labels (path ids) with memoized unions. Id 0 is the
/// empty set.
class LabelSets {
public:
    LabelSets() { sets_.emplace_back(); }

    [[nodiscard]] const std::vector<std::size_t>& get(std::uint32_t id) const { return sets_[id]; }

    std::uint32_t single(std::size_t label) { return intern({label}); }

    std::uint32_t unite(std::uint32_t a, std::uint32_t b) {
        if (a == b || b == 0)
            return a;
        if (a == 0)
            return b;
        const auto key = a < b ? std::pair{a, b} : std::pair{b, a};
        if (auto it = unions_.find(key); it != unions_.end())
            return it->second;
        std::vector<std::size_t> merged;
        merged.reserve(sets_[a].size() + sets_[b].size());
        std::ranges::set_union(sets_[a], sets_[b], std::back_inserter(merged));
        const auto id = intern(std::move(merged));
        unions_.emplace(key, id);
        return id;
    }

private:
    std::uint32_t intern(std::vector<std::size_t> v) {
        std::size_t h = v.size();
        for (const auto x : v)
            h = h * 1000003u ^ x;
        auto& bucket = index_[h];
        for (const auto id : bucket)
            if (sets_[id] == v)
                return id;
        const auto id = static_cast<std::uint32_t>(sets_.size());
        sets_.push_back(std::move(v));
        bucket.push_back(id);
        return id;
    }

    std::vector<std::vector<std::size_t>> sets_; ///< sorted
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> index_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> unions_;
};

} // namespace

void analyzeConvergence(State& st) {
    auto& paths = st.result->paths;
    const auto& nodes = st.sg.nodes();

    // Clocks mixed into the crossing's logic (CDC-009).
    for (auto& p : paths) {
        if (!p.throughLogic || p.srcUnclocked || p.pin == Pin::AsyncReset)
            continue;
        std::set<std::string> names;
        const auto bits = st.sg.pinBits(p.dstRegister, p.pin);
        ConeEntry scratch;
        for (const auto b : p.dstBits) {
            if (b >= bits.size())
                continue;
            for (const auto& e : st.sg.entries(bits[b], scratch)) {
                if (e.node == p.srcNode)
                    continue;
                for (const auto& ce : st.domain[e.node]) {
                    const auto toDst = st.clocks.relation(ce.clock, p.dstClock.clock);
                    const auto toSrc = st.clocks.relation(ce.clock, p.srcClock.clock);
                    const bool crossesDst = toDst == ClockRelation::Asynchronous || toDst == ClockRelation::Unknown;
                    const bool otherThanSrc = toSrc == ClockRelation::Asynchronous || toSrc == ClockRelation::Unknown;
                    if (crossesDst && otherThanSrc)
                        names.insert(st.edgeName(ce));
                }
            }
        }
        p.mixedClocks.assign(names.begin(), names.end());
    }

    // One multi-bit source register split over separate single-bit
    // synchronizers (CDC-008): group per (source, destination clock).
    std::map<std::tuple<std::uint32_t, std::uint32_t, int>, std::vector<std::size_t>> split;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const CdcPath& p = paths[i];
        const bool levelChain = p.syncType == SyncType::TwoFlop || p.syncType == SyncType::ThreeFlop ||
                                p.syncType == SyncType::MultiFlop;
        if (p.pin == Pin::Data && levelChain && p.width == 1 && st.isRegister(p.srcNode) &&
            nodes[p.srcNode].width > 1 && !st.isConstantDataFlop(p.srcNode))
            split[{p.srcNode, p.dstClock.clock.value, static_cast<int>(p.dstClock.edge)}].push_back(i);
    }
    std::vector<std::size_t> splitGroup(paths.size(), kNoIndex);
    auto sameSplit = [&](std::size_t a, std::size_t b) {
        return splitGroup[a] != kNoIndex && splitGroup[a] == splitGroup[b] &&
               paths[a].srcBitLo != paths[b].srcBitLo;
    };
    std::size_t groupId = 0;
    for (const auto& [key, group] : split) {
        std::set<std::uint32_t> bits;
        for (const auto i : group)
            bits.insert(paths[i].srcBitLo);
        if (bits.size() < 2)
            continue; // one bit synchronized several times: divergence, see CDC-005
        // Paths of the same source bit (synchronized twice) are not "other bits".
        for (const auto i : group) {
            splitGroup[i] = groupId;
            paths[i].splitBits = bits.size();
            for (const auto j : group)
                if (paths[i].splitWith.size() < kMaxListed && i != j && paths[i].srcBitLo != paths[j].srcBitLo)
                    paths[i].splitWith.push_back(j);
        }
        ++groupId;
    }

    // Synchronizer outputs: the last stage of every chain (reset bridges excluded).
    std::map<StageRef, std::vector<std::size_t>> lastOf; // last stage bit -> paths
    std::vector<std::uint32_t> lastRegs;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const CdcPath& p = paths[i];
        if (p.pin != Pin::Data || !isChainType(p.syncType) || st.isConstantDataFlop(p.srcNode))
            continue;
        for (const auto& c : st.chains[i]) {
            lastOf[c.stages.back()].push_back(i);
            lastRegs.push_back(SeqGraph::registerNode(c.stages.back().first));
        }
    }
    if (lastOf.empty())
        return;
    std::ranges::sort(lastRegs);
    lastRegs.erase(std::unique(lastRegs.begin(), lastRegs.end()), lastRegs.end());
    st.queryConsumers(lastRegs);

    // Labels: which synchronizers (paths) a register bit depends on. Data
    // pin bits are tracked per bit; enable / reset pins affect every bit of
    // the register (kWhole). Bit-level labels keep packed registers whose
    // bits are unrelated from looking like reconvergence. Label sets are
    // interned (architecture §7.2: bitsets): the bits of a wide datapath
    // usually share one set, and unions are memoized.
    constexpr std::uint32_t kWhole = 0xFFFFFFFFu;
    LabelSets sets;
    using BitLabels = std::map<std::uint32_t, std::uint32_t>; // bit -> label set id
    std::unordered_map<std::size_t, BitLabels> labels;         // register -> bits
    std::map<std::pair<std::size_t, std::size_t>, std::set<StageRef>> perPathReads; // (path, reader) -> last bits
    // Adds `set` to bits [lo, hi] (or kWhole) of `reg`; true if anything changed.
    auto addSet = [&](std::size_t reg, std::uint32_t lo, std::uint32_t hi, std::uint32_t set) {
        bool changed = false;
        auto& bl = labels[reg];
        for (std::uint32_t bit = lo;; ++bit) {
            auto& id = bl[bit];
            const auto merged = sets.unite(id, set);
            changed = changed || merged != id;
            id = merged;
            if (bit == hi)
                break;
        }
        return changed;
    };
    auto pinRange = [&](const Consumer& c) {
        return c.pin == Pin::Data ? std::pair{c.bit, c.bitEnd} : std::pair{kWhole, kWhole};
    };

    // Level 0: direct readers of synchronizer outputs.
    std::unordered_set<std::size_t> seenAtZero;
    std::vector<std::size_t> frontier;
    for (const auto reg : lastRegs) {
        for (const auto& c : st.consumersOf(reg)) {
            if (c.pin == Pin::AsyncReset || st.domain[c.reg] != st.domain[reg])
                continue;
            std::uint32_t read = 0;
            for (auto it = lastOf.lower_bound({reg, c.lo}); it != lastOf.end() && it->first.first == reg &&
                                                            it->first.second <= c.hi;
                 ++it)
                for (const auto pi : it->second) {
                    read = sets.unite(read, sets.single(pi));
                    perPathReads[{pi, c.reg}].insert(it->first);
                }
            if (read == 0)
                continue;
            const auto [lo, hi] = pinRange(c);
            addSet(c.reg, lo, hi, read);
            if (seenAtZero.insert(c.reg).second)
                frontier.push_back(c.reg);
        }
    }
    for (const auto& [key, bitsRead] : perPathReads)
        if (bitsRead.size() >= 2)
            paths[key.first].bitsCombined = true;
    {
        // Bits of one split source read by one register.
        std::map<std::pair<std::size_t, std::size_t>, std::set<std::uint32_t>> groupReads; // (group, reader) -> bits
        for (const auto& [key, unused] : perPathReads)
            if (splitGroup[key.first] != kNoIndex)
                groupReads[{splitGroup[key.first], key.second}].insert(paths[key.first].srcBitLo);
        for (const auto& [key, unused] : perPathReads)
            if (splitGroup[key.first] != kNoIndex && groupReads[{splitGroup[key.first], key.second}].size() >= 2)
                paths[key.first].bitsCombined = true;
    }

    // Two labels qualify as reconvergence: different first stages, related
    // source clocks, and not two bits of one split source (CDC-008).
    auto related = [&](std::size_t a, std::size_t b) {
        const CdcPath& pa = paths[a];
        const CdcPath& pb = paths[b];
        return a != b && pa.dstRegister != pb.dstRegister && st.related(pa.srcClock, pb.srcClock) && !sameSplit(a, b);
    };
    // Number of qualifying partners of every label of a set in O(n * clocks)
    // instead of O(n^2): per source clock class, count the labels, then
    // subtract the ones that do not qualify (same first stage, other bit of
    // the same split source).
    auto partnerCounts = [&](const std::vector<std::size_t>& l) {
        struct ClassCounts {
            clock::ClockEdge clock;
            std::size_t total = 0;
            std::unordered_map<std::size_t, std::size_t> perDst;
            std::unordered_map<std::size_t, std::size_t> perGroup;
            std::map<std::pair<std::size_t, std::uint32_t>, std::size_t> perGroupBit;
        };
        std::vector<ClassCounts> classes;
        std::vector<std::size_t> classOf(l.size());
        for (std::size_t i = 0; i < l.size(); ++i) {
            const CdcPath& p = paths[l[i]];
            auto it = std::ranges::find_if(classes,
                                           [&](const ClassCounts& c) { return c.clock.clock == p.srcClock.clock; });
            if (it == classes.end()) {
                classes.push_back({p.srcClock, 0, {}, {}, {}});
                it = classes.end() - 1;
            }
            classOf[i] = static_cast<std::size_t>(it - classes.begin());
            ++it->total;
            ++it->perDst[p.dstRegister];
            if (const auto g = splitGroup[l[i]]; g != kNoIndex) {
                ++it->perGroup[g];
                ++it->perGroupBit[{g, p.srcBitLo}];
            }
        }
        std::vector<std::size_t> counts(l.size(), 0);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const CdcPath& p = paths[l[i]];
            const auto g = splitGroup[l[i]];
            for (const auto& c : classes) {
                if (!st.related(classes[classOf[i]].clock, c.clock))
                    continue;
                std::size_t n = c.total;
                if (auto it = c.perDst.find(p.dstRegister); it != c.perDst.end())
                    n -= it->second; // includes the label itself when c is its own class
                if (g != kNoIndex)
                    if (auto it = c.perGroup.find(g); it != c.perGroup.end()) {
                        auto bitIt = c.perGroupBit.find({g, p.srcBitLo});
                        n -= it->second - (bitIt == c.perGroupBit.end() ? 0 : bitIt->second);
                    }
                counts[i] += n;
            }
        }
        return counts;
    };
    // Set ids of the meeting bits of a register (whole-register labels merged into every bit).
    std::unordered_map<std::uint32_t, bool> meetsCache;
    auto meets = [&](std::uint32_t id) {
        if (auto it = meetsCache.find(id); it != meetsCache.end())
            return it->second;
        const auto counts = partnerCounts(sets.get(id));
        const bool found = std::ranges::any_of(counts, [](std::size_t n) { return n > 0; });
        meetsCache.emplace(id, found);
        return found;
    };
    auto meeting = [&](std::size_t reg) {
        std::vector<std::uint32_t> out;
        const auto& bl = labels[reg];
        const auto whole = bl.contains(kWhole) ? bl.at(kWhole) : 0U;
        for (const auto& [bit, id] : bl) {
            if (bit == kWhole && bl.size() > 1)
                continue;
            const auto all = sets.unite(id, whole);
            if (meets(all) && std::ranges::find(out, all) == out.end())
                out.push_back(all);
        }
        return out;
    };

    std::map<std::size_t, std::vector<std::uint32_t>> points; // register -> label sets of its meeting bits
    std::unordered_map<std::size_t, std::uint32_t> pointDepth;
    for (const auto reg : frontier)
        if (auto m = meeting(reg); !m.empty()) {
            points.emplace(reg, std::move(m));
            pointDepth.emplace(reg, 0);
        }

    // Forward propagation through destination-domain registers up to depth K.
    for (std::uint32_t depth = 1; depth <= st.opt.reconvergenceDepth && !frontier.empty(); ++depth) {
        std::vector<std::uint32_t> query;
        for (const auto reg : frontier)
            if (!points.contains(reg)) // report the first meeting point only
                query.push_back(SeqGraph::registerNode(reg));
        std::ranges::sort(query);
        st.queryConsumers(query);
        std::vector<std::size_t> next;
        for (const auto n : query) {
            const std::size_t reg = n;
            const BitLabels src = labels[reg];
            const auto whole = src.contains(kWhole) ? src.at(kWhole) : 0U;
            for (const auto& c : st.consumersOf(reg)) {
                if (c.pin == Pin::AsyncReset || st.domain[c.reg] != st.domain[reg] || c.reg == reg)
                    continue;
                // Union of the labels of the bits read.
                std::uint32_t read = whole;
                for (auto it = src.lower_bound(c.lo); it != src.end() && it->first <= c.hi; ++it)
                    read = sets.unite(read, it->second);
                if (read == 0)
                    continue;
                const auto [lo, hi] = pinRange(c);
                if (addSet(c.reg, lo, hi, read))
                    next.push_back(c.reg);
            }
        }
        std::ranges::sort(next);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        for (const auto reg : next)
            if (!points.contains(reg))
                if (auto m = meeting(reg); !m.empty()) {
                    points.emplace(reg, std::move(m));
                    pointDepth.emplace(reg, depth);
                }
        frontier = std::move(next);
    }

    // Report: each path gets one record per meeting register, listing (up to
    // kMaxListed of) the related paths it meets in any bit of that register.
    for (const auto& [reg, meetingSets] : points) {
        std::unordered_map<std::size_t, std::vector<std::size_t>> listed;
        std::unordered_map<std::size_t, std::size_t> counted;
        for (const auto id : meetingSets) {
            const auto& l = sets.get(id);
            const auto counts = partnerCounts(l);
            for (std::size_t i = 0; i < l.size(); ++i) {
                if (counts[i] == 0)
                    continue;
                const auto pa = l[i];
                auto& shown = listed[pa];
                for (std::size_t k = 0; k < l.size() && shown.size() < kMaxListed; ++k)
                    if (related(pa, l[k]) && std::ranges::find(shown, l[k]) == shown.end())
                        shown.push_back(l[k]);
                counted[pa] = std::max(counted[pa], counts[i]); // over all meeting bits (lower bound)
            }
        }
        for (auto& [pa, shown] : listed) {
            if (shown.empty() || paths[pa].reconvergences.size() >= 4)
                continue;
            Reconvergence rv;
            rv.at = nodes[reg].name;
            rv.loc = nodes[reg].loc;
            rv.depth = pointDepth[reg];
            std::ranges::sort(shown);
            rv.paths = std::move(shown);
            rv.partners = std::max(counted[pa], rv.paths.size());
            paths[pa].reconvergences.push_back(std::move(rv));
        }
    }
    st.result->stats.reconvergencePoints = points.size();
}

// ----------------------------------------------------------------------
// Structures
// ----------------------------------------------------------------------

void buildStructures(State& st) {
    auto& res = *st.result;
    auto& paths = res.paths;
    auto add = [&](SyncType type, const CdcPath& main) -> std::size_t {
        SyncStructure s;
        s.id = res.structures.size();
        s.type = type;
        s.name = main.dstName;
        s.srcClock = main.srcClockName;
        s.dstClock = main.dstClockName;
        s.status = main.syncStatus;
        s.registers = main.syncRegisters;
        res.structures.push_back(std::move(s));
        return res.structures.size() - 1;
    };
    auto attach = [&](std::size_t s, std::size_t pathId) {
        auto& v = res.structures[s].paths;
        if (std::ranges::find(v, pathId) == v.end())
            v.push_back(pathId);
        if (paths[pathId].structure == kNoIndex)
            paths[pathId].structure = s;
    };

    // Handshakes and FIFOs first: they include their request/acknowledge paths.
    std::map<std::tuple<SyncType, std::size_t, std::size_t>, std::size_t> protocol;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const CdcPath& p = paths[i];
        if (p.syncType != SyncType::Handshake && p.syncType != SyncType::AsyncFifo)
            continue;
        const std::size_t r = st.qualifierPath.at(i);
        const std::size_t k = st.ackPath.at(i);
        const auto key = std::make_tuple(p.syncType, r, k);
        auto it = protocol.find(key);
        if (it == protocol.end()) {
            const std::size_t s = add(p.syncType, p);
            auto& sv = res.structures[s];
            sv.name = p.syncType == SyncType::AsyncFifo ? p.srcName : paths[r].srcName;
            sv.registers.clear();
            for (const auto& n : {paths[r].srcName, paths[k].srcName})
                sv.registers.push_back(n);
            sv.notes.push_back((p.syncType == SyncType::AsyncFifo ? "write pointer: " : "request: ") +
                               paths[r].srcName + " -> " + paths[r].dstName);
            sv.notes.push_back((p.syncType == SyncType::AsyncFifo ? "read pointer: " : "acknowledge: ") +
                               paths[k].srcName + " -> " + paths[k].dstName);
            it = protocol.emplace(key, s).first;
        }
        attach(it->second, i);
        attach(it->second, r);
        attach(it->second, k);
        res.structures[it->second].registers.push_back(p.dstName);
    }
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const CdcPath& p = paths[i];
        if (p.syncType == SyncType::None || p.structure != kNoIndex)
            continue;
        const std::size_t s = add(p.syncType, p);
        if (p.syncType == SyncType::QualifiedData) {
            res.structures[s].registers.insert(res.structures[s].registers.begin(), p.dstName);
            if (auto q = st.qualifierPath.find(i); q != st.qualifierPath.end())
                res.structures[s].notes.push_back("qualifier synchronized from '" + paths[q->second].srcName + "'");
        }
        attach(s, i);
    }
}

} // namespace opencdc::cdc::detail

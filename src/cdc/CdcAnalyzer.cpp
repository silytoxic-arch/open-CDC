#include "cdc/CdcAnalyzer.h"

#include "cdc/AnalyzerState.h"
#include "ir/Dump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace opencdc::cdc {

using clock::ClockEdge;
using clock::ClockRelation;
using graph::BitRef;
using graph::DriverKind;
using namespace opencdc::ir;

// ----------------------------------------------------------------------
// State helpers
// ----------------------------------------------------------------------

namespace detail {

std::optional<BitRef> State::pinBit(std::size_t g, Pin pin, std::uint32_t bit) const {
    const auto [inst, r] = sg.localRegister(g);
    const auto& reg = view.bodyOf(inst).registers[r];
    switch (pin) {
        case Pin::Data: return view.bitOf(inst, reg.d, bit);
        case Pin::Enable: return reg.enable ? view.bitOf(inst, *reg.enable, 0) : std::nullopt;
        case Pin::SyncReset: return reg.syncReset ? view.bitOf(inst, reg.syncReset->signal, 0) : std::nullopt;
        case Pin::AsyncReset:
            return bit < reg.asyncResets.size() ? view.bitOf(inst, reg.asyncResets[bit].signal, 0) : std::nullopt;
    }
    return std::nullopt;
}

std::string State::stageName(std::size_t reg, std::uint32_t bit) const {
    const auto& n = sg.nodes()[reg];
    if (n.width <= 1)
        return n.name;
    return view.name(BitRef{n.first.inst, n.first.net, n.first.bit + bit});
}

bool State::isConstantDataFlop(std::uint32_t node) const {
    if (!isRegister(node))
        return false;
    const auto bits = sg.pinBits(node, Pin::Data);
    return !bits.empty() && std::ranges::all_of(bits, [](const BitInput& b) { return b.cone == 0; });
}

} // namespace detail

namespace {

using detail::Chain;
using detail::kMaxStages;
using detail::State;
using detail::SyncOut;

std::string fingerprintOf(std::string_view a, std::string_view b, std::string_view c) {
    std::uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (std::string_view part : {a, std::string_view("|"), b, std::string_view("|"), c})
        for (const char ch : part) {
            h ^= static_cast<unsigned char>(ch);
            h *= 1099511628211ULL;
        }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

class Analyzer {
public:
    Analyzer(const graph::DesignView& view, const clock::ClockModel& clocks, const constraints::ConstraintSet& cons,
             const CdcOptions& options, DiagnosticSink& sink)
        : st_(view, clocks, cons, options, sink) {}

    std::shared_ptr<CdcResult> run() {
        assignDomains();
        detail::findResetSynchronizers(st_);
        findCrossings();
        buildChains();
        classify();
        detail::classifyResetPaths(st_);
        detail::recognizeProtocols(st_);
        detail::analyzeConvergence(st_);
        finish();
        detail::buildStructures(st_);
        st_.result->nodes = st_.sg.takeNodes();
        return st_.result;
    }

private:
    // ------------------------------------------------------------------
    // Domains
    // ------------------------------------------------------------------

    void assignDomains() {
        const auto& nodes = st_.sg.nodes();
        st_.domain.assign(nodes.size(), {});
        for (std::size_t g = 0; g < st_.view.registerCount(); ++g) {
            const auto& rc = st_.clocks.registers.at(g);
            if (rc.status == clock::ClockStatus::Resolved)
                st_.domain[g] = rc.clocks;
        }
        // Inputs: set_input_delay -clock assigns a domain.
        std::unordered_map<BitRef, std::vector<ClockEdge>, graph::BitRefHash> inputClock;
        for (const auto& d : st_.cons.inputDelays) {
            auto id = st_.clocks.find(d.clock);
            if (!id) {
                st_.sink.report({"CDC-INPUT-CLOCK", Severity::Warning,
                                 "set_input_delay refers to unknown clock '" + d.clock +
                                     "'; the ports keep no clock domain",
                                 d.loc, {}});
                continue;
            }
            for (const auto& b : d.bits) {
                auto& v = inputClock[BitRef{b.inst, b.net, 0}];
                const ClockEdge ce{*id, d.clockFall ? Edge::Neg : Edge::Pos};
                if (std::ranges::find(v, ce) == v.end())
                    v.push_back(ce);
            }
        }
        for (std::size_t n = st_.view.registerCount(); n < nodes.size(); ++n) {
            if (nodes[n].kind != SourceKind::PrimaryInput)
                continue;
            if (auto it = inputClock.find(nodes[n].first); it != inputClock.end())
                st_.domain[n] = it->second;
        }
    }

    struct CrossInfo {
        bool crossing = false;
        ClockEdge src;
        ClockEdge dst;
        ClockRelation relation = ClockRelation::Unknown;
    };

    CrossInfo crossingBetween(const std::vector<ClockEdge>& s, const std::vector<ClockEdge>& d) const {
        CrossInfo best;
        int bestRank = 0;
        for (const auto& a : s) {
            for (const auto& b : d) {
                const ClockRelation rel = st_.clocks.relation(a.clock, b.clock);
                int rank = 0;
                if (rel == ClockRelation::Asynchronous)
                    rank = 2;
                else if (rel == ClockRelation::Unknown && st_.opt.unknownRelation != UnknownRelationPolicy::AssumeSync)
                    rank = 1;
                if (rank > bestRank) {
                    bestRank = rank;
                    best = {true, a, b, rel};
                }
            }
        }
        return best;
    }

    // ------------------------------------------------------------------
    // Crossings
    // ------------------------------------------------------------------

    struct Agg {
        std::uint32_t node;
        std::vector<std::uint32_t> dstBits;
        std::uint32_t lo = 0xFFFFFFFFu;
        std::uint32_t hi = 0;
        std::uint8_t flags = 0;
        bool allDirect = true;
    };

    void findCrossings() {
        std::set<std::uint32_t> noDomainSources;
        std::size_t unclocked = 0;
        const auto& nodes = st_.sg.nodes();
        for (std::size_t g = 0; g < st_.view.registerCount(); ++g) {
            const auto& dstDomain = st_.domain[g];
            // Whether a source node needs recording for this destination:
            // crossing, or without a clock domain on either side (reported).
            // Wide full-dependency cones read thousands of nodes per bit, so
            // the clock comparison is cached per node.
            std::unordered_map<std::uint32_t, bool> relevantCache;
            auto relevant = [&](std::uint32_t node) {
                auto [it, inserted] = relevantCache.try_emplace(node, false);
                if (inserted) {
                    const auto& srcDomain = st_.domain[node];
                    it->second = !(st_.isRegister(node) && st_.sg.neverLoads(node)) &&
                                 (dstDomain.empty() || srcDomain.empty() ||
                                  crossingBetween(srcDomain, dstDomain).crossing);
                }
                return it->second;
            };
            for (const Pin pin : {Pin::Data, Pin::Enable, Pin::SyncReset, Pin::AsyncReset}) {
                const auto bits = st_.sg.pinBits(g, pin);
                std::vector<Agg> aggs;
                std::unordered_map<std::uint32_t, std::size_t> aggIndex;
                ConeEntry scratch;
                for (std::uint32_t i = 0; i < bits.size(); ++i) {
                    const BitInput& bi = bits[i];
                    for (const auto& e : st_.sg.entries(bi, scratch)) {
                        if (e.node == SeqGraph::registerNode(g))
                            continue; // feedback from itself
                        if (!relevant(e.node))
                            continue; // same or related clock: nothing to record
                        auto [idx, inserted] = aggIndex.try_emplace(e.node, aggs.size());
                        if (inserted)
                            aggs.push_back(Agg{e.node, {}, 0xFFFFFFFFu, 0, 0, true});
                        Agg* it = &aggs[idx->second];
                        it->dstBits.push_back(i);
                        it->lo = std::min(it->lo, e.lo);
                        it->hi = std::max(it->hi, e.hi);
                        it->flags |= e.flags;
                        if (bi.directNode != e.node)
                            it->allDirect = false;
                    }
                }
                for (auto& a : aggs) {
                    const auto& srcDomain = st_.domain[a.node];
                    if (dstDomain.empty()) {
                        if (!srcDomain.empty())
                            ++unclocked;
                        else
                            noDomainSources.insert(a.node);
                        continue;
                    }
                    if (srcDomain.empty()) {
                        // An asynchronous reset from an input (or black box) without a clock
                        // domain is asynchronous to every clock: its release is checked (CDC-006).
                        const SourceKind k = nodes[a.node].kind;
                        if (pin == Pin::AsyncReset && (k == SourceKind::PrimaryInput || k == SourceKind::BlackBox)) {
                            ++st_.result->stats.asyncResetCrossings;
                            CrossInfo ci{true, {}, dstDomain.front(), ClockRelation::Asynchronous};
                            addResetPath(g, a, ci, true);
                        }
                        else {
                            noDomainSources.insert(a.node);
                        }
                        continue;
                    }
                    const CrossInfo ci = crossingBetween(srcDomain, dstDomain);
                    if (!ci.crossing)
                        continue;
                    if (pin == Pin::AsyncReset) {
                        ++st_.result->stats.asyncResetCrossings;
                        addResetPath(g, a, ci, false);
                        continue;
                    }
                    addPath(g, pin, a, ci);
                }
            }
        }
        auto& stats = st_.result->stats;
        stats.excludedNoDomainSources = noDomainSources.size();
        stats.excludedUnclockedDestinations = unclocked;
        stats.combLoops = st_.sg.combLoops();

        if (!noDomainSources.empty()) {
            std::map<SourceKind, std::pair<std::size_t, std::string>> byKind;
            for (const auto n : noDomainSources) {
                auto& e = byKind[nodes[n].kind];
                if (e.first++ == 0)
                    e.second = nodes[n].name;
            }
            for (const auto& [kind, e] : byKind) {
                std::string why;
                switch (kind) {
                    case SourceKind::PrimaryInput:
                        why = "input port(s) without a clock domain (use set_input_delay -clock to assign one)";
                        break;
                    case SourceKind::Latch: why = "latch(es); latch clock domains are not analyzed yet"; break;
                    case SourceKind::BlackBox: why = "black-box output(s) with unknown clock domain"; break;
                    case SourceKind::Register: why = "register(s) whose clock could not be resolved"; break;
                }
                st_.sink.report({"CDC-NO-DOMAIN", Severity::Info,
                                 std::to_string(e.first) + " " + why +
                                     " feed clocked registers; these connections are not checked for CDC (e.g. '" +
                                     e.second + "')",
                                 {}, {}});
            }
        }
        if (unclocked > 0)
            st_.sink.report({"CDC-NO-DOMAIN", Severity::Info,
                             std::to_string(unclocked) +
                                 " connection(s) into registers with an unresolved clock are not checked for CDC",
                             {}, {}});
    }

    CdcPath makePath(std::size_t g, Pin pin, const Agg& a, const CrossInfo& ci, bool unclockedSource) {
        const auto& nodes = st_.sg.nodes();
        CdcPath p;
        p.id = st_.result->paths.size();
        p.srcNode = a.node;
        p.dstRegister = g;
        p.pin = pin;
        p.srcClock = ci.src;
        p.dstClock = ci.dst;
        p.relation = ci.relation;
        p.srcUnclocked = unclockedSource;
        p.srcClockName = unclockedSource ? std::string("(asynchronous input)") : st_.edgeName(ci.src);
        p.dstClockName = st_.edgeName(ci.dst);
        p.srcName = nodes[a.node].name;
        p.dstName = nodes[g].name;
        p.srcLoc = nodes[a.node].loc;
        p.dstLoc = nodes[g].loc;
        p.srcBitLo = a.lo;
        p.srcBitHi = a.hi;
        p.dstBits = a.dstBits;
        p.throughLogic = (a.flags & ConeEntry::kLogic) != 0;
        p.throughSelect = (a.flags & ConeEntry::kSelect) != 0;
        return p;
    }

    void addPath(std::size_t g, Pin pin, const Agg& a, const CrossInfo& ci) {
        const auto& nodes = st_.sg.nodes();
        CdcPath p = makePath(g, pin, a, ci, false);
        // A memory read delivers one element per access, not the whole array.
        std::uint32_t srcBits = a.hi - a.lo + 1;
        if (nodes[a.node].elemWidth > 0)
            srcBits = std::min(srcBits, nodes[a.node].elemWidth);
        p.width = std::max<std::uint32_t>(static_cast<std::uint32_t>(a.dstBits.size()), srcBits);
        st_.allDirect.push_back(a.allDirect && !p.throughLogic);
        reconstruct(p);
        p.fingerprint = fingerprintOf(p.srcName, p.dstName, toString(pin));
        st_.result->paths.push_back(std::move(p));
    }

    /// Asynchronous reset crossings are grouped per (source, destination
    /// clock, reset synchronizer): one external reset typically resets
    /// thousands of registers of a clock domain.
    void addResetPath(std::size_t g, const Agg& a, const CrossInfo& ci, bool unclockedSource) {
        auto chainIt = st_.resetChainOf.find(g);
        const std::size_t chain = chainIt == st_.resetChainOf.end() ? static_cast<std::size_t>(-1) : chainIt->second;
        const auto key = std::make_tuple(a.node, ci.dst.clock.value, static_cast<int>(ci.dst.edge), chain);
        if (auto it = resetGroups_.find(key); it != resetGroups_.end()) {
            CdcPath& p = st_.result->paths[it->second];
            ++p.groupSize;
            p.throughLogic = p.throughLogic || (a.flags & ConeEntry::kLogic) != 0;
            st_.allDirect[it->second] = st_.allDirect[it->second] && a.allDirect && (a.flags & ConeEntry::kLogic) == 0;
            if (p.groupMembers.size() < kMaxGroupMembers)
                p.groupMembers.push_back(st_.sg.nodes()[g].name);
            return;
        }
        CdcPath p = makePath(g, Pin::AsyncReset, a, ci, unclockedSource);
        p.width = 1;
        p.groupMembers.push_back(p.dstName);
        st_.allDirect.push_back(a.allDirect && !p.throughLogic);
        reconstruct(p);
        // The fingerprint names the group by its source and destination clock,
        // so it stays stable when registers are added to or removed from the group.
        p.fingerprint = fingerprintOf(p.srcName, chain == static_cast<std::size_t>(-1) ? p.dstClockName : p.dstName,
                                      toString(Pin::AsyncReset));
        resetGroups_.emplace(key, p.id);
        st_.result->paths.push_back(std::move(p));
    }

    // ------------------------------------------------------------------
    // Path reconstruction (source -> destination steps)
    // ------------------------------------------------------------------

    static bool coneHas(const SeqGraph& sg, const BitInput& bi, std::uint32_t node) {
        ConeEntry scratch;
        const auto c = sg.entries(bi, scratch);
        return std::ranges::any_of(c, [&](const ConeEntry& e) { return e.node == node; });
    }

    void reconstruct(CdcPath& p) {
        std::vector<PathStep> rev; // destination -> source
        rev.push_back({p.dstName, "register", p.dstLoc});
        auto start = st_.pinBit(p.dstRegister, p.pin, p.dstBits.front());
        bool crossingSet = false;
        std::optional<BitRef> cur = start;
        for (int guard = 0; cur && guard < 256; ++guard) {
            std::vector<BitRef> hops;
            const auto d = st_.view.driver(*cur, {}, &hops);
            for (const auto& h : hops) {
                const Net& net = st_.view.bodyOf(h.inst).net(h.net);
                if (net.kind == NetKind::Temp)
                    continue;
                const std::string name = st_.view.design().instance(h.inst).hierPath + "." + net.name;
                if (!crossingSet) {
                    p.crossingSignal = name;
                    crossingSet = true;
                }
                if (rev.back().name != name)
                    rev.push_back({name, net.dir != PortDir::None ? "port" : "net", net.loc});
            }
            if (d.kind != DriverKind::Cell)
                break; // reached the source (register, input, ...)
            const Cell& c = st_.view.bodyOf(d.bit.inst).cells[d.index];
            rev.push_back({st_.view.design().instance(d.bit.inst).hierPath + "." +
                               st_.view.bodyOf(d.bit.inst).net(c.output).name,
                           "logic:" + std::string(toString(c.kind)), c.origin});
            cur.reset();
            for (const auto& ci : st_.sg.cellInputs(d.bit.inst, d.index, d.bit.bit)) {
                if (coneHas(st_.sg, st_.sg.inputOf(ci.bit), p.srcNode)) {
                    cur = ci.bit;
                    break;
                }
            }
        }
        const auto& src = st_.sg.nodes()[p.srcNode];
        if (rev.back().name != src.name)
            rev.push_back({src.name, std::string(toString(src.kind)), src.loc});
        if (!crossingSet)
            p.crossingSignal = src.name;
        p.steps.assign(rev.rbegin(), rev.rend());
    }

    // ------------------------------------------------------------------
    // Synchronizer chains
    // ------------------------------------------------------------------

    void buildChains() {
        // Level-by-level consumer queries: first stages, then the registers they feed directly.
        std::vector<std::uint32_t> level;
        for (const auto& p : st_.result->paths)
            if (p.pin == Pin::Data)
                level.push_back(SeqGraph::registerNode(p.dstRegister));
        std::ranges::sort(level);
        level.erase(std::unique(level.begin(), level.end()), level.end());
        std::unordered_set<std::uint32_t> expanded(level.begin(), level.end());
        for (std::uint32_t depth = 0; depth < kMaxStages && !level.empty(); ++depth) {
            st_.queryConsumers(level);
            std::vector<std::uint32_t> next;
            for (const auto n : level)
                for (const auto& c : st_.consumersOf(n))
                    if (c.pin == Pin::Data && c.direct && expanded.insert(SeqGraph::registerNode(c.reg)).second)
                        next.push_back(SeqGraph::registerNode(c.reg));
            std::ranges::sort(next);
            level = std::move(next);
        }
    }

    Chain chainFrom(std::size_t g, std::uint32_t bit) const {
        Chain ch;
        ch.stages.emplace_back(g, bit);
        std::size_t curReg = g;
        std::uint32_t curBit = bit;
        for (std::uint32_t s = 0; s + 1 < kMaxStages; ++s) {
            const Consumer* next = nullptr;
            std::size_t others = 0;
            for (const auto& c : st_.consumersOf(curReg)) {
                if (curBit < c.lo || curBit > c.hi)
                    continue;
                // A next stage may also be another bit of the same register
                // (vector shift-register synchronizers: s <= {s[0], d}).
                // A synchronizer stage samples every clock: a register with an
                // enable is a capture register, not a stage.
                const bool chainLink = c.pin == Pin::Data && c.direct && c.directBit == curBit &&
                                       st_.domain[c.reg] == st_.domain[curReg] && !(c.reg == curReg && c.bit == curBit) &&
                                       st_.sg.pinBits(c.reg, Pin::Enable).empty();
                if (chainLink && !next)
                    next = &c;
                else
                    others += c.bitEnd - c.bit + 1;
            }
            if (others > 0) {
                // A stage read by other logic once the minimum number of stages
                // is reached is the synchronizer output: the chain ends there
                // (s1 -> s2 -> s3 with pulse = s2 ^ s3: s3 is an edge-detector delay).
                if (s >= 1 && s + 1 >= st_.opt.minSyncStages)
                    break;
                ch.fanoutIssueStages.push_back(s);
                if (s == 0)
                    ch.extraFanoutFirst = others;
            }
            if (!next)
                break;
            if (std::ranges::any_of(ch.stages,
                                    [&](const auto& stg) { return stg.first == next->reg && stg.second == next->bit; }))
                break; // loop through the chain
            ch.stages.emplace_back(next->reg, next->bit);
            curReg = next->reg;
            curBit = next->bit;
        }
        return ch;
    }

    bool hasAsyncRegAttr(std::size_t g) const {
        const auto [inst, r] = st_.sg.localRegister(g);
        const auto& body = st_.view.bodyOf(inst);
        const Net& n = body.net(body.registers[r].q.chunks().front().net);
        const auto* a = n.findAttr("ASYNC_REG");
        if (!a)
            a = n.findAttr("async_reg");
        if (!a) {
            // on the instance of a primitive flip-flop: (* ASYNC_REG *) FDRE u (...)
            const auto& node = st_.view.design().instance(inst);
            if (node.parent.valid()) {
                const auto& pb = st_.view.bodyOf(node.parent);
                if (node.childIndex < pb.children.size())
                    for (const auto& at : pb.children[node.childIndex].attrs)
                        if (at.name == "ASYNC_REG" || at.name == "async_reg")
                            a = &at;
            }
        }
        if (!a)
            return false;
        // "TRUE" (Verilog string), "true" / TRUE (VHDL string or boolean): anything but false/0/no
        std::string v = a->value;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        for (auto& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v != "false" && v != "0" && v != "no";
    }

    // ------------------------------------------------------------------
    // Classification
    // ------------------------------------------------------------------

    void classify() {
        auto& paths = st_.result->paths;
        st_.chains.assign(paths.size(), {});
        std::set<std::size_t> firstStages;

        // Pass 1: flop chains (data pin only).
        for (std::size_t i = 0; i < paths.size(); ++i) {
            CdcPath& p = paths[i];
            if (p.pin != Pin::Data)
                continue;
            auto& chains = st_.chains[i];
            std::uint32_t minStages = kMaxStages;
            for (const auto b : p.dstBits) {
                chains.push_back(chainFrom(p.dstRegister, b));
                minStages = std::min<std::uint32_t>(minStages, static_cast<std::uint32_t>(chains.back().stages.size()));
            }
            if (minStages < 2)
                continue;
            // A destination with an enable captures under a qualifier (pass 2).
            if (!st_.sg.pinBits(p.dstRegister, Pin::Enable).empty())
                continue;
            const bool firstStageFanout =
                std::ranges::any_of(chains, [](const Chain& c) { return c.extraFanoutFirst > 0; });
            bool middleFanout = false;
            for (const auto& c : chains)
                for (const auto s : c.fanoutIssueStages)
                    if (s > 0 && s + 1 < c.stages.size())
                        middleFanout = true;
            p.syncStages = minStages;
            p.syncType = minStages == 2 ? SyncType::TwoFlop : minStages == 3 ? SyncType::ThreeFlop : SyncType::MultiFlop;
            const bool gated = !st_.allDirect[i] && singleAsyncInput(p);
            p.conditions.push_back({std::string(kConditionNoGlitchLogic), st_.allDirect[i] || gated});
            if (gated)
                p.notes.push_back("the first stage is gated by destination-clock logic (enable/clear); each of its bits "
                                  "has a single asynchronous input, so the logic cannot glitch");
            p.conditions.push_back({"the first stage drives only the second stage", !firstStageFanout});
            p.conditions.push_back({"intermediate stages drive only the next stage", !middleFanout});
            p.conditions.push_back({"all stages are clocked by the same clock and edge", true});
            const auto& c0 = chains.front();
            for (const auto& [reg, bit] : c0.stages)
                p.syncRegisters.push_back(st_.stageName(reg, bit));
            if (p.dstBits.size() > 1)
                p.notes.push_back("each of the " + std::to_string(p.dstBits.size()) +
                                  " bits has its own synchronizer chain (shown for the first bit)");
            const bool attrs = std::ranges::all_of(c0.stages, [&](const auto& stg) { return hasAsyncRegAttr(stg.first); });
            p.notes.push_back(attrs ? "all stages carry the ASYNC_REG attribute"
                                    : "not all stages carry an ASYNC_REG attribute (recommended for FPGA place & route)");
            if (firstStageFanout)
                p.notes.push_back("the first stage also drives " + std::to_string(c0.extraFanoutFirst) +
                                  " other register input(s): those readers see a possibly metastable value");
            const bool ok = std::ranges::all_of(p.conditions, [](const Condition& c) { return c.satisfied; });
            if (!ok) {
                p.syncStatus = SyncStatus::StructureRecognized;
            }
            else if (p.width > 1) {
                p.syncStatus = SyncStatus::ProtocolUnverified;
                p.unverified.push_back(
                    "the bits are synchronized independently: the source value must change by at most one bit at a "
                    "time (e.g. Gray code), otherwise the destination can capture an inconsistent value (CDC-008)");
            }
            else {
                p.syncStatus = SyncStatus::NecessaryConditionsVerified;
            }
            firstStages.insert(p.dstRegister);
            // A synchronizer whose source flop has a constant data input only
            // carries a reset release (reset bridge): it qualifies no data.
            if (st_.isConstantDataFlop(p.srcNode)) {
                p.notes.push_back("the source flop has a constant data input: this synchronizer carries a reset "
                                  "release (reset bridge)");
                continue;
            }
            for (const auto& c : chains) {
                const auto& last = c.stages.back();
                st_.syncOutputs.emplace(last.first, SyncOut{p.srcClock, st_.sg.nodes()[last.first].name, i});
                for (std::size_t s = 1; s + 1 < c.stages.size(); ++s)
                    st_.syncOutputs.emplace(c.stages[s].first, SyncOut{p.srcClock, st_.sg.nodes()[c.stages[s].first].name, i});
            }
        }
        st_.result->stats.synchronizers = firstStages.size();

        // Pass 2: qualified data, single flops, unsynchronized.
        for (std::size_t i = 0; i < paths.size(); ++i) {
            CdcPath& p = paths[i];
            if (p.syncType != SyncType::None || p.pin == Pin::AsyncReset)
                continue;
            if (p.pin == Pin::Data) {
                if (auto q = findQualifier(p)) {
                    p.syncType = SyncType::QualifiedData;
                    p.syncStatus = SyncStatus::ProtocolUnverified;
                    p.syncRegisters.push_back(q->name);
                    p.conditions.push_back(
                        {"the destination is enabled or selected by '" + q->name +
                             "', a synchronized signal from the source clock '" +
                             st_.clocks.clock(q->srcClock.clock).name + "'",
                         true});
                    p.unverified.push_back("the source data must be stable whenever the qualifier lets the "
                                           "destination capture it (handshake/protocol timing not checked)");
                    st_.qualifierPath.emplace(i, q->path);
                    continue;
                }
                const bool direct = st_.allDirect[i];
                const std::uint32_t chainLen =
                    st_.chains[i].empty() ? 0 : static_cast<std::uint32_t>(st_.chains[i].front().stages.size());
                if (direct && chainLen == 1) {
                    p.syncType = SyncType::SingleFlop;
                    p.syncStages = 1;
                    p.syncStatus = SyncStatus::StructureRecognized;
                    p.syncRegisters.push_back(p.dstName);
                    p.conditions.push_back({std::string(kConditionNoGlitchLogic), true});
                    p.conditions.push_back({"a second synchronizer stage follows the first", false});
                    continue;
                }
            }
            p.syncStatus = SyncStatus::NotSynchronized;
        }
    }

    /// Every destination bit of the path has exactly one asynchronous source
    /// bit (a bit of a clock unrelated to the destination clock, or of a
    /// source without clock); its other inputs are destination-clock signals
    /// or constants. Such logic (an enable or clear gate) cannot glitch.
    bool singleAsyncInput(const CdcPath& p) const {
        const auto bits = st_.sg.pinBits(p.dstRegister, p.pin);
        ConeEntry scratch;
        for (const auto b : p.dstBits) {
            if (b >= bits.size())
                return false;
            std::uint64_t async = 0;
            for (const auto& e : st_.sg.entries(bits[b], scratch)) {
                const auto& dom = st_.domain[e.node];
                const bool sync = !dom.empty() && std::ranges::any_of(dom, [&](const ClockEdge& ce) {
                    return st_.related(ce, p.dstClock);
                });
                if (!sync)
                    async += std::uint64_t{e.hi} - e.lo + 1;
            }
            if (async != 1)
                return false;
        }
        return true;
    }

    std::optional<SyncOut> findQualifier(const CdcPath& p) const {
        const auto g = p.dstRegister;
        // Control sources of the destination: the enable cone, and data-cone
        // sources that reach it through a mux select or an index. A
        // synchronous reset only clears the register; it qualifies nothing.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> work; // node, depth
        std::unordered_set<std::uint32_t> seen;
        auto pushControls = [&](std::size_t reg, std::uint32_t depth, bool includeData) {
            for (const Pin pin : {Pin::Enable, Pin::Data}) {
                ConeEntry scratch;
                for (const auto& bi : st_.sg.pinBits(reg, pin))
                    for (const auto& e : st_.sg.entries(bi, scratch)) {
                        const bool control = pin != Pin::Data || (e.flags & ConeEntry::kSelect) != 0 || includeData;
                        if (control && e.node != p.srcNode && seen.insert(e.node).second)
                            work.emplace_back(e.node, depth);
                    }
            }
        };
        pushControls(g, 0, false);
        for (std::size_t w = 0; w < work.size(); ++w) {
            const auto [node, depth] = work[w];
            if (!st_.isRegister(node))
                continue;
            if (auto it = st_.syncOutputs.find(node); it != st_.syncOutputs.end()) {
                if (st_.related(it->second.srcClock, p.srcClock))
                    return it->second;
            }
            // Follow registered control logic in the destination domain.
            if (depth < st_.opt.maxQualifierDepth && st_.domain[node] == st_.domain[g])
                pushControls(node, depth + 1, true);
        }
        return std::nullopt;
    }

    void finish() {
        auto& stats = st_.result->stats;
        stats.paths = st_.result->paths.size();
        for (auto& p : st_.result->paths) {
            const bool multi = p.width > 1;
            const bool data = p.pin == Pin::Data &&
                              (p.syncType == SyncType::None || p.syncType == SyncType::QualifiedData ||
                               p.syncType == SyncType::Handshake || p.syncType == SyncType::AsyncFifo);
            if (data)
                p.cls = multi ? CrossingClass::MultiBitData : CrossingClass::SingleBitData;
            else
                p.cls = multi ? CrossingClass::MultiBitControl : CrossingClass::SingleBitControl;
            if (p.relation == ClockRelation::Unknown)
                ++stats.unknownRelation;
            if (p.syncStatus == SyncStatus::NecessaryConditionsVerified ||
                p.syncStatus == SyncStatus::ProtocolUnverified)
                ++stats.synchronized;
            else
                ++stats.unsynchronized;
        }
    }

    static constexpr std::size_t kMaxGroupMembers = 8;

    State st_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, int, std::size_t>, std::size_t> resetGroups_;
};

} // namespace

std::shared_ptr<CdcResult> analyzeCdc(const graph::DesignView& view, const clock::ClockModel& clocks,
                                      const constraints::ConstraintSet& constraints, const CdcOptions& options,
                                      DiagnosticSink& sink) {
    Analyzer a(view, clocks, constraints, options, sink);
    return a.run();
}

} // namespace opencdc::cdc

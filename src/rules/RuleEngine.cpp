#include "rules/RuleEngine.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace opencdc::rules {

using namespace opencdc::cdc;

namespace {

RuleHit makeHit(std::string rule, Severity severity, AnalysisStatus status, std::string message) {
    RuleHit h;
    h.ruleId = std::move(rule);
    h.severity = severity;
    h.status = status;
    h.message = std::move(message);
    return h;
}

std::string pinPhrase(Pin pin) {
    switch (pin) {
        case Pin::Data: return "";
        case Pin::Enable: return " the clock enable of";
        case Pin::SyncReset: return " the synchronous reset of";
        case Pin::AsyncReset: return " the asynchronous reset of";
    }
    return "";
}

std::string crossingText(const CdcPath& p) {
    std::string s = "'" + p.srcName + "' (" + p.srcClockName + ") ->" + pinPhrase(p.pin) + " '" + p.dstName + "' (" +
                    p.dstClockName + ")";
    if (p.throughLogic)
        s += " through combinational logic";
    return s;
}

class Cdc001 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-001",
            "Single-bit signal crosses clock domains without a synchronizer",
            "Detect single-bit crossings that reach a destination register without any synchronizer. The destination "
            "can go metastable and different readers can see different values.",
            "Single-bit crossing (one source bit, one destination bit) between asynchronous clocks, or clocks with "
            "an unknown relation, that enters the destination through combinational logic or through an enable / "
            "synchronous reset pin.",
            Severity::Error,
            "Quasi-static signals (configuration written once while the destination clock is stopped) are reported "
            "too; waivers will be supported in a later version. Unknown clock relation downgrades the result to "
            "needs-review.",
            "Add a two-flop synchronizer (two registers in series in the destination domain, ASYNC_REG) and drive "
            "the destination logic from its output; register the signal in the source domain first so no "
            "combinational logic precedes the synchronizer."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext&) const override {
        if (p.pin == Pin::AsyncReset || p.width != 1 || p.syncType != SyncType::None)
            return std::nullopt;
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                       "unsynchronized single-bit crossing " + crossingText(p));
    }
};

class Cdc002 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-002",
            "Crossing passes only one synchronizer stage",
            "Detect crossings that are captured by a single destination flop whose output is used directly, or "
            "whose first synchronizer stage also feeds other logic. One stage leaves too little time for "
            "metastability to resolve.",
            "Single-bit crossing captured directly (wire or inverter) by a destination register when the flop chain "
            "is shorter than the configured minimum (default 2), or a recognized chain whose first stage has extra "
            "fanout.",
            Severity::Warning,
            "A destination register used only as the first stage of a hand-written synchronizer in another module is "
            "still recognized (the chain is followed across the hierarchy). Very slow destination clocks can make "
            "one stage sufficient; review those cases.",
            "Add a second register stage in the destination domain and use only the last stage; do not tap the "
            "first stage."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext& ctx) const override {
        if (p.pin == Pin::AsyncReset || p.width != 1)
            return std::nullopt;
        const bool shortChain = (p.syncType == SyncType::SingleFlop) ||
                                (p.syncStages > 0 && p.syncStages < ctx.options.minSyncStages &&
                                 p.syncType != SyncType::QualifiedData);
        const bool tappedFirst = std::ranges::any_of(p.conditions, [](const Condition& c) {
            return !c.satisfied && c.text == "the first stage drives only the second stage";
        });
        if (!shortChain && !tappedFirst)
            return std::nullopt;
        const std::string why = shortChain ? "only " + std::to_string(std::max<std::uint32_t>(p.syncStages, 1)) +
                                                 " synchronizer stage (minimum " +
                                                 std::to_string(ctx.options.minSyncStages) + ")"
                                           : "the first synchronizer stage also drives other logic";
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                       why + " for crossing " + crossingText(p));
    }
};

class Cdc003 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-003",
            "Multi-bit data crosses clock domains without synchronization",
            "Detect multi-bit values captured in another clock domain without a synchronizer or a synchronized "
            "qualifier. Bits can be captured from different source cycles (data incoherency).",
            "Crossing with more than one source or destination bit, between asynchronous clocks or clocks with an "
            "unknown relation, that is neither synchronized bit by bit nor captured under an enable/select driven "
            "by a synchronized control signal.",
            Severity::Error,
            "Data qualified by a control signal that is synchronized more than two register levels away from the "
            "destination enable is not recognized and is reported. Multi-bit values synchronized bit by bit are not "
            "reported here (see CDC-008).",
            "Transfer the data with a handshake or MUX synchronizer (capture under an enable driven by a "
            "synchronized request), an asynchronous FIFO, or Gray-code the value if it is a counter."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext&) const override {
        if (p.pin == Pin::AsyncReset || p.width <= 1 ||
            (p.syncType != SyncType::None && p.syncType != SyncType::SingleFlop))
            return std::nullopt;
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                       std::to_string(p.width) + "-bit crossing without synchronization " + crossingText(p));
    }
};

bool isChain(SyncType t) {
    return t == SyncType::TwoFlop || t == SyncType::ThreeFlop || t == SyncType::MultiFlop || t == SyncType::Toggle ||
           t == SyncType::Gray;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v)
        s += (s.empty() ? "'" : ", '") + x + "'";
    return s;
}

class Cdc004 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-004",
            "Combinational logic in front of a synchronizer",
            "Detect synchronizer chains whose first stage is fed through combinational logic. The logic can glitch "
            "while its inputs change, and the synchronizer may capture the glitch as a valid pulse.",
            "A recognized flop chain (two-flop and longer, toggle, Gray) whose first stage receives the crossing "
            "through logic that combines two or more asynchronous bits (bits of clocks other than the destination "
            "clock). Logic whose only other inputs are destination-clock signals or constants (an enable or clear "
            "gate) cannot glitch and is accepted.",
            Severity::Error,
            "Logic whose inputs are all quasi-static is reported too. Logic between two stages breaks the chain and "
            "is reported as a single-stage synchronizer (CDC-002) instead.",
            "Register the combined signal in the source clock domain and synchronize the register output; keep only "
            "wires (or one inverter) between the source flop and the first synchronizer stage."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext&) const override {
        if (p.pin != Pin::Data || !isChain(p.syncType) || p.syncStages < 2)
            return std::nullopt;
        const bool logic = std::ranges::any_of(p.conditions, [](const Condition& c) {
            return !c.satisfied && c.text == kConditionNoGlitchLogic;
        });
        if (!logic)
            return std::nullopt;
        std::string where;
        for (const auto& s : p.steps)
            if (s.kind.starts_with("logic:")) {
                where = " (first logic: " + s.kind.substr(6) + " '" + s.name + "')";
                break;
            }
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                       "combinational logic before synchronizer stage '" + p.dstName + "'" + where + " for crossing " +
                           crossingText(p));
    }
};

class Cdc005 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-005",
            "Reconvergence of independently synchronized signals",
            "Detect signals from one source clock domain that are synchronized separately and then combined again "
            "in the destination domain. Each synchronizer may resolve one destination cycle earlier or later, so "
            "the combination can see a state the source never had.",
            "Two or more recognized synchronizer chains with different first stages, whose source clocks are the "
            "same or synchronous, whose outputs meet in one destination register within the configured number of "
            "register levels (option reconvergenceDepth, default 2).",
            Severity::Warning,
            "Bits of one Gray-coded or toggle synchronizer count as one synchronizer. Signals that are logically "
            "independent (never compared or decoded together) are reported if they meet in one register; review "
            "and waive those.",
            "Synchronize related signals as one encoded value (Gray code), or transfer them with a handshake or "
            "asynchronous FIFO, or synchronize one qualifier and capture the others under it."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext& ctx) const override {
        if (p.reconvergences.empty())
            return std::nullopt;
        const auto& rv = p.reconvergences.front();
        std::vector<std::string> others;
        for (const auto id : rv.paths)
            if (id < ctx.cdc.paths.size() && others.size() < 3)
                others.push_back(ctx.cdc.paths[id].srcName);
        const std::size_t total = std::max(rv.partners, rv.paths.size());
        std::string msg = "synchronized '" + p.srcName + "' (" + p.srcClockName + ") reconverges with " +
                          joined(others) +
                          (total > others.size() ? " and " + std::to_string(total - others.size()) + " other(s)" : "") +
                          " at '" + rv.at + "'";
        msg += rv.depth == 0 ? " (first register after the synchronizers)"
                             : " (" + std::to_string(rv.depth) + " register level(s) after the synchronizers)";
        if (p.reconvergences.size() > 1)
            msg += "; " + std::to_string(p.reconvergences.size() - 1) + " more meeting point(s)";
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::PotentialRisk, msg);
    }
};

class Cdc006 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-006",
            "Asynchronous reset released without a reset synchronizer",
            "Detect asynchronous resets whose release (deassertion) is not synchronized to the clock of the "
            "registers they reset. A release close to the clock edge violates recovery/removal timing and can "
            "leave registers metastable or release them in different cycles.",
            "Registers whose asynchronous reset comes from another clock domain (Violation), or from an input / "
            "black box without a clock domain (PotentialRisk), without passing a reset synchronizer: flops of the "
            "destination clock that share the reset and whose first stage has a constant data input. Also reset "
            "synchronizers that fail a necessary condition (too few stages, tapped stages).",
            Severity::Error,
            "Paths are grouped per reset source and destination clock. An input reset that is synchronized outside "
            "the design is reported unless it is declared with set_input_delay -clock for that clock.",
            "Add a reset synchronizer per clock domain: two or more flops clocked by that clock, asynchronously "
            "reset by the raw reset, with a constant 1 (active-low) shifted through; use its output as the "
            "asynchronous reset of the domain's registers."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext&) const override {
        if (p.pin != Pin::AsyncReset)
            return std::nullopt;
        if (p.syncType == SyncType::ResetSync) {
            if (p.syncStatus != SyncStatus::StructureRecognized)
                return std::nullopt;
            std::string failed;
            for (const auto& c : p.conditions)
                if (!c.satisfied)
                    failed += (failed.empty() ? "" : "; ") + c.text;
            return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                           "reset synchronizer '" + p.dstName + "' for clock " + p.dstClockName +
                               " fails: " + failed);
        }
        const std::string regs = p.groupSize > 1 ? std::to_string(p.groupSize) + " registers" : "register";
        if (p.srcUnclocked)
            return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::PotentialRisk,
                           "asynchronous reset '" + p.srcName + "' (input without clock domain) is released "
                               "asynchronously to clock " +
                               p.dstClockName + " (" + regs + ", e.g. '" + p.dstName + "')");
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::Violation,
                       "asynchronous reset from '" + p.srcName + "' (" + p.srcClockName +
                           ") is released without a reset synchronizer in clock " + p.dstClockName + " (" + regs +
                           ", e.g. '" + p.dstName + "')");
    }
};

class Cdc007 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-007",
            "Pulse may be lost by a level synchronizer",
            "Detect single-cycle pulses synchronized with a level (flop-chain) synchronizer. A pulse shorter than "
            "the destination clock period plus setup/hold margin can fall between two destination edges and be "
            "lost.",
            "Single-bit crossing synchronized by a flop chain (not a toggle synchronizer) whose source register has "
            "no hold path (no enable and no feedback from itself), so it can be high for one source cycle. With "
            "clock periods known, reported when the source period is shorter than 1.5 times the destination "
            "period; without periods, reported as needs-review.",
            Severity::Warning,
            "A register without a hold path may still be driven by a stable level; the rule cannot see protocol "
            "guarantees (e.g. the pulse is stretched upstream). Periods come from create_clock / "
            "create_generated_clock.",
            "Use a toggle (pulse) synchronizer: toggle a flop in the source domain per event, synchronize it, and "
            "detect edges in the destination; or stretch the pulse / use a handshake."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext& ctx) const override {
        if (!p.sourceMayPulse || p.width != 1 || p.pin != Pin::Data || !isChain(p.syncType) ||
            p.syncType == SyncType::Toggle)
            return std::nullopt;
        const auto& sc = ctx.clocks.clock(p.srcClock.clock);
        const auto& dc = ctx.clocks.clock(p.dstClock.clock);
        if (!sc.periodNs || !dc.periodNs)
            return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::NeedsReview,
                           "'" + p.srcName + "' can pulse for one " + p.srcClockName +
                               " cycle and is synchronized as a level into " + p.dstClockName +
                               "; clock periods are unknown, so pulse capture cannot be checked");
        const double src = *sc.periodNs;
        const double dst = *dc.periodNs;
        if (src >= 1.5 * dst)
            return std::nullopt;
        char buf[160];
        std::snprintf(buf, sizeof(buf), " (source period %.4g ns < 1.5 x destination period %.4g ns = %.4g ns)", src,
                      dst, 1.5 * dst);
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::PotentialRisk,
                       "a one-cycle pulse of '" + p.srcName + "' (" + p.srcClockName +
                           ") can be missed by the level synchronizer '" + p.dstName + "' (" + p.dstClockName + ")" +
                           buf);
    }
};

class Cdc008 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-008",
            "Multi-bit value synchronized bit by bit",
            "Detect multi-bit values whose bits pass separate flop-chain synchronizers without Gray coding. "
            "Each bit resolves independently, so the destination can capture a mix of old and new bits.",
            "Crossing with more than one bit synchronized by per-bit flop chains, or bits of one multi-bit source "
            "register synchronized by separate single-bit synchronizers into one clock, where the source is not "
            "recognized as Gray-encoded. Potential risk if one destination register reads two or more of the "
            "synchronized bits; needs-review if every bit is used on its own inside the design.",
            Severity::Error,
            "Independent single-bit flags packed into one vector are fine when used separately (reported as "
            "needs-review). Gray encoders that the recognizer does not understand are reported; see the "
            "synchronizer conditions.",
            "Gray-code the value before synchronizing (counters/pointers), or transfer it with a handshake / "
            "MUX synchronizer or an asynchronous FIFO."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext& ctx) const override {
        const bool level = p.syncType == SyncType::TwoFlop || p.syncType == SyncType::ThreeFlop ||
                           p.syncType == SyncType::MultiFlop;
        if (p.pin != Pin::Data || !level || (p.width <= 1 && p.splitWith.empty()))
            return std::nullopt;
        std::string what;
        if (p.width > 1) {
            what = std::to_string(p.width) + "-bit value '" + p.srcName + "' is synchronized bit by bit";
        }
        else {
            what = "bit " + std::to_string(p.srcBitLo) + " of '" + p.srcName + "' is synchronized separately from " +
                   std::to_string(std::max<std::size_t>(p.splitBits, 1) - 1) + " other bit(s)";
            if (!p.splitWith.empty() && p.splitWith.front() < ctx.cdc.paths.size())
                what += " (e.g. into '" + ctx.cdc.paths[p.splitWith.front()].dstName + "')";
        }
        what += " without Gray coding";
        if (p.bitsCombined)
            return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::PotentialRisk,
                           what + "; the synchronized bits are read together in " + p.dstClockName);
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::NeedsReview,
                       what + "; the synchronized bits are not read together inside the design (independent "
                              "flags, or combined outside it?)");
    }
};

class Cdc009 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-009",
            "Crossing logic mixes signals from several clock domains",
            "Detect combinational logic on a crossing that combines signals from two or more clock domains that "
            "are asynchronous to the destination. The output can glitch whenever any of them changes, and no "
            "single source domain controls when it is stable.",
            "A crossing that passes combinational logic whose inputs also include sources from another clock "
            "(asynchronous or unknown to both the destination and this crossing's source clock), when the "
            "crossing is otherwise synchronized or qualified (unsynchronized crossings are already reported by "
            "CDC-001 / CDC-003).",
            Severity::Warning,
            "Pure inverters and buffers are not logic. Mixing with destination-domain signals is not reported "
            "here.",
            "Synchronize each source domain's signal separately into the destination domain and combine the "
            "synchronized signals there."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext&) const override {
        if (p.mixedClocks.empty() || p.syncType == SyncType::None || p.syncType == SyncType::SingleFlop)
            return std::nullopt;
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::PotentialRisk,
                       "logic in front of '" + p.dstName + "' combines '" + p.srcName + "' (" + p.srcClockName +
                           ") with signals from " + joined(p.mixedClocks));
    }
};

class Cdc010 final : public IRule {
public:
    const RuleMeta& meta() const override {
        static const RuleMeta m{
            "CDC-010",
            "Crossing between clocks with an unknown relation",
            "Point out crossings whose clocks come from independent sources that no constraint relates. Whether "
            "they are real CDC problems depends on the actual clock relation.",
            "Every crossing whose clock pair has the relation 'unknown' while the option unknownClockRelation is "
            "'report' (the default).",
            Severity::Warning,
            "Disappears once the clock relation is constrained (set_clock_groups, create_generated_clock) or the "
            "policy is set to assume_async / assume_sync.",
            "Describe the clocks in the constraint file: set_clock_groups -asynchronous for independent clocks, "
            "create_generated_clock for clocks derived from another clock."};
        return m;
    }
    std::optional<RuleHit> check(const CdcPath& p, const RuleContext& ctx) const override {
        if (p.relation != clock::ClockRelation::Unknown ||
            ctx.options.unknownRelation != UnknownRelationPolicy::Report)
            return std::nullopt;
        return makeHit(meta().id, meta().defaultSeverity, AnalysisStatus::NeedsReview,
                       "relation between '" + ctx.clocks.clock(p.srcClock.clock).name + "' and '" +
                           ctx.clocks.clock(p.dstClock.clock).name + "' is unknown: " +
                           ctx.clocks.reason(p.srcClock.clock, p.dstClock.clock));
    }
};

} // namespace

void RuleRegistry::add(std::unique_ptr<IRule> rule) {
    rules_.push_back(std::move(rule));
}

const IRule* RuleRegistry::find(std::string_view id) const {
    for (const auto& r : rules_)
        if (r->meta().id == id)
            return r.get();
    return nullptr;
}

RuleRegistry RuleRegistry::standard() {
    RuleRegistry r;
    r.add(std::make_unique<Cdc001>());
    r.add(std::make_unique<Cdc002>());
    r.add(std::make_unique<Cdc003>());
    r.add(std::make_unique<Cdc004>());
    r.add(std::make_unique<Cdc005>());
    r.add(std::make_unique<Cdc006>());
    r.add(std::make_unique<Cdc007>());
    r.add(std::make_unique<Cdc008>());
    r.add(std::make_unique<Cdc009>());
    r.add(std::make_unique<Cdc010>());
    return r;
}

void applyRules(const RuleRegistry& registry, CdcResult& result, const clock::ClockModel& clocks,
                const CdcOptions& options) {
    const RuleContext ctx{result, clocks, options};
    for (auto& p : result.paths) {
        p.hits.clear();
        for (const auto& rule : registry.rules())
            if (auto hit = rule->check(p, ctx))
                p.hits.push_back(std::move(*hit));

        const bool unknown = p.relation == clock::ClockRelation::Unknown &&
                             options.unknownRelation == UnknownRelationPolicy::Report;
        AnalysisStatus status = AnalysisStatus::Safe;
        for (auto& h : p.hits) {
            if (unknown && (h.status == AnalysisStatus::Violation || h.status == AnalysisStatus::PotentialRisk)) {
                h.status = AnalysisStatus::NeedsReview;
                h.message += " [clock relation unknown: needs review]";
            }
            status = std::max(status, h.status);
        }
        // A recognized structure whose necessary conditions fail is never "safe",
        // even when no implemented rule describes the failure yet.
        if (status == AnalysisStatus::Safe && p.syncStatus == SyncStatus::StructureRecognized)
            status = AnalysisStatus::NeedsReview;
        p.status = status;
    }
}

std::vector<std::pair<std::string, std::size_t>> countHits(const CdcResult& result) {
    std::map<std::string, std::size_t> m;
    for (const auto& p : result.paths)
        for (const auto& h : p.hits)
            ++m[h.ruleId];
    return {m.begin(), m.end()};
}

} // namespace opencdc::rules

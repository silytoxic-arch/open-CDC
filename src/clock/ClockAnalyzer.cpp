#include "clock/ClockAnalyzer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace opencdc::clock {

using graph::BitRef;
using graph::BitRefHash;
using graph::Driver;
using graph::DriverKind;
using namespace opencdc::ir;

namespace {

struct Trace {
    std::vector<std::pair<ClockId, bool>> sources; ///< clock, inverted
    bool gated = false;
    bool muxed = false;
    std::vector<BitRef> gating;
    ClockStatus status = ClockStatus::Unresolved;
    std::string reason;
};

void absorb(Trace& t, const Trace& sub, bool invert) {
    for (const auto& [c, inv] : sub.sources) {
        std::pair<ClockId, bool> s{c, inv != invert};
        if (std::ranges::find(t.sources, s) == t.sources.end())
            t.sources.push_back(s);
    }
    t.gated = t.gated || sub.gated;
    t.muxed = t.muxed || sub.muxed;
    t.gating.insert(t.gating.end(), sub.gating.begin(), sub.gating.end());
    if (!t.sources.empty())
        t.status = ClockStatus::Resolved;
    else if (t.status != ClockStatus::Resolved) {
        t.status = sub.status;
        if (t.reason.empty())
            t.reason = sub.reason;
    }
}

Trace unresolved(std::string why) {
    Trace t;
    t.status = ClockStatus::Unresolved;
    t.reason = std::move(why);
    return t;
}

bool looksLikeClockName(std::string_view n) {
    std::string lower;
    for (char c : n)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("clk") != std::string::npos || lower.find("clock") != std::string::npos;
}

const char* logicText(Logic l) {
    switch (l) {
        case Logic::L0: return "0";
        case Logic::L1: return "1";
        case Logic::X: return "x";
        case Logic::Z: return "z";
    }
    return "?";
}

class Analyzer {
public:
    Analyzer(const graph::DesignView& view, const constraints::ConstraintSet& cons, DiagnosticSink& sink)
        : view_(view), cons_(cons), sink_(sink), model_(std::make_shared<ClockModel>()) {}

    std::shared_ptr<const ClockModel> run() {
        createConstraintClocks();
        collectKnownRoots();
        traceAllRegisters();
        resolveGeneratedMasters();
        computeRelations();
        reportSummary();
        return model_;
    }

private:
    // ------------------------------------------------------------------
    // Clock creation
    // ------------------------------------------------------------------

    ClockId addClock(Clock c) {
        // Clock names must be unique: suffix automatically created clocks.
        std::string base = c.name;
        for (int n = 2; model_->find(c.name); ++n)
            c.name = base + "#" + std::to_string(n);
        model_->clocks.push_back(std::move(c));
        return ClockId{static_cast<std::uint32_t>(model_->clocks.size() - 1)};
    }

    SourceLocation netLoc(const BitRef& b) const { return view_.bodyOf(b.inst).net(b.net).loc; }

    void createConstraintClocks() {
        for (std::size_t i = 0; i < cons_.clocks.size(); ++i) {
            const auto& def = cons_.clocks[i];
            Clock c;
            c.name = def.name;
            c.kind = def.generated ? ClockKind::Generated : ClockKind::Constraint;
            if (!def.sources.empty()) {
                c.source = def.sources.front();
                c.sourceName = view_.name(def.sources.front());
            }
            c.periodNs = def.periodNs;
            c.waveform = def.waveform;
            c.definedAt = def.loc;
            const ClockId id = addClock(std::move(c));
            if (def.generated)
                generated_.emplace_back(id, i);
            for (const auto& b : def.sources)
                sourceClocks_[b].push_back(id);
        }
    }

    ClockId primaryClock(const BitRef& b) {
        if (auto it = primary_.find(b); it != primary_.end())
            return it->second;
        const Net& n = view_.bodyOf(b.inst).net(b.net);
        Clock c;
        c.name = n.name + ((n.width > 1) ? "[" + std::to_string(n.indexOf(b.bit)) + "]" : "");
        c.kind = ClockKind::Primary;
        c.source = b;
        c.sourceName = view_.name(b);
        c.definedAt = n.loc;
        const ClockId id = addClock(std::move(c));
        primary_.emplace(b, id);
        return id;
    }

    ClockId blackBoxClock(const BitRef& b) {
        if (auto it = blackBox_.find(b); it != blackBox_.end())
            return it->second;
        Clock c;
        c.name = view_.name(b);
        c.kind = ClockKind::BlackBox;
        c.source = b;
        c.sourceName = c.name;
        c.definedAt = netLoc(b);
        const ClockId id = addClock(std::move(c));
        blackBox_.emplace(b, id);
        return id;
    }

    ClockId dividerClock(const Driver& d, int depth) {
        if (auto it = divider_.find(d.bit); it != divider_.end())
            return it->second;
        Clock c;
        c.name = view_.name(d.bit);
        c.kind = ClockKind::Divider;
        c.source = d.bit;
        c.sourceName = c.name;
        c.definedAt = netLoc(d.bit);
        const ClockId id = addClock(std::move(c));
        divider_.emplace(d.bit, id); // before tracing the master: breaks self-clocking loops

        const auto& reg = view_.bodyOf(d.bit.inst).registers.at(d.index);
        auto clkBit = view_.bitOf(d.bit.inst, reg.clock.signal, 0);
        if (!clkBit)
            return id;
        const Trace m = trace(*clkBit, depth + 1);
        for (const auto& [src, inv] : m.sources) {
            if (src != id) {
                model_->clocks[id.index()].master = src;
                break;
            }
        }
        if (!model_->clocks[id.index()].master.valid()) {
            sink_.report({"CLK-UNRESOLVED", Severity::Warning,
                          "divider clock '" + view_.name(d.bit) + "': the clock of its register could not be traced" +
                              (m.reason.empty() ? "" : " (" + m.reason + ")"),
                          reg.origin, {}});
        }
        return id;
    }

    // ------------------------------------------------------------------
    // Tracing
    // ------------------------------------------------------------------

    bool isConstraintSource(const BitRef& b) const { return sourceClocks_.contains(b); }

    Driver driverOf(const BitRef& b) const {
        return view_.driver(b, [this](const BitRef& x) { return isConstraintSource(x); });
    }

    struct Input {
        std::optional<BitRef> bit;
        Logic constant = Logic::X;
    };
    Input input(const BitRef& at, const SigSpec& sig, std::uint32_t index) const {
        Input in;
        in.bit = view_.bitOf(at.inst, sig, index);
        if (!in.bit)
            in.constant = graph::DesignView::constBit(sig, index);
        return in;
    }

    void collectKnownRoots() {
        // Bits that clock registers through buffers/inverters only are
        // certainly clocks; they disambiguate gating logic later.
        const auto& d = view_.design();
        for (std::size_t i = 0; i < d.instances.size(); ++i) {
            const InstanceId inst{static_cast<std::uint32_t>(i)};
            for (const auto& r : view_.bodyOf(inst).registers) {
                auto b = view_.bitOf(inst, r.clock.signal, 0);
                for (int step = 0; b && step < 64; ++step) {
                    const Driver drv = driverOf(*b);
                    if (drv.kind == DriverKind::PrimaryInput || drv.kind == DriverKind::UnresolvedInstance) {
                        knownRoots_.insert(drv.bit);
                        break;
                    }
                    if (drv.kind != DriverKind::Cell)
                        break;
                    const Cell& c = view_.bodyOf(drv.bit.inst).cells[drv.index];
                    if (c.kind != CellKind::Not && !(c.kind == CellKind::LogicNot && c.inputs[0].width() == 1))
                        break;
                    b = view_.bitOf(drv.bit.inst, c.inputs[0], c.kind == CellKind::Not ? drv.bit.bit : 0);
                }
            }
        }
    }

    /// How clock-like a signal is, without creating clocks:
    /// 3 = known clock, 2 = input named like a clock, 1 = other input, 0 = data.
    int score(const BitRef& b, int depth) const {
        if (depth > 16)
            return 0;
        const Driver d = driverOf(b);
        if (d.kind == DriverKind::Undriven && isConstraintSource(d.bit))
            return 3;
        switch (d.kind) {
            case DriverKind::PrimaryInput:
                if (knownRoots_.contains(d.bit))
                    return 3;
                return looksLikeClockName(view_.bodyOf(d.bit.inst).net(d.bit.net).name) ? 2 : 1;
            case DriverKind::UnresolvedInstance: return knownRoots_.contains(d.bit) ? 3 : 1;
            case DriverKind::Register: return divider_.contains(d.bit) ? 3 : 0;
            case DriverKind::Cell: {
                const Cell& c = view_.bodyOf(d.bit.inst).cells[d.index];
                int best = 0;
                switch (c.kind) {
                    case CellKind::Not:
                    case CellKind::And:
                    case CellKind::Or:
                    case CellKind::Xor:
                    case CellKind::Xnor:
                    case CellKind::Mux:
                        for (std::size_t k = 0; k < c.inputs.size() && k < 2; ++k)
                            if (auto in = view_.bitOf(d.bit.inst, c.inputs[k], d.bit.bit))
                                best = std::max(best, score(*in, depth + 1));
                        return best;
                    case CellKind::LogicNot:
                        if (c.inputs[0].width() == 1)
                            if (auto in = view_.bitOf(d.bit.inst, c.inputs[0], 0))
                                return score(*in, depth + 1);
                        return 0;
                    default: return 0;
                }
            }
            default: return 0;
        }
    }

    Trace trace(const BitRef& b, int depth) {
        if (auto it = memo_.find(b); it != memo_.end())
            return it->second;
        if (depth > 256)
            return unresolved("clock path deeper than 256 levels at '" + view_.name(b) + "'");
        if (!inProgress_.insert(b).second)
            return unresolved("combinational loop on the clock path at '" + view_.name(b) + "'");

        Trace t;
        const Driver d = driverOf(b);
        if (d.kind == DriverKind::Undriven && isConstraintSource(d.bit)) {
            for (const ClockId id : sourceClocks_.at(d.bit))
                t.sources.emplace_back(id, false);
            t.status = ClockStatus::Resolved;
        }
        else {
            switch (d.kind) {
                case DriverKind::PrimaryInput:
                    t.sources.emplace_back(primaryClock(d.bit), false);
                    t.status = ClockStatus::Resolved;
                    break;
                case DriverKind::UnresolvedInstance:
                    t.sources.emplace_back(blackBoxClock(d.bit), false);
                    t.status = ClockStatus::Resolved;
                    break;
                case DriverKind::Constant:
                    t.status = ClockStatus::Constant;
                    t.reason = std::string("clock is tied to constant ") + logicText(d.constant);
                    break;
                case DriverKind::Undriven: t = unresolved("clock net '" + view_.name(d.bit) + "' is undriven"); break;
                case DriverKind::Loop: t = unresolved("alias loop at '" + view_.name(d.bit) + "'"); break;
                case DriverKind::Latch:
                    t = unresolved("clock is driven by latch output '" + view_.name(d.bit) + "'");
                    break;
                case DriverKind::Register:
                    t.sources.emplace_back(dividerClock(d, depth), false);
                    t.status = ClockStatus::Resolved;
                    break;
                case DriverKind::Cell: t = traceCell(d, depth); break;
            }
        }
        inProgress_.erase(b);
        memo_.emplace(b, t);
        return t;
    }

    Trace traceInput(const Input& in, int depth) {
        if (!in.bit) {
            Trace t;
            t.status = ClockStatus::Constant;
            t.reason = std::string("clock is tied to constant ") + logicText(in.constant);
            return t;
        }
        return trace(*in.bit, depth + 1);
    }

    Trace traceCell(const Driver& d, int depth) {
        const Cell& c = view_.bodyOf(d.bit.inst).cells[d.index];
        const std::uint32_t ob = d.bit.bit;
        const std::string at = view_.name(d.bit);
        Trace t;

        switch (c.kind) {
            case CellKind::Not: absorb(t, traceInput(input(d.bit, c.inputs[0], ob), depth), true); return t;
            case CellKind::LogicNot:
                if (c.inputs[0].width() != 1)
                    return unresolved("clock passes through a multi-bit logical NOT at '" + at + "'");
                absorb(t, traceInput(input(d.bit, c.inputs[0], 0), depth), true);
                return t;
            case CellKind::And:
            case CellKind::Or:
            case CellKind::Xor:
            case CellKind::Xnor: {
                const Input a = input(d.bit, c.inputs[0], ob);
                const Input b = input(d.bit, c.inputs[1], ob);
                const char* op = c.kind == CellKind::And   ? "AND"
                                 : c.kind == CellKind::Or  ? "OR"
                                 : c.kind == CellKind::Xor ? "XOR"
                                                           : "XNOR";
                if (!a.bit || !b.bit) {
                    const Input& sig = a.bit ? a : b;
                    const Logic k = a.bit ? b.constant : a.constant;
                    if (!sig.bit)
                        return traceInput(sig, depth);
                    const bool blocks = (c.kind == CellKind::And && k == Logic::L0) ||
                                        (c.kind == CellKind::Or && k == Logic::L1);
                    if (blocks) {
                        Trace k0;
                        k0.status = ClockStatus::Constant;
                        k0.reason = std::string("clock is blocked by constant ") + logicText(k) + " at " + op + " '" + at + "'";
                        return k0;
                    }
                    const bool invert = (c.kind == CellKind::Xor && k == Logic::L1) ||
                                        (c.kind == CellKind::Xnor && k == Logic::L0);
                    absorb(t, traceInput(sig, depth), invert);
                    return t;
                }
                const int sa = score(*a.bit, 0);
                const int sb = score(*b.bit, 0);
                const bool invert = c.kind == CellKind::Xnor;
                if (sa >= 2 && sb >= 2) {
                    absorb(t, trace(*a.bit, depth + 1), invert);
                    absorb(t, trace(*b.bit, depth + 1), invert);
                    t.muxed = true;
                    t.reason = std::string("two clocks combined by ") + op + " at '" + at + "'";
                    return t;
                }
                if (sa != sb && std::max(sa, sb) >= 1) {
                    const BitRef& clk = sa > sb ? *a.bit : *b.bit;
                    const BitRef& gate = sa > sb ? *b.bit : *a.bit;
                    absorb(t, trace(clk, depth + 1), invert);
                    t.gated = true;
                    t.gating.push_back(gate);
                    return t;
                }
                return unresolved(std::string("cannot tell which input of the ") + op + " gate at '" + at +
                                  "' is the clock ('" + view_.name(*a.bit) + "' or '" + view_.name(*b.bit) + "')");
            }
            case CellKind::Mux: {
                const Input a = input(d.bit, c.inputs[0], ob);
                const Input b = input(d.bit, c.inputs[1], ob);
                const Input s = input(d.bit, c.inputs[2], 0);
                if (!a.bit && !b.bit)
                    return traceInput(a, depth);
                if (!a.bit || !b.bit) {
                    absorb(t, traceInput(a.bit ? a : b, depth), false);
                    t.gated = true;
                    if (s.bit)
                        t.gating.push_back(*s.bit);
                    return t;
                }
                const int sa = score(*a.bit, 0);
                const int sb = score(*b.bit, 0);
                if (sa >= 1 && sb >= 1) {
                    absorb(t, trace(*a.bit, depth + 1), false);
                    absorb(t, trace(*b.bit, depth + 1), false);
                    t.muxed = true;
                    if (s.bit)
                        t.gating.push_back(*s.bit);
                    return t;
                }
                if (std::max(sa, sb) >= 1) {
                    absorb(t, trace(sa > sb ? *a.bit : *b.bit, depth + 1), false);
                    t.gated = true;
                    if (s.bit)
                        t.gating.push_back(*s.bit);
                    t.gating.push_back(sa > sb ? *b.bit : *a.bit);
                    return t;
                }
                return unresolved("clock multiplexer at '" + at + "' selects between non-clock signals");
            }
            default:
                return unresolved("clock passes through '" + std::string(toString(c.kind)) + "' logic at '" + at + "'");
        }
    }

    void traceAllRegisters() {
        const auto& d = view_.design();
        model_->registers.assign(view_.registerCount(), RegisterClocking{});
        std::map<std::string, std::pair<std::size_t, std::string>> unresolvedByReason; // reason -> count, example
        std::map<std::string, std::pair<std::size_t, SourceLocation>> constantByReg;
        std::size_t gated = 0;
        std::size_t muxed = 0;

        for (std::size_t i = 0; i < d.instances.size(); ++i) {
            const InstanceId inst{static_cast<std::uint32_t>(i)};
            const auto& body = view_.bodyOf(inst);
            model_->latchCount += body.latches.size();
            for (std::size_t r = 0; r < body.registers.size(); ++r) {
                const auto& reg = body.registers[r];
                RegisterClocking& rc = model_->registers[view_.globalRegister(inst, r)];
                Trace t;
                if (auto clk = view_.bitOf(inst, reg.clock.signal, 0)) {
                    t = trace(*clk, 0);
                }
                else {
                    t.status = ClockStatus::Constant;
                    t.reason = std::string("clock is tied to constant ") +
                               logicText(graph::DesignView::constBit(reg.clock.signal, 0));
                }
                rc.status = t.sources.empty() ? (t.status == ClockStatus::Resolved ? ClockStatus::Unresolved : t.status)
                                              : ClockStatus::Resolved;
                rc.gated = t.gated;
                rc.muxed = t.muxed;
                rc.gatingSignals = t.gating;
                rc.reason = t.reason;
                for (const auto& [id, inv] : t.sources) {
                    const Edge e = inv ? (reg.clock.edge == Edge::Pos ? Edge::Neg : Edge::Pos) : reg.clock.edge;
                    rc.clocks.push_back({id, e});
                    auto& c = model_->clocks[id.index()];
                    (e == Edge::Pos ? c.posedgeRegisters : c.negedgeRegisters)++;
                    if (t.gated)
                        ++c.gatedRegisters;
                }
                const std::string qName = d.instance(inst).hierPath + "." + formatSigName(body, reg.q);
                if (rc.status == ClockStatus::Unresolved) {
                    auto& e = unresolvedByReason[rc.reason];
                    if (e.first++ == 0)
                        e.second = qName;
                }
                else if (rc.status == ClockStatus::Constant) {
                    auto& e = constantByReg[rc.reason];
                    if (e.first++ == 0)
                        e.second = reg.origin;
                }
                gated += rc.gated ? 1 : 0;
                muxed += rc.muxed ? 1 : 0;
            }
        }

        for (const auto& [why, e] : unresolvedByReason)
            sink_.report({"CLK-UNRESOLVED", Severity::Warning,
                          std::to_string(e.first) + " register(s) with unresolved clock: " + why + " (e.g. '" + e.second +
                              "'); they are excluded from CDC analysis",
                          {}, {}});
        for (const auto& [why, e] : constantByReg)
            sink_.report({"CLK-CONSTANT", Severity::Warning,
                          std::to_string(e.first) + " register(s) never toggle: " + why, e.second, {}});
        if (gated)
            sink_.report({"CLK-GATED", Severity::Info, std::to_string(gated) + " register(s) use a gated clock", {}, {}});
        if (muxed)
            sink_.report({"CLK-MUX", Severity::Info,
                          std::to_string(muxed) + " register(s) are clocked through a clock multiplexer and belong to "
                                                  "every selectable clock",
                          {}, {}});
    }

    static std::string formatSigName(const ModuleBody& b, const SigSpec& q) {
        const auto& ch = q.chunks().front();
        const Net& n = b.net(ch.net);
        if (ch.offset == 0 && ch.width == n.width)
            return n.name;
        if (ch.width == 1)
            return n.name + "[" + std::to_string(n.indexOf(ch.offset)) + "]";
        return n.name + "[" + std::to_string(n.indexOf(ch.offset + ch.width - 1)) + ":" +
               std::to_string(n.indexOf(ch.offset)) + "]";
    }

    void resolveGeneratedMasters() {
        for (const auto& [id, defIndex] : generated_) {
            const auto& def = cons_.clocks[defIndex];
            ClockId master;
            if (!def.masterClock.empty()) {
                if (auto m = model_->find(def.masterClock))
                    master = *m;
                else
                    sink_.report({"CON-UNMATCHED", Severity::Warning,
                                  "create_generated_clock '" + def.name + "': master clock '" + def.masterClock +
                                      "' does not exist",
                                  def.loc, {}});
            }
            else if (!def.masterSource.empty()) {
                const Trace t = trace(def.masterSource.front(), 0);
                for (const auto& [src, inv] : t.sources)
                    if (src != id) {
                        master = src;
                        break;
                    }
                if (!master.valid())
                    sink_.report({"CON-UNMATCHED", Severity::Warning,
                                  "create_generated_clock '" + def.name + "': no clock reaches the -source pin" +
                                      (t.reason.empty() ? "" : " (" + t.reason + ")"),
                                  def.loc, {}});
            }
            auto& c = model_->clocks[id.index()];
            c.master = master;
            if (master.valid() && !c.periodNs && model_->clocks[master.index()].periodNs)
                c.periodNs = *model_->clocks[master.index()].periodNs * def.divideBy / def.multiplyBy;
        }
        // Derived clocks without constraints inherit nothing but the master link.
    }

    // ------------------------------------------------------------------
    // Relations
    // ------------------------------------------------------------------

    std::vector<ClockId> ancestors(ClockId c) const {
        std::vector<ClockId> out{c};
        for (std::size_t guard = 0; guard < model_->clocks.size(); ++guard) {
            const ClockId m = model_->clocks[out.back().index()].master;
            if (!m.valid() || std::ranges::find(out, m) != out.end())
                break;
            out.push_back(m);
        }
        return out;
    }

    void computeRelations() {
        auto& m = *model_;
        m.initRelations();
        const std::size_t n = m.clocks.size();

        // Resolve set_clock_groups members against the final clock list.
        struct Resolved {
            const constraints::ClockGroups* src;
            std::vector<std::vector<bool>> member; // group -> clock -> member
        };
        std::vector<Resolved> groups;
        for (const auto& g : cons_.groups) {
            Resolved r{&g, {}};
            for (const auto& patterns : g.groups) {
                std::vector<bool> mem(n, false);
                for (const auto& p : patterns) {
                    bool any = false;
                    for (std::size_t c = 0; c < n; ++c)
                        if (constraints::globMatch(p, m.clocks[c].name)) {
                            mem[c] = true;
                            any = true;
                        }
                    if (!any)
                        sink_.report({"CON-UNMATCHED", Severity::Warning,
                                      "set_clock_groups: no clock matches '" + p + "'", g.loc, {}});
                }
                r.member.push_back(std::move(mem));
            }
            groups.push_back(std::move(r));
        }

        auto groupOf = [&](const Resolved& r, ClockId c, ClockId& via) -> int {
            for (const ClockId a : ancestors(c))
                for (std::size_t g = 0; g < r.member.size(); ++g)
                    if (r.member[g][a.index()]) {
                        via = a;
                        return static_cast<int>(g);
                    }
            return -1;
        };

        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                const ClockId a{static_cast<std::uint32_t>(i)};
                const ClockId b{static_cast<std::uint32_t>(j)};
                bool decided = false;
                for (const auto& r : groups) {
                    ClockId viaA;
                    ClockId viaB;
                    const int ga = groupOf(r, a, viaA);
                    const int gb = groupOf(r, b, viaB);
                    bool related = false;
                    if (r.member.size() == 1)
                        related = (ga == 0) != (gb == 0); // one group: asynchronous to all other clocks
                    else
                        related = ga >= 0 && gb >= 0 && ga != gb;
                    if (!related)
                        continue;
                    const auto kind = r.src->kind;
                    std::string why = std::string("set_clock_groups ") +
                                      (kind == constraints::GroupKind::Asynchronous          ? "-asynchronous"
                                       : kind == constraints::GroupKind::LogicallyExclusive ? "-logically_exclusive"
                                                                                             : "-physically_exclusive") +
                                      " at " + toString(r.src->loc);
                    if (ga >= 0 && viaA != a)
                        why += " (via master clock '" + m.clocks[viaA.index()].name + "' of '" + m.clocks[i].name + "')";
                    if (gb >= 0 && viaB != b)
                        why += " (via master clock '" + m.clocks[viaB.index()].name + "' of '" + m.clocks[j].name + "')";
                    m.setRelation(a, b,
                                  kind == constraints::GroupKind::Asynchronous ? ClockRelation::Asynchronous
                                                                               : ClockRelation::Exclusive,
                                  std::move(why));
                    decided = true;
                    break;
                }
                if (decided)
                    continue;

                const auto ancA = ancestors(a);
                const auto ancB = ancestors(b);
                if (std::ranges::find(ancB, a) != ancB.end()) {
                    m.setRelation(a, b, ClockRelation::Synchronous,
                                  "'" + m.clocks[j].name + "' is derived from '" + m.clocks[i].name + "'");
                }
                else if (std::ranges::find(ancA, b) != ancA.end()) {
                    m.setRelation(a, b, ClockRelation::Synchronous,
                                  "'" + m.clocks[i].name + "' is derived from '" + m.clocks[j].name + "'");
                }
                else if (ancA.back() == ancB.back()) {
                    m.setRelation(a, b, ClockRelation::Synchronous,
                                  "both are derived from '" + m.clocks[ancA.back().index()].name + "'");
                }
                else {
                    auto describe = [&](const Clock& c) {
                        std::string s = "'" + c.name + "' (" + std::string(toString(c.kind));
                        if (!c.sourceName.empty())
                            s += " " + c.sourceName;
                        return s + ")";
                    };
                    m.setRelation(a, b, ClockRelation::Unknown,
                                  "independent sources " + describe(m.clocks[ancA.back().index()]) + " and " +
                                      describe(m.clocks[ancB.back().index()]) +
                                      "; no set_clock_groups or generated-clock constraint relates them");
                }
            }
        }
    }

    void reportSummary() {
        const auto& m = *model_;
        const std::size_t n = m.clocks.size();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                const ClockId a{static_cast<std::uint32_t>(i)};
                const ClockId b{static_cast<std::uint32_t>(j)};
                if (m.relation(a, b) == ClockRelation::Unknown && m.clocks[i].registers() > 0 &&
                    m.clocks[j].registers() > 0)
                    sink_.report({"CLK-RELATION-UNKNOWN", Severity::Info,
                                  "relation of clocks '" + m.clocks[i].name + "' and '" + m.clocks[j].name +
                                      "' is unknown: " + m.reason(a, b),
                                  {}, {}});
            }
        }
        for (const auto& c : m.clocks)
            if ((c.kind == ClockKind::Constraint || c.kind == ClockKind::Generated) && c.source.valid() &&
                c.registers() == 0 && !std::ranges::any_of(m.clocks, [&](const Clock& o) {
                    return o.master.valid() && &m.clocks[o.master.index()] == &c;
                }))
                sink_.report({"CLK-UNUSED", Severity::Info,
                              "constrained clock '" + c.name + "' does not clock any register", c.definedAt, {}});
        if (m.latchCount > 0)
            sink_.report({"CLK-LATCHES", Severity::Info,
                          std::to_string(m.latchCount) +
                              " latch(es) are level-sensitive; their clock domains are not analyzed in this version",
                          {}, {}});
    }

    const graph::DesignView& view_;
    const constraints::ConstraintSet& cons_;
    DiagnosticSink& sink_;
    std::shared_ptr<ClockModel> model_;

    std::unordered_map<BitRef, std::vector<ClockId>, BitRefHash> sourceClocks_;
    std::unordered_map<BitRef, ClockId, BitRefHash> primary_;
    std::unordered_map<BitRef, ClockId, BitRefHash> divider_;
    std::unordered_map<BitRef, ClockId, BitRefHash> blackBox_;
    std::unordered_set<BitRef, BitRefHash> knownRoots_;
    std::unordered_map<BitRef, Trace, BitRefHash> memo_;
    std::unordered_set<BitRef, BitRefHash> inProgress_;
    std::vector<std::pair<ClockId, std::size_t>> generated_;
};

} // namespace

std::shared_ptr<const ClockModel> analyzeClocks(const graph::DesignView& view,
                                                const constraints::ConstraintSet& constraints, DiagnosticSink& sink) {
    Analyzer a(view, constraints, sink);
    return a.run();
}

} // namespace opencdc::clock

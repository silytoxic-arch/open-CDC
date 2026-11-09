#include "clock/ClockAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <map>
#include <set>
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
        resolveGroupObjects();
        resolveGeneratedMasters();
        resolveDividerPeriods();
        computeRelations();
        checkConstraints();
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
        // clock outputs of vendor models (GT RXOUTCLK) are named after their pin
        const ir::Net& net = view_.bodyOf(b.inst).net(b.net);
        if (const auto* pin = net.findAttr("OPENCDC_CLOCK_PIN"); pin && c.name.size() >= net.name.size()) {
            std::string v = pin->value;
            std::erase(v, '"');
            c.name = c.name.substr(0, c.name.size() - net.name.size()) + v;
        }
        // the name the IP's own constraints give the clock (Zynq FCLK_CLK0: clk_fpga_0)
        if (const auto* al = net.findAttr("OPENCDC_CLOCK_ALIAS")) {
            std::string v = al->value;
            std::erase(v, '"');
            if (!v.empty())
                c.aliases.push_back(v);
        }
        // a period an IP configuration gives its clock output (a transceiver's TXOUTCLK)
        if (const auto* per = net.findAttr("OPENCDC_CLOCK_PERIOD")) {
            try {
                const double ns = std::stod(per->value);
                if (ns > 0 && std::isfinite(ns))
                    c.periodNs = ns;
            }
            catch (const std::exception&) {
            }
        }
        const ClockId id = addClock(std::move(c));
        blackBox_.emplace(b, id);
        return id;
    }

    /// The constant value of an input port of the instance (its bits traced
    /// to constants in the instantiating modules); nullopt when not constant.
    std::optional<std::uint64_t> constantPort(ir::InstanceId inst, std::string_view port) const {
        const ir::ModuleBody& b = view_.bodyOf(inst);
        for (const auto& p : b.ports) {
            if (p.name != port)
                continue;
            const auto w = b.net(p.net).width;
            if (w == 0 || w > 64)
                return std::nullopt;
            std::uint64_t v = 0;
            for (std::uint32_t i = 0; i < w; ++i) {
                const Driver drv = driverOf(BitRef{inst, p.net, i});
                if (drv.kind != DriverKind::Constant || (drv.constant != ir::Logic::L0 && drv.constant != ir::Logic::L1))
                    return std::nullopt;
                if (drv.constant == ir::Logic::L1)
                    v |= std::uint64_t{1} << i;
            }
            return v;
        }
        return std::nullopt;
    }

    ClockId dividerClock(const Driver& d, int depth, std::optional<double> ratioOverride = std::nullopt) {
        if (auto it = divider_.find(d.bit); it != divider_.end())
            return it->second;
        Clock c;
        c.name = view_.name(d.bit);
        c.kind = ClockKind::Divider;
        c.source = d.bit;
        c.sourceName = c.name;
        c.definedAt = netLoc(d.bit);
        // Built-in clock generator models (MMCM/PLL) describe their outputs:
        // the period relative to the input clock, the input period from the
        // instance parameters, and the output pin name.
        const ir::Net& net = view_.bodyOf(d.bit.inst).net(d.bit.net);
        auto attrNumber = [&](std::string_view key) -> std::optional<double> {
            const auto* a = net.findAttr(key);
            if (!a)
                return std::nullopt;
            std::string v = a->value;
            std::erase(v, '"');
            try {
                std::size_t used = 0;
                const double x = std::stod(v, &used);
                return used > 0 && std::isfinite(x) ? std::optional(x) : std::nullopt;
            }
            catch (const std::exception&) {
                return std::nullopt;
            }
        };
        // frequencies as written on Intel/Gowin PLLs: "200.000000 MHz", "100" (MHz)
        auto attrFrequencyMHz = [&](std::string_view key) -> std::optional<double> {
            const auto* a = net.findAttr(key);
            if (!a)
                return std::nullopt;
            std::string v = a->value;
            std::erase(v, '"');
            try {
                std::size_t used = 0;
                double x = std::stod(v, &used);
                std::string unit = v.substr(used);
                std::erase_if(unit, [](unsigned char ch) { return std::isspace(ch) != 0; });
                std::ranges::transform(unit, unit.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (unit == "ghz")
                    x *= 1000.0;
                else if (unit == "khz")
                    x /= 1000.0;
                else if (unit == "hz")
                    x /= 1e6;
                else if (!unit.empty() && unit != "mhz")
                    return std::nullopt;
                return x > 0 && std::isfinite(x) ? std::optional(x) : std::nullopt;
            }
            catch (const std::exception&) {
                return std::nullopt;
            }
        };
        auto ratio = ratioOverride ? ratioOverride : attrNumber("OPENCDC_CLOCK_RATIO");
        auto inPeriod = attrNumber("OPENCDC_CLOCK_INPUT_PERIOD");
        if (const auto fin = attrFrequencyMHz("OPENCDC_CLOCK_FREQ_IN")) {
            if (const auto fout = attrFrequencyMHz("OPENCDC_CLOCK_FREQ_OUT"); fout && !ratio)
                ratio = *fin / *fout;
            if (!inPeriod)
                inPeriod = 1000.0 / *fin;
        }
        if (const auto* pin = net.findAttr("OPENCDC_CLOCK_PIN"); pin && c.name.size() >= net.name.size()) {
            std::string v = pin->value;
            std::erase(v, '"');
            c.name = c.name.substr(0, c.name.size() - net.name.size()) + v;
            // Vivado's name: the net on the output pin in the instantiating module
            const auto& d0 = view_.design();
            const auto& node = d0.instance(d.bit.inst);
            if (node.parent.valid()) {
                const auto& pb = d0.body(d0.instance(node.parent).body);
                if (node.childIndex < pb.children.size())
                    for (const auto& pbind : pb.children[node.childIndex].ports)
                        if (pbind.port == v && pbind.actual.chunks().size() == 1 && !pbind.actual.chunks()[0].isConst()) {
                            const std::string& local = pb.net(pbind.actual.chunks()[0].net).name;
                            if (!local.empty())
                                c.aliases.push_back(local);
                        }
            }
        }
        // Quartus' name of the clock (derive_pll_clocks): the instance path below the top
        // with '|' separators, then the model's suffix ("general[0].gpll~PLL_OUTPUT_COUNTER|divclk")
        if (const auto* qs = net.findAttr("OPENCDC_CLOCK_QUARTUS_SUFFIX")) {
            std::string v = qs->value;
            std::erase(v, '"');
            const auto& path = view_.design().instance(d.bit.inst).hierPath;
            const auto dot = path.find('.');
            if (!v.empty() && dot != std::string::npos) {
                std::string q = path.substr(dot + 1);
                std::ranges::replace(q, '.', '|');
                q += "|" + v;
                if (std::ranges::find(c.aliases, q) == c.aliases.end())
                    c.aliases.push_back(q);
            }
        }
        // further names (an IP's own clock net, e.g. the memory interface's clk_pll_i)
        if (const auto* al = net.findAttr("OPENCDC_CLOCK_ALIAS")) {
            std::string v = al->value;
            std::erase(v, '"');
            if (!v.empty() && std::ranges::find(c.aliases, v) == c.aliases.end())
                c.aliases.push_back(v);
        }
        // a period fixed by an IP configuration, whatever the input clock
        const auto fixedPeriod = attrNumber("OPENCDC_CLOCK_PERIOD");
        if (fixedPeriod && *fixedPeriod > 0)
            c.periodNs = *fixedPeriod;
        const ClockId id = addClock(std::move(c));
        divider_.emplace(d.bit, id); // before tracing the master: breaks self-clocking loops
        if (ratio && *ratio > 0 && !(fixedPeriod && *fixedPeriod > 0))
            ratio_[id.value] = {*ratio, inPeriod && *inPeriod > 0 ? *inPeriod : 0.0};

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
                case DriverKind::Undriven: {
                    // an unconnected clock input of a vendor primitive: that interface of
                    // the primitive is not used (MiSTer leaves the HPS bridges fpga2hps,
                    // hps2fpga and f2sdram ports 3..5 without a clock)
                    const ir::ModuleBody& body = view_.bodyOf(d.bit.inst);
                    const auto port = std::ranges::find_if(body.ports, [&](const ir::Port& p) {
                        return p.net == d.bit.net && p.dir == ir::PortDir::In;
                    });
                    if (body.vendorModel && port != body.ports.end()) {
                        t = unresolved("clock input '" + port->name + "' of '" +
                                       view_.design().instance(d.bit.inst).hierPath + "' (vendor primitive " +
                                       body.name + ") is not connected: the interface it clocks is not used");
                        unusedInterface_.insert(t.reason);
                    }
                    else
                        t = unresolved("clock net '" + view_.name(d.bit) + "' is undriven");
                    break;
                }
                case DriverKind::Loop: t = unresolved("alias loop at '" + view_.name(d.bit) + "'"); break;
                case DriverKind::Latch:
                    t = unresolved("clock is driven by latch output '" + view_.name(d.bit) + "'");
                    break;
                case DriverKind::Register: {
                    // a divider whose ratio is a port of the model (BUFG_GT: DIV + 1):
                    // DIV = 0 passes the input clock through
                    const ir::Net& qn = view_.bodyOf(d.bit.inst).net(d.bit.net);
                    if (const auto* rp = qn.findAttr("OPENCDC_CLOCK_DIVIDE_PORT")) {
                        std::string port = rp->value;
                        std::erase(port, '"');
                        const auto v = constantPort(d.bit.inst, port);
                        const auto& reg = view_.bodyOf(d.bit.inst).registers.at(d.index);
                        if (v && *v == 0) {
                            if (auto clkBit = view_.bitOf(d.bit.inst, reg.clock.signal, 0)) {
                                t = trace(*clkBit, depth + 1);
                                break;
                            }
                        }
                        if (!v) {
                            sink_.report({"CLK-DIVIDE-UNKNOWN", Severity::Warning,
                                          "clock divider '" + view_.name(d.bit) + "': port " + port +
                                              " is not a constant; the divided clock has no period",
                                          reg.origin, {}});
                            t.sources.emplace_back(dividerClock(d, depth, 0.0), false);
                        }
                        else {
                            t.sources.emplace_back(dividerClock(d, depth, static_cast<double>(*v + 1)), false);
                        }
                        t.status = ClockStatus::Resolved;
                        break;
                    }
                    t.sources.emplace_back(dividerClock(d, depth), false);
                    t.status = ClockStatus::Resolved;
                    break;
                }
                case DriverKind::Cell: t = traceCell(d, depth); break;
            }
        }
        inProgress_.erase(b);
        memo_.emplace(b, t);
        return t;
    }

    /// The register whose output `b` is, directly or through one NOT: (driver, inverted).
    std::optional<std::pair<Driver, bool>> registerBehind(const BitRef& b) const {
        Driver d = driverOf(b);
        bool inverted = false;
        if (d.kind == DriverKind::Cell) {
            const Cell& c = view_.bodyOf(d.bit.inst).cells[d.index];
            if (c.kind != CellKind::Not)
                return std::nullopt;
            const auto in = view_.bitOf(d.bit.inst, c.inputs[0], d.bit.bit);
            if (!in)
                return std::nullopt;
            d = driverOf(*in);
            inverted = true;
        }
        if (d.kind != DriverKind::Register)
            return std::nullopt;
        return std::pair{d, inverted};
    }

    /// Two registers clocked by the same bit (after aliases), on the same edge.
    bool sameRegisterClock(const Driver& x, const Driver& y) const {
        const auto& rx = view_.bodyOf(x.bit.inst).registers.at(x.index);
        const auto& ry = view_.bodyOf(y.bit.inst).registers.at(y.index);
        if (rx.clock.edge != ry.clock.edge)
            return false;
        const auto cx = view_.bitOf(x.bit.inst, rx.clock.signal, 0);
        const auto cy = view_.bitOf(y.bit.inst, ry.clock.signal, 0);
        if (!cx || !cy)
            return false;
        const Driver dx = driverOf(*cx);
        const Driver dy = driverOf(*cy);
        return dx.kind == dy.kind && dx.bit == dy.bit;
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
                // two register outputs of one clock domain, one of them inverted (nestang
                // dualshock_controller.v: ~psSEL & RXWT): a strobe made by logic from registers,
                // gated by an active-low enable; the strobe register is the clock source
                if (sa == 0 && sb == 0 && c.kind == CellKind::And) {
                    const auto ra = registerBehind(*a.bit);
                    const auto rb = registerBehind(*b.bit);
                    if (ra && rb && ra->second != rb->second && sameRegisterClock(ra->first, rb->first)) {
                        const BitRef& clk = ra->second ? *b.bit : *a.bit;
                        const BitRef& gate = ra->second ? *a.bit : *b.bit;
                        absorb(t, trace(clk, depth + 1), false);
                        t.gated = true;
                        t.gating.push_back(gate);
                        return t;
                    }
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
                // the divider register of a model that passes its input through
                // (BUFG_GT with DIV = 0) is not part of the design: no domain
                if (reg.q.width() == 1 && reg.q.chunks().size() == 1 && !reg.q.chunks()[0].isConst())
                    if (const auto* rp = body.net(reg.q.chunks()[0].net).findAttr("OPENCDC_CLOCK_DIVIDE_PORT")) {
                        std::string port = rp->value;
                        std::erase(port, '"');
                        if (const auto v = constantPort(inst, port); v && *v == 0) {
                            rc.status = ClockStatus::Resolved;
                            rc.reason = "divider of a clock buffer that passes its input through (" + port + " = 0)";
                            continue;
                        }
                    }
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

        for (const auto& [why, e] : unresolvedByReason) {
            if (unusedInterface_.contains(why))
                sink_.report({"CLK-UNUSED-INTERFACE", Severity::Info,
                              std::to_string(e.first) + " register(s) in a built-in primitive model without a clock: " +
                                  why + "; they are excluded from CDC analysis",
                              {}, {}});
            else
                sink_.report({"CLK-UNRESOLVED", Severity::Warning,
                              std::to_string(e.first) + " register(s) with unresolved clock: " + why + " (e.g. '" +
                                  e.second + "'); they are excluded from CDC analysis",
                              {}, {}});
        }
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
            if (master.valid() && !c.periodNs && model_->clocks[master.index()].periodNs) {
                c.periodRatio = static_cast<double>(def.divideBy) / def.multiplyBy;
                c.periodBasis = model_->clocks[master.index()].name;
                c.periodNs = *model_->clocks[master.index()].periodNs * *c.periodRatio;
            }
        }
        // Derived clocks without constraints inherit nothing but the master link.
    }

    /// Periods of generator outputs: master period x ratio, or the input
    /// period written on the instance (CLKIN1_PERIOD) when the master has none.
    void resolveDividerPeriods() {
        for (std::size_t round = 0; round < ratio_.size() + 1; ++round) {
            bool changed = false;
            for (const auto& [idv, r] : ratio_) {
                auto& c = model_->clocks[idv];
                if (c.periodNs)
                    continue;
                std::optional<double> base;
                std::string basis;
                if (c.master.valid() && model_->clocks[c.master.index()].periodNs) {
                    base = model_->clocks[c.master.index()].periodNs;
                    basis = model_->clocks[c.master.index()].name;
                    // the generator was configured for another input period
                    if (r.second > 0 && std::abs(*base - r.second) > 0.01 * *base)
                        inputPeriodMismatch_.push_back({c.name, basis, r.second, *base});
                }
                else if (r.second > 0) {
                    base = r.second;
                    basis = "CLKIN1_PERIOD";
                }
                if (base) {
                    c.periodNs = *base * r.first;
                    c.periodRatio = r.first;
                    c.periodBasis = basis;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }

    // ------------------------------------------------------------------
    // Relations
    // ------------------------------------------------------------------

    /// Constraints that contradict the design or each other.
    void checkConstraints() {
        auto& m = *model_;
        const std::size_t n = m.clocks.size();
        auto fmt = [](double v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            return std::string(buf);
        };
        // generator input period differs from the clock that drives it
        for (const auto& e : inputPeriodMismatch_)
            sink_.report({"CON-INPUT-PERIOD", Severity::Warning,
                          "clock generator output '" + e.clock + "': the generator is configured for an input period of " +
                              fmt(e.configured) + " ns (CLKIN1_PERIOD) but its input clock '" + e.input + "' has " +
                              fmt(e.actual) + " ns; the output period follows the input clock",
                          {}, {}});

        // a constraint on a clock generator output that the generator does not produce
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = m.clocks[i];
            if ((c.kind != ClockKind::Constraint && c.kind != ClockKind::Generated) || !c.periodNs || !c.source.valid())
                continue;
            const Driver d = view_.driver(c.source);
            if (d.kind != DriverKind::Register)
                continue;
            const ir::Net& net = view_.bodyOf(d.bit.inst).net(d.bit.net);
            const auto* ra = net.findAttr("OPENCDC_CLOCK_RATIO");
            if (!ra)
                continue;
            double ratio = 0.0;
            try {
                std::string v = ra->value;
                std::erase(v, '"');
                ratio = std::stod(v);
            }
            catch (const std::exception&) {
                continue;
            }
            const auto& rc = m.registers.at(view_.globalRegister(d.bit.inst, d.index));
            if (rc.clocks.empty() || ratio <= 0)
                continue;
            const auto& in = m.clocks[rc.clocks.front().clock.index()];
            if (!in.periodNs)
                continue;
            const double expected = *in.periodNs * ratio;
            if (std::abs(expected - *c.periodNs) > 0.001 * expected)
                sink_.report({"CON-PERIOD-MISMATCH", Severity::Warning,
                              "clock '" + c.name + "' is constrained to " + fmt(*c.periodNs) + " ns, but the generator output " +
                                  view_.name(d.bit) + " has " + fmt(expected) + " ns (input clock '" + in.name + "' " +
                                  fmt(*in.periodNs) + " ns x " + fmt(ratio) + ")",
                              c.definedAt, {}});
        }

        // set_clock_groups: fewer than two groups, a clock in two groups, related clocks declared asynchronous
        auto rootOf = [&](ClockId c) {
            const auto a = ancestors(c);
            return a.back();
        };
        for (const auto& g : cons_.groups) {
            if (g.groups.size() < 2) {
                // SDC (Synopsys, Quartus, Vivado): the clocks of a single group
                // are asynchronous (or exclusive) to all other clocks
                sink_.report({"CON-GROUP-SINGLE", Severity::Info,
                              "set_clock_groups with a single group: its clocks are treated as " +
                                  std::string(g.kind == constraints::GroupKind::Asynchronous ? "asynchronous"
                                                                                             : "exclusive") +
                                  " to all other clocks (and to the clocks derived from those)",
                              g.loc, {}});
                continue;
            }
            std::vector<std::vector<std::size_t>> members(g.groups.size());
            const auto gi = static_cast<std::size_t>(&g - cons_.groups.data());
            for (std::size_t k = 0; k < g.groups.size(); ++k) {
                for (const auto& p : g.groups[k])
                    for (std::size_t c = 0; c < n; ++c)
                        if (clockMatches(p, m.clocks[c]) &&
                            std::ranges::find(members[k], c) == members[k].end())
                            members[k].push_back(c);
                if (gi < groupObjectClocks_.size() && k < groupObjectClocks_[gi].size())
                    for (const ClockId c : groupObjectClocks_[gi][k])
                        if (std::ranges::find(members[k], c.index()) == members[k].end())
                            members[k].push_back(c.index());
            }
            std::vector<int> seenIn(n, -1);
            for (std::size_t k = 0; k < members.size(); ++k)
                for (const auto c : members[k]) {
                    if (seenIn[c] >= 0 && seenIn[c] != static_cast<int>(k))
                        sink_.report({"CON-GROUP-OVERLAP", Severity::Warning,
                                      "set_clock_groups: clock '" + m.clocks[c].name + "' is in more than one group",
                                      g.loc, {}});
                    seenIn[c] = static_cast<int>(k);
                }
            if (g.kind != constraints::GroupKind::Asynchronous)
                continue;
            for (std::size_t k = 0; k < members.size(); ++k)
                for (std::size_t l = k + 1; l < members.size(); ++l)
                    for (const auto a : members[k])
                        for (const auto b : members[l]) {
                            const ClockId ia{static_cast<std::uint32_t>(a)}, ib{static_cast<std::uint32_t>(b)};
                            if (a == b || rootOf(ia) != rootOf(ib))
                                continue;
                            sink_.report({"CON-GROUP-RELATED", Severity::Info,
                                          "set_clock_groups -asynchronous separates '" + m.clocks[a].name + "' and '" +
                                              m.clocks[b].name + "', which are derived from the same clock '" +
                                              m.clocks[rootOf(ia).index()].name +
                                              "'; crossings between them are then checked as asynchronous",
                                          g.loc, {}});
                        }
        }
    }

    /// get_clocks -of_objects in set_clock_groups: the clocks on the named
    /// nets/pins/ports, per constraint and group
    void resolveGroupObjects() {
        // A named net that is not itself on a traced clock path may be another
        // name of one (a port of an instance connected to it, an assign): the
        // nets with the same driver carry the same clocks. Through aliases
        // only, so a data net never names a clock.
        std::unordered_map<BitRef, const Trace*, BitRefHash> byDriver;
        for (const auto& [bit, t] : memo_)
            if (!t.sources.empty())
                byDriver.emplace(driverOf(bit).bit, &t);
        auto traced = [&](const BitRef& b) -> const Trace* {
            if (const auto it = memo_.find(b); it != memo_.end())
                return &it->second;
            if (sourceClocks_.contains(b))
                return nullptr;
            if (const auto it = byDriver.find(driverOf(b).bit); it != byDriver.end())
                return it->second;
            return nullptr;
        };
        // clock exceptions with get_clocks -of_objects
        exceptionClocks_.assign(cons_.clockExceptions.size(), {});
        auto clocksOn = [&](const std::vector<BitRef>& bits, std::vector<ClockId>& out) {
            for (const auto& b : bits) {
                if (const Trace* t = traced(b))
                    for (const auto& [c, inv] : t->sources)
                        out.push_back(c);
                else if (const auto sc = sourceClocks_.find(b); sc != sourceClocks_.end())
                    out.insert(out.end(), sc->second.begin(), sc->second.end());
            }
        };
        for (std::size_t k = 0; k < cons_.clockExceptions.size(); ++k) {
            clocksOn(cons_.clockExceptions[k].fromBits, exceptionClocks_[k].first);
            clocksOn(cons_.clockExceptions[k].toBits, exceptionClocks_[k].second);
        }
        model_->pathExceptionClocks.assign(cons_.pathExceptions.size(), {});
        for (std::size_t k = 0; k < cons_.pathExceptions.size(); ++k) {
            clocksOn(cons_.pathExceptions[k].fromClockBits, model_->pathExceptionClocks[k].first);
            clocksOn(cons_.pathExceptions[k].toClockBits, model_->pathExceptionClocks[k].second);
        }
        // set_input_delay -clock [get_clocks -of_objects ...]
        model_->inputDelayClocks.assign(cons_.inputDelays.size(), std::nullopt);
        for (std::size_t i = 0; i < cons_.inputDelays.size(); ++i) {
            const auto& d = cons_.inputDelays[i];
            for (const auto& b : d.clockBits) {
                if (const Trace* t = traced(b); t && !t->sources.empty())
                    model_->inputDelayClocks[i] = t->sources.front().first;
                else if (const auto sc = sourceClocks_.find(b); sc != sourceClocks_.end() && !sc->second.empty())
                    model_->inputDelayClocks[i] = sc->second.front();
                if (model_->inputDelayClocks[i])
                    break;
            }
            if (!d.clockBits.empty() && !model_->inputDelayClocks[i])
                sink_.report({"CON-UNMATCHED", Severity::Warning,
                              "set_input_delay: get_clocks -of_objects finds no clock on '" +
                                  view_.name(d.clockBits.front()) + "' (no register is clocked through it)",
                              d.loc, {}});
        }
        groupObjectClocks_.assign(cons_.groups.size(), {});
        for (std::size_t g = 0; g < cons_.groups.size(); ++g) {
            const auto& grp = cons_.groups[g];
            groupObjectClocks_[g].resize(grp.groups.size());
            for (std::size_t k = 0; k < grp.memberBits.size() && k < grp.groups.size(); ++k)
                for (const auto& b : grp.memberBits[k]) {
                    // only nets already on a clock path (or constraint sources)
                    // name clocks: tracing a data net would invent a clock
                    Trace t;
                    if (const Trace* tr = traced(b))
                        t = *tr;
                    else if (const auto sc = sourceClocks_.find(b); sc != sourceClocks_.end())
                        for (const ClockId c : sc->second)
                            t.sources.emplace_back(c, false);
                    if (t.sources.empty()) {
                        sink_.report({"CON-UNMATCHED", Severity::Warning,
                                      "set_clock_groups: get_clocks -of_objects finds no clock on '" + view_.name(b) +
                                          "' (no register is clocked through it)",
                                      grp.loc, {}});
                        continue;
                    }
                    for (const auto& [c, inv] : t.sources)
                        if (std::ranges::find(groupObjectClocks_[g][k], c) == groupObjectClocks_[g][k].end())
                            groupObjectClocks_[g][k].push_back(c);
                }
        }
    }

    /// set_false_path / set_max_delay -datapath_only from one clock to the
    /// other (either direction): the designer handles the pair as asynchronous.
    std::optional<std::string> exceptionBetween(ClockId a, ClockId b) const {
        const auto& m = *model_;
        for (std::size_t k = 0; k < cons_.clockExceptions.size(); ++k) {
            const auto& e = cons_.clockExceptions[k];
            auto member = [&](ClockId c, const std::vector<std::string>& names, const std::vector<ClockId>& byBits) {
                return std::ranges::any_of(names, [&](const std::string& p) {
                           return clockMatches(p, m.clocks[c.index()]);
                       }) ||
                       std::ranges::find(byBits, c) != byBits.end();
            };
            const auto& fb = exceptionClocks_[k].first;
            const auto& tb = exceptionClocks_[k].second;
            const bool ab = member(a, e.from, fb) && member(b, e.to, tb);
            const bool ba = member(b, e.from, fb) && member(a, e.to, tb);
            if (ab || ba) {
                const auto& f = m.clocks[(ab ? a : b).index()].name;
                const auto& t = m.clocks[(ab ? b : a).index()].name;
                return e.command + " -from " + f + " -to " + t + " (" + toString(e.loc) +
                       "): crossings in this direction are declared asynchronous";
            }
        }
        return std::nullopt;
    }

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
            const auto gi = static_cast<std::size_t>(&g - cons_.groups.data());
            for (const auto& patterns : g.groups) {
                std::vector<bool> mem(n, false);
                const std::size_t k = r.member.size();
                if (gi < groupObjectClocks_.size() && k < groupObjectClocks_[gi].size())
                    for (const ClockId c : groupObjectClocks_[gi][k])
                        if (c.index() < n)
                            mem[c.index()] = true;
                for (const auto& p : patterns) {
                    bool any = false;
                    for (std::size_t c = 0; c < n; ++c)
                        if (clockMatches(p, m.clocks[c])) {
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
                    if (const auto why = exceptionBetween(a, b)) {
                        m.setRelation(a, b, ClockRelation::Asynchronous, *why);
                        continue;
                    }
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
    std::set<std::string> unusedInterface_; ///< unresolved reasons: an unconnected clock input of a vendor model
    std::vector<std::vector<std::vector<ClockId>>> groupObjectClocks_;
    std::vector<std::pair<std::vector<ClockId>, std::vector<ClockId>>> exceptionClocks_; ///< per clock exception ///< constraint -> group -> clocks (-of_objects)

    std::unordered_map<BitRef, std::vector<ClockId>, BitRefHash> sourceClocks_;
    std::unordered_map<BitRef, ClockId, BitRefHash> primary_;
    std::unordered_map<BitRef, ClockId, BitRefHash> divider_;
    std::map<std::uint32_t, std::pair<double, double>> ratio_; ///< clock -> (period ratio, input period)
    struct InputPeriodMismatch {
        std::string clock, input;
        double configured, actual;
    };
    std::vector<InputPeriodMismatch> inputPeriodMismatch_;
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

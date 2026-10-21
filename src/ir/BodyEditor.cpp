#include "ir/BodyEditor.h"

#include "ir/ConstFold.h"
#include "ir/Dump.h"

#include <algorithm>
#include <optional>
#include <tuple>

namespace opencdc::ir {

namespace {

void appendKey(std::string& key, const SigSpec& s) {
    for (const auto& c : s.chunks()) {
        if (c.isConst())
            key += c.constant.toString();
        else
            key += "n" + std::to_string(c.net.value) + "." + std::to_string(c.offset) + "." + std::to_string(c.width);
        key += ',';
    }
    key += '|';
}

} // namespace

SigSpec BodyEditor::passThroughMuxBits(const SigSpec& s) const {
    // Bits of a multiplexer output whose data inputs agree on that bit (a
    // record field neither branch changes) are that input bit: the select
    // does not matter for them. Cells are simplified as they are emitted, so
    // one level of lookup sees through a whole if/elsif chain.
    bool any = false;
    for (const auto& ch : s.chunks())
        if (!ch.isConst() && ch.width <= 4096)
            if (const Cell* d = driverOf(ch.net); d && d->kind == CellKind::Mux)
                any = true;
    if (!any)
        return s;
    SigSpec out;
    for (const auto& ch : s.chunks()) {
        const Cell* d = ch.isConst() || ch.width > 4096 ? nullptr : driverOf(ch.net);
        if (!d || d->kind != CellKind::Mux || d->inputs.size() != 3 || d->inputs[0].width() != d->width ||
            d->inputs[1].width() != d->width) {
            out.append(ch);
            continue;
        }
        for (std::uint32_t b = 0; b < ch.width; ++b) {
            const std::uint32_t off = ch.offset + b;
            const SigSpec a = d->inputs[0].bit(off);
            if (a == d->inputs[1].bit(off))
                out.append(a);
            else
                out.append(SigSpec::net(ch.net, 1, off));
        }
    }
    return out;
}

SigSpec BodyEditor::emit(Cell c) {
    if (c.width == 0)
        return {};
    for (auto& in : c.inputs)
        in = passThroughMuxBits(in);
    if (auto simplified = simplifyCell(c))
        return simplified->resized(c.width, false);
    // Structural hashing: an identical cell already exists -> reuse its output.
    std::string key = std::to_string(static_cast<int>(c.kind)) + ":" + std::to_string(c.width) + ":" +
                      (c.isSigned ? "s" : "u") + ":" + std::to_string(c.elemWidth) + ":" +
                      std::to_string(c.rangeLeft) + ":" + std::to_string(c.rangeRight) + ":";
    for (const auto& in : c.inputs)
        appendKey(key, in);
    if (auto it = cellCache_.find(key); it != cellCache_.end())
        return it->second;
    SigSpec out = body_.addCell(std::move(c));
    driverCell_[body_.cells.back().output.value] = body_.cells.size() - 1;
    cellCache_.emplace(std::move(key), out);
    return out;
}

SigSpec BodyEditor::cell(CellKind kind, std::vector<SigSpec> inputs, std::uint32_t width, const SourceLocation& origin,
                         bool isSigned) {
    Cell c;
    c.kind = kind;
    c.inputs = std::move(inputs);
    c.width = width;
    c.isSigned = isSigned;
    c.origin = origin;
    return emit(std::move(c));
}

SigSpec BodyEditor::toBool(const SigSpec& s, const SourceLocation& origin) {
    if (s.width() == 1)
        return s;
    if (s.width() == 0)
        return SigSpec::constant(0, 1);
    return cell(CellKind::ReduceOr, {s}, 1, origin);
}

SigSpec BodyEditor::addUnique(Cell c) {
    if (c.width == 0)
        return {};
    SigSpec out = body_.addCell(std::move(c));
    driverCell_[body_.cells.back().output.value] = body_.cells.size() - 1;
    return out;
}

const Cell* BodyEditor::driverOf(NetId net) const {
    auto it = driverCell_.find(net.value);
    return it == driverCell_.end() ? nullptr : &body_.cells[it->second];
}

void BodyEditor::addLatch(Latch l, bool unintended) {
    body_.latches.push_back(std::move(l));
    latchUnintended_.push_back(unintended);
}

void BodyEditor::finish(DiagnosticSink& sink, const std::function<void(const std::vector<std::uint32_t>&)>& onRemap) {
    for (auto& c : body_.connections)
        c.rhs = passThroughMuxBits(c.rhs);
    for (auto& r : body_.registers) {
        r.d = passThroughMuxBits(r.d);
        if (r.enable)
            r.enable = passThroughMuxBits(*r.enable);
    }
    for (auto& l : body_.latches)
        l.d = passThroughMuxBits(l.d);
    pruneDeadStorage(sink);
    removeDeadCells(onRemap);
    checkMultipleDrivers(sink);
}

void BodyEditor::extractControls(Register& r, const SourceLocation& l) {
    // Recognizes D = Mux(S, A, B) structures created by if/else:
    //   sync reset:  if (srst) q <= CONST; else q <= ...   (outermost)
    //   enable:      if (en) q <= d;                       (other branch holds q)
    auto asMux = [&](const SigSpec& d) -> std::optional<std::tuple<SigSpec, SigSpec, SigSpec>> {
        if (d.chunks().size() != 1 || d.chunks()[0].isConst())
            return std::nullopt;
        const auto& ch = d.chunks()[0];
        auto it = driverCell_.find(ch.net.value);
        if (it == driverCell_.end())
            return std::nullopt;
        const Cell& c = body_.cells[it->second];
        if (c.kind != CellKind::Mux)
            return std::nullopt;
        return std::make_tuple(c.inputs[0].extract(ch.offset, ch.width), c.inputs[1].extract(ch.offset, ch.width),
                               c.inputs[2]);
    };

    // Strips 1-bit inverters (~x, !x). Returns the underlying signal and
    // whether the original is active high.
    auto stripInverters = [&](SigSpec sig) {
        bool activeHigh = true;
        for (int i = 0; i < 4 && sig.width() == 1 && sig.chunks().size() == 1 && !sig.chunks()[0].isConst(); ++i) {
            auto it = driverCell_.find(sig.chunks()[0].net.value);
            if (it == driverCell_.end())
                break;
            const Cell& c = body_.cells[it->second];
            if ((c.kind != CellKind::Not && c.kind != CellKind::LogicNot) || c.inputs[0].width() != 1)
                break;
            sig = c.inputs[0];
            activeHigh = !activeHigh;
        }
        return std::make_pair(sig, activeHigh);
    };
    auto isNamedSignal = [&](const SigSpec& sig) {
        return sig.width() == 1 && sig.chunks().size() == 1 && !sig.chunks()[0].isConst() &&
               body_.net(sig.chunks()[0].net).kind != NetKind::Temp;
    };

    for (int guard = 0; guard < 8; ++guard) {
        auto m = asMux(r.d);
        if (!m)
            return;
        auto& [A, B, S] = *m;
        // A synchronous reset is a *named* signal (optionally inverted) that
        // forces a constant; a constant selected by decoded logic (case items,
        // comparisons) is ordinary data and must stay in D.
        const auto [base, high] = stripInverters(S);
        if (!r.enable && !r.syncReset && isNamedSignal(base)) {
            if (B.isFullyConst() && A != r.q) {
                r.syncReset = ResetSpec{base, high, B};
                r.d = A;
                continue;
            }
            if (A.isFullyConst() && B != r.q) {
                r.syncReset = ResetSpec{base, !high, A};
                r.d = B;
                continue;
            }
        }
        if (!r.enable) {
            if (A == r.q) {
                r.enable = S;
                r.d = B;
                continue;
            }
            if (B == r.q) {
                r.enable = cell(CellKind::Not, {S}, 1, l);
                r.d = A;
                continue;
            }
        }
        return;
    }
}

void BodyEditor::pruneDeadStorage(DiagnosticSink& sink) {
    // Registers and latches whose output cannot reach anything observable
    // (ports, continuous assignments, child inputs, or other live storage)
    // are not hardware: temporaries of always blocks, loop indices, unused
    // state. Synthesis removes them; so do we. Liveness is propagated
    // backwards through combinational cells (a storage element feeding only
    // its own hold multiplexer stays dead).
    const std::size_t nNets = body_.nets.size();
    std::vector<std::int64_t> cellOf(nNets, -1);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        cellOf[body_.cells[i].output.index()] = static_cast<std::int64_t>(i);

    std::vector<std::vector<bool>> reached(nNets);
    std::vector<bool> cellDone(body_.cells.size(), false);
    std::vector<SigSpec> work;
    auto visit = [&](const SigSpec& sig) { work.push_back(sig); };
    auto drain = [&] {
        while (!work.empty()) {
            SigSpec sig = std::move(work.back());
            work.pop_back();
            for (const auto& c : sig.chunks()) {
                if (c.isConst())
                    continue;
                auto& m = reached[c.net.index()];
                if (m.empty())
                    m.resize(body_.nets[c.net.index()].width, false);
                for (std::uint32_t i = c.offset; i < c.offset + c.width && i < m.size(); ++i)
                    m[i] = true;
                const auto ci = cellOf[c.net.index()];
                if (ci >= 0 && !cellDone[static_cast<std::size_t>(ci)]) {
                    cellDone[static_cast<std::size_t>(ci)] = true;
                    for (const auto& in : body_.cells[static_cast<std::size_t>(ci)].inputs)
                        work.push_back(in);
                }
            }
        }
    };
    auto isReached = [&](const SigSpec& q) {
        for (const auto& c : q.chunks()) {
            if (c.isConst())
                continue;
            if (body_.nets[c.net.index()].dir != PortDir::None)
                return true;
            const auto& m = reached[c.net.index()];
            for (std::uint32_t i = c.offset; i < c.offset + c.width && i < m.size(); ++i)
                if (m[i])
                    return true;
        }
        return false;
    };

    for (const auto& c : body_.connections)
        visit(c.rhs);
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            if (p.dir != PortDir::Out)
                visit(p.actual);
    drain();

    std::vector<bool> regLive(body_.registers.size(), false);
    std::vector<bool> latchLive(body_.latches.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < body_.registers.size(); ++i) {
            const auto& r = body_.registers[i];
            if (regLive[i] || !isReached(r.q))
                continue;
            regLive[i] = changed = true;
            visit(r.d);
            visit(r.clock.signal);
            for (const auto& a : r.asyncResets) {
                visit(a.signal);
                visit(a.value);
            }
            if (r.syncReset) {
                visit(r.syncReset->signal);
                visit(r.syncReset->value);
            }
            if (r.enable)
                visit(*r.enable);
        }
        for (std::size_t i = 0; i < body_.latches.size(); ++i) {
            if (latchLive[i] || !isReached(body_.latches[i].q))
                continue;
            latchLive[i] = changed = true;
            visit(body_.latches[i].d);
        }
        drain();
    }

    std::size_t removedRegs = 0;
    std::size_t removedLatches = 0;
    std::vector<Register> regs;
    for (std::size_t i = 0; i < body_.registers.size(); ++i) {
        if (regLive[i])
            regs.push_back(std::move(body_.registers[i]));
        else
            ++removedRegs;
    }
    std::vector<Latch> latches;
    std::vector<bool> flags;
    for (std::size_t i = 0; i < body_.latches.size(); ++i) {
        if (latchLive[i]) {
            latches.push_back(std::move(body_.latches[i]));
            flags.push_back(i < latchUnintended_.size() && latchUnintended_[i]);
        }
        else {
            ++removedLatches;
        }
    }
    body_.registers = std::move(regs);
    body_.latches = std::move(latches);
    latchUnintended_ = std::move(flags);

    if (removedRegs + removedLatches > 0) {
        sink.report({"IR-DEAD-STORAGE", Severity::Info,
                       "module '" + body_.name + "': removed " + std::to_string(removedRegs) + " register(s) and " +
                           std::to_string(removedLatches) + " latch(es) whose outputs are never read",
                       body_.loc, {}});
    }
    for (std::size_t i = 0; i < body_.latches.size(); ++i) {
        if (i < latchUnintended_.size() && latchUnintended_[i]) {
            sink.report({"IR-LATCH-INFERRED", Severity::Warning,
                           "latch inferred for '" + formatSig(body_, body_.latches[i].q) + "' in module '" + body_.name +
                               "': not assigned on every path of a combinational " + processWord_ + "",
                           body_.latches[i].origin, {}});
        }
    }
}

void BodyEditor::removeDeadCells(const std::function<void(const std::vector<std::uint32_t>&)>& onRemap) {
    // Live cells are those reachable (backwards) from anything observable:
    // connections, registers, latches and child port bindings.
    const std::size_t nNets = body_.nets.size();
    std::vector<std::int64_t> cellOf(nNets, -1);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        cellOf[body_.cells[i].output.index()] = static_cast<std::int64_t>(i);

    std::vector<bool> live(body_.cells.size(), false);
    std::vector<std::size_t> work;
    auto use = [&](const SigSpec& s) {
        for (const auto& c : s.chunks()) {
            if (c.isConst())
                continue;
            const auto ci = cellOf[c.net.index()];
            if (ci >= 0 && !live[static_cast<std::size_t>(ci)]) {
                live[static_cast<std::size_t>(ci)] = true;
                work.push_back(static_cast<std::size_t>(ci));
            }
        }
    };
    for (const auto& c : body_.connections) {
        use(c.lhs);
        use(c.rhs);
    }
    for (const auto& r : body_.registers) {
        use(r.d);
        use(r.clock.signal);
        for (const auto& a : r.asyncResets) {
            use(a.signal);
            use(a.value);
        }
        if (r.syncReset) {
            use(r.syncReset->signal);
            use(r.syncReset->value);
        }
        if (r.enable)
            use(*r.enable);
    }
    for (const auto& l : body_.latches)
        use(l.d);
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            use(p.actual);
    while (!work.empty()) {
        const auto ci = work.back();
        work.pop_back();
        for (const auto& in : body_.cells[ci].inputs)
            use(in);
    }
    if (std::ranges::all_of(live, [](bool b) { return b; }))
        return;

    // Compact: drop dead cells and their temp nets, renumber everything.
    std::vector<bool> keepNet(nNets, true);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        if (!live[i])
            keepNet[body_.cells[i].output.index()] = false;
    std::vector<std::uint32_t> remap(nNets, 0);
    std::vector<Net> nets;
    for (std::size_t i = 0; i < nNets; ++i) {
        if (!keepNet[i])
            continue;
        remap[i] = static_cast<std::uint32_t>(nets.size());
        nets.push_back(std::move(body_.nets[i]));
    }
    auto fix = [&](SigSpec& s) {
        SigSpec out;
        for (auto c : s.chunks()) {
            if (!c.isConst())
                c.net = NetId{remap[c.net.index()]};
            out.append(c);
        }
        s = std::move(out);
    };
    std::vector<Cell> cells;
    for (std::size_t i = 0; i < body_.cells.size(); ++i) {
        if (!live[i])
            continue;
        Cell c = std::move(body_.cells[i]);
        for (auto& in : c.inputs)
            fix(in);
        c.output = NetId{remap[c.output.index()]};
        cells.push_back(std::move(c));
    }
    for (auto& c : body_.connections) {
        fix(c.lhs);
        fix(c.rhs);
    }
    for (auto& r : body_.registers) {
        fix(r.q);
        fix(r.d);
        fix(r.clock.signal);
        for (auto& a : r.asyncResets) {
            fix(a.signal);
            fix(a.value);
        }
        if (r.syncReset) {
            fix(r.syncReset->signal);
            fix(r.syncReset->value);
        }
        if (r.enable)
            fix(*r.enable);
    }
    for (auto& l : body_.latches) {
        fix(l.q);
        fix(l.d);
    }
    for (auto& ch : body_.children)
        for (auto& p : ch.ports)
            fix(p.actual);
    for (auto& p : body_.ports)
        p.net = NetId{remap[p.net.index()]};
    onRemap(remap);
    body_.nets = std::move(nets);
    body_.cells = std::move(cells);
    driverCell_.clear();
    cellCache_.clear();
}

void BodyEditor::checkMultipleDrivers(DiagnosticSink& sink) {
    struct Drv {
        std::uint32_t lo;
        std::uint32_t hi;
        std::string what;
    };
    std::vector<std::vector<Drv>> drivers(body_.nets.size());
    auto add = [&](const SigSpec& s, const std::string& what) {
        for (const auto& c : s.chunks())
            if (!c.isConst())
                drivers[c.net.index()].push_back({c.offset, c.offset + c.width, what});
    };
    for (const auto& p : body_.ports)
        if (p.dir == PortDir::In)
            add(body_.netSig(p.net), "input port");
    for (const auto& c : body_.connections)
        add(c.lhs, (c.kind == ConnKind::Process ? processWord_ + " at " : std::string("assignment at ")) + toString(c.origin));
    for (const auto& r : body_.registers)
        add(r.q, processWord_ + " at " + toString(r.origin));
    for (const auto& l : body_.latches)
        add(l.q, processWord_ + " at " + toString(l.origin));
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            if (p.dir == PortDir::Out)
                add(p.actual, "output port ." + p.port + " of " + ch.name);

    for (std::size_t n = 0; n < drivers.size(); ++n) {
        auto& ds = drivers[n];
        if (ds.size() < 2)
            continue;
        std::ranges::sort(ds, {}, &Drv::lo);
        for (std::size_t i = 0; i + 1 < ds.size(); ++i) {
            for (std::size_t j = i + 1; j < ds.size() && ds[j].lo < ds[i].hi; ++j) {
                // Several assignments from the same procedural block are one driver.
                if (ds[i].what == ds[j].what)
                    continue;
                const auto& net = body_.nets[n];
                const std::uint32_t bit = ds[j].lo;
                sink.report({"IR-MULTIDRIVER", Severity::Error,
                               "'" + net.name + "' bit [" + std::to_string(net.indexOf(bit)) + "] in module '" +
                                   body_.name + "' has multiple drivers: " + ds[i].what + " and " + ds[j].what,
                               net.loc, {}});
                goto nextNet; // one diagnostic per net is enough
            }
        }
    nextNet:;
    }
}

} // namespace opencdc::ir

// VHDL sequential statements, processes and concurrent assignments.
//
// A process is executed symbolically: variables take their new value
// immediately, signal assignments are collected as next values, if/case
// branches are merged with multiplexers. Loops are unrolled (their bounds
// are static); return / exit / next are handled with execution conditions
// ("alive"), so code after them only takes effect on the paths that did not
// leave. The result is turned into registers (clocked processes), plain
// connections or latches (combinational processes).

#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>

namespace opencdc::vhdl {

namespace {

using ir::SigSpec;

SigSpec one() {
    return SigSpec::constant(1, 1);
}
SigSpec zero() {
    return SigSpec::constant(0, 1);
}
bool isOne(const SigSpec& s) {
    auto c = s.asConst();
    return c && c->width() == 1 && (*c)[0] == ir::Logic::L1;
}
bool isZero(const SigSpec& s) {
    auto c = s.asConst();
    return c && c->width() == 1 && (*c)[0] == ir::Logic::L0;
}

void markBits(BitMask& m, std::uint32_t total, std::uint32_t off, std::uint32_t w) {
    if (m.size() < total)
        m.resize(total, false);
    for (std::uint32_t i = off; i < off + w && i < total; ++i)
        m[i] = true;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> runs(const BitMask& m) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    std::uint32_t i = 0;
    const auto n = static_cast<std::uint32_t>(m.size());
    while (i < n) {
        if (!m[i]) {
            ++i;
            continue;
        }
        std::uint32_t j = i;
        while (j < n && m[j])
            ++j;
        out.emplace_back(i, j - i);
        i = j;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// helpers on conditions
// ---------------------------------------------------------------------------

namespace {

struct CondOps {
    Elaborator& E;
    Loc l;
    SigSpec andB(const SigSpec& a, const SigSpec& b) const {
        if (isZero(a) || isZero(b))
            return zero();
        if (isOne(a))
            return b;
        if (isOne(b))
            return a;
        return E.cell(ir::CellKind::And, {a, b}, 1, l);
    }
    SigSpec orB(const SigSpec& a, const SigSpec& b) const {
        if (isOne(a) || isOne(b))
            return one();
        if (isZero(a))
            return b;
        if (isZero(b))
            return a;
        return E.cell(ir::CellKind::Or, {a, b}, 1, l);
    }
    SigSpec notB(const SigSpec& a) const {
        if (isOne(a))
            return zero();
        if (isZero(a))
            return one();
        return E.cell(ir::CellKind::Not, {a}, 1, l);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------

void Elaborator::assign(const std::vector<Target>& targets, const SVal& value, bool signal, Ctx& ctx, Loc l) {
    if (!ctx.st) {
        error("VHDL-ASSIGN", "assignment outside a process", l);
        return;
    }
    ProcState& st = *ctx.st;
    if (isZero(st.alive))
        return;
    if (targets.size() > 1) {
        // aggregate target: split the value, left target = most significant bits
        SigSpec bits = bitsOf(value, l);
        std::uint32_t total = 0;
        for (const auto& t : targets)
            total += t.width;
        if (bits.width() != total) {
            error("VHDL-LENGTH", "aggregate target has " + std::to_string(total) + " bit(s), value has " +
                                     std::to_string(bits.width()),
                  l);
            return;
        }
        // least significant part first (same register order as a Verilog concatenation target)
        std::uint32_t lo = 0;
        for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
            assign({*it}, makeBits(it->type, bits.extract(lo, it->width)), signal, ctx, l);
            lo += it->width;
        }
        return;
    }
    if (targets.empty())
        return;
    const Target& t = targets.front();
    Object& o = object(t.object);
    if (signal != o.isSignal) {
        error("VHDL-ASSIGN", std::string(signal ? "signal assignment (<=) to variable '" : "variable assignment (:=) to signal '") +
                                 o.name + "'",
              l);
        return;
    }
    if (o.isPort && o.mode == Mode::In) {
        error("VHDL-ASSIGN", "assignment to input port '" + o.name + "'", l);
        return;
    }
    SVal v = t.type ? convert(value, t.type, ctx, l) : value;
    if (!v.valid())
        return;
    const auto W = bitWidth(*o.type);
    // whole-object assignment of a known value: keep the value (also for
    // types without hardware representation, e.g. real variables)
    if (!signal && v.known() && !t.dynamic && t.offset == 0 && (!W || t.width == *W) && isOne(st.alive)) {
        SVal nv = v;
        nv.type = o.type;
        st.cur[t.object] = nv;
        if (W) {
            markBits(st.any[t.object], *W, 0, *W);
            markBits(st.all[t.object], *W, 0, *W);
        }
        return;
    }
    if (!W) {
        error("VHDL-NO-HARDWARE", "'" + o.name + "' of type '" + o.type->name + "' has no hardware representation", l);
        return;
    }
    SigSpec base;
    if (signal) {
        if (const SigSpec* n = st.nba.find(t.object))
            base = *n;
        else
            base = o.storage;
    }
    else if (!t.dynamic && t.offset == 0 && t.width == *W && isOne(st.alive)) {
        // the whole variable is overwritten: its previous value is not read
        base = SigSpec::allX(*W);
    }
    else {
        base = bitsOf(readObject(t.object, ctx, l), l);
    }
    if (base.width() != *W)
        base = base.resized(*W, false);
    SigSpec vb = bitsOf(v, l);
    SigSpec nb;
    if (!t.dynamic) {
        if (vb.width() != t.width) {
            error("VHDL-LENGTH", "value has " + std::to_string(vb.width()) + " bit(s), target '" + o.name + "' part has " +
                                     std::to_string(t.width),
                  l);
            return;
        }
        nb = base.replaced(t.offset, vb);
    }
    else {
        ir::Cell c;
        c.kind = ir::CellKind::DynInsert;
        c.inputs = {base.extract(t.regionOffset, t.regionWidth), t.index, vb.resized(t.elemWidth, false)};
        c.width = t.regionWidth;
        c.elemWidth = t.elemWidth;
        c.isSigned = t.indexSigned;
        c.rangeLeft = static_cast<std::int32_t>(t.range.left);
        c.rangeRight = static_cast<std::int32_t>(t.range.right);
        c.origin = srcLoc(l);
        SigSpec region;
        if (body && body->ed)
            region = body->ed->emit(std::move(c));
        else {
            error("VHDL-NOT-STATIC", "variable index in a constant context", l);
            return;
        }
        nb = base.replaced(t.regionOffset, region);
    }
    if (!isOne(st.alive))
        nb = mux(st.alive, base, nb, l);
    if (signal)
        st.nba[t.object] = nb;
    else
        st.cur[t.object] = makeBits(o.type, nb);
    const std::uint32_t off = t.dynamic ? t.regionOffset : t.offset;
    const std::uint32_t w = t.dynamic ? t.regionWidth : t.width;
    markBits(st.any[t.object], *W, off, w);
    if (!t.dynamic && isOne(st.alive))
        markBits(st.all[t.object], *W, off, w);
    else
        (void)st.all[t.object];
}

// ---------------------------------------------------------------------------
// Merge
// ---------------------------------------------------------------------------

void Elaborator::mergeStates(const ir::SigSpec& cond, ProcState& t, ProcState& f, ProcState& out, Loc l) {
    auto pick = [&](const SigSpec& a, const SigSpec& b) { return mux(cond, a, b, l); }; // a: cond false
    Ctx dummy;
    // variables
    auto varValue = [&](ProcState& s, int idx) -> SVal {
        if (const SVal* v = s.cur.find(idx))
            return *v;
        Object& o = object(idx);
        if (!o.storage.empty()) {
            o.readInitial = true;
            return SVal{o.type, std::nullopt, o.storage};
        }
        return o.initial.valid() ? o.initial : initialValue(o, l);
    };
    std::vector<int> vars;
    for (const auto& [k, v] : t.cur)
        vars.push_back(k);
    for (const auto& [k, v] : f.cur)
        if (std::ranges::find(vars, k) == vars.end())
            vars.push_back(k);
    for (int idx : vars) {
        SVal a = varValue(f, idx);
        SVal b = varValue(t, idx);
        if (a.known() && b.known() && *a.value == *b.value) {
            out.cur[idx] = a;
            continue;
        }
        if (!a.valid() || !b.valid())
            continue;
        if (!bitWidth(*object(idx).type)) {
            error("VHDL-NOT-STATIC", "variable '" + object(idx).name + "' of type '" + object(idx).type->name +
                                         "' depends on a non-static condition",
                  l);
            out.cur[idx] = a;
            continue;
        }
        out.cur[idx] = makeBits(object(idx).type, pick(bitsOf(a, l), bitsOf(b, l)));
    }
    // signals
    std::vector<int> sigs;
    for (const auto& [k, v] : t.nba)
        sigs.push_back(k);
    for (const auto& [k, v] : f.nba)
        if (std::ranges::find(sigs, k) == sigs.end())
            sigs.push_back(k);
    for (int idx : sigs) {
        const SigSpec* a = f.nba.find(idx);
        const SigSpec* b = t.nba.find(idx);
        SigSpec av = a ? *a : object(idx).storage;
        SigSpec bv = b ? *b : object(idx).storage;
        out.nba[idx] = av == bv ? av : pick(av, bv);
    }
    // masks
    auto mergeMasks = [](OrderedMap<int, BitMask>& tm, OrderedMap<int, BitMask>& fm, OrderedMap<int, BitMask>& om,
                         bool isAll) {
        std::vector<int> keys;
        for (const auto& [k, v] : tm)
            keys.push_back(k);
        for (const auto& [k, v] : fm)
            if (std::ranges::find(keys, k) == keys.end())
                keys.push_back(k);
        for (int k : keys) {
            const BitMask* a = tm.find(k);
            const BitMask* b = fm.find(k);
            const std::size_t n = std::max(a ? a->size() : 0, b ? b->size() : 0);
            BitMask m(n, false);
            for (std::size_t i = 0; i < n; ++i) {
                const bool av = a && i < a->size() && (*a)[i];
                const bool bv = b && i < b->size() && (*b)[i];
                m[i] = isAll ? (av && bv) : (av || bv);
            }
            om[k] = std::move(m);
        }
    };
    mergeMasks(t.any, f.any, out.any, false);
    mergeMasks(t.all, f.all, out.all, true);
    out.alive = t.alive == f.alive ? t.alive : pick(f.alive, t.alive);
    out.returned = t.returned == f.returned ? t.returned : pick(f.returned, t.returned);
    out.loops = t.loops;
    for (std::size_t k = 0; k < out.loops.size() && k < f.loops.size(); ++k) {
        if (t.loops[k].exit != f.loops[k].exit)
            out.loops[k].exit = pick(f.loops[k].exit, t.loops[k].exit);
        if (t.loops[k].skip != f.loops[k].skip)
            out.loops[k].skip = pick(f.loops[k].skip, t.loops[k].skip);
    }
    if (t.retValue.valid() && f.retValue.valid()) {
        if (t.retValue.known() && f.retValue.known() && *t.retValue.value == *f.retValue.value)
            out.retValue = t.retValue;
        else if (!bitWidth(*t.retValue.type))
            out.retValue = isOne(cond) ? t.retValue : f.retValue;
        else
            out.retValue = makeBits(t.retValue.type, pick(bitsOf(f.retValue, l), bitsOf(t.retValue, l)));
    }
    else {
        out.retValue = t.retValue.valid() ? t.retValue : f.retValue;
    }
    out.hasWait = t.hasWait || f.hasWait;
    (void)dummy;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void Elaborator::execList(const std::vector<StmtPtr>& list, Ctx& ctx) {
    for (const auto& s : list) {
        if (ctx.st && isZero(ctx.st->alive))
            return;
        exec(*s, ctx);
    }
}

namespace {

/// Recomputes "alive" from the innermost loop frame and the return flag.
SigSpec frameAlive(const ProcState& st, const CondOps& ops, std::size_t upto) {
    SigSpec a = upto < st.loops.size() ? st.loops[upto].aliveStart : one();
    for (std::size_t k = 0; k <= upto && k < st.loops.size(); ++k)
        a = ops.andB(a, ops.andB(ops.notB(st.loops[k].exit), ops.notB(st.loops[k].skip)));
    return ops.andB(a, ops.notB(st.returned));
}

} // namespace

void Elaborator::exec(const Stmt& s, Ctx& ctx) {
    ProcState& st = *ctx.st;
    const CondOps ops{*this, s.loc};
    auto condBit = [&](const Expr& c) -> SVal {
        SVal v = eval(c, ctx, std_.boolean);
        if (v.valid() && v.type->baseTag() == TypeTag::StdUlogic)
            v = makeBits(std_.boolean, bitsOf(v, c.loc)); // VHDL-2008 implicit ??
        else if (v.valid() && v.type->baseTag() != TypeTag::Boolean && v.type->baseTag() != TypeTag::Bit)
            error("VHDL-TYPE", "condition must be boolean, not '" + v.type->name + "'", c.loc);
        return v;
    };

    switch (s.kind) {
        case StmtKind::Null:
        case StmtKind::Assert:
        case StmtKind::Report:
        case StmtKind::Ignored: return;

        case StmtKind::VariableAssign:
        case StmtKind::SignalAssign: {
            const bool signal = s.kind == StmtKind::SignalAssign;
            std::vector<Target> tg;
            const Type* tt = nullptr;
            if (!targets(*s.target, ctx, tg, &tt))
                return;
            if (!tt && tg.size() > 1) {
                // aggregate target: the value's type comes from the right-hand side
            }
            auto valueOf = [&](const AssignAlt& a) -> SVal {
                if (a.wave.empty())
                    return SVal{};
                if (a.wave.size() > 1 && !reported_.contains("wave:" + std::to_string(s.loc.line)))
                    reported_.insert("wave:" + std::to_string(s.loc.line));
                if (!a.wave[0].value)
                    return SVal{};
                return eval(*a.wave[0].value, ctx, tt);
            };
            if (s.form == Stmt::Form::Simple) {
                if (s.alts.empty() || s.alts[0].unaffected)
                    return;
                SVal v = valueOf(s.alts[0]);
                if (v.valid())
                    assign(tg, v, signal, ctx, s.loc);
                return;
            }
            // conditional / selected forms: an if / case over the same target
            std::vector<SigSpec> conds;
            if (s.form == Stmt::Form::Selected) {
                SVal sel = eval(*s.selector, ctx, nullptr);
                for (const auto& a : s.alts) {
                    SigSpec c = zero();
                    for (const auto& ch : a.choices) {
                        if (ch->kind == ExprKind::Others) {
                            c = one();
                            break;
                        }
                        SigSpec m;
                        if (ch->kind == ExprKind::Range || ch->kind == ExprKind::Subtype) {
                            auto r = discreteRange(*ch, ctx);
                            if (!r)
                                continue;
                            SVal lo = makeConst(sel.type, Value::integer(r->low()));
                            SVal hi = makeConst(sel.type, Value::integer(r->high()));
                            std::vector<SVal> a1{sel, lo};
                            std::vector<SVal> a2{sel, hi};
                            m = ops.andB(bitsOf(applyPredefined(Op::Ge, a1, std_.boolean, ctx, ch->loc), ch->loc),
                                         bitsOf(applyPredefined(Op::Le, a2, std_.boolean, ctx, ch->loc), ch->loc));
                        }
                        else {
                            SVal cv = convert(eval(*ch, ctx, sel.type), sel.type, ctx, ch->loc);
                            if (s.matching) {
                                m = cell(ir::CellKind::EqWild, {bitsOf(sel, ch->loc), bitsOf(cv, ch->loc)}, 1, ch->loc);
                            }
                            else {
                                std::vector<SVal> args{sel, cv};
                                m = bitsOf(applyPredefined(Op::Eq, args, std_.boolean, ctx, ch->loc), ch->loc);
                            }
                        }
                        c = ops.orB(c, m);
                    }
                    conds.push_back(c);
                }
            }
            else {
                for (const auto& a : s.alts)
                    conds.push_back(a.cond ? bitsOf(condBit(*a.cond), a.loc) : one());
            }
            // fold from the last alternative: v = c0 ? v0 : (c1 ? v1 : ... hold)
            ProcState saved = st;
            std::vector<ProcState> branchStates;
            // execute as nested if/else so that partial assignment semantics are kept
            std::function<void(std::size_t, ProcState&)> run = [&](std::size_t k, ProcState& state) {
                if (k >= s.alts.size())
                    return;
                const auto& a = s.alts[k];
                const SigSpec& c = conds[k];
                Ctx c2 = ctx;
                c2.st = &state;
                if (isOne(c)) {
                    if (!a.unaffected) {
                        SVal v = valueOf(a);
                        if (v.valid())
                            assign(tg, v, signal, c2, a.loc);
                    }
                    return;
                }
                if (isZero(c)) {
                    run(k + 1, state);
                    return;
                }
                ProcState tst = state;
                Ctx ct = ctx;
                ct.st = &tst;
                if (!a.unaffected) {
                    SVal v = valueOf(a);
                    if (v.valid())
                        assign(tg, v, signal, ct, a.loc);
                }
                ProcState fst = state;
                run(k + 1, fst);
                ProcState merged;
                mergeStates(c, tst, fst, merged, a.loc);
                state = std::move(merged);
            };
            run(0, st);
            (void)saved;
            return;
        }

        case StmtKind::If: {
            std::function<void(std::size_t, ProcState&)> run = [&](std::size_t k, ProcState& state) {
                if (k >= s.branches.size())
                    return;
                const IfBranch& b = s.branches[k];
                Ctx c2 = ctx;
                c2.st = &state;
                if (!b.cond) {
                    execList(b.body, c2);
                    return;
                }
                SVal cv = condBit(*b.cond);
                if (!cv.valid())
                    return;
                if (cv.known()) {
                    if (cv.value->i != 0)
                        execList(b.body, c2);
                    else
                        run(k + 1, state);
                    return;
                }
                const SigSpec c = toBool(bitsOf(cv, b.loc), b.loc);
                ProcState tst = state;
                Ctx ct = ctx;
                ct.st = &tst;
                execList(b.body, ct);
                ProcState fst = state;
                run(k + 1, fst);
                ProcState merged;
                mergeStates(c, tst, fst, merged, b.loc);
                state = std::move(merged);
            };
            run(0, st);
            return;
        }

        case StmtKind::Case: {
            SVal sel = eval(*s.selector, ctx, nullptr);
            if (!sel.valid())
                return;
            // conditions per alternative
            std::vector<SigSpec> conds;
            std::vector<bool> isOthers;
            std::optional<std::size_t> staticPick;
            for (std::size_t k = 0; k < s.caseAlts.size(); ++k) {
                const CaseAlt& a = s.caseAlts[k];
                SigSpec c = zero();
                bool others = false;
                for (const auto& ch : a.choices) {
                    if (ch->kind == ExprKind::Others) {
                        others = true;
                        continue;
                    }
                    SigSpec m;
                    bool range = ch->kind == ExprKind::Range || ch->kind == ExprKind::Subtype;
                    if (!range && (ch->kind == ExprKind::Simple || ch->kind == ExprKind::Selected)) {
                        auto syms = resolveName(*ch, ctx, true);
                        range = !syms.empty() && syms.front()->kind == SymKind::Type;
                    }
                    if (range) {
                        auto r = discreteRange(*ch, ctx);
                        if (!r)
                            continue;
                        SVal lo = makeConst(sel.type, Value::integer(r->low()));
                        SVal hi = makeConst(sel.type, Value::integer(r->high()));
                        std::vector<SVal> a1{sel, lo};
                        std::vector<SVal> a2{sel, hi};
                        m = ops.andB(bitsOf(applyPredefined(Op::Ge, a1, std_.boolean, ctx, ch->loc), ch->loc),
                                     bitsOf(applyPredefined(Op::Le, a2, std_.boolean, ctx, ch->loc), ch->loc));
                    }
                    else {
                        SVal cv = convert(eval(*ch, ctx, sel.type), sel.type, ctx, ch->loc);
                        if (!cv.valid())
                            continue;
                        if (s.matching) {
                            m = cell(ir::CellKind::EqWild, {bitsOf(sel, ch->loc), bitsOf(cv, ch->loc)}, 1, ch->loc);
                        }
                        else {
                            std::vector<SVal> args{sel, cv};
                            m = bitsOf(applyPredefined(Op::Eq, args, std_.boolean, ctx, ch->loc), ch->loc);
                        }
                    }
                    c = ops.orB(c, m);
                }
                if (others)
                    c = one();
                if (sel.known() && isOne(c) && !staticPick)
                    staticPick = k;
                conds.push_back(c);
                isOthers.push_back(others);
            }
            if (sel.known()) {
                if (staticPick) {
                    Ctx c2 = ctx;
                    execList(s.caseAlts[*staticPick].body, c2);
                }
                return;
            }
            std::function<void(std::size_t, ProcState&)> run = [&](std::size_t k, ProcState& state) {
                if (k >= s.caseAlts.size())
                    return;
                Ctx c2 = ctx;
                c2.st = &state;
                if (isOne(conds[k])) {
                    execList(s.caseAlts[k].body, c2);
                    return;
                }
                if (isZero(conds[k])) {
                    run(k + 1, state);
                    return;
                }
                ProcState tst = state;
                Ctx ct = ctx;
                ct.st = &tst;
                execList(s.caseAlts[k].body, ct);
                ProcState fst = state;
                run(k + 1, fst);
                ProcState merged;
                mergeStates(conds[k], tst, fst, merged, s.caseAlts[k].loc);
                state = std::move(merged);
            };
            run(0, st);
            return;
        }

        case StmtKind::Loop: {
            ProcState::LoopFrame frame;
            frame.label = s.label.key;
            frame.aliveStart = st.alive;
            frame.exit = zero();
            frame.skip = zero();
            st.loops.push_back(frame);
            const std::size_t me = st.loops.size() - 1;
            std::uint64_t iter = 0;
            auto beginIteration = [&]() {
                st.loops[me].skip = zero();
                st.alive = frameAlive(st, ops, me);
            };
            auto tooMany = [&]() {
                if (++iter > opts_.maxLoopIterations) {
                    error("VHDL-LOOP", "loop runs more than " + std::to_string(opts_.maxLoopIterations) +
                                           " iterations (or its exit condition is not static)",
                          s.loc);
                    return true;
                }
                return false;
            };
            if (s.loopKind == Stmt::LoopKind::For) {
                const Type* it = nullptr;
                auto r = discreteRange(*s.range, ctx, &it);
                if (r) {
                    Scope* ls = newScope(ctx.scope, ctx.scope->label);
                    Symbol* var = newSymbol(SymKind::Constant, s.loopVar.key, s.loopVar.text, s.loopVar.loc);
                    var->type = it ? it : std_.integer;
                    ls->add(var);
                    Ctx lc = ctx;
                    lc.scope = ls;
                    for (std::uint64_t k = 0; k < r->length(); ++k) {
                        if (tooMany())
                            break;
                        beginIteration();
                        if (isZero(st.alive))
                            break;
                        var->value = makeConst(var->type, Value::integer(r->indexAt(k)));
                        execList(s.body, lc);
                    }
                }
            }
            else {
                for (;;) {
                    if (tooMany())
                        break;
                    beginIteration();
                    if (isZero(st.alive))
                        break;
                    if (s.loopKind == Stmt::LoopKind::While) {
                        SVal c = condBit(*s.cond);
                        if (!c.valid())
                            break;
                        if (!c.known()) {
                            unsupported("IR-UNSUPPORTED-LOOP", "while loop whose condition is not static", s.loc);
                            break;
                        }
                        if (c.value->i == 0)
                            break;
                    }
                    execList(s.body, ctx);
                    if (s.loopKind == Stmt::LoopKind::Plain && !isZero(st.loops[me].exit) &&
                        !isOne(st.loops[me].exit) && iter > 1024) {
                        unsupported("IR-UNSUPPORTED-LOOP", "loop whose exit condition is not static", s.loc);
                        break;
                    }
                    if (isOne(st.loops[me].exit))
                        break;
                }
            }
            // after the loop: paths that exited continue
            st.loops.pop_back();
            st.alive = st.loops.empty() ? ops.andB(frame.aliveStart, ops.notB(st.returned))
                                        : ops.andB(frame.aliveStart, frameAlive(st, ops, st.loops.size() - 1));
            if (st.loops.empty() && !isOne(frame.aliveStart))
                st.alive = ops.andB(frame.aliveStart, ops.notB(st.returned));
            return;
        }

        case StmtKind::Next:
        case StmtKind::Exit: {
            SigSpec c = st.alive;
            if (s.cond) {
                SVal cv = condBit(*s.cond);
                if (!cv.valid())
                    return;
                c = ops.andB(c, toBool(bitsOf(cv, s.loc), s.loc));
            }
            if (st.loops.empty()) {
                error("VHDL-LOOP", std::string(s.kind == StmtKind::Next ? "next" : "exit") + " outside a loop", s.loc);
                return;
            }
            std::size_t target = st.loops.size() - 1;
            if (!s.loopLabel.empty()) {
                bool found = false;
                for (std::size_t k = st.loops.size(); k-- > 0;)
                    if (st.loops[k].label == s.loopLabel.key) {
                        target = k;
                        found = true;
                        break;
                    }
                if (!found) {
                    error("VHDL-LOOP", "no enclosing loop labelled '" + s.loopLabel.text + "'", s.loc);
                    return;
                }
            }
            for (std::size_t k = target + 1; k < st.loops.size(); ++k)
                st.loops[k].exit = ops.orB(st.loops[k].exit, c);
            if (s.kind == StmtKind::Exit)
                st.loops[target].exit = ops.orB(st.loops[target].exit, c);
            else
                st.loops[target].skip = ops.orB(st.loops[target].skip, c);
            st.alive = ops.andB(st.alive, ops.notB(c));
            return;
        }

        case StmtKind::Return: {
            if (s.value) {
                SVal v = eval(*s.value, ctx, ctx.returnType);
                if (ctx.returnType)
                    v = convert(v, ctx.returnType, ctx, s.loc);
                if (!v.valid())
                    return;
                if (!st.retValue.valid() || isOne(st.alive)) {
                    st.retValue = v;
                }
                else if (bitWidth(*v.type)) {
                    st.retValue = makeBits(v.type, mux(st.alive, bitsOf(st.retValue, s.loc), bitsOf(v, s.loc), s.loc));
                }
                else {
                    error("VHDL-NOT-STATIC", "function returns a '" + v.type->name + "' under a non-static condition",
                          s.loc);
                }
            }
            st.returned = ops.orB(st.returned, st.alive);
            st.alive = zero();
            return;
        }

        case StmtKind::Wait:
            st.hasWait = true;
            return;

        case StmtKind::ProcCall: {
            const Expr& call = *s.call;
            const Expr* name = &call;
            static const std::vector<Assoc> none;
            const std::vector<Assoc>* args = &none;
            if (call.kind == ExprKind::Call) {
                name = call.prefix.get();
                args = &call.args;
            }
            auto syms = resolveName(*name, ctx);
            if (syms.empty())
                return;
            Symbol* p = chooseSubprogram(syms, *args, nullptr, false, ctx, s.loc);
            if (!p)
                return;
            std::vector<SVal> values;
            std::vector<std::vector<Target>> outs;
            if (!evalArgs(*p, *args, ctx, s.loc, values, outs))
                return;
            callSubprogram(*p, values, outs, nullptr, ctx, s.loc);
            return;
        }

        default:
            error("VHDL-STATEMENT", "concurrent statement inside a process", s.loc);
            return;
    }
}

// ---------------------------------------------------------------------------
// Clock edges
// ---------------------------------------------------------------------------

std::optional<std::pair<ir::SigSpec, ir::Edge>> Elaborator::clockEdge(const Expr& e, Ctx& ctx, const Expr** rest) {
    if (rest)
        *rest = nullptr;
    const Expr* x = &e;
    while (x->parenthesized && false)
        break;
    // rising_edge(clk) / falling_edge(clk)
    if (x->kind == ExprKind::Call && x->args.size() == 1 &&
        (x->prefix->kind == ExprKind::Simple || x->prefix->kind == ExprKind::Selected)) {
        auto syms = resolveName(*x->prefix, ctx, true);
        for (Symbol* s : syms) {
            if (s->kind == SymKind::Subprogram && (s->builtin == Builtin::RisingEdge || s->builtin == Builtin::FallingEdge)) {
                SVal clk = eval(*x->args[0].actual, ctx, nullptr);
                if (!clk.valid())
                    return std::nullopt;
                return std::make_pair(bitsOf(clk, x->loc).extract(0, 1),
                                      s->builtin == Builtin::RisingEdge ? ir::Edge::Pos : ir::Edge::Neg);
            }
        }
        return std::nullopt;
    }
    if (x->kind != ExprKind::Binary || x->op != Op::And)
        return std::nullopt;
    // clk'event and clk = '1'   (either order), not clk'stable and clk = '1'
    auto isEvent = [&](const Expr& a) -> const Expr* {
        if (a.kind == ExprKind::Attribute && a.ident.key == "event")
            return a.prefix.get();
        if (a.kind == ExprKind::Unary && a.op == Op::Not && a.lhs->kind == ExprKind::Attribute &&
            a.lhs->ident.key == "stable")
            return a.lhs->prefix.get();
        return nullptr;
    };
    auto level = [&](const Expr& a, const Expr& sig) -> std::optional<bool> {
        if (a.kind != ExprKind::Binary || a.op != Op::Eq)
            return std::nullopt;
        auto sameName = [](const Expr& p, const Expr& q) {
            return p.kind == q.kind && p.kind == ExprKind::Simple && p.ident.key == q.ident.key;
        };
        const Expr* lit = nullptr;
        if (sameName(*a.lhs, sig))
            lit = a.rhs.get();
        else if (sameName(*a.rhs, sig))
            lit = a.lhs.get();
        if (!lit || lit->kind != ExprKind::Character)
            return std::nullopt;
        if (lit->ident.key == "'1'")
            return true;
        if (lit->ident.key == "'0'")
            return false;
        return std::nullopt;
    };
    const Expr* ev = isEvent(*x->lhs);
    const Expr* other = x->rhs.get();
    if (!ev) {
        ev = isEvent(*x->rhs);
        other = x->lhs.get();
    }
    if (ev) {
        if (auto lv = level(*other, *ev)) {
            SVal clk = eval(*ev, ctx, nullptr);
            if (!clk.valid())
                return std::nullopt;
            return std::make_pair(bitsOf(clk, x->loc).extract(0, 1), *lv ? ir::Edge::Pos : ir::Edge::Neg);
        }
    }
    // EDGE and enable / enable and EDGE
    const Expr* r = nullptr;
    if (auto ce = clockEdge(*x->lhs, ctx, &r); ce && !r) {
        if (rest)
            *rest = x->rhs.get();
        return ce;
    }
    if (auto ce = clockEdge(*x->rhs, ctx, &r); ce && !r) {
        if (rest)
            *rest = x->lhs.get();
        return ce;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

namespace {

/// Splits the object's bit range [off, off+w) at net boundaries of its storage.
template <class F>
void forEachNetPiece(const SigSpec& storage, std::uint32_t off, std::uint32_t w, F&& fn) {
    std::uint32_t pos = 0;
    for (const auto& ch : storage.chunks()) {
        const std::uint32_t lo = std::max(pos, off);
        const std::uint32_t hi = std::min(pos + ch.width, off + w);
        if (lo < hi)
            fn(lo, hi - lo);
        pos += ch.width;
    }
}

} // namespace

void Elaborator::lowerProcess(const Stmt& p, Scope& scope) {
    const std::string label = p.label.empty() ? "proc" + std::to_string(body->processCounter++) : p.label.text;
    Scope* ps = newScope(&scope, label);
    ProcState st;
    st.alive = one();
    st.returned = zero();
    Ctx ctx;
    ctx.scope = ps;
    ctx.st = &st;
    elaborateDeclarativePart(*ps, p.decls, ctx);
    const Loc l = p.loc;
    const SourceLocation origin = srcLoc(l);

    // --- find the clocked structure -------------------------------------
    struct AsyncBranch {
        const Expr* cond;
        const std::vector<StmtPtr>* body;
    };
    std::vector<AsyncBranch> asyncs;
    const std::vector<StmtPtr>* clockedBody = nullptr;
    std::vector<const Stmt*> prefix; // statements before the clocked if (variables only)
    const Expr* enableExpr = nullptr;
    std::optional<std::pair<SigSpec, ir::Edge>> clock;
    std::vector<StmtPtr> waitBody; // unused holder

    const auto& stmts = p.body;
    std::size_t clockedIndex = stmts.size();
    if (!stmts.empty() && stmts.front()->kind == StmtKind::Wait && stmts.front()->cond) {
        // process ... begin wait until rising_edge(clk); ... end process;
        const Expr* rest = nullptr;
        clock = clockEdge(*stmts.front()->cond, ctx, &rest);
        if (clock) {
            enableExpr = rest;
            clockedIndex = 0;
        }
    }
    else {
        for (std::size_t k = 0; k < stmts.size() && !clock; ++k) {
            const Stmt& s = *stmts[k];
            if (s.kind != StmtKind::If)
                continue;
            for (std::size_t b = 0; b < s.branches.size(); ++b) {
                if (!s.branches[b].cond)
                    break;
                const Expr* rest = nullptr;
                auto ce = clockEdge(*s.branches[b].cond, ctx, &rest);
                if (!ce)
                    continue;
                clock = ce;
                enableExpr = rest;
                clockedIndex = k;
                clockedBody = &s.branches[b].body;
                for (std::size_t a = 0; a < b; ++a)
                    asyncs.push_back({s.branches[a].cond.get(), &s.branches[a].body});
                if (b + 1 < s.branches.size() && !s.branches[b + 1].body.empty())
                    warn("VHDL-CLOCKED-ELSE", "statements after the clock edge branch of a clocked process are ignored",
                         s.branches[b + 1].loc);
                break;
            }
        }
    }

    if (!clock) {
        // ---------------- combinational process ----------------
        bool wait = std::ranges::any_of(stmts, [](const StmtPtr& s) { return s->kind == StmtKind::Wait; });
        if (wait) {
            // "wait on a, b;" as the last statement is a sensitivity list
            const bool sensitivityWait = stmts.back()->kind == StmtKind::Wait && !stmts.back()->cond &&
                                         !stmts.back()->value &&
                                         std::count_if(stmts.begin(), stmts.end(), [](const StmtPtr& s) {
                                             return s->kind == StmtKind::Wait;
                                         }) == 1;
            if (!sensitivityWait) {
                warn("VHDL-TESTBENCH", "process '" + label + "' uses wait statements (simulation code); not analyzed", l,
                     Severity::Info);
                return;
            }
        }
        execList(stmts, ctx);
        for (const auto& [idx, anyMask] : st.any) {
            Object& o = object(idx);
            if (o.storage.empty())
                continue;
            SigSpec value;
            if (o.isSignal) {
                const SigSpec* v = st.nba.find(idx);
                if (!v)
                    continue;
                value = *v;
            }
            else {
                if (!o.readInitial)
                    continue; // variable written before every read: no storage
                const SVal* v = st.cur.find(idx);
                if (!v)
                    continue;
                value = bitsOf(*v, l);
            }
            const BitMask* allMask = st.all.find(idx);
            const auto W = o.storage.width();
            BitMask comb(W, false);
            BitMask latch(W, false);
            for (std::uint32_t i = 0; i < W && i < anyMask.size(); ++i) {
                if (!anyMask[i])
                    continue;
                const bool full = allMask && i < allMask->size() && (*allMask)[i];
                if (full && o.isSignal)
                    comb[i] = true;
                else
                    latch[i] = true;
            }
            for (const auto& [ro, rw] : runs(comb))
                forEachNetPiece(o.storage, ro, rw, [&](std::uint32_t off, std::uint32_t w) {
                    body->body.connections.push_back(
                        {o.storage.extract(off, w), value.extract(off, w), ir::ConnKind::Process, origin});
                });
            for (const auto& [ro, rw] : runs(latch))
                forEachNetPiece(o.storage, ro, rw, [&](std::uint32_t off, std::uint32_t w) {
                    body->ed->addLatch({o.storage.extract(off, w), value.extract(off, w), origin}, true);
                });
        }
        return;
    }

    // ---------------- clocked process ----------------
    if (clock->first.width() != 1 || clock->first.empty()) {
        error("VHDL-CLOCK", "clock of process '" + label + "' is not a 1-bit signal", l);
        return;
    }
    // statements before the clocked if (typically none)
    for (std::size_t k = 0; k < clockedIndex && k < stmts.size(); ++k) {
        if (stmts[k]->kind == StmtKind::Wait)
            continue;
        exec(*stmts[k], ctx);
    }
    ProcState pre = st;
    ProcState clocked = pre;
    {
        Ctx cc = ctx;
        cc.st = &clocked;
        if (clockedBody) {
            execList(*clockedBody, cc);
        }
        else {
            // wait-until form: the rest of the process body
            for (std::size_t k = 1; k < stmts.size(); ++k) {
                if (stmts[k]->kind == StmtKind::Wait) {
                    warn("VHDL-WAIT", "only one 'wait until' per clocked process is supported", stmts[k]->loc);
                    break;
                }
                exec(*stmts[k], cc);
            }
        }
    }
    std::vector<ProcState> asyncStates(asyncs.size(), pre);
    struct Ctrl {
        SigSpec signal;
        bool activeHigh;
    };
    std::vector<Ctrl> ctrls;
    for (std::size_t k = 0; k < asyncs.size(); ++k) {
        Ctx ac = ctx;
        ac.st = &asyncStates[k];
        SVal c = eval(*asyncs[k].cond, ac, std_.boolean);
        SigSpec bit = toBool(bitsOf(c, l), l);
        bool high = true;
        // strip inverters: not rst_n / rst_n = '0'
        for (int guard = 0; guard < 4 && bit.width() == 1 && bit.chunks().size() == 1 && !bit.chunks()[0].isConst();
             ++guard) {
            const ir::Cell* drv = body->ed->driverOf(bit.chunks()[0].net);
            if (!drv || (drv->kind != ir::CellKind::Not && drv->kind != ir::CellKind::LogicNot) ||
                drv->inputs[0].width() != 1)
                break;
            bit = drv->inputs[0];
            high = !high;
        }
        ctrls.push_back({bit, high});
        execList(*asyncs[k].body, ac);
    }
    SigSpec enable;
    if (enableExpr) {
        SVal ev = eval(*enableExpr, ctx, std_.boolean);
        enable = toBool(bitsOf(ev, l), l);
    }

    // objects written in any part
    std::vector<int> objs;
    auto collect = [&](const ProcState& s) {
        for (const auto& [idx, m] : s.any)
            if (std::ranges::find(objs, idx) == objs.end())
                objs.push_back(idx);
    };
    collect(clocked);
    for (const auto& s : asyncStates)
        collect(s);

    for (int idx : objs) {
        Object& o = object(idx);
        if (o.storage.empty())
            continue;
        const auto W = o.storage.width();
        auto finalValue = [&](const ProcState& s) -> SigSpec {
            if (o.isSignal) {
                if (const SigSpec* v = s.nba.find(idx))
                    return *v;
                return o.storage;
            }
            if (const SVal* v = s.cur.find(idx))
                return bitsOf(*v, l);
            return o.storage;
        };
        if (!o.isSignal && !o.readInitial)
            continue; // variable never read before written: combinational inside the process
        BitMask any(W, false);
        auto orMask = [&](const ProcState& s) {
            if (const BitMask* m = s.any.find(idx))
                for (std::size_t i = 0; i < m->size() && i < W; ++i)
                    any[i] = any[i] || (*m)[i];
        };
        orMask(clocked);
        for (const auto& s : asyncStates)
            orMask(s);
        std::vector<std::uint32_t> signature(W, 0);
        for (std::size_t k = 0; k < asyncStates.size() && k < 32; ++k)
            if (const BitMask* m = asyncStates[k].any.find(idx))
                for (std::uint32_t i = 0; i < W && i < m->size(); ++i)
                    if ((*m)[i])
                        signature[i] |= 1U << k;
        SigSpec d = finalValue(clocked).resized(W, false);
        if (!enable.empty())
            d = mux(enable, o.storage, d, l);
        for (const auto& [ro, rw] : runs(any)) {
            std::uint32_t start = ro;
            for (std::uint32_t i = ro + 1; i <= ro + rw; ++i) {
                if (i != ro + rw && signature[i] == signature[start])
                    continue;
                forEachNetPiece(o.storage, start, i - start, [&](std::uint32_t off, std::uint32_t w) {
                    ir::Register r;
                    r.q = o.storage.extract(off, w);
                    r.d = d.extract(off, w);
                    r.clock = {clock->first, clock->second};
                    r.origin = origin;
                    for (std::size_t k = 0; k < ctrls.size(); ++k) {
                        if (k >= 32 || !(signature[off] & (1U << k)))
                            continue;
                        SigSpec value = finalValue(asyncStates[k]).resized(W, false).extract(off, w);
                        if (!value.isFullyConst())
                            warn("VHDL-ASYNC-LOAD",
                                 "asynchronous branch loads a non-constant value into '" + o.name + "'", l);
                        r.asyncResets.push_back({ctrls[k].signal, ctrls[k].activeHigh, value});
                    }
                    body->ed->extractControls(r, origin);
                    body->body.registers.push_back(std::move(r));
                });
                start = i;
            }
        }
    }
}

void Elaborator::lowerConcurrentAssign(const Stmt& s, Scope& scope) {
    // A concurrent assignment is a process sensitive to everything it reads.
    ProcState st;
    st.alive = one();
    st.returned = zero();
    Ctx ctx;
    ctx.scope = &scope;
    ctx.st = &st;
    Stmt seq;
    // reuse the sequential execution of the same assignment forms
    seq.kind = StmtKind::SignalAssign;
    seq.loc = s.loc;
    seq.form = s.form;
    seq.matching = s.matching;
    // non-owning views of the AST: copy the pointers into a temporary
    // statement and release them afterwards
    seq.target.reset(s.target.get());
    seq.selector.reset(s.selector.get());
    for (const auto& a : s.alts) {
        AssignAlt c;
        c.unaffected = a.unaffected;
        c.loc = a.loc;
        c.cond.reset(a.cond.get());
        for (const auto& w : a.wave) {
            WaveElem we;
            we.value.reset(w.value.get());
            we.after.reset(w.after.get());
            c.wave.push_back(std::move(we));
        }
        for (const auto& ch : a.choices)
            c.choices.emplace_back(ch.get());
        seq.alts.push_back(std::move(c));
    }
    exec(seq, ctx);
    (void)seq.target.release();
    (void)seq.selector.release();
    for (auto& a : seq.alts) {
        (void)a.cond.release();
        for (auto& w : a.wave) {
            (void)w.value.release();
            (void)w.after.release();
        }
        for (auto& ch : a.choices)
            (void)ch.release();
    }
    const SourceLocation origin = srcLoc(s.loc);
    for (const auto& [idx, anyMask] : st.any) {
        Object& o = object(idx);
        const SigSpec* v = st.nba.find(idx);
        if (!v || o.storage.empty())
            continue;
        const BitMask* allMask = st.all.find(idx);
        const auto W = o.storage.width();
        BitMask comb(W, false);
        BitMask latch(W, false);
        for (std::uint32_t i = 0; i < W && i < anyMask.size(); ++i) {
            if (!anyMask[i])
                continue;
            if (allMask && i < allMask->size() && (*allMask)[i])
                comb[i] = true;
            else
                latch[i] = true;
        }
        for (const auto& [ro, rw] : runs(comb))
            forEachNetPiece(o.storage, ro, rw, [&](std::uint32_t off, std::uint32_t w) {
                body->body.connections.push_back(
                    {o.storage.extract(off, w), v->extract(off, w), ir::ConnKind::Assign, origin});
            });
        for (const auto& [ro, rw] : runs(latch))
            forEachNetPiece(o.storage, ro, rw, [&](std::uint32_t off, std::uint32_t w) {
                body->ed->addLatch({o.storage.extract(off, w), v->extract(off, w), origin}, true);
            });
    }
}

} // namespace opencdc::vhdl

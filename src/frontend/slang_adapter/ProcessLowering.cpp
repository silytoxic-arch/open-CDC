// Procedural part of the slang -> Design IR conversion.
//
// Every always/initial block is executed symbolically: each variable maps to
// a SigSpec describing its current value as a function of the block inputs.
// if/case merge points become Mux cells, constant-bound for loops are
// unrolled and user functions are inlined. At the end of the block:
//   * combinational blocks drive their variables through Process connections
//     (incompletely assigned bits become latches),
//   * edge-triggered blocks produce Register objects with clock, asynchronous
//     resets (recognized from the leading if/else chain), synchronous reset
//     and clock enable (recognized from the next-state Mux structure).

#include "frontend/slang_adapter/IrBuilder.h"

#include "ir/ConstFold.h"

#include <slang/ast/TimingControl.h>
#include <slang/ast/expressions/AssignmentExpressions.h>
#include <slang/ast/expressions/CallExpression.h>
#include <slang/ast/expressions/ConversionExpression.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/statements/ConditionalStatements.h>
#include <slang/ast/statements/LoopStatements.h>
#include <slang/ast/statements/MiscStatements.h>
#include <slang/ast/symbols/SubroutineSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/types/Type.h>

#include <algorithm>

namespace opencdc::frontend::detail {

using namespace opencdc::ir;

namespace {

constexpr int kMaxLoopIterations = 65536;
constexpr int kMaxInlineDepth = 32;

/// Points the builder's "current procedural state" at a state for a scope.
class ProcGuard {
public:
    ProcGuard(ProcState*& slot, ProcState* value) : slot_(slot), saved_(slot) { slot_ = value; }
    ~ProcGuard() { slot_ = saved_; }
    ProcGuard(const ProcGuard&) = delete;
    ProcGuard& operator=(const ProcGuard&) = delete;

private:
    ProcState*& slot_;
    ProcState* saved_;
};

void markBits(BitMask& mask, std::uint32_t width, std::uint32_t offset, std::uint32_t count) {
    if (mask.size() < width)
        mask.resize(width, false);
    for (std::uint32_t i = offset; i < offset + count && i < mask.size(); ++i)
        mask[i] = true;
}

const sast::Statement& unwrap(const sast::Statement& s) {
    if (s.kind == sast::StatementKind::Block)
        return unwrap(s.as<sast::BlockStatement>().body);
    if (s.kind == sast::StatementKind::List) {
        const auto& list = s.as<sast::StatementList>().list;
        if (list.size() == 1)
            return unwrap(*list[0]);
    }
    return s;
}

/// Contiguous runs [offset, width) of set bits.
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

// ============================================================================
// Values
// ============================================================================

SigSpec BodyBuilder::defaultValue(const sast::ValueSymbol& s) {
    if (auto n = netFor(s))
        return body_.netSig(*n);
    return SigSpec::allX(widthOf(s));
}

SigSpec BodyBuilder::nbaBase(const ProcState& st, const sast::ValueSymbol& s) {
    if (const auto* v = st.nba.find(&s))
        return *v;
    return defaultValue(s);
}

SigSpec BodyBuilder::read(const sast::ValueSymbol& s) {
    if (proc_) {
        if (const auto* v = proc_->cur.find(&s))
            return *v;
    }
    return defaultValue(s);
}

void BodyBuilder::assignTargets(const std::vector<LTarget>& targets, const SigSpec& value, bool nonBlocking,
                                slang::SourceLocation l) {
    std::uint32_t total = 0;
    for (const auto& t : targets)
        total += t.width;
    const SigSpec v = value.resized(total, false);

    std::uint32_t pos = 0;
    for (const auto& t : targets) {
        const SigSpec part = v.extract(pos, t.width);
        pos += t.width;
        if (t.width == 0)
            continue;

        if (!proc_) {
            // Continuous assignment: a static slice of a module net.
            auto net = t.dynamic ? std::nullopt : netFor(*t.sym);
            if (!net) {
                unsupported("IR-UNSUPPORTED-LVALUE",
                            "continuous assignment to '" + std::string(t.sym->name) + "' with a dynamic index", l);
                continue;
            }
            body_.connections.push_back({SigSpec::net(*net, t.width, t.offset), part, ConnKind::Assign, loc(l)});
            continue;
        }

        ProcState& st = *proc_;
        const std::uint32_t W = widthOf(*t.sym);
        SigSpec base;
        if (nonBlocking)
            base = nbaBase(st, *t.sym);
        else if (const auto* cur = st.cur.find(t.sym))
            base = *cur;
        else
            base = defaultValue(*t.sym);
        if (base.width() != W)
            base = base.resized(W, false);

        SigSpec next;
        if (!t.dynamic) {
            if (t.offset + t.width > W)
                continue;
            next = base.replaced(t.offset, part);
            markBits(st.all[t.sym], W, t.offset, t.width);
            markBits(st.any[t.sym], W, t.offset, t.width);
        }
        else {
            const SigSpec region = base.extract(t.offset, t.regionWidth);
            Cell c;
            c.kind = CellKind::DynInsert;
            c.inputs = {region, t.index, part};
            c.width = t.regionWidth;
            c.isSigned = t.indexSigned;
            c.elemWidth = t.unitWidth;
            c.rangeLeft = t.left;
            c.rangeRight = t.right;
            c.origin = loc(l);
            next = base.replaced(t.offset, emit(std::move(c)));
            markBits(st.any[t.sym], W, t.offset, t.regionWidth); // not "all": only one element is written
            (void)st.all[t.sym];
        }
        if (nonBlocking) {
            st.nba[t.sym] = std::move(next);
            st.nbaWritten.insert(t.sym);
        }
        else {
            st.cur[t.sym] = std::move(next);
            st.blockingWritten.insert(t.sym);
        }
    }
}

// ============================================================================
// Statements
// ============================================================================

void BodyBuilder::exec(const sast::Statement& s, ProcState& st) {
    using SK = sast::StatementKind;
    ProcGuard guard(proc_, &st);
    if (st.returned)
        return;
    const auto l = s.sourceRange.start();

    switch (s.kind) {
        case SK::Empty: return;
        case SK::List:
            for (const auto* item : s.as<sast::StatementList>().list) {
                if (st.returned)
                    break;
                exec(*item, st);
            }
            return;
        case SK::Block: {
            const auto& b = s.as<sast::BlockStatement>();
            if (b.blockKind != sast::StatementBlockKind::Sequential) {
                unsupported("IR-UNSUPPORTED-STMT", "fork/join block", l);
                return;
            }
            exec(b.body, st);
            return;
        }
        case SK::ExpressionStatement: execExpressionStatement(s.as<sast::ExpressionStatement>().expr, st); return;
        case SK::VariableDeclaration: {
            const auto& v = s.as<sast::VariableDeclStatement>().symbol;
            if (const auto* init = v.getInitializer())
                assignTargets({wholeTarget(&v, widthOf(v))}, lower(*init), false, l);
            return;
        }
        case SK::Conditional: {
            const auto& c = s.as<sast::ConditionalStatement>();
            if (c.conditions.size() != 1 || c.conditions[0].pattern) {
                unsupported("IR-UNSUPPORTED-STMT", "if statement with pattern matching", l);
                return;
            }
            const SigSpec cond = toBool(lower(*c.conditions[0].expr), l);
            if (auto k = cond.asConst(); k && k->isFullyDefined()) {
                if (!k->isZero())
                    exec(c.ifTrue, st);
                else if (c.ifFalse)
                    exec(*c.ifFalse, st);
                return;
            }
            ProcState t = st;
            exec(c.ifTrue, t);
            ProcState f = st;
            if (c.ifFalse)
                exec(*c.ifFalse, f);
            ProcState merged;
            merge(cond, t, f, merged, l);
            st = std::move(merged);
            return;
        }
        case SK::Case: execCase(s, st); return;
        case SK::ForLoop: execFor(s, st); return;
        case SK::RepeatLoop: {
            const auto& r = s.as<sast::RepeatLoopStatement>();
            auto n = lower(r.count).asConst();
            auto count = n ? n->toUint() : std::nullopt;
            if (!count || *count > static_cast<std::uint64_t>(kMaxLoopIterations)) {
                unsupported("IR-UNSUPPORTED-LOOP", "repeat loop without a constant count", l);
                return;
            }
            for (std::uint64_t i = 0; i < *count && !st.returned; ++i)
                exec(r.body, st);
            return;
        }
        case SK::Return: {
            const auto& r = s.as<sast::ReturnStatement>();
            if (r.expr && retVar_)
                assignTargets({wholeTarget(retVar_, widthOf(*retVar_))}, lower(*r.expr), false, l);
            st.returned = true;
            return;
        }
        case SK::ImmediateAssertion:
        case SK::ConcurrentAssertion:
            return; // verification only, no hardware
        default:
            unsupported("IR-UNSUPPORTED-STMT", "statement kind '" + std::string(sast::toString(s.kind)) + "'", l);
            return;
    }
}

void BodyBuilder::execExpressionStatement(const sast::Expression& e, ProcState& st) {
    ProcGuard guard(proc_, &st);
    const auto l = e.sourceRange.start();
    switch (e.kind) {
        case sast::ExpressionKind::Assignment: {
            const auto& a = e.as<sast::AssignmentExpression>();
            auto targets = lvalue(a.left());
            if (!targets)
                return;
            SigSpec rhs = lower(a.right());
            if (a.isCompound()) {
                // a op= b  ->  a = a op b
                SigSpec cur = lower(a.left());
                const std::uint32_t w = cur.width();
                switch (*a.op) {
                    case sast::BinaryOperator::Add: rhs = cell(CellKind::Add, {cur, rhs.resized(w, false)}, w, l); break;
                    case sast::BinaryOperator::Subtract: rhs = cell(CellKind::Sub, {cur, rhs.resized(w, false)}, w, l); break;
                    case sast::BinaryOperator::BinaryAnd: rhs = cell(CellKind::And, {cur, rhs.resized(w, false)}, w, l); break;
                    case sast::BinaryOperator::BinaryOr: rhs = cell(CellKind::Or, {cur, rhs.resized(w, false)}, w, l); break;
                    case sast::BinaryOperator::BinaryXor: rhs = cell(CellKind::Xor, {cur, rhs.resized(w, false)}, w, l); break;
                    case sast::BinaryOperator::LogicalShiftLeft: rhs = cell(CellKind::Shl, {cur, rhs}, w, l); break;
                    case sast::BinaryOperator::LogicalShiftRight: rhs = cell(CellKind::Shr, {cur, rhs}, w, l); break;
                    default:
                        rhs = opaque(e, "compound assignment operator");
                        break;
                }
            }
            assignTargets(*targets, rhs, a.isNonBlocking(), l);
            return;
        }
        case sast::ExpressionKind::UnaryOp: {
            const auto& u = e.as<sast::UnaryExpression>();
            const bool inc = u.op == sast::UnaryOperator::Preincrement || u.op == sast::UnaryOperator::Postincrement;
            const bool dec = u.op == sast::UnaryOperator::Predecrement || u.op == sast::UnaryOperator::Postdecrement;
            if (!inc && !dec)
                break;
            auto targets = lvalue(u.operand());
            if (!targets)
                return;
            SigSpec cur = lower(u.operand());
            const std::uint32_t w = cur.width();
            SigSpec next = cell(inc ? CellKind::Add : CellKind::Sub, {cur, SigSpec::constant(1, w)}, w, l);
            assignTargets(*targets, next, false, l);
            return;
        }
        case sast::ExpressionKind::Call: {
            const auto& c = e.as<sast::CallExpression>();
            if (c.isSystemCall())
                return; // $display, $finish, $readmemh ...: simulation only
            if (c.getSubroutineKind() == sast::SubroutineKind::Task) {
                unsupported("IR-UNSUPPORTED-STMT", "task call '" + std::string(c.getSubroutineName()) + "'", l);
                return;
            }
            break;
        }
        default: break;
    }
    (void)lower(e); // evaluated for its side effects (e.g. inlined void function)
}

void BodyBuilder::execCase(const sast::Statement& s, ProcState& st) {
    const auto& c = s.as<sast::CaseStatement>();
    const auto l = s.sourceRange.start();
    if (c.condition == sast::CaseStatementCondition::Inside) {
        unsupported("IR-UNSUPPORTED-STMT", "case ... inside", l);
        return;
    }
    const bool wildcard = c.condition != sast::CaseStatementCondition::Normal;
    const SigSpec sel = lower(c.expr);

    auto itemCond = [&](const sast::CaseStatement::ItemGroup& g) {
        SigSpec acc;
        for (const auto* ie : g.expressions) {
            SigSpec v = lower(*ie);
            const std::uint32_t w = std::max(v.width(), sel.width());
            SigSpec a = sel.resized(w, false);
            SigSpec b = v.resized(w, false);
            const auto bc = b.asConst();
            const CellKind k = (wildcard && !(bc && bc->isFullyDefined())) ? CellKind::EqWild : CellKind::Eq;
            SigSpec eq = cell(k, {a, b}, 1, l);
            acc = acc.empty() ? eq : cell(CellKind::Or, {acc, eq}, 1, l);
        }
        return acc.empty() ? SigSpec::constant(0, 1) : acc;
    };

    const ProcState base = st;
    ProcState result = base;
    if (c.defaultCase)
        exec(*c.defaultCase, result);

    std::vector<SigSpec> conds;
    for (const auto& g : c.items)
        conds.push_back(itemCond(g));

    for (std::size_t k = c.items.size(); k-- > 0;) {
        const SigSpec& cond = conds[k];
        if (auto kc = cond.asConst(); kc && kc->isFullyDefined()) {
            if (kc->isZero())
                continue;
            ProcState t = base;
            exec(*c.items[k].stmt, t);
            result = std::move(t);
            continue;
        }
        ProcState t = base;
        exec(*c.items[k].stmt, t);
        ProcState merged;
        merge(cond, t, result, merged, l);
        result = std::move(merged);
    }
    st = std::move(result);
}

void BodyBuilder::execFor(const sast::Statement& s, ProcState& st) {
    ProcGuard guard(proc_, &st);
    const auto& f = s.as<sast::ForLoopStatement>();
    const auto l = s.sourceRange.start();

    for (const auto* v : f.loopVars) {
        loopVars_.insert(v);
        if (const auto* init = v->getInitializer())
            assignTargets({wholeTarget(v, widthOf(*v))}, lower(*init), false, l);
    }
    for (const auto* init : f.initializers) {
        if (init->kind == sast::ExpressionKind::Assignment) {
            const auto& lhs = init->as<sast::AssignmentExpression>().left();
            if (lhs.kind == sast::ExpressionKind::NamedValue)
                loopVars_.insert(&lhs.as<sast::NamedValueExpression>().symbol);
        }
        execExpressionStatement(*init, st);
    }
    if (!f.stopExpr) {
        unsupported("IR-UNSUPPORTED-LOOP", "for loop without a stop condition", l);
        return;
    }
    for (int iter = 0;; ++iter) {
        const SigSpec cond = toBool(lower(*f.stopExpr), l);
        auto k = cond.asConst();
        if (!k || !k->isFullyDefined()) {
            unsupported("IR-UNSUPPORTED-LOOP", "for loop whose condition is not constant after unrolling", l);
            return;
        }
        if (k->isZero())
            return;
        if (iter >= kMaxLoopIterations) {
            unsupported("IR-UNSUPPORTED-LOOP", "for loop exceeds " + std::to_string(kMaxLoopIterations) + " iterations", l);
            return;
        }
        exec(f.body, st);
        for (const auto* step : f.steps)
            execExpressionStatement(*step, st);
    }
}

void BodyBuilder::merge(const SigSpec& cond, ProcState& t, ProcState& f, ProcState& out, slang::SourceLocation l) {
    auto mergeValues = [&](OrderedSymMap<SigSpec>& tm, OrderedSymMap<SigSpec>& fm, OrderedSymMap<SigSpec>& om) {
        auto one = [&](const sast::ValueSymbol* s) {
            if (om.find(s))
                return;
            const SigSpec* tv = tm.find(s);
            const SigSpec* fv = fm.find(s);
            SigSpec a = fv ? *fv : defaultValue(*s);
            SigSpec b = tv ? *tv : defaultValue(*s);
            if (a == b) {
                om[s] = a;
                return;
            }
            const std::uint32_t w = std::max(a.width(), b.width());
            om[s] = cell(CellKind::Mux, {a.resized(w, false), b.resized(w, false), cond}, w, l);
        };
        for (const auto& [s, v] : tm)
            one(s);
        for (const auto& [s, v] : fm)
            one(s);
    };
    mergeValues(t.cur, f.cur, out.cur);
    mergeValues(t.nba, f.nba, out.nba);

    auto mergeMasks = [](OrderedSymMap<BitMask>& tm, OrderedSymMap<BitMask>& fm, OrderedSymMap<BitMask>& om,
                         bool isAll) {
        auto one = [&](const sast::ValueSymbol* s) {
            if (om.find(s))
                return;
            const BitMask* a = tm.find(s);
            const BitMask* b = fm.find(s);
            const std::size_t n = std::max(a ? a->size() : 0, b ? b->size() : 0);
            BitMask m(n, false);
            for (std::size_t i = 0; i < n; ++i) {
                const bool av = a && i < a->size() && (*a)[i];
                const bool bv = b && i < b->size() && (*b)[i];
                m[i] = isAll ? (av && bv) : (av || bv);
            }
            om[s] = std::move(m);
        };
        for (const auto& [s, v] : tm)
            one(s);
        for (const auto& [s, v] : fm)
            one(s);
    };
    mergeMasks(t.any, f.any, out.any, false);
    mergeMasks(t.all, f.all, out.all, true);

    out.blockingWritten = t.blockingWritten;
    out.blockingWritten.insert(f.blockingWritten.begin(), f.blockingWritten.end());
    out.nbaWritten = t.nbaWritten;
    out.nbaWritten.insert(f.nbaWritten.begin(), f.nbaWritten.end());
    if (t.returned != f.returned)
        unsupported("IR-UNSUPPORTED-STMT", "return inside a conditional branch of a function", l);
    out.returned = t.returned && f.returned;
}

// ============================================================================
// Functions
// ============================================================================

SigSpec BodyBuilder::inlineFunction(const sast::Expression& e) {
    const auto& call = e.as<sast::CallExpression>();
    const auto* sub = std::get<0>(call.subroutine);
    const std::uint32_t w = e.type->isIntegral() ? e.type->getBitWidth() : 0;
    if (!sub || sub->subroutineKind != sast::SubroutineKind::Function)
        return opaque(e, "call of task '" + std::string(call.getSubroutineName()) + "' in an expression");
    if (inlineDepth_ >= kMaxInlineDepth)
        return opaque(e, "function recursion deeper than " + std::to_string(kMaxInlineDepth));

    const auto formals = sub->getArguments();
    const auto actuals = call.arguments();
    if (formals.size() != actuals.size())
        return opaque(e, "function call with default arguments");

    ProcState fs;
    // Arguments are evaluated in the caller's context.
    std::vector<SigSpec> argValues;
    for (std::size_t i = 0; i < formals.size(); ++i) {
        if (formals[i]->direction != sast::ArgumentDirection::In)
            return opaque(e, "function '" + std::string(sub->name) + "' with output/inout arguments");
        argValues.push_back(lower(*actuals[i]));
    }
    for (std::size_t i = 0; i < formals.size(); ++i)
        fs.cur[formals[i]] = argValues[i].resized(widthOf(*formals[i]), false);

    const auto* savedRet = retVar_;
    retVar_ = sub->returnValVar;
    ++inlineDepth_;
    exec(sub->getBody(), fs);
    --inlineDepth_;
    retVar_ = savedRet;

    if (sub->returnValVar) {
        if (const auto* v = fs.cur.find(sub->returnValVar))
            return v->resized(w, false);
    }
    return SigSpec::allX(w);
}

// ============================================================================
// Processes
// ============================================================================

void BodyBuilder::lowerProcedure(const sast::ProceduralBlockSymbol& pb) {
    using PK = sast::ProceduralBlockKind;
    const auto l = pb.location;
    const auto& body = pb.getBody();
    switch (pb.procedureKind) {
        case PK::Final: return;
        case PK::Initial: lowerInitial(body, l); return;
        case PK::AlwaysComb: lowerComb(body, false, l); return;
        case PK::AlwaysLatch: lowerComb(body, true, l); return;
        case PK::Always:
        case PK::AlwaysFF: break;
    }

    if (body.kind != sast::StatementKind::Timed) {
        unsupported("IR-UNSUPPORTED-PROCESS", "always block without an event control", l);
        return;
    }
    const auto& timed = body.as<sast::TimedStatement>();
    const auto& timing = timed.timing;

    std::vector<const sast::SignalEventControl*> events;
    switch (timing.kind) {
        case sast::TimingControlKind::ImplicitEvent: lowerComb(timed.stmt, false, l); return;
        case sast::TimingControlKind::SignalEvent: events.push_back(&timing.as<sast::SignalEventControl>()); break;
        case sast::TimingControlKind::EventList:
            for (const auto* ev : timing.as<sast::EventListControl>().events) {
                if (ev->kind != sast::TimingControlKind::SignalEvent) {
                    unsupported("IR-UNSUPPORTED-PROCESS", "complex event control", l);
                    return;
                }
                events.push_back(&ev->as<sast::SignalEventControl>());
            }
            break;
        default:
            unsupported("IR-UNSUPPORTED-PROCESS", "always block with a delay or unsupported timing control", l);
            return;
    }

    std::size_t edges = 0;
    for (const auto* ev : events) {
        if (ev->iffCondition) {
            unsupported("IR-UNSUPPORTED-PROCESS", "event control with 'iff'", l);
            return;
        }
        if (ev->edge == sast::EdgeKind::BothEdges) {
            unsupported("IR-UNSUPPORTED-PROCESS", "dual-edge ('edge') event control", l);
            return;
        }
        if (ev->edge != sast::EdgeKind::None)
            ++edges;
    }
    if (edges == 0) {
        lowerComb(timed.stmt, false, l); // @(a or b): level-sensitive list
        return;
    }
    if (edges != events.size()) {
        unsupported("IR-UNSUPPORTED-PROCESS", "event list mixing edge and level sensitivity", l);
        return;
    }
    std::vector<std::pair<Edge, const sast::Expression*>> list;
    for (const auto* ev : events)
        list.emplace_back(ev->edge == sast::EdgeKind::PosEdge ? Edge::Pos : Edge::Neg, &ev->expr);
    lowerSequential(timed.stmt, std::move(list), l);
}

void BodyBuilder::lowerInitial(const sast::Statement& body, slang::SourceLocation l) {
    ProcState st;
    exec(body, st);
    for (const auto& [s, mask] : st.any) {
        auto net = netFor(*s);
        if (!net)
            continue;
        const SigSpec* v = st.cur.find(s);
        if (!v)
            v = st.nba.find(s);
        if (!v)
            continue;
        if (auto c = v->asConst()) {
            body_.net(*net).init = *c; // FPGA power-up value
        }
        else {
            warn("IR-INITIAL-IGNORED", Severity::Info,
                 "initial block assigns a non-constant value to '" + body_.net(*net).name + "'; ignored", l);
        }
    }
}

void BodyBuilder::lowerComb(const sast::Statement& body, bool intendedLatch, slang::SourceLocation l) {
    ProcState st;
    exec(body, st);

    for (const auto& [s, anyMask] : st.any) {
        const SigSpec* nbaV = st.nba.find(s);
        const SigSpec* curV = st.cur.find(s);
        const SigSpec* value = nbaV ? nbaV : curV;
        if (!value)
            continue;
        if (loopVars_.contains(s) && value->isFullyConst())
            continue; // loop index, no hardware
        auto net = netFor(*s);
        if (!net)
            continue;
        if (st.nbaWritten.contains(s) && st.blockingWritten.contains(s))
            warn("IR-MIXED-ASSIGN", Severity::Warning,
                 "'" + body_.net(*net).name + "' uses both blocking and non-blocking assignments", l);

        const BitMask* allMask = st.all.find(s);
        const auto W = body_.net(*net).width;
        BitMask comb(W, false);
        BitMask latch(W, false);
        for (std::uint32_t i = 0; i < W && i < anyMask.size(); ++i) {
            if (!anyMask[i])
                continue;
            const bool full = allMask && i < allMask->size() && (*allMask)[i];
            if (full && !intendedLatch)
                comb[i] = true;
            else
                latch[i] = true;
        }
        for (const auto& [o, w] : runs(comb))
            body_.connections.push_back({SigSpec::net(*net, w, o), value->extract(o, w), ConnKind::Process, loc(l)});
        // Latch warnings are issued after dead-storage removal (pruneDeadStorage):
        // a "latch" whose output is never read is not hardware.
        for (const auto& [o, w] : runs(latch)) {
            body_.latches.push_back({SigSpec::net(*net, w, o), value->extract(o, w), loc(l)});
            latchUnintended_.push_back(!intendedLatch);
        }
    }
}

std::optional<std::pair<SigSpec, bool>> BodyBuilder::resetCondition(const sast::Expression& cond) {
    const sast::Expression* e = &cond;
    bool activeHigh = true;
    for (;;) {
        if (e->kind == sast::ExpressionKind::UnaryOp) {
            const auto& u = e->as<sast::UnaryExpression>();
            if (u.op == sast::UnaryOperator::LogicalNot || u.op == sast::UnaryOperator::BitwiseNot) {
                activeHigh = !activeHigh;
                e = &u.operand();
                continue;
            }
        }
        if (e->kind == sast::ExpressionKind::BinaryOp) {
            const auto& b = e->as<sast::BinaryExpression>();
            const bool eq = b.op == sast::BinaryOperator::Equality || b.op == sast::BinaryOperator::CaseEquality;
            const bool ne = b.op == sast::BinaryOperator::Inequality || b.op == sast::BinaryOperator::CaseInequality;
            if (eq || ne) {
                auto rc = lower(b.right()).asConst();
                auto lc = lower(b.left()).asConst();
                const sast::Expression* other = rc ? &b.left() : (lc ? &b.right() : nullptr);
                const auto c = rc ? rc : lc;
                if (other && c && c->isFullyDefined()) {
                    const bool zero = c->isZero();
                    // (x == 0) and (x != 1) are active low.
                    if (zero == eq)
                        activeHigh = !activeHigh;
                    e = other;
                    continue;
                }
            }
        }
        if (e->kind == sast::ExpressionKind::Conversion &&
            e->as<sast::ConversionExpression>().isImplicit()) {
            e = &e->as<sast::ConversionExpression>().operand();
            continue;
        }
        break;
    }
    SigSpec sig = lower(*e);
    if (sig.width() != 1)
        return std::nullopt;
    return std::make_pair(sig, activeHigh);
}

void BodyBuilder::lowerSequential(const sast::Statement& body, std::vector<std::pair<Edge, const sast::Expression*>> events,
                                  slang::SourceLocation l) {
    struct AsyncCtrl {
        SigSpec signal;
        bool activeHigh;
        const sast::Statement* branch;
    };
    std::vector<AsyncCtrl> ctrls;

    // Lower event expressions once (continuous context).
    std::vector<std::pair<Edge, SigSpec>> ev;
    for (const auto& [edge, expr] : events)
        ev.emplace_back(edge, lower(*expr));

    // Peel the asynchronous controls: if (rst) ... else if (set) ... else <clocked>
    const sast::Statement* rest = &body;
    while (ev.size() > 1) {
        if (!rest) {
            unsupported("IR-AMBIGUOUS-CLOCK", "edge-sensitive block with several events but no clocked branch", l);
            return;
        }
        const auto& s = unwrap(*rest);
        std::optional<std::pair<SigSpec, bool>> rc;
        if (s.kind == sast::StatementKind::Conditional) {
            const auto& c = s.as<sast::ConditionalStatement>();
            if (c.conditions.size() == 1 && !c.conditions[0].pattern)
                rc = resetCondition(*c.conditions[0].expr);
        }
        auto match = rc ? std::ranges::find_if(ev, [&](const auto& p) { return p.second == rc->first; }) : ev.end();
        if (match == ev.end()) {
            unsupported("IR-AMBIGUOUS-CLOCK",
                        "cannot tell clock from asynchronous control in a block with " + std::to_string(events.size()) +
                            " edge events (expected 'if (reset) ... else ...' testing one of the events)",
                        l);
            return;
        }
        const bool edgeHigh = match->first == Edge::Pos;
        if (edgeHigh != rc->second)
            warn("IR-RESET-POLARITY", Severity::Warning,
                 "asynchronous control is tested with the opposite polarity of its event edge", l);
        const auto& c = s.as<sast::ConditionalStatement>();
        ctrls.push_back({rc->first, rc->second, &c.ifTrue});
        ev.erase(match);
        rest = c.ifFalse;
    }

    ClockSpec clock{ev[0].second, ev[0].first};
    if (clock.signal.width() != 1) {
        warn("IR-CLOCK-WIDTH", Severity::Warning, "clock expression is wider than 1 bit; using bit 0", l);
        clock.signal = clock.signal.extract(0, 1);
    }

    ProcState clocked;
    if (rest)
        exec(*rest, clocked);
    std::vector<ProcState> ctrlStates(ctrls.size());
    for (std::size_t k = 0; k < ctrls.size(); ++k)
        exec(*ctrls[k].branch, ctrlStates[k]);

    // Ordered union of written symbols.
    std::vector<const sast::ValueSymbol*> syms;
    std::unordered_set<const sast::ValueSymbol*> seen;
    auto collect = [&](const ProcState& st) {
        for (const auto& [s, m] : st.any)
            if (seen.insert(s).second)
                syms.push_back(s);
    };
    collect(clocked);
    for (const auto& cs : ctrlStates)
        collect(cs);

    auto finalValue = [&](const ProcState& st, const sast::ValueSymbol* s) -> SigSpec {
        if (const auto* v = st.nba.find(s))
            return *v;
        if (const auto* v = st.cur.find(s))
            return *v;
        return defaultValue(*s);
    };

    for (const auto* s : syms) {
        const SigSpec d = finalValue(clocked, s);
        if (loopVars_.contains(s) && d.isFullyConst())
            continue;
        auto net = netFor(*s);
        if (!net)
            continue; // automatic local: purely combinational inside the block
        const auto W = body_.net(*net).width;
        if (clocked.nbaWritten.contains(s) && clocked.blockingWritten.contains(s))
            warn("IR-MIXED-ASSIGN", Severity::Warning,
                 "'" + body_.net(*net).name + "' uses both blocking and non-blocking assignments", l);

        BitMask any(W, false);
        auto orMask = [&](const ProcState& st) {
            if (const auto* m = st.any.find(s))
                for (std::size_t i = 0; i < m->size() && i < W; ++i)
                    any[i] = any[i] || (*m)[i];
        };
        orMask(clocked);
        for (const auto& cs : ctrlStates)
            orMask(cs);

        // Split into registers whose bits share the same set of asynchronous
        // controls (bits not loaded by a reset branch are not reset).
        std::vector<std::uint32_t> signature(W, 0);
        for (std::size_t k = 0; k < ctrls.size() && k < 32; ++k) {
            if (const auto* m = ctrlStates[k].any.find(s))
                for (std::uint32_t i = 0; i < W && i < m->size(); ++i)
                    if ((*m)[i])
                        signature[i] |= 1U << k;
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pieces;
        for (const auto& [ro, rw] : runs(any)) {
            std::uint32_t start = ro;
            for (std::uint32_t i = ro + 1; i <= ro + rw; ++i) {
                if (i == ro + rw || signature[i] != signature[start]) {
                    pieces.emplace_back(start, i - start);
                    start = i;
                }
            }
        }

        for (const auto& [o, w] : pieces) {
            Register r;
            r.q = SigSpec::net(*net, w, o);
            r.d = d.resized(W, false).extract(o, w);
            r.clock = clock;
            r.origin = loc(l);
            for (std::size_t k = 0; k < ctrls.size(); ++k) {
                if (k >= 32 || !(signature[o] & (1U << k)))
                    continue;
                SigSpec value = finalValue(ctrlStates[k], s).resized(W, false).extract(o, w);
                if (!value.isFullyConst())
                    warn("IR-ASYNC-LOAD", Severity::Warning,
                         "asynchronous control loads a non-constant value into '" + body_.net(*net).name + "'", l);
                r.asyncResets.push_back({ctrls[k].signal, ctrls[k].activeHigh, std::move(value)});
            }
            extractControls(r, l);
            body_.registers.push_back(std::move(r));
        }
    }
}

void BodyBuilder::extractControls(Register& r, slang::SourceLocation l) {
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

} // namespace opencdc::frontend::detail

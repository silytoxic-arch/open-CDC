// VHDL expressions, part 1: values and bits, type inference, overload
// resolution, predefined operators, conversions.

#include "frontend/vhdl/VhdlElab.h"

#include "ir/ConstFold.h"

#include <algorithm>
#include <cmath>

namespace opencdc::vhdl {

// ---------------------------------------------------------------------------
// TypeSet
// ---------------------------------------------------------------------------

void TypeSet::add(const Type* t) {
    if (!t)
        return;
    if (t->kind == TypeKind::UniversalInteger) {
        univInt = true;
        return;
    }
    if (t->kind == TypeKind::UniversalReal) {
        univReal = true;
        return;
    }
    for (const Type* x : types)
        if (&x->baseType() == &t->baseType()) {
            // keep the more specific (constrained) one when both are present
            return;
        }
    types.push_back(t);
}

void TypeSet::merge(const TypeSet& o) {
    for (const Type* t : o.types)
        add(t);
    univInt |= o.univInt;
    univReal |= o.univReal;
    anyString |= o.anyString;
    anyAggregate |= o.anyAggregate;
    anyNull |= o.anyNull;
}

bool TypeSet::accepts(const Type& t) const {
    const Type& b = t.baseType();
    for (const Type* x : types)
        if (&x->baseType() == &b)
            return true;
    if (univInt && t.isIntegerLike())
        return true;
    if (univReal && t.isRealLike())
        return true;
    if (anyString && t.isStringLike())
        return true;
    if (anyAggregate && t.isComposite())
        return true;
    if (anyNull && t.kind == TypeKind::Access)
        return true;
    // universal operands accept universal parameters
    if (t.kind == TypeKind::UniversalInteger && (univInt || std::ranges::any_of(types, [](const Type* x) {
                                                     return x->isIntegerLike();
                                                 })))
        return true;
    if (t.kind == TypeKind::UniversalReal && univReal)
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Values and bits
// ---------------------------------------------------------------------------

SVal Elaborator::makeConst(const Type* t, Value v) {
    SVal s;
    s.type = t;
    s.value = std::move(v);
    return s;
}

SVal Elaborator::makeBits(const Type* t, ir::SigSpec bits) {
    SVal s;
    s.type = t;
    if (t && bits.isFullyConst())
        if (auto c = bits.asConst(); c && c->isFullyDefined())
            s.value = decode(*t, *c);
    s.bits = std::move(bits);
    return s;
}

ir::SigSpec Elaborator::bitsOf(const SVal& v, Loc l) {
    if (!v.type)
        return {};
    if (!v.bits.empty() || !v.known()) {
        if (v.bits.empty() && v.type) {
            if (auto w = bitWidth(*v.type); w && *w == 0)
                return {};
        }
        return v.bits;
    }
    auto c = encode(*v.type, *v.value);
    if (!c) {
        error("VHDL-NO-HARDWARE", "a value of type '" + v.type->name + "' has no hardware representation", l);
        auto w = bitWidth(*v.type);
        return ir::SigSpec::allX(w.value_or(1));
    }
    return ir::SigSpec::constant(*c);
}

ir::SigSpec Elaborator::cell(ir::CellKind k, std::vector<ir::SigSpec> in, std::uint32_t w, Loc l, bool isSigned) {
    ir::Cell c;
    c.kind = k;
    c.inputs = std::move(in);
    c.width = w;
    c.isSigned = isSigned;
    c.origin = srcLoc(l);
    if (body && body->ed)
        return body->ed->emit(std::move(c));
    if (auto s = ir::simplifyCell(c))
        return s->resized(w, false);
    error("VHDL-NOT-STATIC", "expression is not static (it depends on signals or is too wide to evaluate)", l);
    return ir::SigSpec::allX(w);
}

ir::SigSpec Elaborator::toBool(const ir::SigSpec& s, Loc l) {
    if (s.width() == 1)
        return s;
    if (s.width() == 0)
        return ir::SigSpec::constant(0, 1);
    return cell(ir::CellKind::ReduceOr, {s}, 1, l);
}

ir::SigSpec Elaborator::mux(const ir::SigSpec& sel, const ir::SigSpec& whenFalse, const ir::SigSpec& whenTrue, Loc l) {
    if (whenFalse == whenTrue)
        return whenFalse;
    const std::uint32_t w = std::max(whenFalse.width(), whenTrue.width());
    return cell(ir::CellKind::Mux, {whenFalse.resized(w, false), whenTrue.resized(w, false), sel}, w, l);
}

SVal Elaborator::opaque(const Type* t, std::vector<ir::SigSpec> inputs, const std::string& what, Loc l) {
    const auto w = t ? bitWidth(*t) : std::nullopt;
    if (!w) {
        return SVal{t, std::nullopt, {}};
    }
    if (!body || !body->ed) {
        error("VHDL-NOT-STATIC", what + " cannot be evaluated at elaboration time", l);
        return SVal{t, std::nullopt, ir::SigSpec::allX(*w)};
    }
    ir::Cell c;
    c.kind = ir::CellKind::Opaque;
    std::erase_if(inputs, [](const ir::SigSpec& s) { return s.empty(); });
    c.inputs = std::move(inputs);
    c.width = *w;
    c.label = what;
    c.origin = srcLoc(l);
    return makeBits(t, body->ed->addUnique(std::move(c)));
}

namespace {

bool closelyRelatedArrays(const Type& a, const Type& b) {
    if (!a.isArray() || !b.isArray() || a.indexTypes.size() != b.indexTypes.size())
        return false;
    return &a.element->baseType() == &b.element->baseType() ||
           (a.element->isLogicScalar() && b.element->isLogicScalar() &&
            a.element->baseTag() == b.element->baseTag());
}

} // namespace

SVal Elaborator::convert(const SVal& v, const Type* to, Ctx& ctx, Loc l, bool implicit) {
    if (!v.valid() || !to || v.type == to)
        return v;
    const Type& from = *v.type;
    const Type& tb = to->baseType();
    // universal -> concrete numeric
    if (from.kind == TypeKind::UniversalInteger || from.isIntegerLike()) {
        if (to->isIntegerLike()) {
            if (v.known()) {
                if (to->constrained && to->kind == TypeKind::Integer && !to->range.contains(v.value->i) &&
                    !to->range.isNull()) {
                    error("VHDL-RANGE", "value " + std::to_string(v.value->i) + " is outside the range of '" +
                                            to->name + "' (" + std::to_string(to->range.low()) + " to " +
                                            std::to_string(to->range.high()) + ")",
                          l);
                }
                return makeConst(to, *v.value);
            }
            const auto w = bitWidth(*to).value_or(32);
            return makeBits(to, v.bits.resized(w, isSignedInteger(from)));
        }
        if (to->isRealLike() && (v.known() || !implicit)) {
            if (v.known())
                return makeConst(to, Value::real(static_cast<double>(v.value->i)));
            error("VHDL-NOT-STATIC", "conversion to real of a non-static value", l);
            return SVal{to, std::nullopt, {}};
        }
    }
    if (from.isRealLike()) {
        if (to->isRealLike())
            return v.known() ? makeConst(to, *v.value) : SVal{to, std::nullopt, {}};
        if (to->isIntegerLike() && !implicit) {
            if (v.known() && v.value->kind == Value::Kind::Real)
                return makeConst(to, Value::integer(static_cast<std::int64_t>(std::llround(v.value->r))));
            error("VHDL-NOT-STATIC", "conversion of a non-static real value", l);
            return SVal{to, std::nullopt, {}};
        }
    }
    if (from.kind == TypeKind::Physical && to->kind == TypeKind::Physical)
        return v.known() ? makeConst(to, *v.value) : SVal{to, std::nullopt, v.bits};
    if (from.kind == TypeKind::Enum && to->kind == TypeKind::Enum && &from.baseType() == &tb) {
        if (v.known())
            return makeConst(to, *v.value);
        return makeBits(to, v.bits);
    }
    if (from.isArray() && to->isArray() &&
        (&from.baseType() == &tb || (!implicit && closelyRelatedArrays(from, *to)) ||
         (from.element && to->element && from.baseTag() == TypeTag::StdUlogicVector &&
          to->baseTag() == TypeTag::StdUlogicVector))) {
        // same values, new bounds (or the target's bounds when constrained)
        const auto fl = from.indexRanges.empty() || !from.indexRanges.front() ? 0
                                                                               : from.indexRanges.front()->length();
        const Type* rt = to;
        if (!to->isFullyConstrained()) {
            std::vector<IntRange> ranges;
            for (std::size_t k = 0; k < from.indexRanges.size(); ++k) {
                IntRange r = from.indexRanges[k].value_or(IntRange{0, -1, true});
                if (k < to->indexRanges.size() && to->indexRanges[k]) {
                    r = *to->indexRanges[k];
                }
                else if (!implicit && k < to->indexTypes.size()) {
                    // type conversion keeps the operand's bounds
                }
                ranges.push_back(r);
            }
            rt = types.constrainedArray(to, ranges);
            if (to->element && !to->element->isFullyConstrained() && from.element && from.element->isFullyConstrained()) {
                Type copy = *rt;
                copy.element = from.element;
                rt = types.make(std::move(copy));
            }
        }
        else if (to->indexRanges.size() == 1 && to->indexRanges.front()->length() != fl) {
            error("VHDL-LENGTH", "length mismatch: expected " + std::to_string(to->indexRanges.front()->length()) +
                                     " element(s) of '" + to->name + "', got " + std::to_string(fl),
                  l);
            return SVal{rt, std::nullopt, ir::SigSpec::allX(bitWidth(*rt).value_or(1))};
        }
        if (v.known())
            return makeConst(rt, *v.value);
        return makeBits(rt, v.bits);
    }
    if (from.kind == TypeKind::Record && &from.baseType() == &tb)
        return v.known() ? makeConst(to, *v.value) : makeBits(to, v.bits);
    if (&from.baseType() == &tb)
        return v.known() ? makeConst(to, *v.value) : makeBits(to, v.bits);
    // Access, file, ... and genuinely different types
    if (!implicit || !reported_.contains("conv:" + from.name + ">" + to->name)) {
        reported_.insert("conv:" + from.name + ">" + to->name);
        error("VHDL-TYPE", "cannot convert a value of type '" + from.name + "' to '" + to->name + "'", l);
    }
    (void)ctx;
    auto w = bitWidth(*to);
    return SVal{to, std::nullopt, w ? ir::SigSpec::allX(*w) : ir::SigSpec{}};
}

Object& Elaborator::object(int idx) {
    if (idx >= kFrameBase)
        return frameObjects.at(static_cast<std::size_t>(idx - kFrameBase));
    return body->objects.at(static_cast<std::size_t>(idx));
}

int Elaborator::addObject(Object o) {
    frameObjects.push_back(std::move(o));
    return kFrameBase + static_cast<int>(frameObjects.size()) - 1;
}

SVal Elaborator::initialValue(const Object& o, Loc l) {
    // T'left: enumerations 'U' / first literal, integers left bound, arrays of those
    (void)l;
    std::function<std::optional<Value>(const Type&)> left = [&](const Type& t) -> std::optional<Value> {
        switch (t.kind) {
            case TypeKind::Enum:
            case TypeKind::Integer:
            case TypeKind::Physical: return Value::integer(t.constrained ? t.range.left : 0);
            case TypeKind::Real: return Value::real(t.rrange.left);
            case TypeKind::Array: {
                if (!t.isFullyConstrained())
                    return std::nullopt;
                std::uint64_t n = 1;
                for (const auto& r : t.indexRanges)
                    n *= r->length();
                if (n > kMaxObjectBits)
                    return std::nullopt;
                auto e = left(*t.element);
                if (!e)
                    return std::nullopt;
                return Value::composite(std::vector<Value>(n, *e));
            }
            case TypeKind::Record: {
                std::vector<Value> f;
                for (const auto& fl : t.fields) {
                    auto e = left(*fl.type);
                    if (!e)
                        return std::nullopt;
                    f.push_back(*e);
                }
                return Value::composite(std::move(f));
            }
            default: return std::nullopt;
        }
    };
    auto v = left(*o.type);
    if (!v)
        return SVal{o.type, std::nullopt, {}};
    return makeConst(o.type, *v);
}

SVal Elaborator::readObject(int idx, Ctx& ctx, Loc l) {
    Object& o = object(idx);
    if (!o.isSignal) {
        for (ProcState* st = ctx.st; st; st = nullptr) {
            if (const SVal* v = st->cur.find(idx))
                return *v;
        }
        if (o.storage.empty())
            return o.initial.valid() ? o.initial : initialValue(o, l);
        o.readInitial = true;
    }
    if (o.storage.empty()) {
        error("VHDL-NO-HARDWARE", "'" + o.name + "' has no hardware representation here", l);
        return SVal{o.type, std::nullopt, ir::SigSpec::allX(bitWidth(*o.type).value_or(1))};
    }
    if (ctx.constOnly) {
        error("VHDL-NOT-STATIC", "'" + o.name + "' is not a constant", l);
        return SVal{o.type, std::nullopt, o.storage};
    }
    return SVal{o.type, std::nullopt, o.storage};
}

// ---------------------------------------------------------------------------
// Type inference
// ---------------------------------------------------------------------------

namespace {

std::string opKey(Op op) {
    return std::string("\"") + opSymbol(op) + "\"";
}

bool isRelational(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge;
}

bool isLogical(Op op) {
    return op == Op::And || op == Op::Or || op == Op::Nand || op == Op::Nor || op == Op::Xor || op == Op::Xnor;
}

bool isShift(Op op) {
    return op == Op::Sll || op == Op::Srl || op == Op::Sla || op == Op::Sra || op == Op::Rol || op == Op::Ror;
}

bool isMatching(Op op) {
    return op == Op::MatchEq || op == Op::MatchNe || op == Op::MatchLt || op == Op::MatchLe || op == Op::MatchGt ||
           op == Op::MatchGe;
}

bool isReduction(Op op) {
    return op == Op::RedAnd || op == Op::RedOr || op == Op::RedNand || op == Op::RedNor || op == Op::RedXor ||
           op == Op::RedXnor;
}

Op reductionBase(Op op) {
    switch (op) {
        case Op::RedAnd: return Op::And;
        case Op::RedOr: return Op::Or;
        case Op::RedNand: return Op::Nand;
        case Op::RedNor: return Op::Nor;
        case Op::RedXor: return Op::Xor;
        case Op::RedXnor: return Op::Xnor;
        default: return op;
    }
}

bool isBitOrBool(const Type& t) {
    const auto tag = t.baseTag();
    return tag == TypeTag::Bit || tag == TypeTag::Boolean;
}

bool isOneDimArray(const Type& t) {
    return t.isArray() && t.indexTypes.size() == 1;
}

} // namespace

std::vector<Candidate> Elaborator::predefinedCandidates(Op op, const std::vector<TypeSet>& sets, const Type* expected) {
    std::vector<const Type*> ts;
    auto addT = [&](const Type* t) {
        if (!t)
            return;
        const Type* b = &t->baseType();
        if (std::ranges::find(ts, b) == ts.end())
            ts.push_back(b);
    };
    for (const auto& s : sets)
        for (const Type* t : s.types)
            addT(t);
    if (expected && !isRelational(op) && !isMatching(op))
        addT(expected);
    bool anyUniv = false;
    for (const auto& s : sets)
        anyUniv |= s.univInt || s.univReal;
    if (ts.empty() || anyUniv) {
        if (std::ranges::all_of(sets, [](const TypeSet& s) { return s.univInt || s.univReal; })) {
            bool real = std::ranges::any_of(sets, [](const TypeSet& s) { return s.univReal && !s.univInt; });
            addT(real ? std_.univReal : std_.univInt);
        }
    }
    if (op == Op::Concat) {
        // element & element: the result may be any one-dimensional array of that element type
        std::vector<const Type*> elems;
        for (const Type* t : ts)
            if (t->isScalar())
                elems.push_back(t);
        for (const Type* e : elems)
            for (const Type* a : types.arrays())
                if (a->element && &a->element->baseType() == e)
                    addT(a);
    }
    std::vector<Candidate> out;
    auto add = [&](std::vector<const Type*> p, const Type* r) {
        Candidate c;
        c.predef = op;
        c.params = std::move(p);
        c.result = r;
        out.push_back(std::move(c));
    };
    const bool unary = sets.size() == 1;
    for (const Type* t : ts) {
        const Type& T = *t;
        if (T.kind == TypeKind::File || T.kind == TypeKind::Protected)
            continue;
        if (unary) {
            switch (op) {
                case Op::Not:
                    if (isBitOrBool(T) || (isOneDimArray(T) && isBitOrBool(*T.element)))
                        add({t}, t);
                    break;
                case Op::Plus:
                case Op::Minus:
                case Op::Abs:
                    if (T.isIntegerLike() || T.isRealLike() || T.kind == TypeKind::Physical)
                        add({t}, t);
                    break;
                case Op::Condition:
                    if (T.baseTag() == TypeTag::Bit)
                        add({t}, std_.boolean);
                    break;
                default:
                    if (isReduction(op) && isOneDimArray(T) && isBitOrBool(*T.element))
                        add({t}, &T.element->baseType());
                    break;
            }
            continue;
        }
        if (isRelational(op)) {
            if (op == Op::Eq || op == Op::Ne)
                add({t, t}, std_.boolean);
            else if (T.isScalar() || (isOneDimArray(T) && T.element->isDiscrete()))
                add({t, t}, std_.boolean);
            continue;
        }
        if (isMatching(op)) {
            if (T.baseTag() == TypeTag::Bit)
                add({t, t}, std_.bit);
            else if (isOneDimArray(T) && T.element->baseTag() == TypeTag::Bit && (op == Op::MatchEq || op == Op::MatchNe))
                add({t, t}, std_.bit);
            continue;
        }
        if (isLogical(op)) {
            if (isBitOrBool(T)) {
                add({t, t}, t);
            }
            else if (isOneDimArray(T) && isBitOrBool(*T.element)) {
                add({t, t}, t);
                add({&T.element->baseType(), t}, t);
                add({t, &T.element->baseType()}, t);
            }
            continue;
        }
        if (isShift(op)) {
            if (isOneDimArray(T) && isBitOrBool(*T.element))
                add({t, std_.integer}, t);
            continue;
        }
        switch (op) {
            case Op::Add:
            case Op::Sub:
                if (T.isIntegerLike() || T.isRealLike() || T.kind == TypeKind::Physical)
                    add({t, t}, t);
                break;
            case Op::Mul:
                if (T.isIntegerLike() || T.isRealLike())
                    add({t, t}, t);
                if (T.kind == TypeKind::Physical) {
                    add({t, std_.integer}, t);
                    add({std_.integer, t}, t);
                    add({t, std_.real}, t);
                    add({std_.real, t}, t);
                }
                if (T.kind == TypeKind::UniversalReal) {
                    add({std_.univReal, std_.univInt}, std_.univReal);
                    add({std_.univInt, std_.univReal}, std_.univReal);
                }
                break;
            case Op::Div:
                if (T.isIntegerLike() || T.isRealLike())
                    add({t, t}, t);
                if (T.kind == TypeKind::Physical) {
                    add({t, std_.integer}, t);
                    add({t, std_.real}, t);
                    add({t, t}, std_.univInt);
                }
                if (T.kind == TypeKind::UniversalReal)
                    add({std_.univReal, std_.univInt}, std_.univReal);
                break;
            case Op::Mod:
            case Op::Rem:
                if (T.isIntegerLike())
                    add({t, t}, t);
                break;
            case Op::Pow:
                if (T.isIntegerLike() || T.isRealLike())
                    add({t, std_.integer}, t);
                break;
            case Op::Concat:
                if (isOneDimArray(T)) {
                    const Type* e = &T.element->baseType();
                    add({t, t}, t);
                    add({t, e}, t);
                    add({e, t}, t);
                    add({e, e}, t);
                }
                break;
            default: break;
        }
    }
    return out;
}

bool Elaborator::resolveOverload(std::vector<Candidate>& cands, const std::vector<TypeSet>& argSets,
                                 const Type* expected, const std::string& what, Loc l, Candidate& chosen, bool quiet) {
    std::vector<Candidate> ok;
    for (auto& c : cands) {
        if (c.params.size() != argSets.size())
            continue;
        bool fits = true;
        for (std::size_t k = 0; k < argSets.size() && fits; ++k)
            fits = c.params[k] && argSets[k].accepts(*c.params[k]);
        if (fits)
            ok.push_back(c);
    }
    if (ok.empty()) {
        if (!quiet)
            error("VHDL-NO-OVERLOAD", "no visible " + what + " matches the operand types", l);
        return false;
    }
    if (expected && ok.size() > 1) {
        std::vector<Candidate> byResult;
        for (const auto& c : ok)
            if (c.result && (&c.result->baseType() == &expected->baseType() ||
                             (c.result->kind == TypeKind::UniversalInteger && expected->isIntegerLike()) ||
                             (c.result->kind == TypeKind::UniversalReal && expected->isRealLike())))
                byResult.push_back(c);
        if (!byResult.empty())
            ok = std::move(byResult);
    }
    // Explicit declarations hide the implicit (predefined) homograph.
    if (ok.size() > 1) {
        std::vector<Candidate> dedup;
        for (const auto& c : ok) {
            bool hidden = false;
            for (const auto& o : ok) {
                if (&o == &c || o.params.size() != c.params.size())
                    continue;
                bool same = c.result && o.result && &c.result->baseType() == &o.result->baseType();
                for (std::size_t k = 0; same && k < c.params.size(); ++k)
                    same = &c.params[k]->baseType() == &o.params[k]->baseType();
                if (!same)
                    continue;
                if (!c.sym && o.sym) {
                    hidden = true;
                    break;
                }
                // two explicit homographs (same package seen twice): keep the first
                if (c.sym && o.sym && &o < &c) {
                    hidden = true;
                    break;
                }
            }
            if (!hidden)
                dedup.push_back(c);
        }
        if (!dedup.empty())
            ok = std::move(dedup);
    }
    if (ok.size() > 1) {
        // prefer exact (non-universal) matches
        auto score = [&](const Candidate& c) {
            int s = 0;
            for (std::size_t k = 0; k < c.params.size(); ++k)
                for (const Type* t : argSets[k].types)
                    if (&t->baseType() == &c.params[k]->baseType())
                        ++s;
            return s;
        };
        int best = -1;
        for (const auto& c : ok)
            best = std::max(best, score(c));
        std::vector<Candidate> top;
        for (const auto& c : ok)
            if (score(c) == best)
                top.push_back(c);
        ok = std::move(top);
    }
    if (ok.size() > 1) {
        // universal operands: the universal operator wins over implicit conversion (LRM 9.3.6)
        std::vector<Candidate> univ;
        for (const auto& c : ok)
            if (!c.params.empty() && std::ranges::all_of(c.params, [](const Type* t) {
                    return t->kind == TypeKind::UniversalInteger || t->kind == TypeKind::UniversalReal;
                }))
                univ.push_back(c);
        if (univ.size() == 1)
            ok = std::move(univ);
    }
    if (ok.size() > 1) {
        // otherwise ambiguous
        if (!quiet && !reported_.contains("ovl:" + what + std::to_string(l.file) + ":" + std::to_string(l.line))) {
            reported_.insert("ovl:" + what + std::to_string(l.file) + ":" + std::to_string(l.line));
            std::string list;
            for (const auto& c : ok) {
                list += list.empty() ? "" : ", ";
                list += "(";
                for (std::size_t k = 0; k < c.params.size(); ++k)
                    list += (k ? ", " : "") + c.params[k]->name;
                list += ") return " + (c.result ? c.result->name : std::string("-"));
            }
            warn("VHDL-AMBIGUOUS", what + " is ambiguous between " + list + "; using the first", l);
        }
    }
    chosen = ok.front();
    return true;
}

TypeSet Elaborator::typesOf(const Expr& e, Ctx& ctx) {
    TypeSet s;
    switch (e.kind) {
        case ExprKind::Integer: s.univInt = true; return s;
        case ExprKind::Real: s.univReal = true; return s;
        case ExprKind::Physical: {
            for (Symbol* u : lookup(*ctx.scope, e.ident.key))
                if (u->kind == SymKind::Unit)
                    s.add(u->type);
            return s;
        }
        case ExprKind::Character:
            for (Symbol* sym : lookup(*ctx.scope, e.ident.key))
                if (sym->kind == SymKind::EnumLiteral)
                    s.add(sym->type);
            return s;
        case ExprKind::String:
        case ExprKind::BitString: s.anyString = true; return s;
        case ExprKind::Null: s.anyNull = true; return s;
        case ExprKind::Aggregate: s.anyAggregate = true; return s;
        case ExprKind::Qualified: s.add(typeMark(*e.prefix, ctx)); return s;
        case ExprKind::Unary:
        case ExprKind::Binary: {
            std::vector<TypeSet> sets;
            sets.push_back(typesOf(*e.lhs, ctx));
            if (e.kind == ExprKind::Binary)
                sets.push_back(typesOf(*e.rhs, ctx));
            const Op op = e.op;
            const std::string key = isReduction(op) ? opKey(reductionBase(op)) : opKey(op);
            std::vector<Candidate> cands = predefinedCandidates(op, sets, nullptr);
            for (Symbol* f : lookup(*ctx.scope, key)) {
                if (f->kind != SymKind::Subprogram || !f->isFunction)
                    continue;
                resolveParams(*f);
                Candidate c;
                c.sym = f;
                for (const auto& p : f->params)
                    c.params.push_back(p.type);
                c.result = f->type;
                cands.push_back(std::move(c));
            }
            for (const auto& c : cands) {
                if (c.params.size() != sets.size())
                    continue;
                bool fits = true;
                for (std::size_t k = 0; k < sets.size() && fits; ++k)
                    fits = c.params[k] && sets[k].accepts(*c.params[k]);
                if (fits)
                    s.add(c.result);
            }
            return s;
        }
        case ExprKind::Simple:
        case ExprKind::Selected: {
            if (e.kind == ExprKind::Selected) {
                // record field?
                auto pre = e.prefix->kind == ExprKind::Simple || e.prefix->kind == ExprKind::Selected
                               ? resolveName(*e.prefix, ctx, true)
                               : std::vector<Symbol*>{};
                if (pre.empty() || (pre.front()->kind != SymKind::Library && pre.front()->kind != SymKind::Package)) {
                    TypeSet ps = typesOf(*e.prefix, ctx);
                    for (const Type* t : ps.types)
                        if (t->kind == TypeKind::Record)
                            for (const auto& f : t->fields)
                                if (f.key == e.ident.key)
                                    s.add(f.type);
                    return s;
                }
            }
            for (Symbol* sym : resolveName(e, ctx, true)) {
                switch (sym->kind) {
                    case SymKind::Constant:
                    case SymKind::Signal:
                    case SymKind::Variable:
                    case SymKind::EnumLiteral:
                    case SymKind::Unit:
                    case SymKind::File: s.add(sym->type); break;
                    case SymKind::Alias:
                        if (sym->type) {
                            s.add(sym->type);
                        }
                        else if (sym->aliasTarget) {
                            Ctx c2 = ctx;
                            c2.scope = sym->aliasScope;
                            s.merge(typesOf(*sym->aliasTarget, c2));
                        }
                        break;
                    case SymKind::Subprogram:
                        if (sym->isFunction) {
                            resolveParams(*sym);
                            if (std::ranges::all_of(sym->params, [](const ParamInfo& p) { return p.defaultExpr; }))
                                s.add(sym->type);
                        }
                        break;
                    default: break;
                }
            }
            return s;
        }
        case ExprKind::Call: {
            const Expr& pre = *e.prefix;
            std::vector<Symbol*> syms;
            if (pre.kind == ExprKind::Simple || pre.kind == ExprKind::Selected)
                syms = resolveName(pre, ctx, true);
            if (!syms.empty() && syms.front()->kind == SymKind::Type) {
                s.add(syms.front()->type);
                return s;
            }
            if (!syms.empty() && syms.front()->kind == SymKind::Subprogram) {
                std::vector<TypeSet> argSets;
                for (const auto& a : e.args)
                    argSets.push_back(a.actual ? typesOf(*a.actual, ctx) : TypeSet{});
                for (Symbol* f : syms) {
                    if (f->kind != SymKind::Subprogram || !f->isFunction)
                        continue;
                    resolveParams(*f);
                    if (e.args.size() > f->params.size())
                        continue;
                    bool fits = true;
                    std::size_t pos = 0;
                    for (std::size_t k = 0; k < e.args.size() && fits; ++k) {
                        const ParamInfo* p = nullptr;
                        if (e.args[k].formal && e.args[k].formal->kind == ExprKind::Simple) {
                            for (const auto& pp : f->params)
                                if (pp.key == e.args[k].formal->ident.key)
                                    p = &pp;
                        }
                        else {
                            p = pos < f->params.size() ? &f->params[pos++] : nullptr;
                        }
                        fits = p && p->type && argSets[k].accepts(*p->type);
                    }
                    if (fits)
                        s.add(f->type);
                }
                if (!s.empty())
                    return s;
                // zero-parameter function result indexed: f(3)
                for (Symbol* f : syms)
                    if (f->kind == SymKind::Subprogram && f->isFunction && f->type && f->type->isArray())
                        s.add(f->type->element);
                return s;
            }
            TypeSet ps = typesOf(pre, ctx);
            for (const Type* t : ps.types) {
                if (!t->isArray())
                    continue;
                const bool slice = e.args.size() == 1 &&
                                   (e.args[0].actual->kind == ExprKind::Range ||
                                    e.args[0].actual->kind == ExprKind::Subtype ||
                                    (e.args[0].actual->kind == ExprKind::Attribute &&
                                     (e.args[0].actual->ident.key == "range" ||
                                      e.args[0].actual->ident.key == "reverse_range")));
                if (slice)
                    s.add(t);
                else if (e.args.size() == t->indexTypes.size())
                    s.add(t->element);
            }
            return s;
        }
        case ExprKind::Attribute: {
            const std::string& a = e.ident.key;
            if (a == "length" || a == "pos" || a == "count")
                s.univInt = true;
            else if (a == "event" || a == "stable" || a == "active" || a == "quiet" || a == "ascending" ||
                     a == "driving")
                s.add(std_.boolean);
            else if (a == "image" || a == "instance_name" || a == "path_name" || a == "simple_name")
                s.add(std_.string);
            else if (a == "high" || a == "low" || a == "left" || a == "right") {
                TypeSet ps = typesOf(*e.prefix, ctx);
                const Type* t = ps.types.size() == 1 ? ps.types.front() : nullptr;
                if (!t) {
                    auto syms = e.prefix->kind == ExprKind::Simple || e.prefix->kind == ExprKind::Selected
                                    ? resolveName(*e.prefix, ctx, true)
                                    : std::vector<Symbol*>{};
                    if (!syms.empty() && syms.front()->kind == SymKind::Type)
                        t = syms.front()->type;
                }
                if (t && t->isArray()) {
                    std::size_t dim = 0;
                    if (!e.args.empty())
                        if (auto d = constInt(*e.args[0].actual, ctx))
                            dim = static_cast<std::size_t>(std::max<std::int64_t>(*d - 1, 0));
                    s.add(dim < t->indexTypes.size() ? t->indexTypes[dim] : std_.integer);
                }
                else if (t) {
                    s.add(t);
                }
            }
            else if (a == "val" || a == "succ" || a == "pred" || a == "leftof" || a == "rightof" || a == "value" ||
                     a == "last_value" || a == "delayed") {
                const Type* t = nullptr;
                auto syms = e.prefix->kind == ExprKind::Simple || e.prefix->kind == ExprKind::Selected
                                ? resolveName(*e.prefix, ctx, true)
                                : std::vector<Symbol*>{};
                if (!syms.empty() && syms.front()->kind == SymKind::Type)
                    t = syms.front()->type;
                else {
                    TypeSet ps = typesOf(*e.prefix, ctx);
                    t = ps.types.size() == 1 ? ps.types.front() : nullptr;
                }
                s.add(t);
            }
            else {
                // user-defined attribute
                for (Symbol* ad : lookup(*ctx.scope, a))
                    if (ad->kind == SymKind::AttributeDecl)
                        s.add(ad->type);
            }
            return s;
        }
        case ExprKind::External: s.add(e.subtype ? subtype(*e.subtype, ctx) : nullptr); return s;
        default: return s;
    }
}

// ---------------------------------------------------------------------------
// Predefined operators
// ---------------------------------------------------------------------------

SVal Elaborator::applyPredefined(Op op, std::vector<SVal>& a, const Type* result, Ctx& ctx, Loc l) {
    using ir::CellKind;
    const bool allKnown = std::ranges::all_of(a, [](const SVal& v) { return v.known(); });
    const Type* t0 = a[0].type;

    // ---------- compile-time arithmetic on scalars ----------
    if (allKnown && a[0].value->kind != Value::Kind::Composite) {
        auto I = [&](std::size_t k) { return a[k].value->i; };
        auto R = [&](std::size_t k) {
            return a[k].value->kind == Value::Kind::Real ? a[k].value->r : static_cast<double>(a[k].value->i);
        };
        const bool real = std::ranges::any_of(a, [](const SVal& v) { return v.value->kind == Value::Kind::Real; });
        auto boolean = [&](bool b) { return makeConst(std_.boolean, Value::integer(b ? 1 : 0)); };
        auto num = [&](std::int64_t v) { return makeConst(result, Value::integer(v)); };
        auto rnum = [&](double v) { return makeConst(result, Value::real(v)); };
        if (a.size() == 2 && a[1].value->kind != Value::Kind::Composite) {
            switch (op) {
                case Op::Eq: return boolean(real ? R(0) == R(1) : I(0) == I(1));
                case Op::Ne: return boolean(real ? R(0) != R(1) : I(0) != I(1));
                case Op::Lt: return boolean(real ? R(0) < R(1) : I(0) < I(1));
                case Op::Le: return boolean(real ? R(0) <= R(1) : I(0) <= I(1));
                case Op::Gt: return boolean(real ? R(0) > R(1) : I(0) > I(1));
                case Op::Ge: return boolean(real ? R(0) >= R(1) : I(0) >= I(1));
                case Op::Add: return real ? rnum(R(0) + R(1)) : num(I(0) + I(1));
                case Op::Sub: return real ? rnum(R(0) - R(1)) : num(I(0) - I(1));
                case Op::Mul:
                    if (result->kind == TypeKind::Physical && real)
                        return num(static_cast<std::int64_t>(std::llround(R(0) * R(1))));
                    return real ? rnum(R(0) * R(1)) : num(I(0) * I(1));
                case Op::Div:
                    if (!real && I(1) == 0) {
                        error("VHDL-DIV-ZERO", "division by zero", l);
                        return num(0);
                    }
                    if (result->kind == TypeKind::Physical && real)
                        return num(static_cast<std::int64_t>(std::llround(R(0) / R(1))));
                    return real ? rnum(R(0) / R(1)) : num(I(0) / I(1));
                case Op::Mod: {
                    if (I(1) == 0) {
                        error("VHDL-DIV-ZERO", "mod by zero", l);
                        return num(0);
                    }
                    std::int64_t r = I(0) % I(1);
                    if (r != 0 && ((r < 0) != (I(1) < 0)))
                        r += I(1);
                    return num(r);
                }
                case Op::Rem:
                    if (I(1) == 0) {
                        error("VHDL-DIV-ZERO", "rem by zero", l);
                        return num(0);
                    }
                    return num(I(0) % I(1));
                case Op::Pow: {
                    if (real)
                        return rnum(std::pow(R(0), static_cast<double>(I(1))));
                    std::int64_t r = 1;
                    for (std::int64_t k = 0; k < I(1) && k < 64; ++k)
                        r *= I(0);
                    return num(r);
                }
                case Op::And:
                case Op::Or:
                case Op::Nand:
                case Op::Nor:
                case Op::Xor:
                case Op::Xnor: {
                    const bool x = I(0) != 0;
                    const bool y = I(1) != 0;
                    bool r = op == Op::And    ? (x && y)
                             : op == Op::Or   ? (x || y)
                             : op == Op::Nand ? !(x && y)
                             : op == Op::Nor  ? !(x || y)
                             : op == Op::Xor  ? (x != y)
                                              : (x == y);
                    return makeConst(result, Value::integer(r ? 1 : 0));
                }
                default: break;
            }
        }
        if (a.size() == 1) {
            switch (op) {
                case Op::Plus: return a[0].type == result ? a[0] : makeConst(result, *a[0].value);
                case Op::Minus: return real ? rnum(-R(0)) : num(-I(0));
                case Op::Abs: return real ? rnum(std::fabs(R(0))) : num(I(0) < 0 ? -I(0) : I(0));
                case Op::Not: return makeConst(result, Value::integer(I(0) ? 0 : 1));
                case Op::Condition: return boolean(I(0) != 0);
                default: break;
            }
        }
    }
    // ---------- compile-time array operations ----------
    if (allKnown && op == Op::Concat) {
        std::vector<Value> elems;
        for (const SVal& v : a) {
            if (v.type->isArray() && &v.type->baseType() == &result->baseType())
                elems.insert(elems.end(), v.value->elems.begin(), v.value->elems.end());
            else
                elems.push_back(*v.value);
        }
        const auto n = static_cast<std::int64_t>(elems.size());
        IntRange r{0, n - 1, true};
        // VHDL: the result takes the left operand's direction and left bound
        if (a[0].type->isArray() && &a[0].type->baseType() == &result->baseType() && a[0].type->indexRanges[0] &&
            a[0].type->indexRanges[0]->length() > 0) {
            const IntRange& lr = *a[0].type->indexRanges[0];
            r = lr.ascending ? IntRange{lr.left, lr.left + n - 1, true} : IntRange{lr.left, lr.left - n + 1, false};
        }
        else {
            const Type* it = result->indexTypes.front();
            const std::int64_t lo = it->constrained ? it->range.left : 0;
            r = it->range.ascending ? IntRange{lo, lo + n - 1, true} : IntRange{lo, lo - n + 1, false};
        }
        return makeConst(types.constrainedArray(result, {r}), Value::composite(std::move(elems)));
    }
    if (allKnown && (op == Op::Eq || op == Op::Ne) && a[0].value->kind == Value::Kind::Composite) {
        bool eq = *a[0].value == *a[1].value;
        if (a[0].type->isArray() && a[1].type->isArray() && a[0].value->elems.size() != a[1].value->elems.size())
            eq = false;
        return makeConst(std_.boolean, Value::integer((op == Op::Eq) == eq ? 1 : 0));
    }

    // ---------- hardware ----------
    if (ctx.constOnly && !allKnown) {
        error("VHDL-NOT-STATIC", std::string("operator '") + opSymbol(op) + "' on a non-static value", l);
        auto w = result ? bitWidth(*result) : std::nullopt;
        return SVal{result, std::nullopt, ir::SigSpec::allX(w.value_or(1))};
    }
    auto bits = [&](std::size_t k) { return bitsOf(a[k], l); };
    auto boolean = [&](const ir::SigSpec& s) { return makeBits(std_.boolean, s); };
    const bool scalarInt = t0 && t0->isIntegerLike();
    if (isRelational(op)) {
        ir::SigSpec x = bits(0);
        ir::SigSpec y = bits(1);
        bool sg = false;
        std::uint32_t w = std::max(x.width(), y.width());
        if (scalarInt || a[1].type->isIntegerLike()) {
            sg = true;
            w = std::max<std::uint32_t>(w, 32) + 1;
            x = x.resized(w, isSignedInteger(*a[0].type));
            y = y.resized(w, isSignedInteger(*a[1].type));
        }
        else if (a[0].type->isArray() && (op != Op::Eq && op != Op::Ne)) {
            // lexicographic comparison of equal-length discrete arrays (bit vectors)
            if (x.width() != y.width())
                warn("VHDL-LENGTH", "ordering comparison of arrays of different lengths", l);
            x = x.resized(w, false);
            y = y.resized(w, false);
        }
        else if (a[0].type->isArray() && x.width() != y.width()) {
            // "=" of arrays with different lengths is false
            return makeConst(std_.boolean, Value::integer(op == Op::Ne ? 1 : 0));
        }
        else {
            x = x.resized(w, false);
            y = y.resized(w, false);
        }
        // 1-bit equality with a constant: the signal itself (keeps reset /
        // enable signals recognizable as named signals)
        if ((op == Op::Eq || op == Op::Ne) && w == 1) {
            const ir::SigSpec* sig = nullptr;
            std::optional<ir::ConstBits> c;
            if ((c = y.asConst()))
                sig = &x;
            else if ((c = x.asConst()))
                sig = &y;
            if (sig && c->isFullyDefined()) {
                const bool one = (*c)[0] == ir::Logic::L1;
                const bool positive = (op == Op::Eq) == one;
                return boolean(positive ? *sig : cell(CellKind::Not, {*sig}, 1, l));
            }
        }
        const CellKind k = op == Op::Eq   ? CellKind::Eq
                           : op == Op::Ne ? CellKind::Ne
                           : op == Op::Lt ? CellKind::Lt
                           : op == Op::Le ? CellKind::Le
                           : op == Op::Gt ? CellKind::Gt
                                          : CellKind::Ge;
        return boolean(cell(k, {x, y}, 1, l, sg));
    }
    if (isLogical(op)) {
        ir::SigSpec x = bits(0);
        ir::SigSpec y = bits(1);
        const std::uint32_t w = std::max(x.width(), y.width());
        if (x.width() == 1 && w > 1)
            x = x.repeated(w);
        if (y.width() == 1 && w > 1)
            y = y.repeated(w);
        const CellKind k = op == Op::And || op == Op::Nand ? CellKind::And
                           : op == Op::Or || op == Op::Nor ? CellKind::Or
                           : op == Op::Xor                 ? CellKind::Xor
                                                           : CellKind::Xnor;
        ir::SigSpec r = cell(k, {x, y}, w, l);
        if (op == Op::Nand || op == Op::Nor)
            r = cell(CellKind::Not, {r}, w, l);
        return makeBits(a[0].type->isArray() ? a[0].type : a.size() > 1 && a[1].type->isArray() ? a[1].type : result, r);
    }
    if (op == Op::Not) {
        ir::SigSpec x = bits(0);
        return makeBits(a[0].type, cell(CellKind::Not, {x}, x.width(), l));
    }
    if (op == Op::Condition)
        return makeBits(std_.boolean, bits(0));
    if (isReduction(op)) {
        ir::SigSpec x = bits(0);
        const Op b = reductionBase(op);
        const CellKind k = b == Op::And || b == Op::Nand ? CellKind::ReduceAnd
                           : b == Op::Or || b == Op::Nor ? CellKind::ReduceOr
                                                         : CellKind::ReduceXor;
        ir::SigSpec r = cell(k, {x}, 1, l);
        if (b == Op::Nand || b == Op::Nor || b == Op::Xnor)
            r = cell(CellKind::Not, {r}, 1, l);
        return makeBits(result, r);
    }
    if (isMatching(op)) {
        ir::SigSpec x = bits(0);
        ir::SigSpec y = bits(1);
        const std::uint32_t w = std::max(x.width(), y.width());
        return makeBits(result, cell(op == Op::MatchEq ? CellKind::Eq : CellKind::Ne, {x.resized(w, false), y.resized(w, false)}, 1, l));
    }
    if (op == Op::Concat) {
        // a & b: a occupies the most significant bits
        ir::SigSpec r = bits(1);
        r.append(bits(0));
        std::uint64_t n = 0;
        for (const SVal& v : a)
            n += v.type->isArray() && &v.type->baseType() == &result->baseType()
                     ? (v.type->indexRanges[0] ? v.type->indexRanges[0]->length() : 0)
                     : 1;
        IntRange rr{0, static_cast<std::int64_t>(n) - 1, true};
        if (a[0].type->isArray() && &a[0].type->baseType() == &result->baseType() && a[0].type->indexRanges[0] &&
            a[0].type->indexRanges[0]->length() > 0) {
            const IntRange& lr = *a[0].type->indexRanges[0];
            const auto len = static_cast<std::int64_t>(n);
            rr = lr.ascending ? IntRange{lr.left, lr.left + len - 1, true} : IntRange{lr.left, lr.left - len + 1, false};
        }
        else if (!result->indexTypes.empty()) {
            const Type* it = result->indexTypes.front();
            const std::int64_t lo = it->constrained ? it->range.left : 0;
            const auto len = static_cast<std::int64_t>(n);
            rr = it->range.ascending ? IntRange{lo, lo + len - 1, true} : IntRange{lo, lo - len + 1, false};
        }
        const Type* rt = types.constrainedArray(result, {rr});
        if (a[0].type->isArray() && a[0].type->element && &a[0].type->element->baseType() == &result->element->baseType() &&
            a[0].type->element != result->element) {
            Type copy = *rt;
            copy.element = a[0].type->element;
            rt = types.make(std::move(copy));
        }
        return makeBits(rt, r);
    }
    if (isShift(op)) {
        ir::SigSpec x = bits(0);
        const std::uint32_t w = x.width();
        if (a[1].known() && w > 0) {
            std::int64_t n = a[1].value->i;
            bool left = op == Op::Sll || op == Op::Sla || op == Op::Rol;
            if (n < 0) {
                left = !left;
                n = -n;
            }
            const auto k = static_cast<std::uint32_t>(std::min<std::int64_t>(n, w));
            ir::SigSpec r;
            if (op == Op::Rol || op == Op::Ror) {
                const auto kk = static_cast<std::uint32_t>(n % w);
                if (left) {
                    r = x.extract(w - kk, kk);
                    r.append(x.extract(0, w - kk));
                }
                else {
                    r = x.extract(kk, w - kk);
                    r.append(x.extract(0, kk));
                }
            }
            else if (left) {
                r = op == Op::Sla ? x.extract(0, 1).repeated(k) : ir::SigSpec::constant(0, k);
                r.append(x.extract(0, w - k));
            }
            else {
                r = x.extract(k, w - k);
                r.append(op == Op::Sra ? x.extract(w - 1, 1).repeated(k) : ir::SigSpec::constant(0, k));
            }
            return makeBits(a[0].type, r);
        }
        if (op == Op::Rol || op == Op::Ror)
            return opaque(a[0].type, {x, bits(1)}, "rotate by a variable amount", l);
        return makeBits(a[0].type, cell(op == Op::Sll || op == Op::Sla ? CellKind::Shl
                                         : op == Op::Sra               ? CellKind::Sshr
                                                                       : CellKind::Shr,
                                         {x, bits(1)}, w, l, op == Op::Sra));
    }
    // integer arithmetic on signals
    if (t0 && (t0->isIntegerLike() || (a.size() == 2 && a[1].type->isIntegerLike()))) {
        std::uint32_t w = 32;
        for (const SVal& v : a)
            if (auto bw = bitWidth(*v.type); bw && *bw >= 32)
                w = std::max(w, *bw + 1);
        auto ext = [&](std::size_t k) { return bits(k).resized(w, isSignedInteger(*a[k].type)); };
        const Type* rt = result && result->kind == TypeKind::UniversalInteger ? std_.integer : result;
        auto finish = [&](const ir::SigSpec& r) {
            const auto rw = bitWidth(*rt).value_or(32);
            return makeBits(rt, r.resized(rw, true));
        };
        switch (op) {
            case Op::Add: return finish(cell(CellKind::Add, {ext(0), ext(1)}, w, l, true));
            case Op::Sub: return finish(cell(CellKind::Sub, {ext(0), ext(1)}, w, l, true));
            case Op::Mul: return finish(cell(CellKind::Mul, {ext(0), ext(1)}, w, l, true));
            case Op::Div: return finish(cell(CellKind::Div, {ext(0), ext(1)}, w, l, true));
            case Op::Mod:
            case Op::Rem: return finish(cell(CellKind::Mod, {ext(0), ext(1)}, w, l, true));
            case Op::Pow: return finish(cell(CellKind::Pow, {ext(0), ext(1)}, w, l, true));
            case Op::Minus: return finish(cell(CellKind::Neg, {ext(0)}, w, l, true));
            case Op::Plus: return finish(ext(0));
            case Op::Abs: {
                ir::SigSpec v = ext(0);
                ir::SigSpec neg = cell(CellKind::Neg, {v}, w, l, true);
                return finish(mux(v.extract(w - 1, 1), v, neg, l));
            }
            default: break;
        }
    }
    std::vector<ir::SigSpec> ins;
    for (std::size_t k = 0; k < a.size(); ++k)
        ins.push_back(bits(k));
    return opaque(result, std::move(ins), std::string("operator '") + opSymbol(op) + "'", l);
}

} // namespace opencdc::vhdl

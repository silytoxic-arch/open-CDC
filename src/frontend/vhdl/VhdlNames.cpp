// VHDL expressions, part 2: evaluation of names, literals, aggregates,
// attributes, indexing, calls (overload resolution and inlining) and
// assignment targets.

#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace opencdc::vhdl {

namespace {

std::uint64_t arrayLength(const Type& t) {
    std::uint64_t n = 1;
    for (const auto& r : t.indexRanges)
        n *= r ? r->length() : 0;
    return n;
}

bool isRangeChoice(const Expr& e) {
    return e.kind == ExprKind::Range || e.kind == ExprKind::Subtype ||
           (e.kind == ExprKind::Attribute && (e.ident.key == "range" || e.ident.key == "reverse_range"));
}

} // namespace

std::optional<Value> Elaborator::constValue(const Expr& e, Ctx& ctx, const Type* expected) {
    SVal v = eval(e, ctx, expected);
    if (!v.known())
        return std::nullopt;
    return v.value;
}

std::optional<std::int64_t> Elaborator::constInt(const Expr& e, Ctx& ctx, const Type* expected) {
    SVal v = eval(e, ctx, expected);
    if (!v.known() || v.value->kind != Value::Kind::Int)
        return std::nullopt;
    return v.value->i;
}

// ---------------------------------------------------------------------------
// Element access
// ---------------------------------------------------------------------------

SVal Elaborator::indexValue(const SVal& arr, const std::vector<SVal>& idx, Loc l) {
    const Type& T = *arr.type;
    if (!T.isArray() || idx.size() != T.indexTypes.size()) {
        error("VHDL-INDEX", "'" + T.name + "' cannot be indexed with " + std::to_string(idx.size()) + " index(es)", l);
        return SVal{};
    }
    const Type* et = T.element;
    const auto ew = bitWidth(*et);
    const bool allKnown = std::ranges::all_of(idx, [](const SVal& v) { return v.known(); });
    if (allKnown) {
        std::uint64_t flat = 0;
        for (std::size_t d = 0; d < idx.size(); ++d) {
            if (!T.indexRanges[d]) {
                error("VHDL-INDEX", "index into an unconstrained array", l);
                return SVal{};
            }
            const IntRange& r = *T.indexRanges[d];
            const std::int64_t i = idx[d].value->i;
            if (!r.contains(i)) {
                error("VHDL-INDEX", "index " + std::to_string(i) + " is outside the range " + std::to_string(r.left) +
                                        (r.ascending ? " to " : " downto ") + std::to_string(r.right) + " of '" +
                                        T.name + "'",
                      l);
                return SVal{et, std::nullopt, ir::SigSpec::allX(ew.value_or(1))};
            }
            flat = flat * r.length() + r.positionOf(i);
        }
        if (arr.known())
            return makeConst(et, arr.value->elems.at(flat));
        if (!ew)
            return SVal{et, std::nullopt, {}};
        const auto n = arrayLength(T);
        const auto off = static_cast<std::uint32_t>((n - 1 - flat) * *ew);
        return makeBits(et, bitsOf(arr, l).extract(off, *ew));
    }
    if (idx.size() != 1 || !T.indexRanges[0] || !ew) {
        std::vector<ir::SigSpec> ins{bitsOf(arr, l)};
        for (const auto& i : idx)
            ins.push_back(bitsOf(i, l));
        return opaque(et, std::move(ins), "multi-dimensional indexing with a variable index", l);
    }
    const IntRange& r = *T.indexRanges[0];
    ir::Cell c;
    c.kind = ir::CellKind::DynSelect;
    c.inputs = {bitsOf(arr, l), bitsOf(idx[0], l)};
    c.width = *ew;
    c.elemWidth = *ew;
    c.isSigned = isSignedInteger(*idx[0].type);
    c.rangeLeft = static_cast<std::int32_t>(r.left);
    c.rangeRight = static_cast<std::int32_t>(r.right);
    c.origin = srcLoc(l);
    if (!body || !body->ed) {
        error("VHDL-NOT-STATIC", "index is not static", l);
        return SVal{et, std::nullopt, ir::SigSpec::allX(*ew)};
    }
    return makeBits(et, body->ed->emit(std::move(c)));
}

SVal Elaborator::sliceValue(const SVal& arr, const IntRange& r, Loc l) {
    const Type& T = *arr.type;
    if (!T.isArray() || T.indexTypes.size() != 1 || !T.indexRanges[0]) {
        error("VHDL-SLICE", "'" + T.name + "' cannot be sliced", l);
        return SVal{};
    }
    const IntRange& ar = *T.indexRanges[0];
    const Type* st = types.constrainedArray(&T, {r});
    if (r.isNull())
        return makeConst(st, Value::composite({}));
    if (r.ascending != ar.ascending) {
        error("VHDL-SLICE", "slice direction differs from the direction of '" + T.name + "'", l);
        return SVal{};
    }
    if (!ar.contains(r.left) || !ar.contains(r.right)) {
        error("VHDL-SLICE", "slice " + std::to_string(r.left) + (r.ascending ? " to " : " downto ") +
                                std::to_string(r.right) + " is outside the range of '" + T.name + "'",
              l);
        return SVal{st, std::nullopt, ir::SigSpec::allX(bitWidth(*st).value_or(1))};
    }
    const std::uint64_t p0 = ar.positionOf(r.left);
    const std::uint64_t p1 = ar.positionOf(r.right);
    if (arr.known())
        return makeConst(st, Value::composite(std::vector<Value>(arr.value->elems.begin() + static_cast<std::ptrdiff_t>(p0),
                                                                arr.value->elems.begin() + static_cast<std::ptrdiff_t>(p1) + 1)));
    const auto ew = bitWidth(*T.element).value_or(1);
    const auto n = ar.length();
    const auto off = static_cast<std::uint32_t>((n - 1 - p1) * ew);
    const auto w = static_cast<std::uint32_t>((p1 - p0 + 1) * ew);
    return makeBits(st, bitsOf(arr, l).extract(off, w));
}

SVal Elaborator::fieldValue(const SVal& rec, std::size_t field, Loc l) {
    const Type& T = *rec.type;
    const Type* ft = T.fields[field].type;
    if (rec.known())
        return makeConst(ft, rec.value->elems.at(field));
    std::uint32_t off = 0;
    for (std::size_t k = field + 1; k < T.fields.size(); ++k)
        off += bitWidth(*T.fields[k].type).value_or(0);
    return makeBits(ft, bitsOf(rec, l).extract(off, bitWidth(*ft).value_or(0)));
}

// ---------------------------------------------------------------------------
// Literals and aggregates
// ---------------------------------------------------------------------------

SVal Elaborator::evalStringLiteral(const std::string& text, const Type* expected, Loc l) {
    const Type* T = expected && expected->isArray() && expected->indexTypes.size() == 1 ? expected : std_.string;
    const Type& E = T->element->baseType();
    if (E.kind != TypeKind::Enum || !E.enumInfo) {
        error("VHDL-TYPE", "string literal for non-character array type '" + T->name + "'", l);
        return SVal{};
    }
    std::vector<Value> elems;
    elems.reserve(text.size());
    for (const char c : text) {
        const std::string key = std::string("'") + c + "'";
        const auto& keys = E.enumInfo->keys;
        auto it = std::ranges::find(keys, key);
        if (it == keys.end()) {
            error("VHDL-TYPE", "character '" + std::string(1, c) + "' is not a literal of '" + E.name + "'", l);
            return SVal{};
        }
        elems.push_back(Value::integer(it - keys.begin()));
    }
    const auto n = static_cast<std::int64_t>(elems.size());
    const Type* rt = T;
    if (T->indexRanges[0]) {
        if (static_cast<std::uint64_t>(n) != T->indexRanges[0]->length()) {
            error("VHDL-LENGTH", "string literal has " + std::to_string(n) + " element(s), '" + T->name + "' has " +
                                     std::to_string(T->indexRanges[0]->length()),
                  l);
            return SVal{T, std::nullopt, ir::SigSpec::allX(bitWidth(*T).value_or(1))};
        }
    }
    else {
        const Type* it = T->indexTypes[0];
        const std::int64_t left = it->constrained ? it->range.left : 0;
        const bool asc = it->constrained ? it->range.ascending : true;
        rt = types.constrainedArray(T, {asc ? IntRange{left, left + n - 1, true} : IntRange{left, left - n + 1, false}});
    }
    return makeConst(rt, Value::composite(std::move(elems)));
}

SVal Elaborator::evalAggregate(const Expr& e, Ctx& ctx, const Type* expected) {
    if (!expected || !expected->isComposite()) {
        error("VHDL-AGGREGATE", "the type of this aggregate cannot be determined from its context", e.loc);
        return SVal{};
    }
    const Type& T = *expected;
    if (T.kind == TypeKind::Record) {
        std::vector<SVal> vals(T.fields.size());
        std::size_t pos = 0;
        for (const auto& el : e.elements) {
            if (el.choices.empty()) {
                if (pos >= T.fields.size()) {
                    error("VHDL-AGGREGATE", "too many elements for record '" + T.name + "'", el.loc);
                    break;
                }
                vals[pos] = eval(*el.value, ctx, T.fields[pos].type);
                ++pos;
                continue;
            }
            for (const auto& ch : el.choices) {
                if (ch->kind == ExprKind::Others) {
                    for (std::size_t k = 0; k < T.fields.size(); ++k)
                        if (!vals[k].valid())
                            vals[k] = eval(*el.value, ctx, T.fields[k].type);
                    continue;
                }
                if (ch->kind != ExprKind::Simple) {
                    error("VHDL-AGGREGATE", "record aggregate choices must be field names", ch->loc);
                    continue;
                }
                auto it = std::ranges::find(T.fields, ch->ident.key, &FieldInfo::key);
                if (it == T.fields.end()) {
                    error("VHDL-AGGREGATE", "'" + T.name + "' has no field '" + ch->ident.text + "'", ch->loc);
                    continue;
                }
                const auto k = static_cast<std::size_t>(it - T.fields.begin());
                vals[k] = eval(*el.value, ctx, T.fields[k].type);
            }
        }
        bool known = true;
        for (std::size_t k = 0; k < vals.size(); ++k) {
            if (!vals[k].valid()) {
                error("VHDL-AGGREGATE", "field '" + T.fields[k].display + "' of '" + T.name + "' is not given", e.loc);
                return SVal{};
            }
            vals[k] = convert(vals[k], T.fields[k].type, ctx, e.loc);
            known = known && vals[k].known();
        }
        if (known) {
            std::vector<Value> v;
            for (auto& x : vals)
                v.push_back(*x.value);
            return makeConst(&T, Value::composite(std::move(v)));
        }
        ir::SigSpec bits;
        for (std::size_t k = vals.size(); k-- > 0;)
            bits.append(bitsOf(vals[k], e.loc)); // last field at the LSB end
        return makeBits(&T, bits);
    }

    // arrays
    const std::size_t dims = T.indexTypes.size();
    const Type* elemT = T.element;
    if (dims > 1) {
        // array of the remaining dimensions
        Type sub = T;
        sub.base = nullptr;
        sub.isSubtype = false;
        sub.indexTypes.erase(sub.indexTypes.begin());
        sub.indexRanges.erase(sub.indexRanges.begin());
        sub.name = T.name + "(row)";
        elemT = types.make(std::move(sub));
    }
    const Type* indexType = T.indexTypes[0];
    struct Piece {
        std::int64_t index = 0;
        SVal value;
    };
    std::vector<std::pair<std::int64_t, SVal>> named;
    std::vector<SVal> positional;
    const Expr* othersValue = nullptr;
    bool anyNamed = false;
    for (const auto& el : e.elements) {
        if (el.choices.empty()) {
            // VHDL-2008: an element may be an array of the aggregate's type
            TypeSet ts = typesOf(*el.value, ctx);
            if (!ts.accepts(*elemT) && ts.accepts(T) && !ts.anyAggregate) {
                SVal piece = eval(*el.value, ctx, nullptr);
                if (piece.valid() && piece.type->isArray()) {
                    const auto n = arrayLength(*piece.type);
                    for (std::uint64_t k = 0; k < n; ++k) {
                        SVal ix = makeConst(piece.type->indexTypes[0],
                                            Value::integer(piece.type->indexRanges[0]->indexAt(k)));
                        positional.push_back(indexValue(piece, {ix}, el.loc));
                    }
                }
                continue;
            }
            positional.push_back(eval(*el.value, ctx, elemT));
            continue;
        }
        for (const auto& ch : el.choices) {
            if (ch->kind == ExprKind::Others) {
                othersValue = el.value.get();
                continue;
            }
            anyNamed = true;
            if (isRangeChoice(*ch)) {
                auto r = discreteRange(*ch, ctx);
                if (!r)
                    continue;
                SVal v = eval(*el.value, ctx, elemT);
                for (std::uint64_t k = 0; k < r->length(); ++k)
                    named.emplace_back(r->indexAt(k), v);
                continue;
            }
            // a discrete subtype name used as a choice
            if (ch->kind == ExprKind::Simple || ch->kind == ExprKind::Selected) {
                auto syms = resolveName(*ch, ctx, true);
                if (!syms.empty() && syms.front()->kind == SymKind::Type && syms.front()->type->isDiscrete()) {
                    const IntRange r = syms.front()->type->range;
                    SVal v = eval(*el.value, ctx, elemT);
                    for (std::uint64_t k = 0; k < r.length(); ++k)
                        named.emplace_back(r.indexAt(k), v);
                    continue;
                }
            }
            auto i = constInt(*ch, ctx, indexType);
            if (!i) {
                error("VHDL-AGGREGATE", "aggregate choice is not static", ch->loc);
                continue;
            }
            named.emplace_back(*i, eval(*el.value, ctx, elemT));
        }
    }
    if (anyNamed && !positional.empty()) {
        error("VHDL-AGGREGATE", "aggregate mixes positional and named associations", e.loc);
        return SVal{};
    }
    // bounds
    IntRange r;
    if (T.indexRanges[0]) {
        r = *T.indexRanges[0];
    }
    else if (othersValue) {
        error("VHDL-AGGREGATE", "'others' in an aggregate of unconstrained type '" + T.name + "' needs a constrained context",
              e.loc);
        return SVal{};
    }
    else if (!anyNamed) {
        const std::int64_t left = indexType->constrained ? indexType->range.left : 0;
        const bool asc = indexType->constrained ? indexType->range.ascending : true;
        const auto n = static_cast<std::int64_t>(positional.size());
        r = asc ? IntRange{left, left + n - 1, true} : IntRange{left, left - n + 1, false};
    }
    else {
        std::int64_t lo = named.front().first;
        std::int64_t hi = lo;
        for (const auto& [i, v] : named) {
            lo = std::min(lo, i);
            hi = std::max(hi, i);
        }
        const bool asc = indexType->constrained ? indexType->range.ascending : true;
        r = asc ? IntRange{lo, hi, true} : IntRange{hi, lo, false};
    }
    const std::uint64_t n = r.length();
    if (n > kMaxObjectBits) {
        error("VHDL-TOO-WIDE", "aggregate of " + std::to_string(n) + " elements (more than " +
                                   std::to_string(kMaxObjectBits) + ")",
              e.loc);
        return SVal{};
    }
    std::vector<SVal> slots(n);
    if (!anyNamed) {
        if (positional.size() > n || (positional.size() < n && !othersValue)) {
            error("VHDL-LENGTH", "aggregate has " + std::to_string(positional.size()) + " element(s), '" + T.name +
                                     "' needs " + std::to_string(n),
                  e.loc);
            return SVal{};
        }
        for (std::size_t k = 0; k < positional.size(); ++k)
            slots[k] = std::move(positional[k]);
    }
    else {
        for (auto& [i, v] : named) {
            if (!r.contains(i)) {
                error("VHDL-AGGREGATE", "aggregate index " + std::to_string(i) + " is outside the range of '" +
                                            T.name + "'",
                      e.loc);
                continue;
            }
            slots[r.positionOf(i)] = std::move(v);
        }
    }
    if (othersValue) {
        SVal ov;
        for (auto& s : slots) {
            if (s.valid())
                continue;
            if (!ov.valid())
                ov = eval(*othersValue, ctx, elemT);
            s = ov;
        }
    }
    const Type* rt = T.indexRanges[0] ? &T : types.constrainedArray(&T, {r});
    bool known = true;
    const Type* actualElem = nullptr;
    for (auto& s : slots) {
        if (!s.valid()) {
            error("VHDL-AGGREGATE", "aggregate does not give a value for every element of '" + T.name + "'", e.loc);
            return SVal{};
        }
        if (elemT->isFullyConstrained() || !s.type)
            s = convert(s, elemT, ctx, e.loc);
        else if (!actualElem)
            actualElem = s.type;
        known = known && s.known();
    }
    if (actualElem && !rt->element->isFullyConstrained()) {
        Type copy = *rt;
        copy.element = actualElem;
        rt = types.make(std::move(copy));
    }
    if (dims > 1) {
        // flatten rows
        if (known) {
            std::vector<Value> flat;
            for (auto& s : slots)
                flat.insert(flat.end(), s.value->elems.begin(), s.value->elems.end());
            std::vector<IntRange> rr{r};
            for (std::size_t d = 1; d < dims; ++d)
                rr.push_back(T.indexRanges[d].value_or(IntRange{0, -1, true}));
            return makeConst(types.constrainedArray(&T, rr), Value::composite(std::move(flat)));
        }
    }
    if (known) {
        std::vector<Value> v;
        v.reserve(slots.size());
        for (auto& s : slots)
            v.push_back(*s.value);
        return makeConst(rt, Value::composite(std::move(v)));
    }
    ir::SigSpec bits;
    for (std::size_t k = slots.size(); k-- > 0;)
        bits.append(bitsOf(slots[k], e.loc));
    return makeBits(rt, bits);
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

SVal Elaborator::evalAttribute(const Expr& e, Ctx& ctx, const Type* expected) {
    const std::string& a = e.ident.key;
    const Expr& pre = *e.prefix;
    // type-mark prefix?
    const Type* markType = nullptr;
    std::vector<Symbol*> preSyms;
    if (pre.kind == ExprKind::Simple || pre.kind == ExprKind::Selected) {
        preSyms = resolveName(pre, ctx, true);
        if (!preSyms.empty() && preSyms.front()->kind == SymKind::Type)
            markType = preSyms.front()->type;
    }
    else if (pre.kind == ExprKind::Attribute && (pre.ident.key == "base" || pre.ident.key == "subtype" ||
                                                 pre.ident.key == "element")) {
        markType = typeMark(pre, ctx);
    }
    auto prefixType = [&]() -> const Type* {
        if (markType)
            return markType;
        if (!preSyms.empty()) {
            Symbol* s = preSyms.front();
            if ((s->kind == SymKind::Constant || s->kind == SymKind::Signal || s->kind == SymKind::Variable) && s->type &&
                s->type->isFullyConstrained())
                return s->type;
            if (s->kind == SymKind::Constant && s->value.valid())
                return s->value.type;
        }
        // evaluate the prefix for its (constrained) type
        Ctx c2 = ctx;
        SVal v = eval(pre, c2, nullptr);
        return v.type;
    };
    auto dimArg = [&]() -> std::size_t {
        if (e.args.empty())
            return 0;
        auto d = constInt(*e.args.front().actual, ctx);
        return d && *d >= 1 ? static_cast<std::size_t>(*d - 1) : 0;
    };

    if (a == "length" || a == "high" || a == "low" || a == "left" || a == "right" || a == "ascending" ||
        a == "range" || a == "reverse_range") {
        const Type* T = prefixType();
        if (!T)
            return SVal{};
        if (T->isArray()) {
            const std::size_t d = dimArg();
            if (d >= T->indexRanges.size() || !T->indexRanges[d]) {
                error("VHDL-ATTRIBUTE", "'" + e.ident.text + " of an unconstrained array", e.loc);
                return SVal{};
            }
            const IntRange& r = *T->indexRanges[d];
            const Type* it = T->indexTypes[d];
            if (a == "length")
                return makeConst(std_.univInt, Value::integer(static_cast<std::int64_t>(r.length())));
            if (a == "ascending")
                return makeConst(std_.boolean, Value::integer(r.ascending ? 1 : 0));
            const std::int64_t v = a == "high" ? r.high() : a == "low" ? r.low() : a == "left" ? r.left : r.right;
            return makeConst(it->isIntegerLike() ? std_.univInt : it, Value::integer(v));
        }
        if (T->isScalar()) {
            if (T->isRealLike()) {
                const RealRange& r = T->rrange;
                const double v = a == "high"   ? std::max(r.left, r.right)
                                 : a == "low"  ? std::min(r.left, r.right)
                                 : a == "left" ? r.left
                                               : r.right;
                return makeConst(T, Value::real(v));
            }
            const IntRange& r = T->range;
            if (a == "ascending")
                return makeConst(std_.boolean, Value::integer(r.ascending ? 1 : 0));
            if (a == "length")
                return makeConst(std_.univInt, Value::integer(static_cast<std::int64_t>(r.length())));
            const std::int64_t v = a == "high" ? r.high() : a == "low" ? r.low() : a == "left" ? r.left : r.right;
            return makeConst(T, Value::integer(v));
        }
        error("VHDL-ATTRIBUTE", "'" + e.ident.text + " is not defined for '" + T->name + "'", e.loc);
        return SVal{};
    }
    if (a == "pos") {
        const Type* T = markType;
        if (!T || e.args.empty())
            return SVal{};
        SVal v = eval(*e.args.front().actual, ctx, T);
        if (v.known())
            return makeConst(std_.univInt, Value::integer(v.value->i));
        return makeBits(std_.integer, bitsOf(v, e.loc).resized(32, isSignedInteger(*T)));
    }
    if (a == "val" || a == "succ" || a == "pred" || a == "leftof" || a == "rightof") {
        const Type* T = markType;
        if (!T || e.args.empty())
            return SVal{};
        SVal v = eval(*e.args.front().actual, ctx, a == "val" ? std_.integer : T);
        if (!v.known()) {
            if (a == "val")
                return makeBits(T, bitsOf(v, e.loc).resized(bitWidth(*T).value_or(1), false));
            return opaque(T, {bitsOf(v, e.loc)}, "'" + e.ident.text, e.loc);
        }
        std::int64_t i = v.value->i;
        const bool asc = T->range.ascending;
        if (a == "succ")
            ++i;
        else if (a == "pred")
            --i;
        else if (a == "rightof")
            i += asc ? 1 : -1;
        else if (a == "leftof")
            i -= asc ? 1 : -1;
        return makeConst(T, Value::integer(i));
    }
    if (a == "image" || a == "simple_name" || a == "path_name" || a == "instance_name" || a == "value")
        return SVal{a == "value" ? markType : std_.string, std::nullopt, {}};
    if (a == "event" || a == "stable" || a == "last_value" || a == "delayed" || a == "active" || a == "quiet" ||
        a == "last_event" || a == "last_active" || a == "transaction" || a == "driving" || a == "driving_value") {
        SVal v = eval(pre, ctx, nullptr);
        unsupported("IR-UNSUPPORTED-ATTRIBUTE",
                    "signal attribute '" + e.ident.text + " outside a recognized clock condition", e.loc);
        const Type* rt = (a == "last_value" || a == "delayed" || a == "driving_value") ? v.type : std_.boolean;
        return opaque(rt, {bitsOf(v, e.loc)}, "'" + e.ident.text, e.loc);
    }
    // user-defined attribute of a named entity
    for (Symbol* s : preSyms)
        for (const auto& [name, val] : s->attributes)
            if (name == a)
                return val;
    (void)expected;
    error("VHDL-ATTRIBUTE", "attribute '" + e.ident.text + "' is not defined for this prefix", e.loc);
    return SVal{};
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

bool Elaborator::mapArgs(Symbol& sub, const std::vector<Assoc>& args, std::vector<const Expr*>& perFormal) {
    resolveParams(sub);
    perFormal.assign(sub.params.size(), nullptr);
    std::vector<bool> given(sub.params.size(), false);
    std::size_t pos = 0;
    bool namedSeen = false;
    for (const auto& a : args) {
        if (!a.formal) {
            if (namedSeen || pos >= sub.params.size())
                return false;
            perFormal[pos] = a.actual.get();
            given[pos] = true;
            ++pos;
            continue;
        }
        namedSeen = true;
        if (a.formal->kind != ExprKind::Simple)
            return false; // partial / converted formals: not supported for subprograms
        std::size_t k = 0;
        for (; k < sub.params.size(); ++k)
            if (sub.params[k].key == a.formal->ident.key)
                break;
        if (k == sub.params.size() || given[k])
            return false;
        perFormal[k] = a.actual.get();
        given[k] = true;
    }
    for (std::size_t k = 0; k < sub.params.size(); ++k) {
        if (perFormal[k] && perFormal[k]->kind == ExprKind::Open)
            perFormal[k] = nullptr;
        if (!perFormal[k] && !sub.params[k].defaultExpr && sub.params[k].mode == Mode::In)
            return false;
    }
    return true;
}

Symbol* Elaborator::chooseSubprogram(const std::vector<Symbol*>& syms, const std::vector<Assoc>& args,
                                     const Type* expected, bool functions, Ctx& ctx, Loc l, bool quiet) {
    std::vector<TypeSet> argSets;
    argSets.reserve(args.size());
    for (const auto& a : args)
        argSets.push_back(a.actual && a.actual->kind != ExprKind::Open ? typesOf(*a.actual, ctx) : TypeSet{});
    struct Fit {
        Symbol* sym;
        int score;
    };
    std::vector<Fit> fits;
    for (Symbol* s : syms) {
        if (s->kind != SymKind::Subprogram || s->isFunction != functions || s->implicitOp)
            continue;
        std::vector<const Expr*> map;
        if (!mapArgs(*s, args, map))
            continue;
        bool ok = true;
        int score = 0;
        for (std::size_t k = 0; k < map.size() && ok; ++k) {
            if (!map[k])
                continue;
            // find the actual's TypeSet
            std::size_t ai = 0;
            for (; ai < args.size(); ++ai)
                if (args[ai].actual.get() == map[k])
                    break;
            const TypeSet& ts = argSets[ai];
            if (!s->params[k].type) {
                ok = false;
                break;
            }
            if (ts.empty()) {
                // names we cannot type (e.g. unknown); accept but do not score
                continue;
            }
            ok = ts.accepts(*s->params[k].type);
            for (const Type* t : ts.types)
                if (&t->baseType() == &s->params[k].type->baseType())
                    ++score;
        }
        if (ok)
            fits.push_back({s, score});
    }
    if (fits.empty()) {
        if (!quiet && !syms.empty())
            error("VHDL-NO-OVERLOAD",
                  "no visible " + std::string(functions ? "function" : "procedure") + " '" + syms.front()->display +
                      "' matches the arguments",
                  l);
        return nullptr;
    }
    if (expected && fits.size() > 1) {
        std::vector<Fit> byResult;
        for (const auto& f : fits)
            if (f.sym->type && (&f.sym->type->baseType() == &expected->baseType() ||
                                (f.sym->type->isIntegerLike() && expected->isIntegerLike() &&
                                 f.sym->type->kind == TypeKind::UniversalInteger)))
                byResult.push_back(f);
        if (!byResult.empty())
            fits = std::move(byResult);
    }
    if (fits.size() > 1) {
        int best = -1;
        for (const auto& f : fits)
            best = std::max(best, f.score);
        std::erase_if(fits, [&](const Fit& f) { return f.score != best; });
    }
    if (fits.size() > 1) {
        // the same subprogram reached twice (alias / two use clauses) is not ambiguous
        auto sameProfile = [](const Symbol* a, const Symbol* b) {
            if (a->params.size() != b->params.size() || a->type != b->type)
                return false;
            for (std::size_t k = 0; k < a->params.size(); ++k)
                if (a->params[k].type != b->params[k].type)
                    return false;
            return a->builtin == b->builtin && a->decl == b->decl;
        };
        std::vector<Fit> uniq;
        for (const auto& f : fits)
            if (std::ranges::none_of(uniq, [&](const Fit& u) { return sameProfile(u.sym, f.sym); }))
                uniq.push_back(f);
        fits = std::move(uniq);
    }
    if (fits.size() > 1 && !quiet) {
        const std::string key = "amb:" + fits.front().sym->key + ":" + std::to_string(l.file) + ":" + std::to_string(l.line);
        if (!reported_.contains(key)) {
            reported_.insert(key);
            warn("VHDL-AMBIGUOUS", "call of '" + fits.front().sym->display + "' is ambiguous (" +
                                       std::to_string(fits.size()) + " candidates); using the first visible one",
                 l);
        }
    }
    return fits.front().sym;
}

bool Elaborator::evalArgs(Symbol& sub, const std::vector<Assoc>& args, Ctx& ctx, Loc l, std::vector<SVal>& values,
                          std::vector<std::vector<Target>>& outTargets) {
    std::vector<const Expr*> map;
    if (!mapArgs(sub, args, map))
        return false;
    values.assign(sub.params.size(), SVal{});
    outTargets.assign(sub.params.size(), {});
    for (std::size_t k = 0; k < sub.params.size(); ++k) {
        const ParamInfo& p = sub.params[k];
        if (p.mode == Mode::Out || p.mode == Mode::InOut || p.mode == Mode::Buffer) {
            if (!map[k])
                continue;
            if (!targets(*map[k], ctx, outTargets[k])) {
                error("VHDL-TARGET", "actual of out parameter '" + p.display + "' is not a signal or variable", l);
                continue;
            }
            if (p.mode == Mode::InOut)
                values[k] = convert(eval(*map[k], ctx, p.type), p.type, ctx, l);
            continue;
        }
        if (map[k]) {
            values[k] = convert(eval(*map[k], ctx, p.type), p.type, ctx, l);
        }
        else if (p.defaultExpr) {
            Ctx c2 = ctx;
            c2.scope = p.defaultScope ? p.defaultScope : ctx.scope;
            values[k] = convert(eval(*p.defaultExpr, c2, p.type), p.type, c2, l);
        }
    }
    return true;
}

SVal Elaborator::callSubprogram(Symbol& sym, std::vector<SVal>& args, std::vector<std::vector<Target>>& outTargets,
                                const Type* result, Ctx& ctx, Loc l) {
    if (sym.builtin != Builtin::None || (!sym.decl && sym.paramsResolved)) {
        if (!sym.isFunction && sym.builtin == Builtin::Ignored)
            return SVal{};
        return callBuiltin(sym.builtin, sym, args, result, ctx, l);
    }
    if (!sym.body) {
        // body in a package body not yet elaborated
        for (auto& [key, pkg] : packages)
            if (&pkg->scope == sym.declScope)
                elaboratePackageBody(*pkg);
    }
    if (!sym.body) {
        error("VHDL-NO-BODY", "subprogram '" + sym.display + "' has no body", l);
        return SVal{result, std::nullopt, {}};
    }
    if (ctx.inlineDepth >= static_cast<int>(opts_.maxInlineDepth)) {
        error("VHDL-RECURSION", "subprogram '" + sym.display + "' nests deeper than " +
                                    std::to_string(opts_.maxInlineDepth) + " calls (recursion?)",
              l);
        return SVal{result, std::nullopt, {}};
    }
    Scope* fs = newScope(sym.bodyScope, ctx.scope->label);
    Ctx fctx;
    fctx.scope = fs;
    fctx.constOnly = ctx.constOnly;
    fctx.inlineDepth = ctx.inlineDepth + 1;
    // parameters
    std::vector<int> outObjects(sym.params.size(), -1);
    for (std::size_t k = 0; k < sym.params.size(); ++k) {
        const ParamInfo& p = sym.params[k];
        const bool out = p.mode == Mode::Out || p.mode == Mode::InOut || p.mode == Mode::Buffer;
        const Type* pt = args[k].valid() && p.type && !p.type->isFullyConstrained() ? args[k].type : p.type;
        if (out && !outTargets[k].empty() && pt && !pt->isFullyConstrained()) {
            // unconstrained out parameter: take the actual's subtype
            const Target& t0 = outTargets[k].front();
            if (outTargets[k].size() == 1 && t0.type)
                pt = t0.type;
        }
        if (!out && (p.cls == InterfaceDecl::Class::Constant || p.cls == InterfaceDecl::Class::Signal ||
                     p.cls == InterfaceDecl::Class::Default || p.cls == InterfaceDecl::Class::File)) {
            Symbol* s = newSymbol(SymKind::Constant, p.key, p.display, l);
            s->type = pt;
            s->value = args[k];
            if (s->value.valid())
                s->value.type = pt ? pt : s->value.type;
            fs->add(s);
            continue;
        }
        Object o;
        o.name = p.display;
        o.type = pt;
        o.loc = l;
        o.initial = args[k].valid() ? args[k] : (pt ? initialValue(o, l) : SVal{});
        const int idx = addObject(std::move(o));
        outObjects[k] = idx;
        Symbol* s = newSymbol(SymKind::Variable, p.key, p.display, l);
        s->type = pt;
        s->object = idx;
        fs->add(s);
    }
    elaborateDeclarativePart(*fs, sym.body->decls, fctx);

    if (sym.isFunction) {
        ProcState st;
        st.alive = ir::SigSpec::constant(1, 1);
        st.returned = ir::SigSpec::constant(0, 1);
        fctx.st = &st;
        fctx.returnType = result ? result : sym.type;
        execList(sym.body->body, fctx);
        if (!st.retValue.valid()) {
            error("VHDL-NO-RETURN", "function '" + sym.display + "' ended without a return statement", l);
            return SVal{result, std::nullopt, {}};
        }
        return st.retValue;
    }

    // procedure: runs in the caller's process state
    ProcState local;
    ProcState* st = ctx.st;
    if (!st) {
        local.alive = ir::SigSpec::constant(1, 1);
        local.returned = ir::SigSpec::constant(0, 1);
        st = &local;
    }
    const ir::SigSpec savedAlive = st->alive;
    const ir::SigSpec savedReturned = st->returned;
    st->returned = ir::SigSpec::constant(0, 1);
    fctx.st = st;
    execList(sym.body->body, fctx);
    st->alive = savedAlive;
    st->returned = savedReturned;
    // copy out parameters back to the actuals
    for (std::size_t k = 0; k < sym.params.size(); ++k) {
        if (outObjects[k] < 0 || outTargets[k].empty())
            continue;
        const BitMask* written = st->any.find(outObjects[k]);
        if (!written || std::ranges::none_of(*written, [](bool b) { return b; }))
            continue;
        SVal v = readObject(outObjects[k], fctx, l);
        const bool signal = sym.params[k].cls == InterfaceDecl::Class::Signal;
        assign(outTargets[k], v, signal, ctx, l);
    }
    return SVal{};
}

SVal Elaborator::evalCall(const Expr& e, Ctx& ctx, const Type* expected) {
    const Expr& pre = *e.prefix;
    std::vector<Symbol*> syms;
    if (pre.kind == ExprKind::Simple || pre.kind == ExprKind::Selected) {
        const bool fieldSel = pre.kind == ExprKind::Selected &&
                              [&] {
                                  auto pp = pre.prefix->kind == ExprKind::Simple || pre.prefix->kind == ExprKind::Selected
                                                ? resolveName(*pre.prefix, ctx, true)
                                                : std::vector<Symbol*>{};
                                  return pp.empty() || (pp.front()->kind != SymKind::Library &&
                                                        pp.front()->kind != SymKind::Package);
                              }();
        if (!fieldSel)
            syms = resolveName(pre, ctx);
        if (!fieldSel && syms.empty())
            return SVal{expected, std::nullopt, {}};
    }
    // type conversion
    if (!syms.empty() && syms.front()->kind == SymKind::Type) {
        const Type* T = syms.front()->type;
        if (e.args.size() != 1) {
            error("VHDL-CONVERSION", "type conversion takes one operand", e.loc);
            return SVal{};
        }
        const Expr& operand = *e.args[0].actual;
        TypeSet ts = typesOf(operand, ctx);
        const Type* ot = ts.types.size() == 1 ? ts.types.front() : nullptr;
        if (!ot && ts.univInt && !ts.univReal)
            ot = std_.univInt;
        if (!ot && ts.univReal)
            ot = std_.univReal;
        if (!ot && (ts.anyString || ts.anyAggregate))
            ot = T; // literal operand: typed by the conversion's target
        if (!ot && ts.types.size() > 1) {
            // prefer a closely related candidate
            for (const Type* t : ts.types)
                if ((t->isArray() && T->isArray()) || (t->isScalar() && T->isScalar()))
                    ot = t;
        }
        SVal v = eval(operand, ctx, ot);
        return convert(v, T, ctx, e.loc, /*implicit=*/false);
    }
    // function call
    if (!syms.empty() && syms.front()->kind == SymKind::Subprogram) {
        Symbol* f = chooseSubprogram(syms, e.args, expected, true, ctx, e.loc, /*quiet=*/true);
        if (f) {
            std::vector<SVal> args;
            std::vector<std::vector<Target>> outs;
            if (!evalArgs(*f, e.args, ctx, e.loc, args, outs))
                return SVal{};
            return callSubprogram(*f, args, outs, f->type, ctx, e.loc);
        }
        // parameterless function whose result is indexed: f(3)
        for (Symbol* s : syms) {
            if (s->kind != SymKind::Subprogram || !s->isFunction)
                continue;
            resolveParams(*s);
            if (!std::ranges::all_of(s->params, [](const ParamInfo& p) { return p.defaultExpr; }) || !s->type ||
                !s->type->isArray())
                continue;
            std::vector<SVal> args;
            std::vector<std::vector<Target>> outs;
            static const std::vector<Assoc> none;
            if (!evalArgs(*s, none, ctx, e.loc, args, outs))
                continue;
            SVal arr = callSubprogram(*s, args, outs, s->type, ctx, e.loc);
            Expr fake(ExprKind::Call, e.loc);
            (void)fake;
            std::vector<SVal> idx;
            for (const auto& a : e.args)
                idx.push_back(eval(*a.actual, ctx, arr.type->indexTypes.front()));
            return indexValue(arr, idx, e.loc);
        }
        chooseSubprogram(syms, e.args, expected, true, ctx, e.loc, /*quiet=*/false); // report
        return SVal{expected, std::nullopt, {}};
    }
    // indexing / slicing of a value
    SVal arr = eval(pre, ctx, nullptr);
    if (!arr.valid())
        return SVal{};
    if (!arr.type->isArray()) {
        error("VHDL-INDEX", "'" + arr.type->name + "' is not an array and cannot be indexed", e.loc);
        return SVal{};
    }
    if (e.args.size() == 1 && isRangeChoice(*e.args[0].actual)) {
        auto r = discreteRange(*e.args[0].actual, ctx);
        if (!r)
            return SVal{};
        return sliceValue(arr, *r, e.loc);
    }
    std::vector<SVal> idx;
    for (std::size_t k = 0; k < e.args.size(); ++k) {
        const Type* it = k < arr.type->indexTypes.size() ? arr.type->indexTypes[k] : std_.integer;
        idx.push_back(convert(eval(*e.args[k].actual, ctx, it), it, ctx, e.loc));
    }
    return indexValue(arr, idx, e.loc);
}

SVal Elaborator::evalOperator(const Expr& e, Ctx& ctx, const Type* expected) {
    const Op op = e.op;
    const bool binary = e.kind == ExprKind::Binary;
    std::vector<TypeSet> sets;
    sets.push_back(typesOf(*e.lhs, ctx));
    if (binary)
        sets.push_back(typesOf(*e.rhs, ctx));
    std::vector<Candidate> cands = predefinedCandidates(op, sets, expected);
    const bool reduction = op == Op::RedAnd || op == Op::RedOr || op == Op::RedNand || op == Op::RedNor ||
                           op == Op::RedXor || op == Op::RedXnor;
    std::string key = std::string("\"") + opSymbol(op) + "\"";
    (void)reduction;
    for (Symbol* f : lookup(*ctx.scope, key)) {
        if (f->kind != SymKind::Subprogram || !f->isFunction || f->implicitOp)
            continue;
        resolveParams(*f);
        Candidate c;
        c.sym = f;
        for (const auto& p : f->params)
            c.params.push_back(p.type);
        c.result = f->type;
        cands.push_back(std::move(c));
    }
    Candidate chosen;
    if (!resolveOverload(cands, sets, expected, std::string("operator \"") + opSymbol(op) + "\"", e.loc, chosen)) {
        auto w = expected ? bitWidth(*expected) : std::nullopt;
        return SVal{expected, std::nullopt, w ? ir::SigSpec::allX(*w) : ir::SigSpec{}};
    }
    std::vector<SVal> args;
    args.push_back(convert(eval(*e.lhs, ctx, chosen.params[0]), chosen.params[0], ctx, e.loc));
    // short-circuit boolean and/or (also avoids evaluating guarded expressions)
    if (binary && !chosen.sym && args[0].known() && args[0].type && args[0].type->baseTag() == TypeTag::Boolean) {
        const bool lv = args[0].value->i != 0;
        if ((op == Op::And && !lv) || (op == Op::Or && lv))
            return makeConst(std_.boolean, Value::integer(lv ? 1 : 0));
        if ((op == Op::Nand && !lv) || (op == Op::Nor && lv))
            return makeConst(std_.boolean, Value::integer(lv ? 0 : 1));
    }
    if (binary)
        args.push_back(convert(eval(*e.rhs, ctx, chosen.params[1]), chosen.params[1], ctx, e.loc));
    if (std::ranges::any_of(args, [](const SVal& v) { return !v.valid(); })) {
        auto w = chosen.result ? bitWidth(*chosen.result) : std::nullopt;
        return SVal{chosen.result, std::nullopt, w ? ir::SigSpec::allX(*w) : ir::SigSpec{}};
    }
    if (chosen.sym) {
        std::vector<std::vector<Target>> outs(args.size());
        return callSubprogram(*chosen.sym, args, outs, chosen.result, ctx, e.loc);
    }
    return applyPredefined(op, args, chosen.result, ctx, e.loc);
}

// ---------------------------------------------------------------------------
// eval
// ---------------------------------------------------------------------------

SVal Elaborator::eval(const Expr& e, Ctx& ctx, const Type* expected) {
    switch (e.kind) {
        case ExprKind::Integer:
            if (e.overflow)
                error("VHDL-RANGE", "integer literal " + e.text + " does not fit in 64 bits", e.loc);
            if (expected && expected->isRealLike())
                return makeConst(expected, Value::real(static_cast<double>(e.ival)));
            return makeConst(std_.univInt, Value::integer(e.ival));
        case ExprKind::Real: return makeConst(std_.univReal, Value::real(e.rval));
        case ExprKind::Physical: {
            for (Symbol* u : lookup(*ctx.scope, e.ident.key)) {
                if (u->kind != SymKind::Unit)
                    continue;
                double n = 1;
                if (e.lhs)
                    n = e.lhs->kind == ExprKind::Real ? e.lhs->rval : static_cast<double>(e.lhs->ival);
                return makeConst(u->type, Value::integer(static_cast<std::int64_t>(std::llround(n * static_cast<double>(u->pos)))));
            }
            error("VHDL-UNDECLARED", "unit '" + e.ident.text + "' is not declared", e.loc);
            return SVal{};
        }
        case ExprKind::Character: {
            auto syms = lookup(*ctx.scope, e.ident.key);
            Symbol* pick = nullptr;
            for (Symbol* s : syms) {
                if (s->kind != SymKind::EnumLiteral)
                    continue;
                if (expected && &s->type->baseType() == &expected->baseType()) {
                    pick = s;
                    break;
                }
                if (!pick)
                    pick = s;
            }
            if (!pick) {
                error("VHDL-UNDECLARED", "character literal " + e.ident.text + " is not visible", e.loc);
                return SVal{};
            }
            return makeConst(expected && &pick->type->baseType() == &expected->baseType() ? expected : pick->type,
                             Value::integer(pick->pos));
        }
        case ExprKind::String:
        case ExprKind::BitString: {
            const Type* t = expected;
            if (!t || !t->isArray())
                t = e.kind == ExprKind::BitString ? std_.bitVector : std_.string;
            SVal v = evalStringLiteral(e.text, t, e.loc);
            if (e.kind == ExprKind::BitString && e.bitWidth == 0 && !v.valid() && t != std_.bitVector)
                v = evalStringLiteral(e.text, std_.bitVector, e.loc);
            return v;
        }
        case ExprKind::Null:
            return SVal{expected, std::nullopt, {}};
        case ExprKind::Aggregate: return evalAggregate(e, ctx, expected);
        case ExprKind::Qualified: {
            const Type* T = typeMark(*e.prefix, ctx);
            if (!T)
                return SVal{};
            SVal v = eval(*e.lhs, ctx, T);
            return convert(v, T, ctx, e.loc);
        }
        case ExprKind::Unary:
        case ExprKind::Binary: return evalOperator(e, ctx, expected);
        case ExprKind::Call: return evalCall(e, ctx, expected);
        case ExprKind::Attribute: return evalAttribute(e, ctx, expected);
        case ExprKind::Simple:
        case ExprKind::Selected: {
            if (e.kind == ExprKind::Selected) {
                auto pp = e.prefix->kind == ExprKind::Simple || e.prefix->kind == ExprKind::Selected
                              ? resolveName(*e.prefix, ctx, true)
                              : std::vector<Symbol*>{};
                if (pp.empty() || (pp.front()->kind != SymKind::Library && pp.front()->kind != SymKind::Package)) {
                    SVal rec = eval(*e.prefix, ctx, nullptr);
                    if (!rec.valid())
                        return SVal{};
                    if (rec.type->kind != TypeKind::Record) {
                        error("VHDL-SELECT", "'" + rec.type->name + "' is not a record (selecting '" + e.ident.text + "')",
                              e.loc);
                        return SVal{};
                    }
                    auto it = std::ranges::find(rec.type->fields, e.ident.key, &FieldInfo::key);
                    if (it == rec.type->fields.end()) {
                        error("VHDL-SELECT", "record '" + rec.type->name + "' has no field '" + e.ident.text + "'", e.loc);
                        return SVal{};
                    }
                    return fieldValue(rec, static_cast<std::size_t>(it - rec.type->fields.begin()), e.loc);
                }
            }
            auto syms = resolveName(e, ctx);
            if (syms.empty())
                return SVal{};
            Symbol* s = syms.front();
            switch (s->kind) {
                case SymKind::Constant:
                    if (s->deferred && !s->value.valid())
                        for (auto& [key, pkg] : packages)
                            if (pkg->scope.local(s->key))
                                elaboratePackageBody(*pkg);
                    if (!s->value.valid()) {
                        error("VHDL-NO-VALUE", "constant '" + s->display + "' has no value", e.loc);
                        return SVal{s->type, std::nullopt, {}};
                    }
                    if (s->type && s->value.type != s->type && s->type->isFullyConstrained()) {
                        SVal v = s->value;
                        v.type = s->type;
                        return v;
                    }
                    return s->value;
                case SymKind::Signal:
                case SymKind::Variable:
                    if (s->object < 0) {
                        error("VHDL-NO-HARDWARE", "'" + s->display + "' (declared in a package) is not supported here",
                              e.loc);
                        return SVal{s->type, std::nullopt, {}};
                    }
                    return readObject(s->object, ctx, e.loc);
                case SymKind::Alias: {
                    Ctx c2 = ctx;
                    c2.scope = s->aliasScope;
                    SVal v = eval(*s->aliasTarget, c2, s->type);
                    if (s->type && s->type->isFullyConstrained() && v.valid() && v.type != s->type)
                        v = convert(v, s->type, ctx, e.loc, false);
                    return v;
                }
                case SymKind::EnumLiteral: {
                    Symbol* pick = s;
                    for (Symbol* c : syms)
                        if (c->kind == SymKind::EnumLiteral && expected && &c->type->baseType() == &expected->baseType())
                            pick = c;
                    return makeConst(pick->type, Value::integer(pick->pos));
                }
                case SymKind::Subprogram: {
                    static const std::vector<Assoc> none;
                    Symbol* f = chooseSubprogram(syms, none, expected, true, ctx, e.loc);
                    if (!f)
                        return SVal{};
                    std::vector<SVal> args;
                    std::vector<std::vector<Target>> outs;
                    if (!evalArgs(*f, none, ctx, e.loc, args, outs))
                        return SVal{};
                    return callSubprogram(*f, args, outs, f->type, ctx, e.loc);
                }
                case SymKind::Unit: return makeConst(s->type, Value::integer(s->pos));
                default:
                    error("VHDL-NOT-VALUE", "'" + s->display + "' is not a value", e.loc);
                    return SVal{};
            }
        }
        case ExprKind::External:
            unsupported("IR-UNSUPPORTED-EXTERNAL-NAME", "VHDL-2008 external name", e.loc);
            return opaque(e.subtype ? subtype(*e.subtype, ctx) : expected, {}, "external name", e.loc);
        case ExprKind::Inertial: return eval(*e.lhs, ctx, expected);
        default:
            error("VHDL-NOT-VALUE", "expected an expression", e.loc);
            return SVal{};
    }
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

bool Elaborator::targets(const Expr& e, Ctx& ctx, std::vector<Target>& out, const Type** typeOut) {
    auto setType = [&](const Type* t) {
        if (typeOut)
            *typeOut = t;
    };
    switch (e.kind) {
        case ExprKind::Simple: {
            auto syms = resolveName(e, ctx);
            if (syms.empty())
                return false;
            Symbol* s = syms.front();
            if (s->kind == SymKind::Alias) {
                Ctx c2 = ctx;
                c2.scope = s->aliasScope;
                return targets(*s->aliasTarget, c2, out, typeOut);
            }
            if ((s->kind != SymKind::Signal && s->kind != SymKind::Variable) || s->object < 0) {
                error("VHDL-TARGET", "'" + s->display + "' cannot be assigned", e.loc);
                return false;
            }
            Target t;
            t.object = s->object;
            t.type = object(s->object).type;
            t.width = bitWidth(*t.type).value_or(0);
            out.push_back(t);
            setType(t.type);
            return true;
        }
        case ExprKind::Selected: {
            auto pp = e.prefix->kind == ExprKind::Simple || e.prefix->kind == ExprKind::Selected
                          ? resolveName(*e.prefix, ctx, true)
                          : std::vector<Symbol*>{};
            if (!pp.empty() && (pp.front()->kind == SymKind::Library || pp.front()->kind == SymKind::Package)) {
                auto syms = resolveName(e, ctx);
                if (syms.empty())
                    return false;
                error("VHDL-TARGET", "'" + syms.front()->display + "' (declared in a package) cannot be assigned here",
                      e.loc);
                return false;
            }
            std::vector<Target> base;
            const Type* bt = nullptr;
            if (!targets(*e.prefix, ctx, base, &bt) || base.size() != 1)
                return false;
            Target t = base.front();
            if (!t.type || t.type->kind != TypeKind::Record) {
                error("VHDL-SELECT", "selecting '" + e.ident.text + "' from a non-record target", e.loc);
                return false;
            }
            if (t.dynamic) {
                unsupported("IR-UNSUPPORTED-TARGET", "record field of a dynamically indexed element as target", e.loc);
                return false;
            }
            auto it = std::ranges::find(t.type->fields, e.ident.key, &FieldInfo::key);
            if (it == t.type->fields.end()) {
                error("VHDL-SELECT", "record '" + t.type->name + "' has no field '" + e.ident.text + "'", e.loc);
                return false;
            }
            std::uint32_t off = 0;
            for (auto f = it + 1; f != t.type->fields.end(); ++f)
                off += bitWidth(*f->type).value_or(0);
            t.offset += off;
            t.type = it->type;
            t.width = bitWidth(*t.type).value_or(0);
            out.push_back(t);
            setType(t.type);
            return true;
        }
        case ExprKind::Call: {
            std::vector<Target> base;
            if (!targets(*e.prefix, ctx, base) || base.size() != 1)
                return false;
            Target t = base.front();
            const Type* T = t.type;
            if (!T || !T->isArray()) {
                error("VHDL-INDEX", "indexing a non-array target", e.loc);
                return false;
            }
            if (t.dynamic) {
                unsupported("IR-UNSUPPORTED-TARGET", "nested indexing below a variable index as assignment target", e.loc);
                return false;
            }
            const auto ew = bitWidth(*T->element).value_or(0);
            if (e.args.size() == 1 && isRangeChoice(*e.args[0].actual)) {
                auto r = discreteRange(*e.args[0].actual, ctx);
                if (!r || !T->indexRanges[0])
                    return false;
                const IntRange& ar = *T->indexRanges[0];
                if (r->isNull())
                    return true;
                if (!ar.contains(r->left) || !ar.contains(r->right) || r->ascending != ar.ascending) {
                    error("VHDL-SLICE", "slice target outside the range of '" + T->name + "'", e.loc);
                    return false;
                }
                const auto p1 = ar.positionOf(r->right);
                const auto p0 = ar.positionOf(r->left);
                t.offset += static_cast<std::uint32_t>((ar.length() - 1 - p1) * ew);
                t.width = static_cast<std::uint32_t>((p1 - p0 + 1) * ew);
                t.type = types.constrainedArray(T, {*r});
                out.push_back(t);
                setType(t.type);
                return true;
            }
            if (e.args.size() != T->indexTypes.size()) {
                error("VHDL-INDEX", "wrong number of indexes for '" + T->name + "'", e.loc);
                return false;
            }
            std::vector<SVal> idx;
            for (std::size_t k = 0; k < e.args.size(); ++k)
                idx.push_back(convert(eval(*e.args[k].actual, ctx, T->indexTypes[k]), T->indexTypes[k], ctx, e.loc));
            if (std::ranges::all_of(idx, [](const SVal& v) { return v.known(); })) {
                std::uint64_t flat = 0;
                for (std::size_t d = 0; d < idx.size(); ++d) {
                    const IntRange& r = *T->indexRanges[d];
                    if (!r.contains(idx[d].value->i)) {
                        error("VHDL-INDEX", "index " + std::to_string(idx[d].value->i) + " outside the range of '" +
                                                T->name + "'",
                              e.loc);
                        return false;
                    }
                    flat = flat * r.length() + r.positionOf(idx[d].value->i);
                }
                const auto n = std::accumulate(T->indexRanges.begin(), T->indexRanges.end(), std::uint64_t{1},
                                               [](std::uint64_t a, const auto& r) { return a * r->length(); });
                t.offset += static_cast<std::uint32_t>((n - 1 - flat) * ew);
                t.width = ew;
                t.type = T->element;
                out.push_back(t);
                setType(t.type);
                return true;
            }
            if (idx.size() != 1) {
                unsupported("IR-UNSUPPORTED-TARGET", "multi-dimensional target with a variable index", e.loc);
                return false;
            }
            Target d = t;
            d.dynamic = true;
            d.index = bitsOf(idx[0], e.loc);
            d.indexSigned = isSignedInteger(*idx[0].type);
            d.elemWidth = ew;
            d.regionOffset = t.offset;
            d.regionWidth = t.width;
            d.range = *T->indexRanges[0];
            d.type = T->element;
            d.width = ew;
            out.push_back(d);
            setType(d.type);
            return true;
        }
        case ExprKind::Aggregate: {
            // (a, b, c) <= value : each element is a target, left to right
            for (const auto& el : e.elements) {
                if (!el.choices.empty()) {
                    unsupported("IR-UNSUPPORTED-TARGET", "named aggregate as assignment target", e.loc);
                    return false;
                }
                if (!targets(*el.value, ctx, out))
                    return false;
            }
            setType(nullptr);
            return true;
        }
        default: break;
    }
    if (e.parenthesized)
        return false;
    error("VHDL-TARGET", "this expression cannot be assigned", e.loc);
    return false;
}

} // namespace opencdc::vhdl

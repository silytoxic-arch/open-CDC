#include "frontend/vhdl/VhdlTypes.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace opencdc::vhdl {

bool Type::isFullyConstrained() const noexcept {
    switch (kind) {
        case TypeKind::Array:
            if (std::ranges::any_of(indexRanges, [](const auto& r) { return !r.has_value(); }))
                return false;
            return element && element->isFullyConstrained();
        case TypeKind::Record:
            return std::ranges::all_of(fields, [](const FieldInfo& f) { return f.type && f.type->isFullyConstrained(); });
        default: return true;
    }
}

bool Type::isStringLike() const noexcept {
    if (kind != TypeKind::Array || indexTypes.size() != 1 || !element)
        return false;
    const auto& e = element->baseType();
    if (e.kind != TypeKind::Enum || !e.enumInfo)
        return false;
    return std::ranges::any_of(e.enumInfo->keys, [](const std::string& k) { return !k.empty() && k[0] == '\''; });
}

bool Type::isLogicScalar() const noexcept {
    const auto t = baseTag();
    return t == TypeTag::StdUlogic || t == TypeTag::Bit || t == TypeTag::Boolean;
}

bool Type::isLogicVector() const noexcept {
    if (kind != TypeKind::Array || indexTypes.size() != 1 || !element)
        return false;
    const auto t = element->baseTag();
    return t == TypeTag::StdUlogic || t == TypeTag::Bit;
}

Type* TypeArena::make(Type t) {
    types_.push_back(std::move(t));
    Type* p = &types_.back();
    if (!p->base)
        p->base = p;
    if (p->base == p && p->kind == TypeKind::Array && p->indexTypes.size() == 1)
        arrays_.push_back(p);
    return p;
}

const Type* TypeArena::rangeSubtype(const Type* base, IntRange r, std::string name) {
    Type t = *base;
    t.base = &base->baseType();
    t.isSubtype = true;
    t.constrained = true;
    t.range = r;
    t.name = name.empty() ? base->name : std::move(name);
    return make(std::move(t));
}

const Type* TypeArena::constrainedArray(const Type* a, std::vector<IntRange> ranges) {
    Type t = *a;
    t.base = &a->baseType();
    t.isSubtype = true;
    t.indexRanges.clear();
    for (auto& r : ranges)
        t.indexRanges.emplace_back(r);
    return make(std::move(t));
}

const Type* TypeArena::anonymousArray(const Type* element, const Type* indexType, IntRange range) {
    Type t;
    t.kind = TypeKind::Array;
    t.name = "array of " + element->name;
    t.indexTypes = {indexType};
    t.indexRanges = {range};
    t.element = element;
    return make(std::move(t));
}

std::uint32_t integerBits(std::int64_t low, std::int64_t high) {
    auto bitsFor = [](std::uint64_t v) {
        std::uint32_t n = 0;
        while (v) {
            ++n;
            v >>= 1U;
        }
        return n;
    };
    if (low >= 0)
        return std::max<std::uint32_t>(1, bitsFor(static_cast<std::uint64_t>(high)));
    const std::uint64_t negMag = static_cast<std::uint64_t>(-(low + 1));
    const std::uint64_t posMag = high > 0 ? static_cast<std::uint64_t>(high) : 0;
    return std::max(bitsFor(negMag), bitsFor(posMag)) + 1;
}

bool isSignedInteger(const Type& t) {
    if (t.kind == TypeKind::UniversalInteger)
        return true;
    if (t.kind != TypeKind::Integer)
        return false;
    return t.range.low() < 0;
}

std::optional<std::uint32_t> bitWidth(const Type& t) {
    switch (t.kind) {
        case TypeKind::Enum: {
            if (t.isLogicScalar())
                return 1;
            const auto n = t.baseType().enumInfo ? t.baseType().enumInfo->keys.size() : 1;
            return integerBits(0, static_cast<std::int64_t>(n > 0 ? n - 1 : 0));
        }
        case TypeKind::Integer:
            if (!t.constrained)
                return 32;
            if (t.range.isNull())
                return 1;
            return integerBits(t.range.low(), t.range.high());
        case TypeKind::UniversalInteger: return 32;
        case TypeKind::Physical: return 64;
        case TypeKind::Array: {
            if (!t.element)
                return std::nullopt;
            auto ew = bitWidth(*t.element);
            if (!ew)
                return std::nullopt;
            std::uint64_t n = *ew;
            for (const auto& r : t.indexRanges) {
                if (!r)
                    return std::nullopt;
                n *= r->length();
            }
            if (n > 0xFFFFFFFFULL)
                return std::nullopt;
            return static_cast<std::uint32_t>(n);
        }
        case TypeKind::Record: {
            std::uint64_t n = 0;
            for (const auto& f : t.fields) {
                auto w = f.type ? bitWidth(*f.type) : std::nullopt;
                if (!w)
                    return std::nullopt;
                n += *w;
            }
            return static_cast<std::uint32_t>(n);
        }
        default: return std::nullopt;
    }
}

namespace {

ir::Logic logicOf(std::int64_t pos, TypeTag tag) {
    if (tag != TypeTag::StdUlogic)
        return pos != 0 ? ir::Logic::L1 : ir::Logic::L0;
    switch (pos) {
        case SL_0:
        case SL_L: return ir::Logic::L0;
        case SL_1:
        case SL_H: return ir::Logic::L1;
        case SL_Z: return ir::Logic::Z;
        default: return ir::Logic::X;
    }
}

/// Element type and element count of a (fully constrained, possibly
/// multi-dimensional) array.
std::uint64_t elementCount(const Type& t) {
    std::uint64_t n = 1;
    for (const auto& r : t.indexRanges)
        n *= r ? r->length() : 0;
    return n;
}

bool encodeInto(const Type& t, const Value& v, std::vector<ir::Logic>& msbFirst) {
    switch (t.kind) {
        case TypeKind::Enum: {
            if (v.kind != Value::Kind::Int)
                return false;
            const auto w = *bitWidth(t);
            if (w == 1 && t.isLogicScalar()) {
                msbFirst.push_back(logicOf(v.i, t.baseTag()));
                return true;
            }
            for (std::uint32_t k = w; k-- > 0;)
                msbFirst.push_back(((static_cast<std::uint64_t>(v.i) >> k) & 1U) ? ir::Logic::L1 : ir::Logic::L0);
            return true;
        }
        case TypeKind::Integer:
        case TypeKind::UniversalInteger:
        case TypeKind::Physical: {
            if (v.kind != Value::Kind::Int)
                return false;
            const auto w = *bitWidth(t);
            for (std::uint32_t k = w; k-- > 0;) {
                const bool b = k < 64 ? ((static_cast<std::uint64_t>(v.i) >> k) & 1U) != 0 : v.i < 0;
                msbFirst.push_back(b ? ir::Logic::L1 : ir::Logic::L0);
            }
            return true;
        }
        case TypeKind::Array: {
            if (v.kind != Value::Kind::Composite || !t.element)
                return false;
            if (v.elems.size() != elementCount(t))
                return false;
            for (const auto& e : v.elems)
                if (!encodeInto(*t.element, e, msbFirst))
                    return false;
            return true;
        }
        case TypeKind::Record: {
            if (v.kind != Value::Kind::Composite || v.elems.size() != t.fields.size())
                return false;
            for (std::size_t k = 0; k < t.fields.size(); ++k)
                if (!encodeInto(*t.fields[k].type, v.elems[k], msbFirst))
                    return false;
            return true;
        }
        default: return false;
    }
}

bool decodeFrom(const Type& t, const std::vector<ir::Logic>& msbFirst, std::size_t& pos, Value& out) {
    auto take = [&](std::uint32_t w, std::uint64_t& bits, bool& defined) {
        bits = 0;
        defined = true;
        for (std::uint32_t k = 0; k < w; ++k) {
            if (pos >= msbFirst.size())
                return false;
            const auto l = msbFirst[pos++];
            if (l != ir::Logic::L0 && l != ir::Logic::L1)
                defined = false;
            if (k < 64)
                bits = (bits << 1U) | (l == ir::Logic::L1 ? 1U : 0U);
        }
        return true;
    };
    switch (t.kind) {
        case TypeKind::Enum: {
            const auto w = *bitWidth(t);
            if (w == 1 && t.baseTag() == TypeTag::StdUlogic) {
                if (pos >= msbFirst.size())
                    return false;
                const auto l = msbFirst[pos++];
                out = Value::integer(l == ir::Logic::L0   ? SL_0
                                     : l == ir::Logic::L1 ? SL_1
                                     : l == ir::Logic::Z  ? SL_Z
                                                          : SL_X);
                return true;
            }
            std::uint64_t bits = 0;
            bool defined = true;
            if (!take(w, bits, defined) || !defined)
                return false;
            out = Value::integer(static_cast<std::int64_t>(bits));
            return true;
        }
        case TypeKind::Integer:
        case TypeKind::UniversalInteger:
        case TypeKind::Physical: {
            const auto w = *bitWidth(t);
            std::uint64_t bits = 0;
            bool defined = true;
            if (!take(w, bits, defined) || !defined || w > 64)
                return false;
            std::int64_t v = static_cast<std::int64_t>(bits);
            if (isSignedInteger(t) && w < 64 && ((bits >> (w - 1)) & 1U))
                v = static_cast<std::int64_t>(bits | (~0ULL << w));
            out = Value::integer(v);
            return true;
        }
        case TypeKind::Array: {
            const auto n = elementCount(t);
            std::vector<Value> elems;
            elems.reserve(n);
            for (std::uint64_t k = 0; k < n; ++k) {
                Value e;
                if (!decodeFrom(*t.element, msbFirst, pos, e))
                    return false;
                elems.push_back(std::move(e));
            }
            out = Value::composite(std::move(elems));
            return true;
        }
        case TypeKind::Record: {
            std::vector<Value> elems;
            for (const auto& f : t.fields) {
                Value e;
                if (!decodeFrom(*f.type, msbFirst, pos, e))
                    return false;
                elems.push_back(std::move(e));
            }
            out = Value::composite(std::move(elems));
            return true;
        }
        default: return false;
    }
}

} // namespace

std::optional<ir::ConstBits> encode(const Type& t, const Value& v) {
    if (!bitWidth(t))
        return std::nullopt;
    std::vector<ir::Logic> msbFirst;
    if (!encodeInto(t, v, msbFirst))
        return std::nullopt;
    std::reverse(msbFirst.begin(), msbFirst.end());
    return ir::ConstBits(std::move(msbFirst));
}

std::optional<Value> decode(const Type& t, const ir::ConstBits& bits) {
    const auto w = bitWidth(t);
    if (!w || *w != bits.width())
        return std::nullopt;
    std::vector<ir::Logic> msbFirst(bits.bits().rbegin(), bits.bits().rend());
    std::size_t pos = 0;
    Value out;
    if (!decodeFrom(t, msbFirst, pos, out))
        return std::nullopt;
    return out;
}

std::string formatValue(const Type& t, const Value& v) {
    switch (v.kind) {
        case Value::Kind::None: return "<none>";
        case Value::Kind::Int:
            if (t.kind == TypeKind::Enum && t.baseType().enumInfo) {
                const auto& d = t.baseType().enumInfo->display;
                if (v.i >= 0 && static_cast<std::size_t>(v.i) < d.size())
                    return d[static_cast<std::size_t>(v.i)];
            }
            return std::to_string(v.i);
        case Value::Kind::Real: {
            std::ostringstream os;
            os << v.r;
            return os.str();
        }
        case Value::Kind::Composite: {
            if (t.kind == TypeKind::Array && t.isStringLike()) {
                std::string s = "\"";
                for (const auto& e : v.elems) {
                    std::string lit = formatValue(*t.element, e);
                    s += lit.size() == 3 && lit.front() == '\'' ? lit.substr(1, 1) : lit;
                }
                return s + "\"";
            }
            std::string s = "(";
            for (std::size_t k = 0; k < v.elems.size(); ++k) {
                if (k)
                    s += ", ";
                const Type* et = t.kind == TypeKind::Array    ? t.element
                                 : k < t.fields.size()        ? t.fields[k].type
                                                              : nullptr;
                s += et ? formatValue(*et, v.elems[k]) : "?";
            }
            return s + ")";
        }
    }
    return "?";
}

} // namespace opencdc::vhdl

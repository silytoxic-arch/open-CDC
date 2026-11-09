#include "ir/ConstFold.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace opencdc::ir {

namespace {

std::uint64_t mask(std::uint32_t width) {
    return width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
}

std::int64_t signExtend(std::uint64_t v, std::uint32_t width) {
    if (width == 0 || width >= 64)
        return static_cast<std::int64_t>(v);
    if ((v >> (width - 1)) & 1U)
        v |= ~mask(width);
    return static_cast<std::int64_t>(v);
}

std::optional<std::uint64_t> constUint(const SigSpec& s) {
    if (s.width() == 0 || s.width() > 64)
        return std::nullopt;
    auto c = s.asConst();
    if (!c)
        return std::nullopt;
    return c->toUint();
}

SigSpec bool1(bool v) {
    return SigSpec::constant(v ? 1 : 0, 1);
}

std::optional<SigSpec> foldUnary(const Cell& c, std::uint64_t a, std::uint32_t aw) {
    const std::uint32_t w = c.width;
    switch (c.kind) {
        case CellKind::Not: return SigSpec::constant(~a & mask(w), w);
        case CellKind::Neg: return SigSpec::constant((~a + 1) & mask(w), w);
        case CellKind::ReduceAnd: return bool1((a & mask(aw)) == mask(aw)).resized(w, false);
        case CellKind::ReduceOr: return bool1(a != 0).resized(w, false);
        case CellKind::ReduceXor: return bool1((std::popcount(a) & 1) != 0).resized(w, false);
        case CellKind::LogicNot: return bool1(a == 0).resized(w, false);
        default: return std::nullopt;
    }
}

std::optional<SigSpec> foldBinary(const Cell& c, std::uint64_t a, std::uint64_t b, std::uint32_t aw,
                                  std::uint32_t bw) {
    const std::uint32_t w = c.width;
    const auto m = mask(w);
    const std::int64_t sa = signExtend(a, aw);
    const std::int64_t sb = signExtend(b, bw);
    // Unsigned comparisons must not go through int64 for values >= 2^63.
    auto ucmp = [&](auto pred) -> SigSpec {
        const bool r = c.isSigned ? pred(sa, sb) : pred(a, b);
        return bool1(r).resized(w, false);
    };
    switch (c.kind) {
        case CellKind::And: return SigSpec::constant(a & b & m, w);
        case CellKind::Or: return SigSpec::constant((a | b) & m, w);
        case CellKind::Xor: return SigSpec::constant((a ^ b) & m, w);
        case CellKind::Xnor: return SigSpec::constant(~(a ^ b) & m, w);
        case CellKind::Add: return SigSpec::constant((a + b) & m, w);
        case CellKind::Sub: return SigSpec::constant((a - b) & m, w);
        case CellKind::Mul: return SigSpec::constant((a * b) & m, w);
        case CellKind::Div:
            if (b == 0)
                return SigSpec::allX(w);
            return SigSpec::constant((c.isSigned ? static_cast<std::uint64_t>(sa / sb) : a / b) & m, w);
        case CellKind::Mod:
            if (b == 0)
                return SigSpec::allX(w);
            return SigSpec::constant((c.isSigned ? static_cast<std::uint64_t>(sa % sb) : a % b) & m, w);
        case CellKind::Pow: {
            std::uint64_t r = 1;
            for (std::uint64_t i = 0; i < b && i < 128; ++i)
                r *= a;
            return SigSpec::constant(r & m, w);
        }
        case CellKind::Shl: return SigSpec::constant(b >= 64 ? 0 : (a << b) & m, w);
        case CellKind::Shr: return SigSpec::constant(b >= 64 ? 0 : (a >> b) & m, w);
        case CellKind::Sshr: {
            const std::int64_t v = c.isSigned ? sa : static_cast<std::int64_t>(a);
            if (!c.isSigned)
                return SigSpec::constant(b >= 64 ? 0 : (a >> b) & m, w);
            return SigSpec::constant(static_cast<std::uint64_t>(b >= 63 ? (v < 0 ? -1 : 0) : (v >> b)) & m, w);
        }
        case CellKind::Eq: return bool1(a == b).resized(w, false);
        case CellKind::Ne: return bool1(a != b).resized(w, false);
        case CellKind::Lt: return ucmp([](auto x, auto y) { return x < y; });
        case CellKind::Le: return ucmp([](auto x, auto y) { return x <= y; });
        case CellKind::Gt: return ucmp([](auto x, auto y) { return x > y; });
        case CellKind::Ge: return ucmp([](auto x, auto y) { return x >= y; });
        case CellKind::LogicAnd: return bool1(a != 0 && b != 0).resized(w, false);
        case CellKind::LogicOr: return bool1(a != 0 || b != 0).resized(w, false);
        default: return std::nullopt;
    }
}

// Values wider than 64 bits (string parameters, wide masks): bit vectors, LSB
// first, for the operators whose result needs no multiplication or division.
using Bits = std::vector<bool>;

std::optional<Bits> constBits(const SigSpec& s) {
    auto c = s.asConst();
    if (!c || c->width() == 0 || !c->isFullyDefined())
        return std::nullopt;
    Bits out(c->width());
    for (std::uint32_t i = 0; i < c->width(); ++i)
        out[i] = (*c)[i] == Logic::L1;
    return out;
}

Bits extended(const Bits& v, std::size_t n, bool sign) {
    Bits out(n, sign && !v.empty() && v.back());
    for (std::size_t i = 0; i < std::min(n, v.size()); ++i)
        out[i] = v[i];
    return out;
}

SigSpec toSig(const Bits& v, std::uint32_t w) {
    std::vector<Logic> bits(w, Logic::L0);
    for (std::uint32_t i = 0; i < w && i < v.size(); ++i)
        bits[i] = v[i] ? Logic::L1 : Logic::L0;
    return SigSpec::constant(ConstBits(std::move(bits)));
}

bool anySet(const Bits& v) {
    return std::ranges::any_of(v, [](bool b) { return b; });
}

Bits addBits(const Bits& a, const Bits& b, bool carry) {
    Bits out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int sum = int{a[i]} + int{b[i]} + int{carry};
        out[i] = (sum & 1) != 0;
        carry = sum >= 2;
    }
    return out;
}

// -1, 0, 1: a compared with b (same width)
int compareBits(const Bits& a, const Bits& b, bool isSigned) {
    const std::size_t n = a.size();
    if (isSigned && n > 0 && a[n - 1] != b[n - 1])
        return a[n - 1] ? -1 : 1;
    for (std::size_t i = n; i-- > 0;)
        if (a[i] != b[i])
            return a[i] ? 1 : -1;
    return 0;
}

std::optional<SigSpec> foldWide(const Cell& c) {
    const std::uint32_t w = c.width;
    const auto n = inputCount(c.kind);
    if (n == 1) {
        const auto a = constBits(c.inputs.at(0));
        if (!a)
            return std::nullopt;
        switch (c.kind) {
            case CellKind::Not: {
                Bits r = extended(*a, w, false);
                r.flip();
                return toSig(r, w);
            }
            case CellKind::Neg: {
                Bits r = extended(*a, w, false);
                r.flip();
                return toSig(addBits(r, Bits(w, false), true), w);
            }
            case CellKind::ReduceAnd: return bool1(std::ranges::all_of(*a, [](bool b) { return b; })).resized(w, false);
            case CellKind::ReduceOr: return bool1(anySet(*a)).resized(w, false);
            case CellKind::ReduceXor:
                return bool1(std::ranges::count(*a, true) % 2 == 1).resized(w, false);
            case CellKind::LogicNot: return bool1(!anySet(*a)).resized(w, false);
            default: return std::nullopt;
        }
    }
    if (n != 2)
        return std::nullopt;
    const auto a = constBits(c.inputs.at(0));
    const auto b = constBits(c.inputs.at(1));
    if (!a || !b)
        return std::nullopt;
    auto bitwise = [&](auto op) {
        const Bits x = extended(*a, w, false);
        const Bits y = extended(*b, w, false);
        Bits r(w);
        for (std::uint32_t i = 0; i < w; ++i)
            r[i] = op(x[i], y[i]);
        return toSig(r, w);
    };
    auto compare = [&](auto pred) {
        const std::size_t m = std::max(a->size(), b->size());
        return bool1(pred(compareBits(extended(*a, m, c.isSigned), extended(*b, m, c.isSigned), c.isSigned)))
            .resized(w, false);
    };
    // shift amount: anything at least the width shifts everything out
    auto amount = [&]() -> std::size_t {
        std::size_t v = 0;
        for (std::size_t i = b->size(); i-- > 0;) {
            if (v > (std::size_t{1} << 40))
                return v;
            v = v * 2 + ((*b)[i] ? 1U : 0U);
        }
        return v;
    };
    switch (c.kind) {
        case CellKind::And: return bitwise([](bool x, bool y) { return x && y; });
        case CellKind::Or: return bitwise([](bool x, bool y) { return x || y; });
        case CellKind::Xor: return bitwise([](bool x, bool y) { return x != y; });
        case CellKind::Xnor: return bitwise([](bool x, bool y) { return x == y; });
        case CellKind::Add: return toSig(addBits(extended(*a, w, false), extended(*b, w, false), false), w);
        case CellKind::Sub: {
            Bits y = extended(*b, w, false);
            y.flip();
            return toSig(addBits(extended(*a, w, false), y, true), w);
        }
        case CellKind::Shl: {
            const std::size_t k = amount();
            const Bits x = extended(*a, w, false);
            Bits r(w, false);
            for (std::size_t i = k; i < w; ++i)
                r[i] = x[i - k];
            return toSig(r, w);
        }
        case CellKind::Shr:
        case CellKind::Sshr: {
            const std::size_t k = amount();
            const bool arith = c.kind == CellKind::Sshr && c.isSigned;
            const std::size_t m = std::max<std::size_t>(a->size(), w);
            const Bits x = extended(*a, m, arith);
            const bool fill = arith && !a->empty() && a->back();
            Bits r(w, fill);
            for (std::size_t i = 0; i < w; ++i)
                if (i + k < m && k < m)
                    r[i] = x[i + k];
            return toSig(r, w);
        }
        case CellKind::Eq: return compare([](int r) { return r == 0; });
        case CellKind::Ne: return compare([](int r) { return r != 0; });
        case CellKind::Lt: return compare([](int r) { return r < 0; });
        case CellKind::Le: return compare([](int r) { return r <= 0; });
        case CellKind::Gt: return compare([](int r) { return r > 0; });
        case CellKind::Ge: return compare([](int r) { return r >= 0; });
        case CellKind::LogicAnd: return bool1(anySet(*a) && anySet(*b)).resized(w, false);
        case CellKind::LogicOr: return bool1(anySet(*a) || anySet(*b)).resized(w, false);
        default: return std::nullopt;
    }
}

} // namespace

std::optional<std::uint32_t> elementOffset(std::int64_t index, std::int32_t left, std::int32_t right) noexcept {
    const std::int64_t lo = std::min(left, right);
    const std::int64_t hi = std::max(left, right);
    if (index < lo || index > hi)
        return std::nullopt;
    return static_cast<std::uint32_t>(left >= right ? index - right : right - index);
}

std::optional<SigSpec> simplifyCell(const Cell& c) {
    switch (c.kind) {
        case CellKind::Mux: {
            const auto& A = c.inputs.at(0);
            const auto& B = c.inputs.at(1);
            if (A == B)
                return A;
            if (auto s = c.inputs.at(2).asConst()) {
                if (s->isFullyDefined())
                    return s->isZero() ? A : B;
            }
            return std::nullopt;
        }
        case CellKind::DynSelect: {
            auto idx = constUint(c.inputs.at(1));
            if (!idx)
                return std::nullopt;
            const std::int64_t index = c.isSigned ? signExtend(*idx, c.inputs[1].width()) : static_cast<std::int64_t>(*idx);
            auto off = elementOffset(index, c.rangeLeft, c.rangeRight);
            const auto& A = c.inputs.at(0);
            if (!off || (*off + 1) * c.elemWidth > A.width() || c.width > A.width() - *off * c.elemWidth)
                return SigSpec::allX(c.width);
            return A.extract(*off * c.elemWidth, c.width);
        }
        case CellKind::DynInsert: {
            auto idx = constUint(c.inputs.at(1));
            if (!idx)
                return std::nullopt;
            const std::int64_t index = c.isSigned ? signExtend(*idx, c.inputs[1].width()) : static_cast<std::int64_t>(*idx);
            auto off = elementOffset(index, c.rangeLeft, c.rangeRight);
            const auto& A = c.inputs.at(0);
            const auto& D = c.inputs.at(2);
            if (!off || *off * c.elemWidth + D.width() > A.width())
                return A; // out-of-range write has no effect
            return A.replaced(*off * c.elemWidth, D);
        }
        case CellKind::Opaque: return std::nullopt;
        default: break;
    }

    const auto n = inputCount(c.kind);
    if ((n == 1 || n == 2) && (c.width > 64 || std::ranges::any_of(c.inputs, [](const SigSpec& in) {
                                   return in.width() > 64;
                               })))
        if (auto r = foldWide(c))
            return r;
    if (n == 1) {
        auto a = constUint(c.inputs.at(0));
        if (!a)
            return std::nullopt;
        return foldUnary(c, *a, c.inputs[0].width());
    }
    if (n == 2) {
        // identities of the bitwise operators: x & 1..1 = x, x & 0 = 0, x | 0 = x, x | 1..1 = 1..1, x ^ 0 = x
        if ((c.kind == CellKind::And || c.kind == CellKind::Or || c.kind == CellKind::Xor) &&
            c.inputs.at(0).width() == c.width && c.inputs.at(1).width() == c.width) {
            for (int k = 0; k < 2; ++k) {
                const auto cb = c.inputs[static_cast<std::size_t>(k)].asConst();
                if (!cb || !cb->isFullyDefined())
                    continue;
                const auto& other = c.inputs[static_cast<std::size_t>(1 - k)];
                const bool zero = cb->isZero();
                const bool ones = std::ranges::all_of(cb->bits(), [](Logic b) { return b == Logic::L1; });
                if (c.kind == CellKind::And && zero)
                    return c.inputs[static_cast<std::size_t>(k)];
                if (c.kind == CellKind::And && ones)
                    return other;
                if (c.kind == CellKind::Or && zero)
                    return other;
                if (c.kind == CellKind::Or && ones)
                    return c.inputs[static_cast<std::size_t>(k)];
                if (c.kind == CellKind::Xor && zero)
                    return other;
            }
        }
        // x && 0 = 0, x || 1 = 1: the result does not depend on x (BrianHG DDR3: busy = busy_e ^
        // (ena && TOGGLE[x]) with TOGGLE[x] = 0 is no combinational path from ena to busy)
        if ((c.kind == CellKind::LogicAnd || c.kind == CellKind::LogicOr))
            for (const auto& in : c.inputs) {
                const auto cv = in.asConst();
                if (!cv || !cv->isFullyDefined())
                    continue;
                if (c.kind == CellKind::LogicAnd && cv->isZero())
                    return bool1(false).resized(c.width, false);
                if (c.kind == CellKind::LogicOr && !cv->isZero())
                    return bool1(true).resized(c.width, false);
            }
        auto a = constUint(c.inputs.at(0));
        auto b = constUint(c.inputs.at(1));
        if (!a || !b)
            return std::nullopt;
        return foldBinary(c, *a, *b, c.inputs[0].width(), c.inputs[1].width());
    }
    return std::nullopt;
}

} // namespace opencdc::ir

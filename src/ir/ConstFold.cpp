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
        auto a = constUint(c.inputs.at(0));
        auto b = constUint(c.inputs.at(1));
        if (!a || !b)
            return std::nullopt;
        return foldBinary(c, *a, *b, c.inputs[0].width(), c.inputs[1].width());
    }
    return std::nullopt;
}

} // namespace opencdc::ir

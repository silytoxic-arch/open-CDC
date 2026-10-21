#include "ir/Const.h"

#include <algorithm>

namespace opencdc::ir {

ConstBits ConstBits::fromUint(std::uint64_t value, std::uint32_t width) {
    std::vector<Logic> bits(width, Logic::L0);
    for (std::uint32_t i = 0; i < width && i < 64; ++i)
        bits[i] = ((value >> i) & 1U) != 0 ? Logic::L1 : Logic::L0;
    return ConstBits(std::move(bits));
}

bool ConstBits::isFullyDefined() const noexcept {
    return std::ranges::all_of(bits_, [](Logic b) { return b == Logic::L0 || b == Logic::L1; });
}

std::optional<std::uint64_t> ConstBits::toUint() const {
    if (!isFullyDefined())
        return std::nullopt;
    std::uint64_t v = 0;
    for (std::uint32_t i = 0; i < width(); ++i) {
        if (bits_[i] == Logic::L1) {
            if (i >= 64)
                return std::nullopt;
            v |= (std::uint64_t{1} << i);
        }
    }
    return v;
}

std::optional<std::int64_t> ConstBits::toInt() const {
    if (width() == 0 || width() > 64)
        return std::nullopt;
    auto u = toUint();
    if (!u)
        return std::nullopt;
    if (width() < 64 && bits_.back() == Logic::L1)
        *u |= ~((std::uint64_t{1} << width()) - 1); // sign extend
    return static_cast<std::int64_t>(*u);
}

bool ConstBits::isZero() const noexcept {
    return std::ranges::all_of(bits_, [](Logic b) { return b == Logic::L0; });
}

ConstBits ConstBits::extract(std::uint32_t offset, std::uint32_t w) const {
    std::vector<Logic> out;
    out.reserve(w);
    for (std::uint32_t i = 0; i < w; ++i)
        out.push_back(offset + i < width() ? bits_[offset + i] : Logic::X);
    return ConstBits(std::move(out));
}

void ConstBits::append(const ConstBits& msbSide) {
    bits_.insert(bits_.end(), msbSide.bits_.begin(), msbSide.bits_.end());
}

std::string ConstBits::toString() const {
    std::string out = std::to_string(width());
    if (width() > 64 && isFullyDefined()) {
        out += "'h";
        static constexpr char kHex[] = "0123456789abcdef";
        const std::uint32_t digits = (width() + 3) / 4;
        for (std::uint32_t d = digits; d-- > 0;) {
            unsigned v = 0;
            for (std::uint32_t b = 0; b < 4; ++b) {
                const std::uint32_t i = d * 4 + b;
                if (i < width() && bits_[i] == Logic::L1)
                    v |= 1U << b;
            }
            out += kHex[v];
        }
        return out;
    }
    out += "'b";
    for (std::uint32_t i = width(); i-- > 0;) {
        switch (bits_[i]) {
            case Logic::L0: out += '0'; break;
            case Logic::L1: out += '1'; break;
            case Logic::X: out += 'x'; break;
            case Logic::Z: out += 'z'; break;
        }
    }
    return out;
}

} // namespace opencdc::ir

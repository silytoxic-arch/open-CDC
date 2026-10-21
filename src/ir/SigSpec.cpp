#include "ir/SigSpec.h"

#include <algorithm>
#include <cassert>

namespace opencdc::ir {

SigSpec SigSpec::net(NetId net, std::uint32_t width, std::uint32_t offset) {
    SigSpec s;
    if (width > 0)
        s.append(SigChunk{net, offset, width, {}});
    return s;
}

SigSpec SigSpec::constant(ConstBits value) {
    SigSpec s;
    if (value.width() > 0) {
        const auto w = value.width();
        s.append(SigChunk{NetId{}, 0, w, std::move(value)});
    }
    return s;
}

SigSpec SigSpec::constant(std::uint64_t value, std::uint32_t width) {
    return constant(ConstBits::fromUint(value, width));
}

SigSpec SigSpec::allX(std::uint32_t width) {
    return constant(ConstBits(width, Logic::X));
}

void SigSpec::append(const SigChunk& chunk) {
    if (chunk.width == 0)
        return;
    width_ += chunk.width;
    if (!chunks_.empty()) {
        SigChunk& last = chunks_.back();
        if (last.isConst() && chunk.isConst()) {
            last.constant.append(chunk.constant);
            last.width += chunk.width;
            return;
        }
        if (!last.isConst() && last.net == chunk.net && last.offset + last.width == chunk.offset) {
            last.width += chunk.width;
            return;
        }
    }
    chunks_.push_back(chunk);
}

void SigSpec::append(const SigSpec& msbSide) {
    for (const auto& c : msbSide.chunks_)
        append(c);
}

SigSpec SigSpec::extract(std::uint32_t offset, std::uint32_t width) const {
    assert(offset + width <= width_);
    SigSpec out;
    std::uint32_t pos = 0;
    for (const auto& c : chunks_) {
        const std::uint32_t lo = std::max(offset, pos);
        const std::uint32_t hi = std::min(offset + width, pos + c.width);
        if (lo < hi) {
            const std::uint32_t inner = lo - pos;
            if (c.isConst())
                out.append(SigChunk{NetId{}, 0, hi - lo, c.constant.extract(inner, hi - lo)});
            else
                out.append(SigChunk{c.net, c.offset + inner, hi - lo, {}});
        }
        pos += c.width;
        if (pos >= offset + width)
            break;
    }
    return out;
}

SigSpec SigSpec::replaced(std::uint32_t offset, const SigSpec& value) const {
    assert(offset + value.width() <= width_);
    SigSpec out = extract(0, offset);
    out.append(value);
    const std::uint32_t end = offset + value.width();
    out.append(extract(end, width_ - end));
    return out;
}

SigSpec SigSpec::resized(std::uint32_t width, bool signExtend) const {
    if (width <= width_)
        return extract(0, width);
    SigSpec out = *this;
    if (signExtend && width_ > 0)
        out.append(bit(width_ - 1).repeated(width - width_));
    else
        out.append(constant(0, width - width_));
    return out;
}

SigSpec SigSpec::repeated(std::uint32_t count) const {
    SigSpec out;
    for (std::uint32_t i = 0; i < count; ++i)
        out.append(*this);
    return out;
}

bool SigSpec::isFullyConst() const noexcept {
    return std::ranges::all_of(chunks_, [](const SigChunk& c) { return c.isConst(); });
}

std::optional<ConstBits> SigSpec::asConst() const {
    if (!isFullyConst())
        return std::nullopt;
    if (chunks_.empty())
        return ConstBits{};
    return chunks_.front().constant; // normalized: all constants merge into one chunk
}

bool SigSpec::onlyReferences(NetId n) const noexcept {
    return !chunks_.empty() &&
           std::ranges::all_of(chunks_, [n](const SigChunk& c) { return !c.isConst() && c.net == n; });
}

bool SigSpec::references(NetId n) const noexcept {
    return std::ranges::any_of(chunks_, [n](const SigChunk& c) { return !c.isConst() && c.net == n; });
}

} // namespace opencdc::ir

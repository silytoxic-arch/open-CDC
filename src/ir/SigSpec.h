#pragma once

#include "core/Id.h"
#include "ir/Const.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace opencdc::ir {

using NetId = Id<struct NetTag>;   ///< net inside one ModuleBody
using CellId = Id<struct CellTag>; ///< cell inside one ModuleBody

/// A contiguous run of bits: either bits [offset, offset+width) of a net, or a constant.
/// Bit offsets are normalized: 0 is the least significant (rightmost) bit,
/// independent of how the net's range was declared.
struct SigChunk {
    NetId net;                 ///< invalid => constant chunk
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    ConstBits constant;        ///< only for constant chunks (width == constant.width())

    [[nodiscard]] bool isConst() const noexcept { return !net.valid(); }
    friend bool operator==(const SigChunk&, const SigChunk&) = default;
};

/// A word-level signal value: a concatenation of chunks, LSB first. This is
/// how bit selects, part selects, concatenations and constants are represented
/// uniformly in the IR.
class SigSpec {
public:
    SigSpec() = default;

    static SigSpec net(NetId net, std::uint32_t width, std::uint32_t offset = 0);
    static SigSpec constant(ConstBits value);
    static SigSpec constant(std::uint64_t value, std::uint32_t width);
    static SigSpec allX(std::uint32_t width);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] bool empty() const noexcept { return width_ == 0; }
    [[nodiscard]] const std::vector<SigChunk>& chunks() const noexcept { return chunks_; }

    /// Appends `msbSide` above the current most significant bit (like {msbSide, *this}).
    void append(const SigSpec& msbSide);
    void append(const SigChunk& chunk);

    [[nodiscard]] SigSpec extract(std::uint32_t offset, std::uint32_t width) const;
    /// Replaces bits [offset, offset+value.width()) with `value`.
    [[nodiscard]] SigSpec replaced(std::uint32_t offset, const SigSpec& value) const;
    [[nodiscard]] SigSpec bit(std::uint32_t index) const { return extract(index, 1); }

    /// Zero- or sign-extends, or truncates, to `width`.
    [[nodiscard]] SigSpec resized(std::uint32_t width, bool signExtend) const;
    [[nodiscard]] SigSpec repeated(std::uint32_t count) const;

    [[nodiscard]] bool isFullyConst() const noexcept;
    [[nodiscard]] std::optional<ConstBits> asConst() const;
    /// True if every bit refers to `net`.
    [[nodiscard]] bool onlyReferences(NetId net) const noexcept;
    [[nodiscard]] bool references(NetId net) const noexcept;

    friend bool operator==(const SigSpec&, const SigSpec&) = default;

private:
    std::vector<SigChunk> chunks_; ///< normalized: adjacent mergeable chunks are merged
    std::uint32_t width_ = 0;
};

} // namespace opencdc::ir

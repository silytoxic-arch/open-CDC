#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opencdc::ir {

/// One four-state logic value.
enum class Logic : std::uint8_t { L0 = 0, L1 = 1, X = 2, Z = 3 };

/// A four-state constant of arbitrary width, stored LSB first.
class ConstBits {
public:
    ConstBits() = default;
    explicit ConstBits(std::vector<Logic> bits) : bits_(std::move(bits)) {}
    ConstBits(std::uint32_t width, Logic fill) : bits_(width, fill) {}

    /// Low `width` bits of `value` (bits above 64 are zero).
    static ConstBits fromUint(std::uint64_t value, std::uint32_t width);

    [[nodiscard]] std::uint32_t width() const noexcept { return static_cast<std::uint32_t>(bits_.size()); }
    [[nodiscard]] bool empty() const noexcept { return bits_.empty(); }
    [[nodiscard]] Logic operator[](std::uint32_t i) const { return bits_[i]; }
    [[nodiscard]] Logic& operator[](std::uint32_t i) { return bits_[i]; }
    [[nodiscard]] const std::vector<Logic>& bits() const noexcept { return bits_; }

    /// True if no bit is X or Z.
    [[nodiscard]] bool isFullyDefined() const noexcept;
    /// Value if fully defined and width <= 64.
    [[nodiscard]] std::optional<std::uint64_t> toUint() const;
    /// Two's-complement value if fully defined and width in [1, 64].
    [[nodiscard]] std::optional<std::int64_t> toInt() const;
    [[nodiscard]] bool isZero() const noexcept;

    [[nodiscard]] ConstBits extract(std::uint32_t offset, std::uint32_t width) const;
    void append(const ConstBits& msbSide);

    /// Verilog literal: 4'b10x1 (binary, MSB first). Large fully-defined
    /// values (> 64 bits) use hexadecimal.
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const ConstBits&, const ConstBits&) = default;

private:
    std::vector<Logic> bits_;
};

} // namespace opencdc::ir

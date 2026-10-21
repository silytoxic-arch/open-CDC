#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace opencdc {

/// Strongly typed 32-bit index. Different Tag types cannot be mixed up, which
/// prevents e.g. passing a NetId where a CellId is expected.
template <class Tag>
struct Id {
    using ValueType = std::uint32_t;
    static constexpr ValueType kInvalid = std::numeric_limits<ValueType>::max();

    ValueType value = kInvalid;

    constexpr Id() noexcept = default;
    constexpr explicit Id(ValueType v) noexcept : value(v) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return value != kInvalid; }
    [[nodiscard]] constexpr std::size_t index() const noexcept { return static_cast<std::size_t>(value); }

    constexpr auto operator<=>(const Id&) const noexcept = default;
};

} // namespace opencdc

template <class Tag>
struct std::hash<opencdc::Id<Tag>> {
    std::size_t operator()(const opencdc::Id<Tag>& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.value);
    }
};

#pragma once

#include "ir/Design.h"

#include <optional>

namespace opencdc::ir {

/// Tries to replace a cell by an equivalent SigSpec without creating it:
///  * all inputs constant (fully defined, <= 64 bits) -> constant result
///  * Mux with a constant select, or with identical data inputs -> one input
///  * DynSelect / DynInsert with a constant index -> static slice / replacement
/// Returns nullopt if the cell must be created. Used for constant propagation
/// (parameters, generate conditions, unrolled loop indices).
[[nodiscard]] std::optional<SigSpec> simplifyCell(const Cell& cell);

/// Element offset (in elements, from the LSB) of declared index `index` in a
/// range [left:right]; nullopt if out of range.
[[nodiscard]] std::optional<std::uint32_t> elementOffset(std::int64_t index, std::int32_t left,
                                                         std::int32_t right) noexcept;

} // namespace opencdc::ir

#pragma once

#include "core/Diagnostic.h"

#include <cassert>
#include <utility>
#include <variant>

namespace opencdc {

/// Value-or-error type (C++20 stand-in for std::expected).
/// The error is always a Diagnostic so it can be shown to the user directly.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}          // NOLINT
    Result(Diagnostic error) : storage_(std::in_place_index<1>, std::move(error)) {} // NOLINT

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { assert(ok()); return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { assert(ok()); return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { assert(ok()); return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Diagnostic& error() const& { assert(!ok()); return std::get<1>(storage_); }
    [[nodiscard]] Diagnostic&& error() && { assert(!ok()); return std::get<1>(std::move(storage_)); }

private:
    std::variant<T, Diagnostic> storage_;
};

} // namespace opencdc

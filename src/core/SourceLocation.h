#pragma once

#include <cstdint>
#include <string>

namespace opencdc {

/// A position in a user source file. Lines and columns are 1-based; 0 means unknown.
/// The location owns its file path so it stays valid after the frontend that
/// produced it has been destroyed (results outlive the analysis session).
struct SourceLocation {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    [[nodiscard]] bool valid() const noexcept { return !file.empty() && line > 0; }

    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

/// "file:line:col", omitting parts that are unknown.
std::string toString(const SourceLocation& loc);

} // namespace opencdc

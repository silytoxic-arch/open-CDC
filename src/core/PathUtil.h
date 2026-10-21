#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace opencdc {

/// All strings inside OpenCDC are UTF-8. These helpers convert paths without
/// going through the Windows ANSI code page (important for non-ASCII paths,
/// e.g. Chinese directory names).
[[nodiscard]] inline std::string toUtf8(const std::filesystem::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

[[nodiscard]] inline std::filesystem::path fromUtf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

} // namespace opencdc

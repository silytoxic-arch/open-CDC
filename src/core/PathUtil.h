#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

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

/// A token that is unique across processes and calls (64 random bits and a
/// counter, hexadecimal): for temporary file names that several OpenCDC
/// processes (parallel analyses, parallel tests) may create at the same time.
[[nodiscard]] inline std::string uniqueToken() {
    static std::atomic<std::uint32_t> counter{0};
    std::random_device rd;
    const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (int i = 60; i >= 0; i -= 4)
        s += kHex[(r >> i) & 0xF];
    return s + "_" + std::to_string(counter++);
}

/// Creates a directory <system temp>/<prefix><token> that did not exist
/// before (an existing one is never shared). Empty path on failure.
[[nodiscard]] inline std::filesystem::path createUniqueTempDir(const std::string& prefix) {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec)
        return {};
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto dir = base / (prefix + uniqueToken());
        if (std::filesystem::create_directory(dir, ec) && !ec)
            return dir;
    }
    return {};
}

} // namespace opencdc

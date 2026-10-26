#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace opencdc {

/// Source text converted to UTF-8.
struct DecodedText {
    std::string utf8;
    /// "UTF-8" (unchanged), "UTF-8 with BOM", "UTF-16LE", "UTF-16BE",
    /// "GB18030" (covers GBK/GB2312, the default of Chinese Windows editors)
    /// or "Windows-1252" (last resort, every byte decodes)
    std::string encoding;
    [[nodiscard]] bool transcoded() const { return encoding != "UTF-8"; }
};

[[nodiscard]] bool isValidUtf8(std::string_view bytes) noexcept;

/// Decodes HDL / constraint source bytes: UTF-8 as is; otherwise UTF-16 with
/// a byte order mark, GB18030, and finally Windows-1252.
[[nodiscard]] DecodedText decodeSourceText(std::string_view bytes);

/// Reads a file and decodes it; nullopt if it cannot be read.
[[nodiscard]] std::optional<DecodedText> readSourceText(const std::filesystem::path& file);

} // namespace opencdc

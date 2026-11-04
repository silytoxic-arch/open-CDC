#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace opencdc {

/// Decompresses a raw DEFLATE stream (RFC 1951); nullopt if it is malformed
/// or would decompress to more than `limit` bytes.
[[nodiscard]] std::optional<std::string> inflate(std::string_view data, std::size_t limit = 256u << 20);

/// The files of a ZIP archive (stored or deflated entries, no encryption, no
/// ZIP64): entry name -> contents. nullopt if the archive cannot be read.
/// Vivado's .xcix (an IP with its output products) is such an archive.
[[nodiscard]] std::optional<std::map<std::string, std::string>> readZip(std::string_view archive);
[[nodiscard]] std::optional<std::map<std::string, std::string>> readZipFile(const std::filesystem::path& file);

} // namespace opencdc

#pragma once

// A YAML subset for FuseSoC core files (CAPI2): block mappings and sequences,
// flow mappings and sequences ({a: 1}, [x, y]), plain, single- and double-quoted
// scalars, comments, anchors and aliases (&name, *name) and merge keys (<<).
// Not supported: multi-line block scalars (| and >), tags, multiple documents.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencdc::engine {

struct YamlNode {
    enum class Kind { Null, Scalar, Sequence, Mapping };
    Kind kind = Kind::Null;
    std::string scalar;
    std::vector<std::shared_ptr<const YamlNode>> items;                           ///< Sequence
    std::vector<std::pair<std::string, std::shared_ptr<const YamlNode>>> entries; ///< Mapping, in order

    [[nodiscard]] bool isMap() const noexcept { return kind == Kind::Mapping; }
    [[nodiscard]] bool isSeq() const noexcept { return kind == Kind::Sequence; }
    [[nodiscard]] bool isScalar() const noexcept { return kind == Kind::Scalar; }
    /// The value of `key` in a mapping (the last one if repeated), or nullptr.
    [[nodiscard]] const YamlNode* get(std::string_view key) const;
    /// The scalar text of `key`, or `def`.
    [[nodiscard]] std::string str(std::string_view key, std::string def = {}) const;
};

/// The document `text`; nullopt and `error` (with a line number) when it is not
/// in the supported subset.
[[nodiscard]] std::optional<std::shared_ptr<const YamlNode>> parseYaml(std::string_view text, std::string& error);

} // namespace opencdc::engine

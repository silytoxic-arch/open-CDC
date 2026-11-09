#pragma once

// A small XML reader for vendor project files (.xpr, .gprj): elements and
// attributes; text content is not kept.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencdc::engine {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    [[nodiscard]] std::string attr(std::string_view n) const {
        for (const auto& [k, v] : attrs)
            if (k == n)
                return v;
        return {};
    }
    [[nodiscard]] std::vector<const XmlNode*> all(std::string_view n) const {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c->name == n)
                out.push_back(c.get());
        return out;
    }
    [[nodiscard]] const XmlNode* first(std::string_view n) const {
        for (const auto& c : children)
            if (c->name == n)
                return c.get();
        return nullptr;
    }
};

/// The element tree of `text` (an unnamed root holding the top elements), or
/// nullptr when it is not well formed.
[[nodiscard]] std::unique_ptr<XmlNode> parseXml(std::string_view text);

} // namespace opencdc::engine

#include "engine/Xml.h"

#include <cctype>
#include <cstdlib>

namespace opencdc::engine {

namespace {
std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const auto e = s.substr(i + 1, semi - i - 1);
        if (e == "amp")
            out += '&';
        else if (e == "lt")
            out += '<';
        else if (e == "gt")
            out += '>';
        else if (e == "quot")
            out += '"';
        else if (e == "apos")
            out += '\'';
        else if (!e.empty() && e[0] == '#') {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            const auto code = std::strtoul(std::string(e.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
            if (code < 0x80)
                out += static_cast<char>(code);
        }
        else {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

} // namespace

std::unique_ptr<XmlNode> parseXml(std::string_view text) {
    auto root = std::make_unique<XmlNode>();
    std::vector<XmlNode*> stack{root.get()};
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        if (text.substr(i, 4) == "<!--") {
            const auto e = text.find("-->", i);
            if (e == std::string_view::npos)
                return nullptr;
            i = e + 3;
            continue;
        }
        if (text.substr(i, 2) == "<?" || text.substr(i, 2) == "<!") {
            const auto e = text.find('>', i);
            if (e == std::string_view::npos)
                return nullptr;
            i = e + 1;
            continue;
        }
        // the end of the tag: '>' outside quoted attribute values
        std::size_t e = i + 1;
        char quote = 0;
        for (; e < text.size(); ++e) {
            if (quote) {
                if (text[e] == quote)
                    quote = 0;
            }
            else if (text[e] == '"' || text[e] == '\'')
                quote = text[e];
            else if (text[e] == '>')
                break;
        }
        if (e >= text.size())
            return nullptr;
        std::string_view tag = text.substr(i + 1, e - i - 1);
        i = e + 1;
        if (!tag.empty() && tag[0] == '/') {
            if (stack.size() < 2)
                return nullptr;
            stack.pop_back();
            continue;
        }
        const bool selfClosing = !tag.empty() && tag.back() == '/';
        if (selfClosing)
            tag.remove_suffix(1);
        auto node = std::make_unique<XmlNode>();
        std::size_t p = 0;
        while (p < tag.size() && !std::isspace(static_cast<unsigned char>(tag[p])))
            ++p;
        node->name = std::string(tag.substr(0, p));
        while (p < tag.size()) {
            while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p])))
                ++p;
            const auto eq = tag.find('=', p);
            if (eq == std::string_view::npos)
                break;
            std::string key(tag.substr(p, eq - p));
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            auto q = eq + 1;
            while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q])))
                ++q;
            if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\''))
                return nullptr;
            const auto close = tag.find(tag[q], q + 1);
            if (close == std::string_view::npos)
                return nullptr;
            node->attrs.emplace_back(std::move(key), decodeEntities(tag.substr(q + 1, close - q - 1)));
            p = close + 1;
        }
        XmlNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClosing)
            stack.push_back(raw);
    }
    if (stack.size() != 1)
        return nullptr;
    return root;
}

} // namespace opencdc::engine

#include "engine/Yaml.h"

#include <algorithm>
#include <cctype>

namespace opencdc::engine {

const YamlNode* YamlNode::get(std::string_view key) const {
    const YamlNode* found = nullptr;
    for (const auto& [k, v] : entries)
        if (k == key)
            found = v.get();
    return found;
}

std::string YamlNode::str(std::string_view key, std::string def) const {
    const auto* n = get(key);
    return n && n->isScalar() ? n->scalar : def;
}

namespace {

using NodePtr = std::shared_ptr<YamlNode>;

struct Line {
    std::size_t indent = 0;
    std::string text; ///< without indentation and comment
    std::size_t number = 0;
};

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

/// The line without its comment ('#' at the start or after a space, outside quotes).
std::string stripComment(std::string_view s) {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == quote) {
                if (quote == '\'' && i + 1 < s.size() && s[i + 1] == '\'')
                    ++i; // '' inside single quotes
                else
                    quote = 0;
            }
            else if (quote == '"' && c == '\\')
                ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            // a quote starts a quoted scalar only at the start of a value
            std::size_t j = i;
            while (j > 0 && s[j - 1] == ' ')
                --j;
            if (j == 0 || s[j - 1] == ':' || s[j - 1] == '-' || s[j - 1] == ',' || s[j - 1] == '[' ||
                s[j - 1] == '{' || s[j - 1] == '?')
                quote = c;
            continue;
        }
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
            return std::string(s.substr(0, i));
    }
    return std::string(s);
}

int bracketBalance(std::string_view s) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            else if (quote == '"' && c == '\\')
                ++i;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '[' || c == '{')
            ++depth;
        else if (c == ']' || c == '}')
            --depth;
    }
    return depth;
}

class Parser {
public:
    Parser(std::vector<Line> lines, std::string& error) : lines_(std::move(lines)), error_(error) {}

    std::optional<NodePtr> document() {
        if (lines_.empty())
            return std::make_shared<YamlNode>();
        auto n = block(lines_[0].indent);
        if (!n)
            return std::nullopt;
        if (pos_ < lines_.size())
            return fail(lines_[pos_].number, "unexpected indentation");
        return n;
    }

private:
    std::optional<NodePtr> fail(std::size_t line, std::string msg) {
        if (error_.empty())
            error_ = "line " + std::to_string(line) + ": " + std::move(msg);
        return std::nullopt;
    }

    static bool isSeqItem(const std::string& t) { return t == "-" || t.starts_with("- "); }

    /// Position of the ':' that ends a mapping key in `t` (followed by a space or the end), npos if none.
    static std::size_t keyColon(const std::string& t) {
        char quote = 0;
        int depth = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (quote) {
                if (c == quote)
                    quote = 0;
                continue;
            }
            if ((c == '"' || c == '\'') && i == 0) {
                quote = c;
                continue;
            }
            if (c == '[' || c == '{')
                ++depth;
            else if (c == ']' || c == '}')
                --depth;
            else if (c == ':' && depth == 0 && (i + 1 == t.size() || t[i + 1] == ' '))
                return i;
            if (i == 0 && (c == '[' || c == '{'))
                return std::string::npos; // a flow collection, not a key
        }
        return std::string::npos;
    }

    static std::string unquote(std::string s) {
        s = trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            std::string out;
            for (std::size_t i = 1; i + 1 < s.size(); ++i) {
                if (s[i] == '\\' && i + 2 < s.size()) {
                    const char n = s[++i];
                    out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                }
                else
                    out += s[i];
            }
            return out;
        }
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
            std::string out;
            for (std::size_t i = 1; i + 1 < s.size(); ++i) {
                out += s[i];
                if (s[i] == '\'' && s[i + 1] == '\'')
                    ++i;
            }
            return out;
        }
        return s;
    }

    static NodePtr scalar(std::string s) {
        auto n = std::make_shared<YamlNode>();
        s = trim(s);
        if (s.empty() || s == "~" || s == "null") {
            n->kind = YamlNode::Kind::Null;
            return n;
        }
        n->kind = YamlNode::Kind::Scalar;
        n->scalar = unquote(std::move(s));
        return n;
    }

    // ---- flow collections ----
    std::optional<NodePtr> flow(std::string_view s, std::size_t& i, std::size_t line) {
        auto skip = [&] {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
        };
        skip();
        if (i >= s.size())
            return scalar("");
        if (s[i] == '[' || s[i] == '{') {
            const bool map = s[i] == '{';
            const char close = map ? '}' : ']';
            ++i;
            auto n = std::make_shared<YamlNode>();
            n->kind = map ? YamlNode::Kind::Mapping : YamlNode::Kind::Sequence;
            skip();
            if (i < s.size() && s[i] == close) {
                ++i;
                return n;
            }
            for (;;) {
                if (map) {
                    std::size_t k = i;
                    std::string key;
                    // the key: up to ':' (outside quotes)
                    char quote = 0;
                    for (; k < s.size(); ++k) {
                        if (quote) {
                            if (s[k] == quote)
                                quote = 0;
                            continue;
                        }
                        if (s[k] == '"' || s[k] == '\'')
                            quote = s[k];
                        else if (s[k] == ':' || s[k] == ',' || s[k] == close)
                            break;
                    }
                    key = unquote(std::string(s.substr(i, k - i)));
                    i = k;
                    NodePtr value = scalar("");
                    if (i < s.size() && s[i] == ':') {
                        ++i;
                        auto v = flow(s, i, line);
                        if (!v)
                            return std::nullopt;
                        value = *v;
                    }
                    n->entries.emplace_back(key, value);
                }
                else {
                    auto v = flow(s, i, line);
                    if (!v)
                        return std::nullopt;
                    n->items.push_back(*v);
                }
                skip();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    skip();
                    if (i < s.size() && s[i] == close) { // a trailing comma
                        ++i;
                        return n;
                    }
                    continue;
                }
                if (i < s.size() && s[i] == close) {
                    ++i;
                    return n;
                }
                return fail(line, "unterminated flow collection");
            }
        }
        // a scalar inside a flow collection: up to ',' ']' '}' (outside quotes)
        std::size_t k = i;
        if (s[i] == '"' || s[i] == '\'') {
            const char q = s[i];
            for (k = i + 1; k < s.size(); ++k) {
                if (s[k] == '\\' && q == '"') {
                    ++k;
                    continue;
                }
                if (s[k] == q)
                    break;
            }
            ++k;
        }
        else
            while (k < s.size() && s[k] != ',' && s[k] != ']' && s[k] != '}' &&
                   !(s[k] == ':' && (k + 1 == s.size() || s[k + 1] == ' ' || s[k + 1] == ',')))
                ++k;
        auto n = scalar(std::string(s.substr(i, k - i)));
        i = std::min(k, s.size());
        return n;
    }

    /// A value written on the line of its key or sequence dash.
    std::optional<NodePtr> inlineValue(std::string text, std::size_t line, std::size_t childIndent) {
        text = trim(text);
        std::string anchor;
        if (text.starts_with("&")) {
            const auto sp = text.find(' ');
            anchor = text.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
            text = sp == std::string::npos ? std::string() : trim(text.substr(sp));
        }
        std::optional<NodePtr> value;
        if (text.empty()) {
            // the value is the block below (deeper, or a sequence at the key's indentation)
            if (pos_ < lines_.size() && (lines_[pos_].indent > childIndent ||
                                         (lines_[pos_].indent == childIndent && isSeqItem(lines_[pos_].text))))
                value = block(lines_[pos_].indent);
            else
                value = scalar("");
        }
        else if (text.starts_with("*")) {
            const auto it = anchors_.find(trim(text.substr(1)));
            if (it == anchors_.end())
                return fail(line, "unknown alias '" + text + "'");
            value = it->second;
        }
        else if (text.starts_with("[") || text.starts_with("{")) {
            std::size_t i = 0;
            value = flow(text, i, line);
        }
        else if (text == "|" || text == ">" || text.starts_with("|") || text.starts_with(">")) {
            return fail(line, "block scalars are not supported");
        }
        else if ((text.front() == '"' || text.front() == '\'') &&
                 (text.size() < 2 || text.back() != text.front() ||
                  (text.front() == '"' && text[text.size() - 2] == '\\'))) {
            return fail(line, "unclosed quoted scalar (multi-line scalars are not supported)");
        }
        else
            value = scalar(text);
        if (value && !anchor.empty())
            anchors_[anchor] = *value;
        return value;
    }

    std::optional<NodePtr> block(std::size_t indent) {
        if (pos_ >= lines_.size())
            return scalar("");
        if (isSeqItem(lines_[pos_].text))
            return sequence(indent);
        return mapping(indent);
    }

    std::optional<NodePtr> sequence(std::size_t indent) {
        auto n = std::make_shared<YamlNode>();
        n->kind = YamlNode::Kind::Sequence;
        while (pos_ < lines_.size() && lines_[pos_].indent == indent && isSeqItem(lines_[pos_].text)) {
            Line& l = lines_[pos_];
            std::size_t off = 2;
            while (off < l.text.size() && l.text[off] == ' ')
                ++off;
            std::string rest = l.text.substr(std::min(off, l.text.size()));
            if (!rest.empty() && keyColon(rest) != std::string::npos && !rest.starts_with("&") &&
                !rest.starts_with("*")) {
                // "- key: value": a mapping whose first entry is on this line
                l.indent = indent + off;
                l.text = rest;
                auto m = mapping(l.indent);
                if (!m)
                    return std::nullopt;
                n->items.push_back(*m);
                continue;
            }
            ++pos_;
            auto v = inlineValue(rest, l.number, indent);
            if (!v)
                return std::nullopt;
            n->items.push_back(*v);
        }
        return n;
    }

    std::optional<NodePtr> mapping(std::size_t indent) {
        auto n = std::make_shared<YamlNode>();
        n->kind = YamlNode::Kind::Mapping;
        std::vector<std::pair<std::string, std::shared_ptr<const YamlNode>>> merged;
        while (pos_ < lines_.size() && lines_[pos_].indent == indent && !isSeqItem(lines_[pos_].text)) {
            const Line l = lines_[pos_];
            const auto c = keyColon(l.text);
            if (c == std::string::npos)
                return fail(l.number, "expected 'key: value'");
            const std::string key = unquote(l.text.substr(0, c));
            ++pos_;
            auto v = inlineValue(l.text.substr(c + 1), l.number, indent);
            if (!v)
                return std::nullopt;
            if (key == "<<") {
                const auto add = [&](const YamlNode& src) {
                    for (const auto& e : src.entries)
                        merged.push_back(e);
                };
                if ((*v)->isMap())
                    add(**v);
                else if ((*v)->isSeq())
                    for (const auto& it : (*v)->items)
                        if (it->isMap())
                            add(*it);
                continue;
            }
            n->entries.emplace_back(key, *v);
        }
        if (pos_ < lines_.size() && lines_[pos_].indent > indent)
            return fail(lines_[pos_].number, "unexpected indentation");
        // merged entries: only the keys the mapping does not give itself, before its own
        if (!merged.empty()) {
            std::vector<std::pair<std::string, std::shared_ptr<const YamlNode>>> all;
            for (const auto& e : merged)
                if (!n->get(e.first))
                    all.push_back(e);
            all.insert(all.end(), n->entries.begin(), n->entries.end());
            n->entries = std::move(all);
        }
        return n;
    }

    std::vector<Line> lines_;
    std::string& error_;
    std::size_t pos_ = 0;
    std::map<std::string, NodePtr> anchors_;
};

} // namespace

std::optional<std::shared_ptr<const YamlNode>> parseYaml(std::string_view text, std::string& error) {
    std::vector<Line> lines;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string raw(text.substr(start, end - start));
        start = end + 1;
        ++number;
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        if (raw.find('\t') != std::string::npos && raw.find_first_not_of(" \t") != std::string::npos &&
            raw[raw.find_first_not_of(' ')] == '\t') {
            error = "line " + std::to_string(number) + ": tabs are not allowed for indentation";
            return std::nullopt;
        }
        std::string body = stripComment(raw);
        const auto first = body.find_first_not_of(' ');
        if (first == std::string::npos || trim(body).empty())
            continue;
        if (trim(body) == "---" || trim(body) == "...")
            continue;
        Line l{first, trim(body), number};
        // a flow collection over several lines: joined
        while (bracketBalance(l.text) > 0 && start <= text.size()) {
            auto e2 = text.find('\n', start);
            if (e2 == std::string_view::npos)
                e2 = text.size();
            l.text += " " + trim(stripComment(text.substr(start, e2 - start)));
            start = e2 + 1;
            ++number;
        }
        if (start > text.size() + 1)
            break;
        lines.push_back(std::move(l));
        if (end == text.size())
            break;
    }
    Parser p(std::move(lines), error);
    auto doc = p.document();
    if (!doc)
        return std::nullopt;
    return std::shared_ptr<const YamlNode>(*doc);
}

} // namespace opencdc::engine

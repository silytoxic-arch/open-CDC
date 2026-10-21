#include "frontend/vhdl/VhdlLexer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace opencdc::vhdl {

namespace {

struct KwEntry {
    const char* text;
    Kw kw;
};

constexpr std::array kKeywords{
    KwEntry{"abs", Kw::Abs},
    KwEntry{"access", Kw::Access},
    KwEntry{"after", Kw::After},
    KwEntry{"alias", Kw::Alias},
    KwEntry{"all", Kw::All},
    KwEntry{"and", Kw::And},
    KwEntry{"architecture", Kw::Architecture},
    KwEntry{"array", Kw::Array},
    KwEntry{"assert", Kw::Assert},
    KwEntry{"attribute", Kw::Attribute},
    KwEntry{"begin", Kw::Begin},
    KwEntry{"block", Kw::Block},
    KwEntry{"body", Kw::Body},
    KwEntry{"buffer", Kw::Buffer},
    KwEntry{"bus", Kw::Bus},
    KwEntry{"case", Kw::Case},
    KwEntry{"component", Kw::Component},
    KwEntry{"configuration", Kw::Configuration},
    KwEntry{"constant", Kw::Constant},
    KwEntry{"disconnect", Kw::Disconnect},
    KwEntry{"downto", Kw::Downto},
    KwEntry{"else", Kw::Else},
    KwEntry{"elsif", Kw::Elsif},
    KwEntry{"end", Kw::End},
    KwEntry{"entity", Kw::Entity},
    KwEntry{"exit", Kw::Exit},
    KwEntry{"file", Kw::File},
    KwEntry{"for", Kw::For},
    KwEntry{"function", Kw::Function},
    KwEntry{"generate", Kw::Generate},
    KwEntry{"generic", Kw::Generic},
    KwEntry{"group", Kw::Group},
    KwEntry{"guarded", Kw::Guarded},
    KwEntry{"if", Kw::If},
    KwEntry{"impure", Kw::Impure},
    KwEntry{"in", Kw::In},
    KwEntry{"inertial", Kw::Inertial},
    KwEntry{"inout", Kw::Inout},
    KwEntry{"is", Kw::Is},
    KwEntry{"label", Kw::Label},
    KwEntry{"library", Kw::Library},
    KwEntry{"linkage", Kw::Linkage},
    KwEntry{"literal", Kw::Literal},
    KwEntry{"loop", Kw::Loop},
    KwEntry{"map", Kw::Map},
    KwEntry{"mod", Kw::Mod},
    KwEntry{"nand", Kw::Nand},
    KwEntry{"new", Kw::New},
    KwEntry{"next", Kw::Next},
    KwEntry{"nor", Kw::Nor},
    KwEntry{"not", Kw::Not},
    KwEntry{"null", Kw::Null},
    KwEntry{"of", Kw::Of},
    KwEntry{"on", Kw::On},
    KwEntry{"open", Kw::Open},
    KwEntry{"or", Kw::Or},
    KwEntry{"others", Kw::Others},
    KwEntry{"out", Kw::Out},
    KwEntry{"package", Kw::Package},
    KwEntry{"port", Kw::Port},
    KwEntry{"postponed", Kw::Postponed},
    KwEntry{"procedure", Kw::Procedure},
    KwEntry{"process", Kw::Process},
    KwEntry{"protected", Kw::Protected},
    KwEntry{"pure", Kw::Pure},
    KwEntry{"range", Kw::Range},
    KwEntry{"record", Kw::Record},
    KwEntry{"register", Kw::Register},
    KwEntry{"reject", Kw::Reject},
    KwEntry{"rem", Kw::Rem},
    KwEntry{"report", Kw::Report},
    KwEntry{"return", Kw::Return},
    KwEntry{"rol", Kw::Rol},
    KwEntry{"ror", Kw::Ror},
    KwEntry{"select", Kw::Select},
    KwEntry{"severity", Kw::Severity},
    KwEntry{"signal", Kw::Signal},
    KwEntry{"shared", Kw::Shared},
    KwEntry{"sla", Kw::Sla},
    KwEntry{"sll", Kw::Sll},
    KwEntry{"sra", Kw::Sra},
    KwEntry{"srl", Kw::Srl},
    KwEntry{"subtype", Kw::Subtype},
    KwEntry{"then", Kw::Then},
    KwEntry{"to", Kw::To},
    KwEntry{"transport", Kw::Transport},
    KwEntry{"type", Kw::Type},
    KwEntry{"unaffected", Kw::Unaffected},
    KwEntry{"units", Kw::Units},
    KwEntry{"until", Kw::Until},
    KwEntry{"use", Kw::Use},
    KwEntry{"variable", Kw::Variable},
    KwEntry{"wait", Kw::Wait},
    KwEntry{"when", Kw::When},
    KwEntry{"while", Kw::While},
    KwEntry{"with", Kw::With},
    KwEntry{"xnor", Kw::Xnor},
    KwEntry{"xor", Kw::Xor},
};

const std::unordered_map<std::string, Kw>& keywordMap() {
    static const auto map = [] {
        std::unordered_map<std::string, Kw> m;
        for (const auto& e : kKeywords)
            m.emplace(e.text, e.kw);
        return m;
    }();
    return map;
}

bool isLetter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0xC0;
}
bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
bool isIdentChar(char c) {
    return isLetter(c) || isDigit(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

int digitValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

/// "-- synthesis translate_off" and the vendor variants.
enum class Pragma { None, Off, On };

Pragma pragmaOf(std::string_view comment) {
    const std::string c = lowerAscii(comment);
    std::vector<std::string> words;
    std::string w;
    for (const char ch : c) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
            w += ch;
        }
        else if (!w.empty()) {
            words.push_back(std::move(w));
            w.clear();
        }
    }
    if (!w.empty())
        words.push_back(std::move(w));
    if (words.size() < 2)
        return Pragma::None;
    static const std::array<std::string_view, 9> prefixes{"synthesis", "synopsys", "pragma",  "rtl_synthesis", "xilinx",
                                                          "altera",    "intel",    "exemplar", "cadence"};
    if (std::ranges::find(prefixes, words[0]) == prefixes.end())
        return Pragma::None;
    const auto& d = words[1];
    if (d == "translate_off" || d == "synthesis_off" || (d == "off" && words[0] == "rtl_synthesis"))
        return Pragma::Off;
    if (d == "translate_on" || d == "synthesis_on" || (d == "on" && words[0] == "rtl_synthesis"))
        return Pragma::On;
    return Pragma::None;
}

class Lexer {
public:
    Lexer(std::string_view text, std::uint32_t file) : s_(text), file_(file) {}

    LexResult run() {
        LexResult r;
        for (;;) {
            Token t = next(r);
            if (t.kind == Tok::End) {
                if (skipping_)
                    r.errors.push_back({"translate_off region is not closed by translate_on", offLoc_});
                r.tokens.push_back(std::move(t));
                break;
            }
            if (skipping_)
                continue;
            r.tokens.push_back(std::move(t));
        }
        r.translateOffRegions = offRegions_;
        return r;
    }

private:
    [[nodiscard]] char peek(std::size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    [[nodiscard]] Loc here() const { return {file_, line_, static_cast<std::uint32_t>(i_ - lineStart_ + 1)}; }

    void newline() {
        ++line_;
        lineStart_ = i_;
    }

    void skipTrivia(LexResult& r) {
        for (;;) {
            const char c = peek();
            if (c == '\n') {
                ++i_;
                newline();
            }
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' ||
                     static_cast<unsigned char>(c) == 0xA0) {
                ++i_;
            }
            else if (c == '-' && peek(1) == '-') {
                const Loc start = here();
                const std::size_t b = i_ + 2;
                while (i_ < s_.size() && s_[i_] != '\n')
                    ++i_;
                switch (pragmaOf(s_.substr(b, i_ - b))) {
                    case Pragma::Off:
                        if (!skipping_) {
                            skipping_ = true;
                            offLoc_ = start;
                            ++offRegions_;
                        }
                        break;
                    case Pragma::On: skipping_ = false; break;
                    case Pragma::None: break;
                }
            }
            else if (c == '/' && peek(1) == '*') {
                const Loc start = here();
                i_ += 2;
                bool closed = false;
                while (i_ < s_.size()) {
                    if (s_[i_] == '*' && peek(1) == '/') {
                        i_ += 2;
                        closed = true;
                        break;
                    }
                    if (s_[i_] == '\n') {
                        ++i_;
                        newline();
                    }
                    else {
                        ++i_;
                    }
                }
                if (!closed)
                    r.errors.push_back({"unterminated /* comment", start});
            }
            else {
                return;
            }
        }
    }

    /// Is a quote at the current position a tick (attribute / qualified
    /// expression) rather than the start of a character literal?
    [[nodiscard]] bool tickContext() const {
        switch (prev_) {
            case Tok::Identifier:
            case Tok::ExtIdentifier:
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::StringLiteral: // "foo"'length is not valid, but keep it a tick
                return true;
            case Tok::Keyword: return prevKw_ == Kw::All; // x.all'length
            default: return false;
        }
    }

    Token make(Tok k, Loc l, std::string text = {}) {
        Token t;
        t.kind = k;
        t.loc = l;
        t.text = std::move(text);
        return t;
    }

    Token next(LexResult& r) {
        skipTrivia(r);
        Token t = scan(r);
        if (t.kind != Tok::End && !skipping_) {
            prev_ = t.kind;
            prevKw_ = t.kw;
        }
        return t;
    }

    Token scan(LexResult& r) {
        const Loc l = here();
        if (i_ >= s_.size())
            return make(Tok::End, l);
        const char c = peek();

        // Bit-string literal: [size] base "..." (base letters handled below)
        if (isLetter(c)) {
            const std::size_t b = i_;
            while (i_ < s_.size() && isIdentChar(s_[i_]))
                ++i_;
            std::string word(s_.substr(b, i_ - b));
            if (peek() == '"' && isBaseSpecifier(word))
                return bitString(r, l, b);
            Token t = make(Tok::Identifier, l, word);
            const std::string key = lowerAscii(word);
            const auto& kws = keywordMap();
            if (auto it = kws.find(key); it != kws.end()) {
                t.kind = Tok::Keyword;
                t.kw = it->second;
            }
            t.value = key;
            if (word.find("__") != std::string::npos || word.back() == '_')
                r.errors.push_back({"identifier '" + word + "' has consecutive or trailing underscores", l});
            return t;
        }
        if (isDigit(c))
            return number(r, l);

        switch (c) {
            case '\\': {
                const std::size_t b = i_++;
                while (i_ < s_.size() && s_[i_] != '\n') {
                    if (s_[i_] == '\\') {
                        if (peek(1) == '\\') {
                            i_ += 2;
                            continue;
                        }
                        ++i_;
                        Token t = make(Tok::ExtIdentifier, l, std::string(s_.substr(b, i_ - b)));
                        t.value = t.text;
                        return t;
                    }
                    ++i_;
                }
                r.errors.push_back({"unterminated extended identifier", l});
                Token t = make(Tok::ExtIdentifier, l, std::string(s_.substr(b, i_ - b)) + "\\");
                t.value = t.text;
                return t;
            }
            case '"': return stringLit(r, l);
            case '\'':
                if (!tickContext() && i_ + 2 < s_.size() && s_[i_ + 2] == '\'') {
                    std::string text(s_.substr(i_, 3));
                    i_ += 3;
                    Token t = make(Tok::CharLiteral, l, text);
                    t.value = text;
                    return t;
                }
                ++i_;
                return make(Tok::Tick, l);
            default: break;
        }

        auto two = [&](char a, char b) { return c == a && peek(1) == b; };
        auto take = [&](Tok k, std::size_t n) {
            i_ += n;
            return make(k, l);
        };
        if (c == '?') {
            if (peek(1) == '?')
                return take(Tok::Cond, 2);
            if (peek(1) == '=')
                return take(Tok::MatchEq, 2);
            if (peek(1) == '/' && peek(2) == '=')
                return take(Tok::MatchNe, 3);
            if (peek(1) == '<')
                return peek(2) == '=' ? take(Tok::MatchLe, 3) : take(Tok::MatchLt, 2);
            if (peek(1) == '>')
                return peek(2) == '=' ? take(Tok::MatchGe, 3) : take(Tok::MatchGt, 2);
            return take(Tok::Question, 1);
        }
        if (two('=', '>'))
            return take(Tok::Arrow, 2);
        if (two('*', '*'))
            return take(Tok::DoubleStar, 2);
        if (two(':', '='))
            return take(Tok::VarAssign, 2);
        if (two('/', '='))
            return take(Tok::Ne, 2);
        if (two('>', '='))
            return take(Tok::Ge, 2);
        if (two('<', '='))
            return take(Tok::Le, 2);
        if (two('<', '>'))
            return take(Tok::Box, 2);
        if (two('<', '<'))
            return take(Tok::DLt, 2);
        if (two('>', '>'))
            return take(Tok::DGt, 2);
        switch (c) {
            case '&': return take(Tok::Amp, 1);
            case '(': return take(Tok::LParen, 1);
            case ')': return take(Tok::RParen, 1);
            case '*': return take(Tok::Star, 1);
            case '+': return take(Tok::Plus, 1);
            case ',': return take(Tok::Comma, 1);
            case '-': return take(Tok::Minus, 1);
            case '.': return take(Tok::Dot, 1);
            case '/': return take(Tok::Slash, 1);
            case ':': return take(Tok::Colon, 1);
            case ';': return take(Tok::Semicolon, 1);
            case '<': return take(Tok::Lt, 1);
            case '=': return take(Tok::Eq, 1);
            case '>': return take(Tok::Gt, 1);
            case '|': return take(Tok::Bar, 1);
            case '!': return take(Tok::Bar, 1); // VHDL-87 replacement character
            case '[': return take(Tok::LBracket, 1);
            case ']': return take(Tok::RBracket, 1);
            case '@': return take(Tok::At, 1);
            case '`': return take(Tok::Backquote, 1);
            default: break;
        }
        ++i_;
        r.errors.push_back({std::string("unexpected character '") + c + "'", l});
        return make(Tok::Invalid, l, std::string(1, c));
    }

    static bool isBaseSpecifier(std::string_view w) {
        const std::string k = lowerAscii(w);
        return k == "b" || k == "o" || k == "x" || k == "d" || k == "ub" || k == "uo" || k == "ux" || k == "sb" ||
               k == "so" || k == "sx";
    }

    Token bitString(LexResult& r, Loc l, std::size_t begin) {
        // i_ is at the opening quote
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\n')
            ++i_;
        if (peek() != '"') {
            r.errors.push_back({"unterminated bit string literal", l});
        }
        else {
            ++i_;
        }
        Token t = make(Tok::BitStringLiteral, l);
        t.value = std::string(s_.substr(begin, i_ - begin));
        std::string err;
        if (!expandBitString(t.value, t.text, err))
            r.errors.push_back({err, l});
        return t;
    }

    Token stringLit(LexResult& r, Loc l) {
        ++i_;
        std::string v;
        for (;;) {
            if (i_ >= s_.size() || s_[i_] == '\n') {
                r.errors.push_back({"unterminated string literal", l});
                break;
            }
            if (s_[i_] == '"') {
                if (peek(1) == '"') {
                    v += '"';
                    i_ += 2;
                    continue;
                }
                ++i_;
                break;
            }
            v += s_[i_++];
        }
        return make(Tok::StringLiteral, l, std::move(v));
    }

    Token number(LexResult& r, Loc l) {
        const std::size_t b = i_;
        auto digits = [&](bool based) {
            while (i_ < s_.size() &&
                   (isDigit(s_[i_]) || s_[i_] == '_' || (based && std::isxdigit(static_cast<unsigned char>(s_[i_])) != 0)))
                ++i_;
        };
        digits(false);
        // Sized bit string (VHDL-2008): 12X"ABC"
        if (isLetter(peek())) {
            std::size_t j = i_;
            while (j < s_.size() && isLetter(s_[j]))
                ++j;
            if (j < s_.size() && s_[j] == '"' && isBaseSpecifier(s_.substr(i_, j - i_))) {
                i_ = j;
                return bitString(r, l, b);
            }
        }
        if ((peek() == '#' || peek() == ':') && std::isxdigit(static_cast<unsigned char>(peek(1))) != 0) {
            const char delim = peek();
            ++i_;
            digits(true);
            if (peek() == '.') {
                ++i_;
                digits(true);
            }
            if (peek() == delim)
                ++i_;
            else
                r.errors.push_back({"malformed based literal", l});
        }
        else if (peek() == '.' && isDigit(peek(1))) {
            ++i_;
            digits(false);
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            i_ += 2;
            digits(false);
        }
        return make(Tok::AbstractLiteral, l, std::string(s_.substr(b, i_ - b)));
    }

    std::string_view s_;
    std::uint32_t file_;
    std::size_t i_ = 0;
    std::uint32_t line_ = 1;
    std::size_t lineStart_ = 0;
    Tok prev_ = Tok::End;
    Kw prevKw_ = Kw::None;
    bool skipping_ = false;
    Loc offLoc_;
    std::uint32_t offRegions_ = 0;
};

} // namespace

const char* keywordText(Kw k) noexcept {
    for (const auto& e : kKeywords)
        if (e.kw == k)
            return e.text;
    return "";
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool expandBitString(std::string_view literal, std::string& bits, std::string& error) {
    bits.clear();
    const auto q = literal.find('"');
    if (q == std::string_view::npos) {
        error = "malformed bit string literal";
        return false;
    }
    std::size_t p = 0;
    std::uint32_t size = 0;
    bool hasSize = false;
    while (p < q && literal[p] >= '0' && literal[p] <= '9') {
        size = size * 10 + static_cast<std::uint32_t>(literal[p] - '0');
        hasSize = true;
        ++p;
    }
    const std::string base = lowerAscii(literal.substr(p, q - p));
    const bool isSigned = base.size() == 2 && base[0] == 's';
    const char b = base.back();
    std::string digits;
    for (std::size_t k = q + 1; k < literal.size() && literal[k] != '"'; ++k)
        if (literal[k] != '_')
            digits += literal[k];

    if (b == 'd') {
        // decimal: value to binary (no other characters allowed)
        std::uint64_t v = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                error = "invalid digit in decimal bit string";
                return false;
            }
            const std::uint64_t next = v * 10 + static_cast<std::uint64_t>(c - '0');
            if (next / 10 != v) {
                error = "decimal bit string value too large";
                return false;
            }
            v = next;
        }
        std::string out;
        do {
            out.insert(out.begin(), static_cast<char>('0' + (v & 1U)));
            v >>= 1U;
        } while (v != 0);
        bits = out;
    }
    else {
        const int per = b == 'b' ? 1 : b == 'o' ? 3 : 4;
        for (const char c : digits) {
            const int v = digitValue(c);
            if (v < (1 << per)) {
                for (int k = per - 1; k >= 0; --k)
                    bits += static_cast<char>('0' + ((v >> k) & 1));
            }
            else if (std::isalnum(static_cast<unsigned char>(c)) != 0 && v < 36 &&
                     std::string_view("xXzZuUwWlLhH-").find(c) == std::string_view::npos) {
                error = std::string("invalid digit '") + c + "' in bit string";
                return false;
            }
            else {
                bits.append(static_cast<std::size_t>(per), c); // metavalue: replicated
            }
        }
    }
    if (hasSize) {
        if (bits.size() < size) {
            const char pad = isSigned && !bits.empty() ? bits.front() : '0';
            bits.insert(bits.begin(), size - bits.size(), pad);
        }
        else if (bits.size() > size) {
            const std::size_t cut = bits.size() - size;
            const char keep = isSigned ? bits[cut] : '0';
            for (std::size_t k = 0; k < cut; ++k)
                if (bits[k] != keep) {
                    error = "bit string literal does not fit in " + std::to_string(size) + " bits";
                    return false;
                }
            bits.erase(0, cut);
        }
    }
    return true;
}

LexResult lex(std::string_view text, std::uint32_t file) {
    // Skip a UTF-8 byte order mark.
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    return Lexer(text, file).run();
}

} // namespace opencdc::vhdl

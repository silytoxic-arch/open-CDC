// SDC/XDC subset: a small Tcl-like interpreter plus design object resolution.
//
// Supported Tcl: commands separated by newlines or ';', '#' comments,
// backslash-newline continuation, "quoted" and {braced} words, [command]
// substitution, $var / ${var} and `set`. Control flow (if/for/proc/...) is
// not supported and is reported. Only commands that matter for clock
// analysis are executed; every other command is counted and reported.

#include "constraints/Sdc.h"

#include "core/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace opencdc::constraints {

using graph::BitRef;

// ============================================================================
// Glob matching
// ============================================================================

bool globMatch(std::string_view p, std::string_view t, bool nocase) {
    auto eq = [nocase](char a, char b) {
        return nocase ? std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b))
                      : a == b;
    };
    std::size_t pi = 0;
    std::size_t ti = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starT = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == '*') {
            starP = pi++;
            starT = ti;
            continue;
        }
        if (pi < p.size()) {
            char pc = p[pi];
            std::size_t adv = 1;
            if (pc == '\\' && pi + 1 < p.size()) {
                pc = p[pi + 1];
                adv = 2;
                if (eq(pc, t[ti])) {
                    pi += adv;
                    ++ti;
                    continue;
                }
            }
            else if (pc == '?' || eq(pc, t[ti])) {
                pi += adv;
                ++ti;
                continue;
            }
        }
        if (starP != std::string_view::npos) {
            pi = starP + 1;
            ti = ++starT;
            continue;
        }
        return false;
    }
    while (pi < p.size() && p[pi] == '*')
        ++pi;
    return pi == p.size();
}

namespace {

/// Splits "base[idx]" (unescaped brackets) at the last '['.
std::optional<std::pair<std::string, std::string>> splitIndex(std::string_view pattern) {
    if (pattern.size() < 3 || pattern.back() != ']')
        return std::nullopt;
    // Find the matching '[' of the trailing ']' (allow escaped brackets).
    std::size_t open = std::string_view::npos;
    for (std::size_t i = pattern.size() - 1; i-- > 0;) {
        if (pattern[i] == '[') {
            open = i;
            break;
        }
    }
    if (open == std::string_view::npos || open == 0)
        return std::nullopt;
    std::size_t baseEnd = open;
    std::size_t idxEnd = pattern.size() - 1;
    if (pattern[open - 1] == '\\')
        --baseEnd;
    if (pattern[idxEnd - 1] == '\\')
        --idxEnd;
    return std::make_pair(std::string(pattern.substr(0, baseEnd)), std::string(pattern.substr(open + 1, idxEnd - open - 1)));
}

} // namespace

// ============================================================================
// Design object resolver
// ============================================================================

DesignObjectResolver::DesignObjectResolver(const graph::DesignView& view) : view_(view) {}

namespace {

std::vector<std::string> instancePrefixes(const ir::Design& d) {
    // Relative instance path prefixes ("" for tops, "u_rx/" ...).
    std::vector<std::string> prefix(d.instances.size());
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const auto& node = d.instances[i];
        if (node.parent.valid())
            prefix[i] = prefix[node.parent.index()] + node.name + "/";
    }
    return prefix;
}

} // namespace

void DesignObjectResolver::buildNames() {
    if (namesBuilt_)
        return;
    namesBuilt_ = true;
    const auto& d = view_.design();
    const auto prefix = instancePrefixes(d);
    auto named = [](std::string n, BitRef first, const ir::Net& net) {
        Named x;
        x.name = std::move(n);
        x.first = first;
        x.width = net.width;
        x.msb = net.msb;
        x.lsb = net.lsb;
        x.indexable = net.width > 1 && net.arrayDepth == 0;
        return x;
    };
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const ir::InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (const auto& p : body.ports) {
            if (!node.parent.valid())
                ports_.push_back(named(p.name, BitRef{inst, p.net, 0}, body.net(p.net)));
            else
                pins_.push_back(named(prefix[i] + p.name, BitRef{inst, p.net, 0}, body.net(p.net)));
        }
        for (std::size_t n = 0; n < body.nets.size(); ++n) {
            const auto& net = body.nets[n];
            if (net.kind != ir::NetKind::Temp)
                nets_.push_back(named(prefix[i] + net.name, BitRef{inst, ir::NetId{static_cast<std::uint32_t>(n)}, 0}, net));
        }
    }
}

void DesignObjectResolver::buildRegisters() {
    if (registersBuilt_)
        return;
    registersBuilt_ = true;
    const auto& d = view_.design();
    const auto prefix = instancePrefixes(d);
    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const ir::InstanceId inst{static_cast<std::uint32_t>(i)};
        const auto& body = d.body(d.instances[i].body);
        for (const auto& r : body.registers) {
            const auto& ch = r.q.chunks().front();
            const auto& net = body.net(ch.net);
            if (net.arrayDepth > 0)
                continue; // memories are not flip-flops after synthesis
            Named x;
            x.name = prefix[i] + net.name + "_reg";
            x.first = BitRef{inst, ch.net, ch.offset};
            x.width = ch.width;
            x.msb = net.indexOf(ch.offset + ch.width - 1);
            x.lsb = net.indexOf(ch.offset);
            x.indexable = net.width > 1;
            regs_.push_back(std::move(x));
            auto clk = view_.bitOf(inst, r.clock.signal, 0);
            regClock_.push_back(clk ? *clk : BitRef{});
            regsByNet_[(static_cast<std::uint64_t>(inst.value) << 32) | ch.net.value].push_back(regs_.size() - 1);
        }
    }
}

std::vector<Object> DesignObjectResolver::match(const std::vector<Named>& items, ObjectKind kind,
                                                std::string_view pattern, bool hierarchical, bool nocase) const {
    std::vector<Object> out;
    const auto split = splitIndex(pattern);
    auto candidates = [&](const std::string& name) {
        std::vector<std::string_view> c{name};
        if (hierarchical)
            for (std::size_t i = 0; i < name.size(); ++i)
                if (name[i] == '/')
                    c.push_back(std::string_view(name).substr(i + 1));
        return c;
    };
    for (const auto& it : items) {
        Object o;
        o.kind = kind;
        for (auto cand : candidates(it.name)) {
            if (globMatch(pattern, cand, nocase)) {
                o.name = it.name;
                if (!it.explicitBits.empty())
                    o.bits = it.explicitBits;
                else
                    for (std::uint32_t b = 0; b < it.width; ++b)
                        o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
                break;
            }
            if (split && it.indexable && globMatch(split->first, cand, nocase)) {
                for (std::uint32_t b = 0; b < it.width; ++b) {
                    const std::int32_t index = it.msb >= it.lsb ? it.lsb + static_cast<std::int32_t>(b)
                                                                : it.lsb - static_cast<std::int32_t>(b);
                    if (globMatch(split->second, std::to_string(index), nocase))
                        o.bits.push_back(BitRef{it.first.inst, it.first.net, it.first.bit + b});
                }
                if (!o.bits.empty()) {
                    o.name = it.name + "[" + split->second + "]";
                    break;
                }
            }
        }
        if (!o.bits.empty())
            out.push_back(std::move(o));
    }
    return out;
}

std::vector<Object> DesignObjectResolver::ports(std::string_view pattern, bool nocase) {
    buildNames();
    return match(ports_, ObjectKind::Port, pattern, false, nocase);
}

std::vector<Object> DesignObjectResolver::pins(std::string_view pattern, bool hierarchical, bool nocase) {
    buildNames();
    auto out = match(pins_, ObjectKind::Pin, pattern, hierarchical, nocase);
    // Synthesized flip-flop pins: <reg>_reg[/bit]/Q and /C (Vivado naming).
    const auto slash = pattern.rfind('/');
    if (slash == std::string_view::npos)
        return out;
    const auto pin = pattern.substr(slash + 1);
    const bool q = globMatch(pin, "Q", nocase);
    const bool c = globMatch(pin, "C", nocase);
    if (!q && !c)
        return out;
    buildRegisters();
    const auto regPattern = pattern.substr(0, slash);
    for (auto o : match(regs_, ObjectKind::Pin, regPattern, hierarchical, nocase)) {
        if (q) {
            Object qo = o;
            qo.name += "/Q";
            out.push_back(std::move(qo));
        }
        if (c) {
            // Map every matched Q bit to the clock of its register.
            Object co;
            co.kind = ObjectKind::Pin;
            co.name = o.name + "/C";
            for (const auto& b : o.bits) {
                auto it = regsByNet_.find((static_cast<std::uint64_t>(b.inst.value) << 32) | b.net.value);
                if (it == regsByNet_.end())
                    continue;
                for (const std::size_t r : it->second) {
                    const auto& reg = regs_[r];
                    if (reg.first.inst == b.inst && reg.first.net == b.net && b.bit >= reg.first.bit &&
                        b.bit < reg.first.bit + reg.width && regClock_[r].valid()) {
                        if (std::ranges::find(co.bits, regClock_[r]) == co.bits.end())
                            co.bits.push_back(regClock_[r]);
                        break;
                    }
                }
            }
            if (!co.bits.empty())
                out.push_back(std::move(co));
        }
    }
    return out;
}

std::vector<Object> DesignObjectResolver::nets(std::string_view pattern, bool hierarchical, bool nocase) {
    buildNames();
    return match(nets_, ObjectKind::Net, pattern, hierarchical, nocase);
}

// ============================================================================
// Tcl subset
// ============================================================================

namespace {

struct Word {
    enum class Kind { Bare, Quoted, Braced } kind = Kind::Bare;
    std::string raw;
};

struct Command {
    std::vector<Word> words;
    std::uint32_t line = 0;
};

struct Value {
    std::string text;
    std::vector<Object> objects;
    bool hasObjects = false;
};

/// Splits a script into commands and words (no substitution yet).
class Tokenizer {
public:
    Tokenizer(std::string_view text, std::uint32_t firstLine) : s_(text), line_(firstLine) {}

    /// Returns false at end of input. `error` is set on malformed input.
    bool next(Command& cmd, std::string& error) {
        cmd = {};
        // Skip separators and comments.
        for (;;) {
            while (i_ < s_.size() && (isSpace(s_[i_]) || s_[i_] == '\n' || s_[i_] == ';')) {
                if (s_[i_] == '\n')
                    ++line_;
                ++i_;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == '\n') {
                i_ += 2;
                ++line_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '#') {
                while (i_ < s_.size() && s_[i_] != '\n') {
                    if (s_[i_] == '\\' && i_ + 1 < s_.size() && s_[i_ + 1] == '\n') {
                        ++line_;
                        ++i_;
                    }
                    ++i_;
                }
                continue;
            }
            break;
        }
        if (i_ >= s_.size())
            return false;
        cmd.line = line_;
        for (;;) {
            while (i_ < s_.size() && (isSpace(s_[i_]) || (s_[i_] == '\\' && i_ + 1 < s_.size() && s_[i_ + 1] == '\n'))) {
                if (s_[i_] == '\\') {
                    ++line_;
                    i_ += 2;
                }
                else {
                    ++i_;
                }
            }
            if (i_ >= s_.size() || s_[i_] == '\n' || s_[i_] == ';')
                break;
            Word w;
            if (s_[i_] == '{') {
                w.kind = Word::Kind::Braced;
                if (!readBraced(w.raw)) {
                    error = "missing close-brace";
                    return true;
                }
            }
            else if (s_[i_] == '"') {
                w.kind = Word::Kind::Quoted;
                if (!readQuoted(w.raw)) {
                    error = "missing close-quote";
                    return true;
                }
            }
            else if (!readBare(w.raw)) {
                error = "missing close-bracket";
                return true;
            }
            cmd.words.push_back(std::move(w));
        }
        return true;
    }

private:
    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

    bool readBraced(std::string& out) {
        int depth = 1;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                out += c;
                out += s_[i_ + 1];
                if (s_[i_ + 1] == '\n')
                    ++line_;
                i_ += 2;
                continue;
            }
            if (c == '\n')
                ++line_;
            if (c == '{')
                ++depth;
            if (c == '}' && --depth == 0) {
                ++i_;
                return true;
            }
            out += c;
            ++i_;
        }
        return false;
    }

    bool readBracket(std::string& out) {
        // s_[i_] == '['; copies up to and including the matching ']'.
        int depth = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                out += c;
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == '{') {
                std::string inner;
                if (!readBraced(inner))
                    return false;
                out += "{" + inner + "}";
                continue;
            }
            if (c == '\n')
                ++line_;
            out += c;
            ++i_;
            if (c == '[')
                ++depth;
            else if (c == ']' && --depth == 0)
                return true;
        }
        return false;
    }

    bool readQuoted(std::string& out) {
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                out += s_[i_];
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (s_[i_] == '[') {
                if (!readBracket(out))
                    return false;
                continue;
            }
            if (s_[i_] == '\n')
                ++line_;
            out += s_[i_++];
        }
        if (i_ >= s_.size())
            return false;
        ++i_;
        return true;
    }

    bool readBare(std::string& out) {
        while (i_ < s_.size() && !isSpace(s_[i_]) && s_[i_] != '\n' && s_[i_] != ';') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                if (s_[i_ + 1] == '\n')
                    break; // continuation ends the word
                out += s_[i_];
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (s_[i_] == '[') {
                if (!readBracket(out))
                    return false;
                continue;
            }
            out += s_[i_++];
        }
        return true;
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::uint32_t line_;
};

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            break;
        std::string item;
        if (text[i] == '{') {
            int depth = 1;
            ++i;
            while (i < text.size() && depth > 0) {
                if (text[i] == '{')
                    ++depth;
                else if (text[i] == '}' && --depth == 0)
                    break;
                item += text[i++];
            }
            ++i;
        }
        else {
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                item += text[i++];
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::optional<double> toNumber(std::string_view s) {
    double v = 0;
    std::string tmp(s);
    char* end = nullptr;
    v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0')
        return std::nullopt;
    return v;
}

const std::set<std::string, std::less<>>& knownCommands() {
    static const std::set<std::string, std::less<>> cmds = {
        "set", "create_clock", "create_generated_clock", "set_clock_groups", "set_input_delay", "get_ports", "get_pins", "get_nets",
        "get_clocks", "all_clocks", "get_cells", "all_inputs", "all_outputs", "all_registers"};
    return cmds;
}

/// Commands that are valid SDC/XDC but not used by clock analysis yet.
bool isTimingOrMiscCommand(std::string_view c) {
    static const std::set<std::string, std::less<>> cmds = {
        "set_output_delay", "set_false_path", "set_max_delay", "set_min_delay",
        "set_multicycle_path", "set_clock_uncertainty", "set_clock_latency", "set_input_jitter",
        "set_system_jitter", "set_clock_sense", "set_case_analysis", "set_property", "set_load",
        "set_drive", "set_driving_cell", "set_input_transition", "set_disable_timing", "set_units",
        "current_design", "current_instance", "set_max_fanout", "set_max_transition", "set_bus_skew",
        "set_max_time_borrow", "set_propagated_clock", "set_ideal_network", "set_dont_touch",
        "set_operating_conditions", "set_wire_load_model", "group_path", "set_clock_transition",
        "create_pblock", "add_cells_to_pblock", "resize_pblock", "set_clock_gating_check", "set_data_check",
        "set_external_delay", "config_timing_pessimism", "set_assert", "set_hierarchy_separator"};
    return cmds.contains(c);
}

class Interp {
public:
    Interp(std::string file, IObjectResolver& resolver, ConstraintSet& out, DiagnosticSink& sink)
        : file_(std::move(file)), resolver_(resolver), out_(out), sink_(sink) {}

    Value runScript(std::string_view text, std::uint32_t firstLine) {
        Tokenizer tok(text, firstLine);
        Command cmd;
        Value last;
        for (;;) {
            std::string error;
            if (!tok.next(cmd, error))
                break;
            if (!error.empty()) {
                diag("CON-PARSE", Severity::Error, error, cmd.line);
                break;
            }
            if (!cmd.words.empty())
                last = exec(cmd);
        }
        return last;
    }

    void reportIgnored() {
        if (ignored_.empty())
            return;
        std::string list;
        for (const auto& [name, count] : ignored_) {
            out_.ignoredCommands[name] += count;
            if (!list.empty())
                list += ", ";
            list += name + " x" + std::to_string(count);
        }
        diag("CON-IGNORED", Severity::Info,
             "commands not used by clock analysis (recorded, not applied): " + list, 0);
    }

private:
    void diag(const std::string& code, Severity sev, const std::string& msg, std::uint32_t line) {
        sink_.report({code, sev, msg, {file_, line, 0}, {}});
    }

    Value subst(const Word& w, std::uint32_t line) {
        if (w.kind == Word::Kind::Braced)
            return Value{w.raw, {}, false};
        const std::string& raw = w.raw;
        // A word that is exactly one command substitution keeps its objects.
        if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']' && matchingBracket(raw, 0) == raw.size() - 1) {
            auto inner = std::string_view(raw).substr(1, raw.size() - 2);
            if (looksLikeCommand(inner))
                return runScript(inner, line);
        }
        Value v;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '\\' && i + 1 < raw.size()) {
                const char n = raw[++i];
                v.text += n == 'n' ? '\n' : n == 't' ? '\t' : n;
                continue;
            }
            if (c == '$') {
                std::string name;
                if (i + 1 < raw.size() && raw[i + 1] == '{') {
                    auto close = raw.find('}', i + 2);
                    if (close == std::string::npos) {
                        v.text += c;
                        continue;
                    }
                    name = raw.substr(i + 2, close - i - 2);
                    i = close;
                }
                else {
                    std::size_t j = i + 1;
                    while (j < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[j])) || raw[j] == '_' ||
                                              raw[j] == ':'))
                        ++j;
                    if (j == i + 1) {
                        v.text += c;
                        continue;
                    }
                    name = raw.substr(i + 1, j - i - 1);
                    i = j - 1;
                }
                auto it = vars_.find(name);
                if (it == vars_.end()) {
                    diag("CON-PARSE", Severity::Warning, "undefined variable '$" + name + "'", line);
                    continue;
                }
                v.text += it->second.text;
                if (it->second.hasObjects) {
                    v.objects.insert(v.objects.end(), it->second.objects.begin(), it->second.objects.end());
                    v.hasObjects = true;
                }
                continue;
            }
            if (c == '[') {
                const auto close = matchingBracket(raw, i);
                if (close != std::string::npos) {
                    auto inner = std::string_view(raw).substr(i + 1, close - i - 1);
                    if (looksLikeCommand(inner)) {
                        Value r = runScript(inner, line);
                        v.text += r.text;
                        if (r.hasObjects) {
                            v.objects.insert(v.objects.end(), r.objects.begin(), r.objects.end());
                            v.hasObjects = true;
                        }
                        i = close;
                        continue;
                    }
                }
                // Not a command (e.g. data[3], bus[*]): keep literally.
            }
            v.text += c;
        }
        return v;
    }

    static std::size_t matchingBracket(const std::string& s, std::size_t open) {
        int depth = 0;
        for (std::size_t i = open; i < s.size(); ++i) {
            if (s[i] == '\\') {
                ++i;
                continue;
            }
            if (s[i] == '[')
                ++depth;
            else if (s[i] == ']' && --depth == 0)
                return i;
        }
        return std::string::npos;
    }

    bool looksLikeCommand(std::string_view inner) const {
        std::size_t i = 0;
        while (i < inner.size() && std::isspace(static_cast<unsigned char>(inner[i])))
            ++i;
        std::size_t j = i;
        while (j < inner.size() && !std::isspace(static_cast<unsigned char>(inner[j])))
            ++j;
        const auto name = inner.substr(i, j - i);
        return knownCommands().contains(name) || isTimingOrMiscCommand(name) || name == "expr" ||
               name == "list" || name == "concat";
    }

    struct Args {
        std::map<std::string, std::vector<Value>> opts;
        std::set<std::string> flags;
        std::vector<Value> positional;
    };

    /// spec: option -> takes a value.
    Args parseArgs(const Command& cmd, const std::string& name, const std::map<std::string, bool>& spec) {
        Args a;
        for (std::size_t i = 1; i < cmd.words.size(); ++i) {
            const Word& w = cmd.words[i];
            if (w.kind == Word::Kind::Bare && !w.raw.empty() && w.raw[0] == '-' && w.raw.size() > 1 &&
                !std::isdigit(static_cast<unsigned char>(w.raw[1]))) {
                std::string opt = w.raw;
                if (opt == "-hier")
                    opt = "-hierarchical";
                auto it = spec.find(opt);
                if (it == spec.end()) {
                    // Most options take a value: skip it too unless it is another option.
                    const bool nextIsValue = i + 1 < cmd.words.size() &&
                                             !(cmd.words[i + 1].kind == Word::Kind::Bare &&
                                               cmd.words[i + 1].raw.starts_with('-'));
                    diag("CON-UNKNOWN-OPTION", Severity::Warning,
                         name + ": unknown option '" + w.raw + "' ignored" +
                             (nextIsValue ? " (together with its value '" + cmd.words[i + 1].raw + "')" : ""),
                         cmd.line);
                    if (nextIsValue)
                        ++i;
                    continue;
                }
                if (it->second) {
                    if (i + 1 >= cmd.words.size()) {
                        diag("CON-PARSE", Severity::Error, name + ": option " + opt + " requires a value", cmd.line);
                        continue;
                    }
                    a.opts[opt].push_back(subst(cmd.words[++i], cmd.line));
                }
                else {
                    a.flags.insert(opt);
                }
                continue;
            }
            a.positional.push_back(subst(w, cmd.line));
        }
        return a;
    }

    std::vector<Object> objectsOf(const Value& v, const Command& cmd, const std::string& ctx) {
        if (v.hasObjects)
            return v.objects;
        // Plain names: search ports, then pins, then nets (Synopsys behaviour).
        std::vector<Object> out;
        for (const auto& pat : splitList(v.text)) {
            auto r = resolver_.ports(pat, false);
            if (r.empty())
                r = resolver_.pins(pat, false, false);
            if (r.empty())
                r = resolver_.nets(pat, false, false);
            if (r.empty())
                diag("CON-UNMATCHED", Severity::Warning, ctx + ": no port, pin or net matches '" + pat + "'", cmd.line);
            out.insert(out.end(), r.begin(), r.end());
        }
        return out;
    }

    Value objectsValue(std::vector<Object> objs) {
        Value v;
        for (const auto& o : objs) {
            if (!v.text.empty())
                v.text += ' ';
            v.text += o.name;
        }
        v.objects = std::move(objs);
        v.hasObjects = true;
        return v;
    }

    Value getObjects(const Command& cmd, const std::string& name) {
        const bool clocks = name == "get_clocks";
        std::map<std::string, bool> spec = {{"-quiet", false}, {"-nocase", false}, {"-regexp", false},
                                            {"-filter", true},  {"-of_objects", true}, {"-hierarchical", false},
                                            {"-include_generated_clocks", false}};
        Args a = parseArgs(cmd, name, spec);
        const bool quiet = a.flags.contains("-quiet");
        const bool nocase = a.flags.contains("-nocase");
        const bool hier = a.flags.contains("-hierarchical");
        if (a.flags.contains("-regexp"))
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning, name + ": -regexp is not supported; patterns are treated as globs",
                 cmd.line);
        if (a.opts.contains("-filter"))
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning, name + ": -filter is not supported and was ignored", cmd.line);
        if (a.opts.contains("-of_objects")) {
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                 name + ": -of_objects is not supported; the command returns nothing", cmd.line);
            return objectsValue({});
        }
        std::vector<Object> result;
        for (const auto& pv : a.positional) {
            std::vector<std::string> patterns;
            if (pv.hasObjects)
                for (const auto& o : pv.objects)
                    patterns.push_back(o.name);
            else
                patterns = splitList(pv.text);
            for (const auto& pat : patterns) {
                if (clocks) {
                    result.push_back({ObjectKind::Clock, pat, {}});
                    continue;
                }
                std::vector<Object> r = name == "get_ports" ? resolver_.ports(pat, nocase)
                                        : name == "get_pins" ? resolver_.pins(pat, hier, nocase)
                                                             : resolver_.nets(pat, hier, nocase);
                if (r.empty() && !quiet)
                    diag("CON-UNMATCHED", Severity::Warning, name + ": nothing matches '" + pat + "'", cmd.line);
                result.insert(result.end(), r.begin(), r.end());
            }
        }
        return objectsValue(std::move(result));
    }

    static std::vector<BitRef> bitsOf(const std::vector<Object>& objs) {
        std::vector<BitRef> out;
        for (const auto& o : objs)
            out.insert(out.end(), o.bits.begin(), o.bits.end());
        return out;
    }

    void addClock(ClockDef def, std::uint32_t line) {
        if (!def.add) {
            auto it = std::ranges::find(out_.clocks, def.name, &ClockDef::name);
            if (it != out_.clocks.end()) {
                diag("CON-DUPLICATE-CLOCK", Severity::Warning,
                     "clock '" + def.name + "' redefined; the earlier definition (" + toString(it->loc) + ") is replaced",
                     line);
                out_.clocks.erase(it);
            }
        }
        out_.clocks.push_back(std::move(def));
    }

    Value createClock(const Command& cmd) {
        Args a = parseArgs(cmd, "create_clock",
                           {{"-name", true}, {"-period", true}, {"-waveform", true}, {"-add", false}, {"-comment", true}});
        ClockDef def;
        def.loc = {file_, cmd.line, 0};
        def.add = a.flags.contains("-add");
        if (a.opts.contains("-period")) {
            auto p = toNumber(a.opts["-period"].back().text);
            if (!p || *p <= 0)
                diag("CON-PARSE", Severity::Error, "create_clock: invalid -period '" + a.opts["-period"].back().text + "'",
                     cmd.line);
            else
                def.periodNs = *p;
        }
        else {
            diag("CON-PARSE", Severity::Warning, "create_clock without -period", cmd.line);
        }
        if (a.opts.contains("-waveform"))
            for (const auto& w : splitList(a.opts["-waveform"].back().text))
                if (auto n = toNumber(w))
                    def.waveform.push_back(*n);
        std::vector<Object> objs;
        for (const auto& pv : a.positional) {
            auto o = objectsOf(pv, cmd, "create_clock");
            objs.insert(objs.end(), o.begin(), o.end());
        }
        def.sources = bitsOf(objs);
        for (const auto& o : objs)
            def.sourceNames.push_back(o.name);
        if (a.opts.contains("-name"))
            def.name = a.opts["-name"].back().text;
        else if (!objs.empty())
            def.name = objs.front().name;
        if (def.name.empty()) {
            diag("CON-PARSE", Severity::Error, "create_clock: a virtual clock needs -name", cmd.line);
            return {};
        }
        const std::string name = def.name;
        addClock(std::move(def), cmd.line);
        return Value{name, {{ObjectKind::Clock, name, {}}}, true};
    }

    Value createGeneratedClock(const Command& cmd) {
        Args a = parseArgs(cmd, "create_generated_clock",
                           {{"-name", true}, {"-source", true}, {"-master_clock", true}, {"-divide_by", true},
                            {"-multiply_by", true}, {"-invert", false}, {"-edges", true}, {"-edge_shift", true},
                            {"-duty_cycle", true}, {"-add", false}, {"-combinational", false}, {"-comment", true}});
        ClockDef def;
        def.generated = true;
        def.loc = {file_, cmd.line, 0};
        def.add = a.flags.contains("-add");
        def.invert = a.flags.contains("-invert");
        if (a.opts.contains("-edges") || a.opts.contains("-edge_shift"))
            diag("CON-UNSUPPORTED-OPTION", Severity::Warning,
                 "create_generated_clock: -edges/-edge_shift are not supported; the clock is treated as synchronous "
                 "to its master without waveform",
                 cmd.line);
        auto intOpt = [&](const char* opt, int& dst) {
            if (!a.opts.contains(opt))
                return;
            auto n = toNumber(a.opts[opt].back().text);
            if (!n || *n < 1)
                diag("CON-PARSE", Severity::Error, std::string("create_generated_clock: invalid ") + opt, cmd.line);
            else
                dst = static_cast<int>(*n);
        };
        intOpt("-divide_by", def.divideBy);
        intOpt("-multiply_by", def.multiplyBy);
        if (a.opts.contains("-source"))
            def.masterSource = bitsOf(objectsOf(a.opts["-source"].back(), cmd, "create_generated_clock -source"));
        if (a.opts.contains("-master_clock")) {
            const auto& mv = a.opts["-master_clock"].back();
            def.masterClock = mv.hasObjects && !mv.objects.empty() ? mv.objects.front().name : mv.text;
        }
        if (def.masterSource.empty() && def.masterClock.empty())
            diag("CON-PARSE", Severity::Error, "create_generated_clock: -source or -master_clock is required", cmd.line);
        std::vector<Object> objs;
        for (const auto& pv : a.positional) {
            auto o = objectsOf(pv, cmd, "create_generated_clock");
            objs.insert(objs.end(), o.begin(), o.end());
        }
        def.sources = bitsOf(objs);
        for (const auto& o : objs)
            def.sourceNames.push_back(o.name);
        def.name = a.opts.contains("-name") ? a.opts["-name"].back().text : (objs.empty() ? "" : objs.front().name);
        if (def.name.empty() || def.sources.empty()) {
            diag("CON-PARSE", Severity::Error, "create_generated_clock: no target object", cmd.line);
            return {};
        }
        const std::string name = def.name;
        addClock(std::move(def), cmd.line);
        return Value{name, {{ObjectKind::Clock, name, {}}}, true};
    }

    Value setInputDelay(const Command& cmd) {
        Args a = parseArgs(cmd, "set_input_delay",
                           {{"-clock", true}, {"-clock_fall", false}, {"-max", false}, {"-min", false}, {"-add_delay", false},
                            {"-rise", false}, {"-fall", false}, {"-network_latency_included", false},
                            {"-source_latency_included", false}, {"-reference_pin", true}});
        if (!a.opts.contains("-clock")) {
            // A delay without a clock does not assign a domain.
            ++ignored_["set_input_delay (without -clock)"];
            return {};
        }
        InputDelay d;
        d.loc = {file_, cmd.line, 0};
        d.clockFall = a.flags.contains("-clock_fall");
        const auto& cv = a.opts["-clock"].back();
        d.clock = cv.hasObjects && !cv.objects.empty() ? cv.objects.front().name : cv.text;
        // Positional arguments: <delay_value> <port_pin_list> (the number is skipped).
        for (const auto& pv : a.positional) {
            if (!pv.hasObjects && toNumber(pv.text))
                continue;
            auto objs = objectsOf(pv, cmd, "set_input_delay");
            for (const auto& o : objs) {
                d.portNames.push_back(o.name);
                d.bits.insert(d.bits.end(), o.bits.begin(), o.bits.end());
            }
        }
        if (d.bits.empty()) {
            diag("CON-PARSE", Severity::Warning, "set_input_delay: no port given", cmd.line);
            return {};
        }
        out_.inputDelays.push_back(std::move(d));
        return {};
    }

    Value setClockGroups(const Command& cmd) {
        Args a = parseArgs(cmd, "set_clock_groups",
                           {{"-name", true}, {"-group", true}, {"-asynchronous", false}, {"-logically_exclusive", false},
                            {"-physically_exclusive", false}, {"-allow_paths", false}, {"-comment", true}});
        ClockGroups g;
        g.loc = {file_, cmd.line, 0};
        if (a.opts.contains("-name"))
            g.name = a.opts["-name"].back().text;
        const int kinds = static_cast<int>(a.flags.contains("-asynchronous")) +
                          static_cast<int>(a.flags.contains("-logically_exclusive")) +
                          static_cast<int>(a.flags.contains("-physically_exclusive"));
        if (kinds != 1) {
            diag("CON-PARSE", Severity::Error,
                 "set_clock_groups needs exactly one of -asynchronous, -logically_exclusive, -physically_exclusive",
                 cmd.line);
            return {};
        }
        g.kind = a.flags.contains("-asynchronous")          ? GroupKind::Asynchronous
                 : a.flags.contains("-logically_exclusive") ? GroupKind::LogicallyExclusive
                                                            : GroupKind::PhysicallyExclusive;
        for (const auto& gv : a.opts["-group"]) {
            std::vector<std::string> names;
            if (gv.hasObjects)
                for (const auto& o : gv.objects)
                    names.push_back(o.name);
            else
                names = splitList(gv.text);
            g.groups.push_back(std::move(names));
        }
        if (g.groups.empty()) {
            diag("CON-PARSE", Severity::Error, "set_clock_groups without -group", cmd.line);
            return {};
        }
        out_.groups.push_back(std::move(g));
        return {};
    }

    Value exec(const Command& cmd) {
        const Value head = subst(cmd.words[0], cmd.line);
        const std::string& name = head.text;
        if (name == "set") {
            if (cmd.words.size() == 3) {
                Value v = subst(cmd.words[2], cmd.line);
                vars_[subst(cmd.words[1], cmd.line).text] = v;
                return v;
            }
            if (cmd.words.size() == 2) {
                auto it = vars_.find(subst(cmd.words[1], cmd.line).text);
                return it == vars_.end() ? Value{} : it->second;
            }
            diag("CON-PARSE", Severity::Error, "set: wrong number of arguments", cmd.line);
            return {};
        }
        if (name == "create_clock")
            return createClock(cmd);
        if (name == "create_generated_clock")
            return createGeneratedClock(cmd);
        if (name == "set_input_delay")
            return setInputDelay(cmd);
        if (name == "set_clock_groups")
            return setClockGroups(cmd);
        if (name == "get_ports" || name == "get_pins" || name == "get_nets" || name == "get_clocks")
            return getObjects(cmd, name);
        if (name == "all_clocks")
            return objectsValue({{ObjectKind::Clock, "*", {}}});
        if (name == "list" || name == "concat") {
            Value v;
            for (std::size_t i = 1; i < cmd.words.size(); ++i) {
                Value w = subst(cmd.words[i], cmd.line);
                if (!v.text.empty())
                    v.text += ' ';
                v.text += cmd.words[i].kind == Word::Kind::Braced && name == "list" ? "{" + w.text + "}" : w.text;
                if (w.hasObjects) {
                    v.objects.insert(v.objects.end(), w.objects.begin(), w.objects.end());
                    v.hasObjects = true;
                }
            }
            return v;
        }
        if (name == "get_cells" || name == "all_inputs" || name == "all_outputs" || name == "all_registers") {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning, name + " is not supported; it returns nothing", cmd.line);
            return objectsValue({});
        }
        if (name == "if" || name == "for" || name == "foreach" || name == "while" || name == "proc" ||
            name == "source" || name == "expr" || name == "switch" || name == "eval") {
            diag("CON-UNSUPPORTED-COMMAND", Severity::Warning,
                 "Tcl command '" + name + "' is not supported; the command was skipped", cmd.line);
            return {};
        }
        if (isTimingOrMiscCommand(name)) {
            ++ignored_[name];
            return {};
        }
        diag("CON-UNKNOWN-COMMAND", Severity::Warning, "unknown command '" + name + "' skipped", cmd.line);
        return {};
    }

    std::string file_;
    IObjectResolver& resolver_;
    ConstraintSet& out_;
    DiagnosticSink& sink_;
    std::unordered_map<std::string, Value> vars_;
    std::map<std::string, int> ignored_;
};

} // namespace

void parseSdc(std::string_view text, const std::string& fileName, IObjectResolver& resolver, ConstraintSet& out,
              DiagnosticSink& sink) {
    Interp interp(fileName, resolver, out, sink);
    interp.runScript(text, 1);
    interp.reportIgnored();
}

bool readSdcFile(const std::filesystem::path& file, IObjectResolver& resolver, ConstraintSet& out,
                 DiagnosticSink& sink) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        sink.report({"CON-OPEN", Severity::Error, "cannot open constraint file", {toUtf8(file), 0, 0}, {}});
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    out.files.push_back(file);
    parseSdc(text.str(), toUtf8(file), resolver, out, sink);
    return true;
}

} // namespace opencdc::constraints

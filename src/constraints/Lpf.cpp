#include "constraints/Lpf.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

namespace opencdc::constraints {

namespace {

std::string upper(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

struct Word {
    std::string text;
    bool quoted = false;
};

struct Statement {
    std::vector<Word> words;
    std::uint32_t line = 1;
};

/// Statements end with ';'; '#' and '//' start a comment to the end of the line.
std::vector<Statement> statements(std::string_view t) {
    std::vector<Statement> out;
    Statement cur;
    std::uint32_t line = 1;
    std::size_t i = 0;
    const auto flush = [&] {
        if (!cur.words.empty())
            out.push_back(std::move(cur));
        cur = {};
    };
    while (i < t.size()) {
        const char c = t[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '#' || (c == '/' && i + 1 < t.size() && t[i + 1] == '/')) {
            while (i < t.size() && t[i] != '\n')
                ++i;
            continue;
        }
        if (c == ';') {
            flush();
            ++i;
            continue;
        }
        if (cur.words.empty())
            cur.line = line;
        Word w;
        if (c == '"') {
            w.quoted = true;
            ++i;
            while (i < t.size() && t[i] != '"') {
                if (t[i] == '\n')
                    ++line;
                w.text += t[i++];
            }
            ++i; // the closing quote
        }
        else {
            while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != ';' && t[i] != '"')
                w.text += t[i++];
        }
        cur.words.push_back(std::move(w));
    }
    flush();
    return out;
}

/// "{name}" for a Tcl word: braces keep brackets and '$' literal.
std::string braced(const std::string& s) { return "{" + s + "}"; }

std::optional<double> number(const std::string& s) {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used == s.size() && v > 0)
            return v;
    }
    catch (const std::exception&) {
    }
    return std::nullopt;
}

/// The period in ns of "<value> <unit>" (MHZ, KHZ, HZ, GHZ, NS, PS, US).
std::optional<double> periodNs(const std::string& value, const std::string& unitIn, bool frequency) {
    const auto v = number(value);
    if (!v)
        return std::nullopt;
    const std::string unit = upper(unitIn);
    if (frequency) {
        double mhz = 0;
        if (unit == "MHZ")
            mhz = *v;
        else if (unit == "KHZ")
            mhz = *v / 1e3;
        else if (unit == "HZ")
            mhz = *v / 1e6;
        else if (unit == "GHZ")
            mhz = *v * 1e3;
        else
            return std::nullopt;
        return 1000.0 / mhz;
    }
    if (unit == "NS")
        return *v;
    if (unit == "PS")
        return *v / 1e3;
    if (unit == "US")
        return *v * 1e3;
    return std::nullopt;
}

std::string text(double v) {
    std::ostringstream s;
    s.precision(12);
    s << v;
    return s.str();
}

/// An endpoint of BLOCK PATH: PORT, NET, CELL or CLKNET "name".
std::optional<std::string> endpoint(const std::string& kind, const std::string& name, bool& clock) {
    const std::string k = upper(kind);
    clock = false;
    if (k == "PORT")
        return "[get_ports " + braced(name) + "]";
    if (k == "NET")
        return "[get_nets " + braced(name) + "]";
    if (k == "CELL" || k == "COMP")
        return "[get_cells " + braced(name) + "]";
    if (k == "CLKNET") {
        clock = true;
        return "[get_clocks -of_objects [get_nets " + braced(name) + "]]";
    }
    return std::nullopt;
}

} // namespace

bool isLpfFile(std::string_view fileName) {
    if (fileName.size() < 4)
        return false;
    return upper(std::string(fileName.substr(fileName.size() - 4))) == ".LPF";
}

std::string lpfToSdc(std::string_view input, const std::string& fileName, DiagnosticSink& sink) {
    const auto stmts = statements(input);
    std::uint32_t lines = 1;
    for (const char c : input)
        lines += c == '\n' ? 1U : 0U;
    std::vector<std::string> out(lines + 1);
    std::map<std::string, int> ignored; // timing preferences that are not used
    const auto emit = [&](std::uint32_t line, std::string cmd) {
        auto& slot = out[std::min<std::size_t>(line, out.size() - 1)];
        slot += (slot.empty() ? "" : "; ") + cmd;
    };
    const auto warn = [&](const Statement& s, std::string msg) {
        sink.report({"CON-LPF-PARSE", Severity::Warning, std::move(msg), {fileName, s.line, 0}, {}});
    };
    static const std::vector<std::string> kTiming = {
        "MULTICYCLE", "MAXDELAY", "INPUT_SETUP", "CLOCK_TO_OUT", "CLKSKEWDIFF", "MAXSKEW", "PROHIBIT_PATH",
        "HOLD_MARGIN", "OFFSET", "PROPDELAY", "DEFINE"};
    for (const auto& s : stmts) {
        const std::string head = upper(s.words[0].text);
        const auto word = [&](std::size_t k) -> std::string { return k < s.words.size() ? s.words[k].text : ""; };
        const auto key = [&](std::size_t k) { return upper(word(k)); };
        if (head == "FREQUENCY" || head == "PERIOD") {
            const bool freq = head == "FREQUENCY";
            const std::string kind = key(1);
            if (kind != "PORT" && kind != "NET") {
                // FREQUENCY 100 MHZ: a default for unconstrained clocks, not a clock
                ++ignored[head + (kind.empty() ? "" : " " + (number(word(1)) ? std::string("(global)") : kind))];
                continue;
            }
            const auto p = periodNs(word(3), word(4), freq);
            if (word(2).empty() || !p) {
                warn(s, head + ": expected " + head + " " + kind + " \"<name>\" <value> " + (freq ? "MHZ" : "NS"));
                continue;
            }
            const std::string name = word(2);
            emit(s.line, "create_clock -name " + braced(name) + " -period " + text(*p) + " " +
                             (kind == "PORT" ? "[get_ports " : "[get_nets ") + braced(name) + "]");
            continue;
        }
        if (head == "BLOCK") {
            const std::string what = key(1);
            if (what == "RESETPATHS" || what == "ASYNCPATHS" || what == "JTAGPATHS")
                continue; // paths from asynchronous inputs, resets, JTAG: not timed by CDC analysis either
            if (what == "PATH") {
                // BLOCK PATH FROM <kind> "a" TO <kind> "b" (either side may be missing)
                std::string from, to;
                bool ok = true;
                for (std::size_t k = 2; k < s.words.size() && ok; k += 3) {
                    const std::string dir = key(k);
                    bool clk = false;
                    const auto e = k + 2 < s.words.size() && (dir == "FROM" || dir == "TO")
                                       ? endpoint(word(k + 1), word(k + 2), clk)
                                       : std::nullopt;
                    if (e)
                        (dir == "FROM" ? from : to) = *e;
                    else
                        ok = false;
                }
                if (!ok || (from.empty() && to.empty())) {
                    warn(s, "BLOCK PATH: only FROM/TO with PORT, NET, CELL or CLKNET \"<name>\" endpoints are read");
                    continue;
                }
                emit(s.line, "set_false_path" + (from.empty() ? std::string() : " -from " + from) +
                                 (to.empty() ? std::string() : " -to " + to));
                continue;
            }
            if (what == "INTERCLOCKDOMAIN") {
                warn(s, "BLOCK INTERCLOCKDOMAIN PATHS is not read: clock pairs keep the relation found by the "
                        "analysis (declare the unrelated clocks with BLOCK PATH FROM CLKNET ... TO CLKNET ...)");
                continue;
            }
            ++ignored["BLOCK " + what];
            continue;
        }
        if (std::ranges::find(kTiming, head) != kTiming.end()) {
            ++ignored[head];
            continue;
        }
        // physical and device preferences (LOCATE, IOBUF, SYSCONFIG, COMMERCIAL, BANK, USE, UGROUP, ...)
    }
    if (!ignored.empty()) {
        std::vector<std::string> notes;
        for (const auto& [k, n] : ignored)
            notes.push_back(k + (n > 1 ? " (x" + std::to_string(n) + ")" : ""));
        sink.report({"CON-LPF-IGNORED", Severity::Info,
                     "timing preferences not used by the CDC analysis: " + std::to_string(ignored.size()) + " kind(s)",
                     {fileName, 0, 0}, std::move(notes)});
    }
    std::string sdc;
    for (std::size_t l = 1; l < out.size(); ++l) {
        sdc += out[l];
        sdc += '\n';
    }
    return sdc;
}

} // namespace opencdc::constraints

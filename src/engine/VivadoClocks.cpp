#include "engine/VivadoClocks.h"

#include "engine/ClockNaming.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace opencdc::engine {

namespace {

/// Whitespace-separated tokens; "{a b}" stays one token.
std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (const char c : line) {
        if (c == '{')
            ++depth;
        if (c == '}' && depth > 0)
            --depth;
        if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty())
                out.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

std::optional<double> number(const std::string& s) {
    if (s.empty())
        return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

/// "{a b}" -> {"a", "b"}; "a" -> {"a"}.
std::vector<std::string> braceList(std::string s) {
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}')
        s = s.substr(1, s.size() - 2);
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string t; in >> t;)
        out.push_back(t);
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

/// Matching key: lower case, '.' and '/' both '/'.
std::string key(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '.')
            c = '/';
    }
    return s;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

std::string listed(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < 8; ++i)
        out += (i ? ", " : "") + names[i];
    if (names.size() > 8)
        out += ", ... (" + std::to_string(names.size() - 8) + " more)";
    return out;
}

} // namespace

std::optional<std::vector<VivadoClock>> parseVivadoClockReport(std::string_view text, std::string& error) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (const char c : text) {
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            }
            else if (c != '\r')
                cur += c;
        }
        if (!cur.empty())
            lines.push_back(std::move(cur));
    }

    std::vector<VivadoClock> out;
    bool table = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto head = tokens(lines[i]);
        if (head.size() < 3 || head[0] != "Clock")
            continue;
        int periodCol = -1, waveCol = -1;
        for (std::size_t k = 1; k < head.size(); ++k) {
            if (head[k].starts_with("Period"))
                periodCol = static_cast<int>(k);
            if (head[k].starts_with("Waveform"))
                waveCol = static_cast<int>(k);
        }
        if (periodCol < 0 || waveCol < 0)
            continue;
        const bool current = periodCol < waveCol; // Clock Period Waveform Attributes Sources
        table = true;
        bool rows = false;
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            const std::string t = trim(lines[j]);
            if (t.empty()) {
                if (rows)
                    break;
                continue;
            }
            if (t.find_first_not_of("- ") == std::string::npos)
                continue;
            const auto tok = tokens(t);
            if (tok.size() < 3)
                break;
            VivadoClock c;
            c.name = tok[0];
            c.line = static_cast<std::uint32_t>(j + 1);
            const auto period = number(tok[current ? 1 : 2]);
            if (!period || *period <= 0.0)
                break;
            c.periodNs = *period;
            if (current) {
                std::size_t k = 3;
                for (; k < tok.size() && tok[k].front() != '{'; ++k)
                    c.attributes += (c.attributes.empty() ? "" : " ") + tok[k];
                if (k < tok.size())
                    c.sources = braceList(tok[k]);
            }
            out.push_back(std::move(c));
            rows = true;
            i = j;
        }
    }
    if (!table) {
        error = "no clock table found (expected the output of Vivado 'report_clocks')";
        return std::nullopt;
    }

    // older reports: sources of generated clocks in the detail section
    std::string generated;
    for (const auto& l : lines) {
        const auto colon = l.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string field = trim(std::string_view(l).substr(0, colon));
        const std::string value = trim(std::string_view(l).substr(colon + 1));
        if (field == "Generated Clock")
            generated = value;
        else if (field == "Generated Sources" && !generated.empty())
            for (auto& c : out)
                if (c.name == generated && c.sources.empty())
                    c.sources = braceList(value);
    }
    return out;
}

VivadoClockCheck checkVivadoClocks(const clock::ClockModel& model, const std::string& top,
                                   const std::vector<VivadoClock>& vivado, const std::string& reportFile,
                                   DiagnosticSink& sink) {
    VivadoClockCheck r;
    r.vivadoClocks = vivado.size();
    const std::size_t n = model.clocks.size();
    std::map<std::string, std::size_t> bySource, byName;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& c = model.clocks[i];
        bySource.try_emplace(key(naming::vivadoSource(c, top)), i);
        const std::string rel = naming::relative(c.name, top);
        for (const auto& k : {c.name, rel, naming::tclName(rel)})
            byName.try_emplace(key(k), i);
    }

    std::vector<bool> used(n, false);
    std::vector<std::string> unmatched;
    for (const auto& v : vivado) {
        std::optional<std::size_t> hit;
        for (const auto& s : v.sources)
            if (const auto it = bySource.find(key(s)); it != bySource.end()) {
                hit = it->second;
                break;
            }
        if (!hit)
            if (const auto it = byName.find(key(v.name)); it != byName.end())
                hit = it->second;
        if (!hit) {
            // virtual clocks (no source) have nothing to match in the design, and
            // MMCM/PLL feedback clocks (CLKFBOUT) clock no register
            const bool feedback = std::ranges::all_of(v.sources, [](const std::string& s) {
                const std::string k = key(s);
                return k.ends_with("/clkfbout") || k.ends_with("/clkfboutb");
            });
            if (!v.sources.empty() && !feedback) {
                unmatched.push_back(v.name + " (" + listed(v.sources) + ")");
                ++r.unmatched;
            }
            continue;
        }
        ++r.matched;
        used[*hit] = true;
        const auto& c = model.clocks[*hit];
        const SourceLocation loc{reportFile, v.line, 0};
        if (!c.periodNs) {
            ++r.unknownInModel;
            sink.report({"VIV-PERIOD-UNKNOWN", Severity::Info,
                         "Vivado clock '" + v.name + "' has a period of " + fmt(v.periodNs) +
                             " ns; OpenCDC has no period for the matching clock '" + c.name +
                             "' (add a " +
                             (c.kind == clock::ClockKind::Divider ? "create_generated_clock" : "create_clock") +
                             " so both tools use the same clock)",
                         loc, {}});
            continue;
        }
        const double diff = std::abs(*c.periodNs - v.periodNs);
        if (diff > 1e-3 * v.periodNs) {
            ++r.mismatched;
            std::vector<std::string> notes;
            if (c.periodRatio && !c.periodBasis.empty())
                notes.push_back("OpenCDC derived it as " + c.periodBasis + " x " + fmt(*c.periodRatio) +
                                " from the parameters of the vendor model");
            notes.push_back("check the generator parameters, the input clock constraint and the report's "
                            "implementation run");
            sink.report({"VIV-PERIOD-MISMATCH", Severity::Warning,
                         "clock '" + c.name + "': OpenCDC computes a period of " + fmt(*c.periodNs) +
                             " ns but Vivado reports " + fmt(v.periodNs) + " ns for '" + v.name + "'",
                         loc, std::move(notes)});
        }
    }
    if (!unmatched.empty())
        sink.report({"VIV-CLOCK-UNMATCHED", Severity::Info,
                     std::to_string(unmatched.size()) +
                         " Vivado clock(s) have no counterpart in the OpenCDC clock model: " + listed(unmatched),
                     {reportFile, 0, 0},
                     {"the report may come from a different revision of the design, or the clock is created "
                      "inside logic OpenCDC treats as a black box"}});
    std::vector<std::string> missing;
    for (std::size_t i = 0; i < n; ++i)
        if (!used[i]) {
            const auto& c = model.clocks[i];
            missing.push_back(c.name + (c.kind == clock::ClockKind::Divider && c.name == c.sourceName
                                            ? " (fabric divider)"
                                            : ""));
        }
    r.missing = missing.size();
    if (!missing.empty())
        sink.report({"VIV-CLOCK-MISSING", Severity::Info,
                     std::to_string(missing.size()) + " OpenCDC clock(s) are not in the Vivado report: " +
                         listed(missing),
                     {reportFile, 0, 0},
                     {"Vivado does not propagate clocks through register outputs: a fabric divider without a "
                      "create_generated_clock is not timed by Vivado"}});
    return r;
}

std::optional<VivadoClockCheck> checkVivadoClockReport(const clock::ClockModel& model, const std::string& top,
                                                       const std::filesystem::path& file, DiagnosticSink& sink) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        sink.report({"VIV-REPORT-READ", Severity::Error, "cannot read Vivado clock report '" + file.string() + "'",
                     {}, {}});
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string error;
    const auto clocks = parseVivadoClockReport(text.str(), error);
    if (!clocks) {
        sink.report({"VIV-REPORT-READ", Severity::Error, "'" + file.string() + "': " + error, {}, {}});
        return std::nullopt;
    }
    return checkVivadoClocks(model, top, *clocks, file.string(), sink);
}

} // namespace opencdc::engine

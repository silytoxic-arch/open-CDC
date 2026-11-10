#include "constraints/Nextpnr.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <regex>
#include <sstream>
#include <vector>

namespace opencdc::constraints {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool endsWith(std::string_view name, std::string_view ext) {
    return name.size() > ext.size() && lower(name.substr(name.size() - ext.size())) == ext;
}

std::optional<double> number(std::string_view s) {
    double v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::string text(double v) {
    std::ostringstream o;
    o.precision(12);
    o << v;
    return o.str();
}

/// create_clock for a net of Yosys' naming ("u_pll.clk_out": '.' between levels), on the
/// line of its constraint. The clock takes the net's name, as nextpnr reports it.
std::string createClock(const std::string& net, double mhz) {
    std::string path = net;
    if (!path.empty() && path.front() == '\\')
        path.erase(0, 1); // a Yosys escaped name
    std::ranges::replace(path, '.', '/');
    const std::string period = text(1000.0 / mhz);
    // (a top-level port is a net of the top too; a net that only passes a port on is the port)
    return "create_clock -opencdc_physical_net -name {" + net + "} -period " + period + " [get_nets {" + path + "}]";
}

std::vector<std::string> lines(std::string_view t) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : t) {
        if (c == '\n') {
            out.push_back(std::move(cur));
            cur.clear();
        }
        else if (c != '\r')
            cur += c;
    }
    out.push_back(std::move(cur));
    return out;
}

} // namespace

bool isPcfFile(std::string_view fileName) {
    return endsWith(fileName, ".pcf");
}

bool isNextpnrScript(std::string_view fileName) {
    return endsWith(fileName, ".py");
}

std::string pcfToSdc(std::string_view input, const std::string& fileName, DiagnosticSink& sink) {
    std::string out;
    std::uint32_t n = 0;
    for (const auto& l : lines(input)) {
        ++n;
        std::string cmd;
        std::istringstream ws(l.substr(0, l.find('#')));
        std::vector<std::string> words;
        for (std::string w; ws >> w;)
            words.push_back(w);
        if (!words.empty() && words[0] == "set_frequency") {
            const auto mhz = words.size() == 3 ? number(words[2]) : std::nullopt;
            if (!mhz || *mhz <= 0)
                sink.report({"CON-NEXTPNR-PARSE", Severity::Warning,
                             "set_frequency: expected set_frequency <net> <MHz>", {fileName, n, 0}, {}});
            else
                cmd = createClock(words[1], *mhz);
        }
        out += cmd + "\n";
    }
    return out;
}

std::string nextpnrScriptToSdc(std::string_view input, const std::string& fileName, DiagnosticSink& sink) {
    static const std::regex call(R"(^\s*ctx\.addClock\(\s*(["'])([^"']+)\1\s*,\s*([0-9.eE+-]+)\s*\)\s*(#.*)?$)");
    std::string out;
    std::uint32_t n = 0;
    std::uint32_t other = 0;
    for (const auto& l : lines(input)) {
        ++n;
        std::string cmd;
        std::smatch m;
        std::string_view trimmed = l;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)
            trimmed.remove_prefix(1);
        if (std::regex_match(l, m, call)) {
            const auto mhz = number(m[3].str());
            if (!mhz || *mhz <= 0)
                sink.report({"CON-NEXTPNR-PARSE", Severity::Warning, "ctx.addClock: the frequency is not a number of MHz",
                             {fileName, n, 0}, {}});
            else
                cmd = createClock(m[2].str(), *mhz);
        }
        else if (trimmed.find("addClock") != std::string_view::npos)
            sink.report({"CON-NEXTPNR-PARSE", Severity::Warning,
                         "ctx.addClock: expected ctx.addClock(\"<net>\", <MHz>) with a literal net and frequency",
                         {fileName, n, 0}, {}});
        else if (!trimmed.empty() && trimmed.front() != '#')
            ++other;
        out += cmd + "\n";
    }
    if (other > 0)
        sink.report({"CON-NEXTPNR-IGNORED", Severity::Info,
                     std::to_string(other) + " statement(s) of the nextpnr script other than ctx.addClock are not read",
                     {fileName, 0, 0}, {}});
    return out;
}

} // namespace opencdc::constraints

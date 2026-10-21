#include "frontend/Primitives.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace opencdc::frontend {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

const PrimitiveLibrary& PrimitiveLibrary::instance() {
    static const PrimitiveLibrary lib;
    return lib;
}

PrimitiveLibrary::PrimitiveLibrary() {
    for (std::size_t i = 0; i < detail::kPrimitiveSourceCount; ++i) {
        const auto& src = detail::kPrimitiveSources[i];
        std::string text;
        for (std::size_t k = 0; k < src.count; ++k)
            text += src.parts[k];
        const std::string file = src.name;
        if (file.ends_with(".vhd")) {
            std::string libName = file.starts_with("unisim")  ? "unisim"
                                  : file.starts_with("xpm")   ? "xpm"
                                                              : "altera_mf";
            packages_.push_back({libName, file, std::move(text)});
            continue;
        }
        // split a Verilog file into its modules
        static const std::regex moduleRe(R"(^module\s+([A-Za-z_][A-Za-z0-9_]*))");
        std::size_t pos = 0;
        while ((pos = text.find("\nmodule ", pos)) != std::string::npos) {
            const std::size_t start = pos + 1;
            const std::size_t end = text.find("endmodule", start);
            if (end == std::string::npos)
                break;
            const std::string body = text.substr(start, end + 9 - start) + "\n";
            std::smatch m;
            if (std::regex_search(body, m, moduleRe))
                models_.push_back({m[1].str(), body, {}});
            pos = end;
        }
    }
    // clock generators (port lists of UG953 / UG974)
    using P = std::vector<std::pair<std::string, int>>;
    const P mmcmBaseOut = {{"CLKFBOUT", 1}, {"CLKFBOUTB", 1}, {"CLKOUT0", 1}, {"CLKOUT0B", 1}, {"CLKOUT1", 1},
                           {"CLKOUT1B", 1}, {"CLKOUT2", 1}, {"CLKOUT2B", 1}, {"CLKOUT3", 1}, {"CLKOUT3B", 1},
                           {"CLKOUT4", 1}, {"CLKOUT5", 1}, {"CLKOUT6", 1}, {"LOCKED", 1}};
    const P mmcmBaseIn = {{"CLKFBIN", 1}, {"CLKIN1", 1}, {"PWRDWN", 1}, {"RST", 1}};
    P mmcmAdvOut = mmcmBaseOut;
    mmcmAdvOut.insert(mmcmAdvOut.end(),
                      {{"CLKFBSTOPPED", 1}, {"CLKINSTOPPED", 1}, {"DO", 16}, {"DRDY", 1}, {"PSDONE", 1}});
    P mmcmAdvIn = mmcmBaseIn;
    mmcmAdvIn.insert(mmcmAdvIn.end(), {{"CLKIN2", 1}, {"CLKINSEL", 1}, {"DADDR", 7}, {"DCLK", 1}, {"DEN", 1},
                                       {"DI", 16}, {"DWE", 1}, {"PSCLK", 1}, {"PSEN", 1}, {"PSINCDEC", 1}});
    P mmcmUsAdvOut = mmcmAdvOut;
    mmcmUsAdvOut.push_back({"CDDCDONE", 1});
    P mmcmUsAdvIn = mmcmAdvIn;
    mmcmUsAdvIn.push_back({"CDDCREQ", 1});
    const P pll2BaseOut = {{"CLKFBOUT", 1}, {"CLKOUT0", 1}, {"CLKOUT1", 1}, {"CLKOUT2", 1},
                           {"CLKOUT3", 1},  {"CLKOUT4", 1}, {"CLKOUT5", 1}, {"LOCKED", 1}};
    P pll2AdvOut = pll2BaseOut;
    pll2AdvOut.insert(pll2AdvOut.end(), {{"DO", 16}, {"DRDY", 1}});
    P pll2AdvIn = mmcmBaseIn;
    pll2AdvIn.insert(pll2AdvIn.end(),
                     {{"CLKIN2", 1}, {"CLKINSEL", 1}, {"DADDR", 7}, {"DCLK", 1}, {"DEN", 1}, {"DI", 16}, {"DWE", 1}});
    const P pllUsBaseOut = {{"CLKFBOUT", 1}, {"CLKOUT0", 1}, {"CLKOUT0B", 1}, {"CLKOUT1", 1},
                            {"CLKOUT1B", 1}, {"CLKOUTPHY", 1}, {"LOCKED", 1}};
    const P pllUsBaseIn = {{"CLKFBIN", 1}, {"CLKIN", 1}, {"CLKOUTPHYEN", 1}, {"PWRDWN", 1}, {"RST", 1}};
    P pllUsAdvOut = pllUsBaseOut;
    pllUsAdvOut.insert(pllUsAdvOut.end(), {{"DO", 16}, {"DRDY", 1}});
    P pllUsAdvIn = pllUsBaseIn;
    pllUsAdvIn.insert(pllUsAdvIn.end(), {{"DADDR", 7}, {"DCLK", 1}, {"DEN", 1}, {"DI", 16}, {"DWE", 1}});
    generators_ = {
        {"MMCME2_BASE", "CLKIN1", mmcmBaseIn, mmcmBaseOut},  {"MMCME2_ADV", "CLKIN1", mmcmAdvIn, mmcmAdvOut},
        {"MMCME3_BASE", "CLKIN1", mmcmBaseIn, mmcmBaseOut},  {"MMCME3_ADV", "CLKIN1", mmcmUsAdvIn, mmcmUsAdvOut},
        {"MMCME4_BASE", "CLKIN1", mmcmBaseIn, mmcmBaseOut},  {"MMCME4_ADV", "CLKIN1", mmcmUsAdvIn, mmcmUsAdvOut},
        {"PLLE2_BASE", "CLKIN1", mmcmBaseIn, pll2BaseOut},   {"PLLE2_ADV", "CLKIN1", pll2AdvIn, pll2AdvOut},
        {"PLLE3_BASE", "CLKIN", pllUsBaseIn, pllUsBaseOut},  {"PLLE3_ADV", "CLKIN", pllUsAdvIn, pllUsAdvOut},
        {"PLLE4_BASE", "CLKIN", pllUsBaseIn, pllUsBaseOut},  {"PLLE4_ADV", "CLKIN", pllUsAdvIn, pllUsAdvOut},
    };
    for (const auto& m : models_)
        names_.push_back(m.name);
    for (const auto& g : generators_)
        names_.push_back(g.name);
    // dependencies: other modelled modules instantiated by a model
    for (auto& m : models_)
        for (const auto& o : models_)
            if (&o != &m && std::regex_search(m.text, std::regex("(^|[^A-Za-z0-9_$])" + o.name +
                                                                 "(\\s*#|\\s+[A-Za-z_][A-Za-z0-9_]*\\s*\\()")))
                m.deps.push_back(o.name);
}

std::optional<std::string> PrimitiveLibrary::find(const std::string& name) const {
    for (const auto& n : names_)
        if (n == name)
            return n;
    const std::string key = lower(name);
    for (const auto& n : names_)
        if (lower(n) == key)
            return n;
    return std::nullopt;
}

bool PrimitiveLibrary::isClockGenerator(const std::string& name) const {
    const auto canon = find(name); // accepts the lowercase VHDL spelling
    return canon && std::ranges::any_of(generators_, [&](const Generator& g) { return g.name == *canon; });
}

std::string PrimitiveLibrary::generatorModel(const std::string& name, const std::set<std::string>& params) const {
    const auto it = std::ranges::find_if(generators_, [&](const Generator& g) { return g.name == name; });
    if (it == generators_.end())
        return {};
    const Generator& g = *it;
    const bool mmcm = g.name.starts_with("MMCM");
    const std::string inPeriod = g.clockIn + "_PERIOD";
    const std::string mult = mmcm ? "CLKFBOUT_MULT_F" : "CLKFBOUT_MULT";
    // Parameters that set the output frequencies, with the UNISIM defaults;
    // every other parameter the design writes is accepted and ignored.
    std::vector<std::pair<std::string, std::string>> known = {
        {"real " + inPeriod, "0.0"}, {"integer DIVCLK_DIVIDE", "1"}, {"real " + mult, "5.0"}};
    auto divideOf = [&](const std::string& out) -> std::string {
        // CLKOUT0B -> CLKOUT0; CLKOUT0 of an MMCM has a fractional divider
        std::string base = out;
        if (base.size() > 7 && base.back() == 'B')
            base.pop_back();
        if (base == "CLKFBOUT")
            return "1.0";
        if (mmcm && base == "CLKOUT0")
            return "CLKOUT0_DIVIDE_F";
        return base + "_DIVIDE";
    };
    for (const auto& [n, w] : g.outputs) {
        if (!n.starts_with("CLKOUT") || n == "CLKOUTPHY")
            continue;
        const std::string d = divideOf(n);
        const std::string decl = (d == "CLKOUT0_DIVIDE_F" ? "real " : "integer ") + d;
        if (std::ranges::find(known, decl, &std::pair<std::string, std::string>::first) == known.end())
            known.emplace_back(decl, d == "CLKOUT0_DIVIDE_F" ? "1.0" : "1");
    }
    auto width = [](int w) { return w > 1 ? "[" + std::to_string(w - 1) + ":0] " : std::string(); };
    std::string t = "// OpenCDC model of " + g.name + ": every clock output is derived from " + g.clockIn +
                    "; its period is the input period x " + (mmcm ? "DIVCLK_DIVIDE x divide / CLKFBOUT_MULT_F"
                                                                    : "DIVCLK_DIVIDE x divide / CLKFBOUT_MULT") +
                    ". LOCKED has no clock domain.\n";
    t += "module " + g.name + " #(";
    bool first = true;
    for (const auto& [decl, def] : known) {
        t += (first ? "" : ", ") + std::string("parameter ") + decl + " = " + def;
        first = false;
    }
    for (const auto& p : params) {
        const bool isKnown = std::ranges::any_of(known, [&](const auto& k) { return k.first.ends_with(" " + p); });
        if (!isKnown)
            t += ", parameter " + p + " = 0";
    }
    t += ") (";
    first = true;
    for (const auto& [n, w] : g.inputs) {
        t += (first ? "input " : ", input ") + width(w) + n;
        first = false;
    }
    for (const auto& [n, w] : g.outputs)
        t += ", output " + width(w) + n;
    t += ");\n";
    for (const auto& [n, w] : g.outputs) {
        if (n.starts_with("CLK") && n != "CLKFBSTOPPED" && n != "CLKINSTOPPED") {
            const std::string ratio = n == "CLKOUTPHY" ? "(1.0 * DIVCLK_DIVIDE) / " + mult
                                                       : "(1.0 * DIVCLK_DIVIDE * " + divideOf(n) + ") / " + mult;
            t += "    (* OPENCDC_CLOCK_PIN = \"" + n + "\", OPENCDC_CLOCK_RATIO = " + ratio +
                 ", OPENCDC_CLOCK_INPUT_PERIOD = " + inPeriod + " *)\n";
            t += "    reg " + n + "_q = 1'b0;\n";
            t += "    always @(posedge " + g.clockIn + ") " + n + "_q <= ~" + n + "_q;\n";
            t += "    assign " + n + " = " + n + "_q;\n";
        }
        else if (n == "LOCKED") {
            // asynchronous to every clock, like a real lock detector
            t += "    opencdc_async_source u_locked (LOCKED);\n";
        }
        else {
            t += "    assign " + n + " = " + std::to_string(w) + "'d0;\n";
        }
    }
    t += "endmodule\n";
    return t;
}

std::string PrimitiveLibrary::verilogModel(const std::string& name) const {
    if (isClockGenerator(name))
        return generatorModel(name, {});
    std::string out;
    std::vector<std::string> done;
    std::vector<std::string> work{name};
    while (!work.empty()) {
        const std::string n = work.back();
        work.pop_back();
        if (std::ranges::find(done, n) != done.end())
            continue;
        done.push_back(n);
        for (const auto& m : models_)
            if (m.name == n) {
                out += m.text;
                for (const auto& d : m.deps)
                    work.push_back(d);
            }
    }
    return out;
}

} // namespace opencdc::frontend

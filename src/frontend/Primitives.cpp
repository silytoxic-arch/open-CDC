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
    auto width = [](int w) { return w > 1 ? "[" + std::to_string(w - 1) + ":0] " : std::string(); };
    std::string t = "// OpenCDC model of " + g.name +
                    ": every clock output is derived from " + g.clockIn +
                    " (synchronous to it and to the other outputs; frequencies are not modelled).\n";
    t += "module " + g.name;
    if (!params.empty()) {
        t += " #(";
        bool first = true;
        for (const auto& p : params) {
            t += (first ? "" : ", ") + std::string("parameter ") + p + " = 0";
            first = false;
        }
        t += ")";
    }
    t += " (";
    bool first = true;
    for (const auto& [n, w] : g.inputs) {
        t += (first ? "input " : ", input ") + width(w) + n;
        first = false;
    }
    for (const auto& [n, w] : g.outputs)
        t += ", output " + width(w) + n;
    t += ");\n";
    for (const auto& [n, w] : g.outputs) {
        if (n.starts_with("CLK") && n != "CLKFBSTOPPED" && n != "CLKINSTOPPED") {
            t += "    reg " + n + "_q = 1'b0;\n";
            t += "    always @(posedge " + g.clockIn + ") " + n + "_q <= ~" + n + "_q;\n";
            t += "    assign " + n + " = " + n + "_q;\n";
        }
        else if (n == "LOCKED") {
            t += "    reg locked_q = 1'b0;\n";
            t += "    always @(posedge " + g.clockIn + ") locked_q <= 1'b1;\n";
            t += "    assign LOCKED = locked_q;\n";
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

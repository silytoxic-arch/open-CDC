#pragma once
// Vivado-style names of clocks and their source pins (shared by the
// constraint suggestion and the Vivado clock report cross-check).

#include "clock/ClockModel.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace opencdc::engine::naming {

/// Hierarchical name without the top instance ("top.u_a.x" -> "u_a.x").
inline std::string relative(const std::string& name, const std::string& top) {
    if (!top.empty() && name.size() > top.size() + 1 && name.starts_with(top + "."))
        return name.substr(top.size() + 1);
    return name;
}

/// "u_a.u_b.x" -> "u_a/u_b/x" (Vivado hierarchy separator).
inline std::string slashed(std::string s) {
    std::ranges::replace(s, '.', '/');
    return s;
}

/// A clock name usable without quoting in Tcl.
inline std::string tclName(const std::string& s) {
    std::string out;
    for (const char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    while (out.find("__") != std::string::npos)
        out.replace(out.find("__"), 2, "_");
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out.empty() ? std::string("clk") : out;
}

/// Register pin name of a fabric register ("u_a.cnt[3]" -> "u_a/cnt_reg[3]/Q").
inline std::string registerPin(const std::string& rel, const char* pin) {
    std::string base = rel;
    std::string index;
    if (const auto b = rel.find('['); b != std::string::npos && rel.back() == ']' &&
                                      rel.find('.', b) == std::string::npos) {
        base = rel.substr(0, b);
        index = rel.substr(b);
    }
    return slashed(base) + "_reg" + index + "/" + pin;
}

/// The object Vivado reports as the source of this clock: a port, a
/// primitive output pin, or a register output pin.
inline std::string vivadoSource(const clock::Clock& c, const std::string& top) {
    const std::string rel = relative(c.sourceName, top);
    switch (c.kind) {
        case clock::ClockKind::Divider:
            return c.name != c.sourceName ? slashed(relative(c.name, top)) : registerPin(rel, "Q");
        case clock::ClockKind::BlackBox:
            return slashed(relative(c.name != c.sourceName ? c.name : c.sourceName, top));
        default: return slashed(rel);
    }
}

} // namespace opencdc::engine::naming

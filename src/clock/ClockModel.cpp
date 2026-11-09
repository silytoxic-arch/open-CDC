#include "clock/ClockModel.h"

#include "constraints/Sdc.h"

#include <algorithm>

namespace opencdc::clock {

std::string_view toString(ClockKind k) noexcept {
    switch (k) {
        case ClockKind::Primary: return "primary";
        case ClockKind::Constraint: return "constraint";
        case ClockKind::Generated: return "generated";
        case ClockKind::Divider: return "divider";
        case ClockKind::BlackBox: return "black-box";
    }
    return "?";
}

std::string_view toString(ClockRelation r) noexcept {
    switch (r) {
        case ClockRelation::Same: return "same";
        case ClockRelation::Synchronous: return "synchronous";
        case ClockRelation::Asynchronous: return "asynchronous";
        case ClockRelation::Exclusive: return "exclusive";
        case ClockRelation::Unknown: return "unknown";
    }
    return "?";
}

void ClockModel::initRelations() {
    const std::size_t n = clocks.size();
    rel_.assign(n * n, ClockRelation::Unknown);
    relReason_.assign(n * n, std::string());
    for (std::size_t i = 0; i < n; ++i) {
        rel_[i * n + i] = ClockRelation::Same;
        relReason_[i * n + i] = "same clock";
    }
}

void ClockModel::setRelation(ClockId a, ClockId b, ClockRelation r, std::string why) {
    const std::size_t n = clocks.size();
    rel_[a.index() * n + b.index()] = r;
    rel_[b.index() * n + a.index()] = r;
    relReason_[a.index() * n + b.index()] = why;
    relReason_[b.index() * n + a.index()] = std::move(why);
}

ClockRelation ClockModel::relation(ClockId a, ClockId b) const {
    return rel_.at(a.index() * clocks.size() + b.index());
}

const std::string& ClockModel::reason(ClockId a, ClockId b) const {
    return relReason_.at(a.index() * clocks.size() + b.index());
}

std::optional<ClockId> ClockModel::find(std::string_view name) const {
    for (std::size_t i = 0; i < clocks.size(); ++i)
        if (clocks[i].name == name)
            return ClockId{static_cast<std::uint32_t>(i)};
    for (std::size_t i = 0; i < clocks.size(); ++i)
        if (std::ranges::find(clocks[i].aliases, name) != clocks[i].aliases.end())
            return ClockId{static_cast<std::uint32_t>(i)};
    // a pattern (set_input_delay -clock [get_clocks {*DDR3_PLL5*clk[2]}], BrianHG DDR3): the
    // first clock whose name or one of whose aliases matches
    if (name.find_first_of("*?") != std::string_view::npos)
        for (std::size_t i = 0; i < clocks.size(); ++i)
            if (constraints::globMatch(name, clocks[i].name) ||
                std::ranges::any_of(clocks[i].aliases, [&](const std::string& a) { return constraints::globMatch(name, a); }))
                return ClockId{static_cast<std::uint32_t>(i)};
    return std::nullopt;
}

ClockId ClockModel::root(ClockId id) const {
    ClockId cur = id;
    for (std::size_t guard = 0; guard <= clocks.size(); ++guard) {
        const ClockId m = clock(cur).master;
        if (!m.valid())
            return cur;
        cur = m;
    }
    return cur; // master cycle: stop
}

std::size_t ClockModel::resolvedRegisters() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        registers, [](const RegisterClocking& r) { return r.status == ClockStatus::Resolved; }));
}

std::size_t ClockModel::unresolvedRegisters() const {
    return registers.size() - resolvedRegisters();
}

bool clockMatches(std::string_view pattern, const Clock& c) {
    if (constraints::globMatch(pattern, c.name))
        return true;
    return std::ranges::any_of(c.aliases, [&](const std::string& a) { return constraints::globMatch(pattern, a); });
}

} // namespace opencdc::clock

#include "rules/Waiver.h"

#include <algorithm>

namespace opencdc::rules {

using namespace opencdc::cdc;

bool globMatch(std::string_view pattern, std::string_view text) noexcept {
    // Iterative matcher with backtracking to the last '*'.
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        }
        else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        }
        else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

bool covers(const WaiverRecord& w, const CdcPath& p, const RuleHit& hit) {
    if (!w.rule.empty() && !globMatch(w.rule, hit.ruleId))
        return false;
    if (!w.fingerprint.empty() && w.fingerprint != p.fingerprint)
        return false;
    if (!w.source.empty() && !globMatch(w.source, p.srcName))
        return false;
    if (!w.destination.empty() && !globMatch(w.destination, p.dstName))
        return false;
    return true;
}

namespace {
/// Days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9; // March = 0
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::optional<std::int64_t> dayNumber(std::string_view s) noexcept {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-')
        return std::nullopt;
    auto num = [&](std::size_t at, std::size_t n) -> std::optional<unsigned> {
        unsigned v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9')
                return std::nullopt;
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
        }
        return v;
    };
    const auto y = num(0, 4), m = num(5, 2), d = num(8, 2);
    if (!y || !m || !d || *m < 1 || *m > 12 || *d < 1 || *d > 31)
        return std::nullopt;
    return daysFromCivil(*y, *m, *d);
}
} // namespace

std::string_view waiverState(const WaiverRecord& w) noexcept {
    if (w.expired)
        return "expired";
    if (w.uses == 0)
        return "unused";
    return w.expiresInDays >= 0 ? "expiring" : "active";
}

std::optional<std::int64_t> daysBetween(std::string_view from, std::string_view to) noexcept {
    const auto a = dayNumber(from), b = dayNumber(to);
    if (!a || !b)
        return std::nullopt;
    return *b - *a;
}

void applyWaivers(std::vector<WaiverRecord> waivers, CdcResult& result, std::string_view today,
                  DiagnosticSink& sink, std::uint32_t expiryWarningDays) {
    for (auto& w : waivers) {
        w.uses = 0;
        w.expiresInDays = -1;
        w.expired = !w.expires.empty() && !today.empty() && w.expires < today;
        if (!w.expired && expiryWarningDays > 0 && !w.expires.empty() && !today.empty())
            if (const auto left = daysBetween(today, w.expires); left && *left >= 0 && *left <= expiryWarningDays) {
                w.expiresInDays = static_cast<std::int32_t>(*left);
                sink.report({"WAIVER-EXPIRING", Severity::Warning,
                             "waiver '" + w.id + "' expires on " + w.expires +
                                 (*left == 0 ? std::string(" (today)") : " (in " + std::to_string(*left) + " day(s))") +
                                 ": review it, renew its date or fix the crossing",
                             w.loc, {}});
            }
        if (w.expired)
            sink.report({"WAIVER-EXPIRED", Severity::Warning,
                         "waiver '" + w.id + "' expired on " + w.expires + " and is not applied", w.loc, {}});
    }
    result.stats.waivedHits = 0;
    for (auto& p : result.paths) {
        bool any = false;
        for (auto& h : p.hits) {
            h.waiver.clear();
            for (auto& w : waivers) {
                if (w.expired || !covers(w, p, h))
                    continue;
                h.waiver = w.id;
                ++w.uses;
                ++result.stats.waivedHits;
                any = true;
                break;
            }
        }
        p.waived = !p.hits.empty() && std::ranges::all_of(p.hits, [](const RuleHit& h) { return !h.waiver.empty(); });
        if (!any)
            continue;
        // Status from the hits that remain. A recognized structure whose
        // conditions fail keeps needing review unless a waiver covered it.
        AnalysisStatus status = AnalysisStatus::Safe;
        for (const auto& h : p.hits)
            if (h.waiver.empty())
                status = std::max(status, h.status);
        p.status = status;
    }
    // output ports timed against an asynchronous clock (CDC-OUTPUT-CLOCK):
    // a waiver covers a record when its source and destination globs match
    // one of the drivers and one of the ports
    for (auto& o : result.outputs) {
        o.waiver.clear();
        for (auto& w : waivers) {
            if (w.expired || (!w.rule.empty() && !globMatch(w.rule, "CDC-OUTPUT-CLOCK")) ||
                (!w.fingerprint.empty() && w.fingerprint != o.fingerprint) ||
                (!w.source.empty() &&
                 std::ranges::none_of(o.sources, [&](const std::string& n) { return globMatch(w.source, n); })) ||
                (!w.destination.empty() &&
                 std::ranges::none_of(o.ports, [&](const std::string& n) { return globMatch(w.destination, n); })))
                continue;
            o.waiver = w.id;
            ++w.uses;
            ++result.stats.waivedHits;
            break;
        }
    }
    for (const auto& w : waivers)
        if (!w.expired && w.uses == 0)
            sink.report({"WAIVER-UNUSED", Severity::Warning,
                         "waiver '" + w.id + "' does not match any rule hit (stale or too narrow?)", w.loc, {}});
    result.waivers = std::move(waivers);
}

void reportOutputCrossings(CdcResult& result, DiagnosticSink& sink) {
    auto listOf = [](const std::vector<std::string>& v) {
        std::string t;
        for (std::size_t i = 0; i < v.size() && i < 4; ++i)
            t += (i ? ", " : "") + ("'" + v[i] + "'");
        if (v.size() > 4)
            t += " (+" + std::to_string(v.size() - 4) + " more)";
        return t;
    };
    result.stats.outputCrossings = 0;
    for (const auto& o : result.outputs) {
        if (!o.waiver.empty())
            continue;
        ++result.stats.outputCrossings;
        const bool unknown = o.relation == clock::ClockRelation::Unknown;
        sink.report({"CDC-OUTPUT-CLOCK", Severity::Warning,
                     "output " + listOf(o.ports) + " is timed against clock '" + o.outClock +
                         "' (set_output_delay) but driven by " + listOf(o.sources) + " of clock '" + o.srcClock + "' (" +
                         (unknown ? "relation unknown" : "asynchronous") +
                         "): the receiver samples data that changes asynchronously to its clock",
                     o.loc,
                     {unknown ? "relate the clocks (create_generated_clock, set_clock_groups) if they are synchronous, "
                                "or synchronize the output"
                              : "drive the output from the clock it is timed against, or exempt it with set_false_path "
                                "-to (or a waiver for rule CDC-OUTPUT-CLOCK) if the receiver synchronizes it",
                      "fingerprint " + o.fingerprint}});
    }
}

} // namespace opencdc::rules

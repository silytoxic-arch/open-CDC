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

void applyWaivers(std::vector<WaiverRecord> waivers, CdcResult& result, std::string_view today,
                  DiagnosticSink& sink) {
    for (auto& w : waivers) {
        w.uses = 0;
        w.expired = !w.expires.empty() && !today.empty() && w.expires < today;
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
    for (const auto& w : waivers)
        if (!w.expired && w.uses == 0)
            sink.report({"WAIVER-UNUSED", Severity::Warning,
                         "waiver '" + w.id + "' does not match any rule hit (stale or too narrow?)", w.loc, {}});
    result.waivers = std::move(waivers);
}

} // namespace opencdc::rules

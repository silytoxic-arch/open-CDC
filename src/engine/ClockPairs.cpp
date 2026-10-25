#include "engine/ClockPairs.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace opencdc::engine {

std::vector<ClockPairSummary> summarizeClockPairs(const AnalysisResult& result) {
    return result.cdc ? summarizeClockPairs(*result.cdc) : std::vector<ClockPairSummary>{};
}

std::vector<ClockPairSummary> summarizeClockPairs(const cdc::CdcResult& cdc) {
    std::vector<ClockPairSummary> out;
    std::map<std::pair<std::string, std::string>, std::size_t> index;
    for (const auto& p : cdc.paths) {
        const auto key = std::make_pair(p.srcClockName, p.dstClockName);
        auto [it, added] = index.try_emplace(key, out.size());
        if (added) {
            ClockPairSummary s;
            s.srcClock = p.srcClockName;
            s.dstClock = p.dstClockName;
            s.relation = std::string(clock::toString(p.relation));
            out.push_back(std::move(s));
        }
        auto& s = out[it->second];
        ++s.paths;
        if (p.syncStatus == cdc::SyncStatus::NecessaryConditionsVerified ||
            p.syncStatus == cdc::SyncStatus::ProtocolUnverified)
            ++s.synchronized;
        switch (p.status) {
            case cdc::AnalysisStatus::Violation: ++s.violations; break;
            case cdc::AnalysisStatus::PotentialRisk: ++s.potentialRisk; break;
            case cdc::AnalysisStatus::NeedsReview: ++s.needsReview; break;
            case cdc::AnalysisStatus::Safe: break;
        }
    }
    std::ranges::stable_sort(out, [](const ClockPairSummary& a, const ClockPairSummary& b) {
        if (a.paths != b.paths)
            return a.paths > b.paths;
        return std::tie(a.srcClock, a.dstClock) < std::tie(b.srcClock, b.dstClock);
    });
    return out;
}

} // namespace opencdc::engine

#pragma once

#include "engine/AnalysisSession.h"

#include <cstddef>
#include <string>
#include <vector>

namespace opencdc::engine {

/// Crossings between one source clock and one destination clock: how many
/// there are and what became of them. Shows at a glance where the paths
/// that need attention come from (for example all review paths from one
/// unconstrained clock).
struct ClockPairSummary {
    std::string srcClock;
    std::string dstClock;
    std::string relation;           ///< asynchronous / unknown / ...
    std::size_t paths = 0;
    std::size_t synchronized = 0;   ///< necessary conditions verified or protocol unverified
    std::size_t violations = 0;     ///< status violation
    std::size_t potentialRisk = 0;
    std::size_t needsReview = 0;
};

/// One entry per (source clock, destination clock), most paths first.
/// Empty when CDC analysis did not run.
[[nodiscard]] std::vector<ClockPairSummary> summarizeClockPairs(const cdc::CdcResult& cdc);
[[nodiscard]] std::vector<ClockPairSummary> summarizeClockPairs(const AnalysisResult& result);

} // namespace opencdc::engine

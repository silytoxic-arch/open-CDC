#pragma once

#include "engine/AnalysisSession.h"

#include <cstddef>
#include <string>
#include <vector>

namespace opencdc::engine {

/// Constraints that would complete the clock model of an analysis result:
/// clocks the design uses without a constraint, generated clocks for fabric
/// dividers, and clock groups for the clock families whose relation is
/// unknown. Every guess is marked "TODO" (a period nobody knows) or "VERIFY"
/// (an assumption about the design), so the text is a starting point for a
/// review, never a substitute for one.
struct SdcSuggestion {
    std::string text;                ///< SDC/XDC subset that OpenCDC and Vivado read
    std::size_t clocks = 0;          ///< create_clock / create_generated_clock lines
    std::size_t groups = 0;          ///< set_clock_groups lines
    std::size_t todos = 0;           ///< periods that must be filled in
    std::vector<std::string> unknownPairs; ///< "a <-> b" pairs the groups resolve
};

/// Empty text when the result has no clock analysis.
[[nodiscard]] SdcSuggestion suggestConstraints(const AnalysisResult& result);

} // namespace opencdc::engine

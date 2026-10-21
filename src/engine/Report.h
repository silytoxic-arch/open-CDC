#pragma once

#include "engine/AnalysisSession.h"

#include <cstddef>
#include <string>

namespace opencdc::engine {

struct HtmlReportOptions {
    bool schematics = true;        ///< embed an SVG schematic in every path's details
    std::size_t maxSchematics = 400; ///< beyond this many paths, schematics are left out (size)
    /// Beyond this many paths, the path table, structure table and details
    /// list only the paths that need attention (any rule hit, waived or not,
    /// or a status other than safe), at most this many; the CSV and JSON
    /// outputs always hold every path.
    std::size_t maxPaths = 5000;
};

/// Self-contained HTML report (one file: inline CSS, inline SVG, no
/// scripts needed): run summary, stages, clocks and their relations, rule
/// summary, synchronizer structures, all CDC paths with details (path,
/// synchronizer conditions, rule hits with fix suggestions, waivers,
/// schematic), waivers, diagnostics and constructs that were not analyzed.
[[nodiscard]] std::string toHtmlReport(const AnalysisResult& result, const HtmlReportOptions& options = {});

/// CSV (RFC 4180, UTF-8 with BOM for spreadsheet programs) of all CDC paths.
[[nodiscard]] std::string pathsCsv(const AnalysisResult& result);
/// CSV of all rule hits (one row per hit, waived ones included with their waiver).
[[nodiscard]] std::string violationsCsv(const AnalysisResult& result);

} // namespace opencdc::engine

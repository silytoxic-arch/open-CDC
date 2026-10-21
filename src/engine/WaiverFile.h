#pragma once

#include "cdc/CdcModel.h"
#include "core/Diagnostic.h"

#include <filesystem>
#include <vector>

namespace opencdc::engine {

inline constexpr const char* kWaiverFormatName = "opencdc-waivers";
inline constexpr int kWaiverFormatVersion = 1;

/// Reads a waiver file (JSON):
/// {
///   "format":  "opencdc-waivers",
///   "version": 1,
///   "waivers": [
///     {"id":          "W1",                         (required, unique across all waiver files)
///      "rule":        "CDC-007",                    (glob; at least one of rule/fingerprint/
///      "fingerprint": "4fc20667fa8023be",           ( exact)  source/destination is required)
///      "source":      "top.u_tx.*",                 (glob on the source name)
///      "destination": "top.u_rx.*",                 (glob on the destination name)
///      "reason":      "quasi-static configuration", (required)
///      "author":      "jdoe",                       (optional)
///      "expires":     "2027-06-30"}                 (optional, YYYY-MM-DD)
///   ]
/// }
/// Invalid entries are reported (WAIVER-OPEN, WAIVER-SCHEMA, WAIVER-INVALID,
/// errors) and skipped; valid ones are appended to `out`. Each waiver's
/// location is the line of its "id".
void readWaiverFile(const std::filesystem::path& file, std::vector<cdc::WaiverRecord>& out, DiagnosticSink& sink);

} // namespace opencdc::engine

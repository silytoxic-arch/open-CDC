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

/// Problems that would make readWaiverFile skip `w` (missing id or reason,
/// no selector, malformed expiry date, id already in `taken`); empty if none.
[[nodiscard]] std::string waiverProblem(const cdc::WaiverRecord& w, const std::vector<std::string>& taken);

/// Appends a waiver to a waiver file. A missing file is created; an existing
/// one keeps its entries, their field order and any other top-level fields.
/// `taken`: ids of the waivers already in the project (ids are unique across
/// all of its waiver files). The file is replaced only once the new text is
/// completely written. Returns an error message, empty on success.
[[nodiscard]] std::string appendWaiver(const std::filesystem::path& file, const cdc::WaiverRecord& w,
                                       const std::vector<std::string>& taken);

/// Replaces waiver `id` in `file` by `w` (its id may change; `taken` as for
/// appendWaiver, `id` itself excluded). Fields other than the waiver's
/// (e.g. "comment") and the entry's position are kept. Returns an error
/// message, empty on success.
[[nodiscard]] std::string updateWaiver(const std::filesystem::path& file, const std::string& id,
                                       const cdc::WaiverRecord& w, const std::vector<std::string>& taken);

/// Removes waiver `id` from `file`. Returns an error message, empty on success.
[[nodiscard]] std::string removeWaiver(const std::filesystem::path& file, const std::string& id);

/// The first id "W-<n>" (n = 1, 2, ...) not in `taken`.
[[nodiscard]] std::string nextWaiverId(const std::vector<std::string>& taken);

/// A waiver for one CDC path's rule hit (rule + path fingerprint) or for one
/// output crossing (CDC-OUTPUT-CLOCK + its fingerprint); id, reason, author
/// and expiry are left to the caller.
[[nodiscard]] cdc::WaiverRecord waiverFor(const cdc::CdcPath& path, const cdc::RuleHit& hit);
[[nodiscard]] cdc::WaiverRecord waiverFor(const cdc::OutputCrossing& output);

struct ProjectConfig;

struct AddedWaiver {
    std::string error;          ///< empty on success
    std::string id;             ///< the waiver's id (generated when it had none)
    std::filesystem::path file; ///< the waiver file written
    bool projectChanged = false; ///< the file was added to the project's waiver files (save the project)
};

/// Adds a waiver to a project: to `file`, or (empty) to the project's first
/// waiver file, or to "waivers.json" next to the project file. A file the
/// project does not list yet is added to cfg.waivers. Without an id the
/// waiver gets the next free "W-<n>" (ids are unique across all of the
/// project's waiver files).
[[nodiscard]] AddedWaiver addProjectWaiver(ProjectConfig& cfg, cdc::WaiverRecord w,
                                           std::filesystem::path file = {});

} // namespace opencdc::engine

#pragma once

#include "cdc/CdcModel.h"
#include "core/Diagnostic.h"

#include <string>
#include <string_view>
#include <vector>

namespace opencdc::rules {

/// Glob match: `*` matches any sequence (including dots), `?` one character.
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view text) noexcept;

/// Does the waiver cover this hit of this path? Empty fields match anything;
/// `rule`, `source`, `destination` are globs, `fingerprint` is exact.
[[nodiscard]] bool covers(const cdc::WaiverRecord& w, const cdc::CdcPath& p, const cdc::RuleHit& hit);

/// Applies waivers to a rule-checked result (after applyRules): the first
/// matching, not expired waiver covers a hit (RuleHit::waiver). Path status
/// is recomputed from the hits that are not waived; a path whose hits are
/// all waived is safe and marked `waived`. Waivers whose expiry date
/// (YYYY-MM-DD) is before `today` are not applied (WAIVER-EXPIRED); waivers
/// that cover nothing are reported (WAIVER-UNUSED). The records, with their
/// use counts, are stored in result.waivers.
void applyWaivers(std::vector<cdc::WaiverRecord> waivers, cdc::CdcResult& result, std::string_view today,
                  DiagnosticSink& sink);

} // namespace opencdc::rules

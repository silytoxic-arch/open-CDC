#pragma once

#include "cdc/CdcAnalyzer.h"
#include "cdc/CdcModel.h"
#include "clock/ClockModel.h"
#include "core/Diagnostic.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opencdc::rules {

/// Static description of a rule (also rendered in reports and the GUI).
/// The full specification of each rule is in docs/rules/<id>.md.
struct RuleMeta {
    std::string id;            ///< "CDC-001"
    std::string title;
    std::string purpose;
    std::string applicability;
    Severity defaultSeverity = Severity::Warning;
    std::string falsePositiveNotes;
    std::string fixSuggestion;
};

/// Read-only inputs of a rule check.
struct RuleContext {
    const cdc::CdcResult& cdc;
    const clock::ClockModel& clocks;
    const cdc::CdcOptions& options;
};

/// A rule adds hits to individual paths. Rules never change the analysis
/// itself (paths, synchronizer verdicts), only judge it.
class IRule {
public:
    IRule() = default;
    IRule(const IRule&) = delete;
    IRule& operator=(const IRule&) = delete;
    virtual ~IRule() = default;

    [[nodiscard]] virtual const RuleMeta& meta() const = 0;
    /// Returns the hit for `path`, if the rule applies to it.
    [[nodiscard]] virtual std::optional<cdc::RuleHit> check(const cdc::CdcPath& path, const RuleContext& ctx) const = 0;
};

/// Explicitly populated registry (no static self-registration).
class RuleRegistry {
public:
    void add(std::unique_ptr<IRule> rule);
    [[nodiscard]] std::span<const std::unique_ptr<IRule>> rules() const { return rules_; }
    [[nodiscard]] const IRule* find(std::string_view id) const;

    /// Registry with every implemented rule (CDC-001, 002, 003, 010).
    [[nodiscard]] static RuleRegistry standard();

private:
    std::vector<std::unique_ptr<IRule>> rules_;
};

/// Runs all rules over all paths and sets each path's hits and overall
/// status. Paths between clocks with an unknown relation (policy "report")
/// are downgraded from Violation to NeedsReview and get a CDC-010 hit.
void applyRules(const RuleRegistry& registry, cdc::CdcResult& result, const clock::ClockModel& clocks,
                const cdc::CdcOptions& options);

/// Rule hit counts per rule id (all statuses).
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> countHits(const cdc::CdcResult& result);

} // namespace opencdc::rules

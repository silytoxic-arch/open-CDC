#pragma once

#include "cdc/CdcModel.h"
#include "clock/ClockModel.h"
#include "constraints/Sdc.h"
#include "core/Diagnostic.h"
#include "graph/DesignView.h"
#include "core/Progress.h"
#include "engine/ProjectConfig.h"
#include "frontend/Frontend.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opencdc::engine {

/// Status of one analysis stage, reported so that users can see exactly
/// which parts of the flow actually ran.
enum class StageStatus { Completed, Failed, Cancelled, NotImplemented };

[[nodiscard]] std::string_view toString(StageStatus status) noexcept;

struct StageReport {
    std::string name;
    StageStatus status = StageStatus::NotImplemented;
    std::string detail;
    /// Wall time since the previous stage was recorded. Stages computed in
    /// one step report the whole time on the first of them (RTL extraction is
    /// part of the frontend; synchronizer and advanced analysis are part of
    /// "CDC path analysis").
    double seconds = 0.0;
};

/// Immutable result of one analysis run. Shared read-only between the
/// engine, CLI and GUI (std::shared_ptr<const AnalysisResult>).
struct AnalysisResult {
    ProjectConfig project;
    std::string startedAtUtc; ///< ISO-8601
    double elapsedSeconds = 0.0;
    std::optional<frontend::ElaborationSummary> elaboration;
    std::shared_ptr<const ir::Design> design; ///< null if RTL extraction did not run
    std::shared_ptr<const graph::DesignView> view;              ///< over `design` (declared after it)
    std::shared_ptr<const constraints::ConstraintSet> constraints;
    std::shared_ptr<const clock::ClockModel> clocks;           ///< null if clock analysis did not run
    std::shared_ptr<const cdc::CdcResult> cdc;                  ///< null if CDC analysis did not run
    std::vector<Diagnostic> diagnostics;
    std::vector<StageReport> stages;

    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] std::size_t count(Severity severity) const noexcept;
    /// Rule hits with status Violation and this severity that are not waived.
    [[nodiscard]] std::size_t violations(Severity severity) const noexcept;
    /// Rule hits covered by a waiver.
    [[nodiscard]] std::size_t waivedHits() const noexcept;
    /// Number of CDC paths with the given overall status.
    [[nodiscard]] std::size_t pathsWithStatus(cdc::AnalysisStatus status) const noexcept;
};

/// Facade used by both the CLI and the GUI. A session is cheap; create one
/// per run. `run` is safe to call from a worker thread.
class AnalysisSession {
public:
    AnalysisSession();
    explicit AnalysisSession(std::unique_ptr<frontend::IFrontend> frontend);

    [[nodiscard]] std::shared_ptr<const AnalysisResult> run(const ProjectConfig& config,
                                                            ProgressSink& progress,
                                                            const CancellationToken& cancel);

private:
    std::unique_ptr<frontend::IFrontend> frontend_;
};

} // namespace opencdc::engine

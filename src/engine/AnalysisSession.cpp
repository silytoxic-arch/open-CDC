#include "engine/AnalysisSession.h"

#include "engine/WaiverFile.h"
#include "rules/Waiver.h"

#include "cdc/CdcAnalyzer.h"
#include "clock/ClockAnalyzer.h"
#include "rules/RuleEngine.h"

#include <algorithm>
#include <map>
#include <chrono>
#include <ctime>
#include <utility>

namespace opencdc::engine {

std::string_view toString(StageStatus status) noexcept {
    switch (status) {
        case StageStatus::Completed: return "completed";
        case StageStatus::Failed: return "failed";
        case StageStatus::Cancelled: return "cancelled";
        case StageStatus::NotImplemented: return "not-implemented";
    }
    return "unknown";
}

bool AnalysisResult::hasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const Diagnostic& d) { return d.isError(); });
}

std::size_t AnalysisResult::violations(Severity severity) const noexcept {
    if (!cdc)
        return 0;
    std::size_t n = 0;
    for (const auto& p : cdc->paths)
        for (const auto& h : p.hits)
            if (h.status == cdc::AnalysisStatus::Violation && h.severity == severity && h.waiver.empty())
                ++n;
    return n;
}

std::size_t AnalysisResult::waivedHits() const noexcept {
    return cdc ? cdc->stats.waivedHits : 0;
}

std::size_t AnalysisResult::pathsWithStatus(cdc::AnalysisStatus status) const noexcept {
    if (!cdc)
        return 0;
    return static_cast<std::size_t>(
        std::ranges::count_if(cdc->paths, [status](const cdc::CdcPath& p) { return p.status == status; }));
}

std::size_t AnalysisResult::count(Severity severity) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        diagnostics, [severity](const Diagnostic& d) { return d.severity == severity; }));
}

namespace {

/// Today's date (UTC) as YYYY-MM-DD, for waiver expiry.
std::string todayUtc() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string utcNowIso8601() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// Stages that exist in the architecture but are not implemented yet.
/// They are reported explicitly so no output can be mistaken for real
/// analysis. Since stage 7 every planned stage is implemented.
const std::vector<std::pair<std::string, std::string>>& pendingStages() {
    static const std::vector<std::pair<std::string, std::string>> stages;
    return stages;
}

} // namespace

AnalysisSession::AnalysisSession() : AnalysisSession(frontend::makeSlangFrontend()) {}

AnalysisSession::AnalysisSession(std::unique_ptr<frontend::IFrontend> frontend)
    : frontend_(std::move(frontend)) {}

std::shared_ptr<const AnalysisResult> AnalysisSession::run(const ProjectConfig& config,
                                                           ProgressSink& progress,
                                                           const CancellationToken& cancel) {
    auto result = std::make_shared<AnalysisResult>();
    result->project = config;
    result->startedAtUtc = utcNowIso8601();
    const auto t0 = std::chrono::steady_clock::now();
    // Stages are recorded right after their work: the time since the
    // previous record is the stage's wall time.
    auto pushStage = [&result, last = t0](StageReport stage) mutable {
        const auto now = std::chrono::steady_clock::now();
        stage.seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        result->stages.push_back(std::move(stage));
    };

    CollectingDiagnosticSink sink;

    // ---- Stage: frontend (parse + elaborate) ----
    const std::string frontendStage = "Parse & elaborate (frontend)";
    if (cancel.cancelled()) {
        pushStage({frontendStage, StageStatus::Cancelled, "cancelled before start"});
    }
    else {
        progress.update(frontendStage, 0.0);
        auto elab = frontend_->elaborate(config.toFrontendInput(), sink);
        if (elab) {
            auto output = std::move(elab).value();
            result->elaboration = std::move(output.summary);
            result->design = std::move(output.design);
            const bool errors = sink.hasErrors();
            pushStage(
                {frontendStage, errors ? StageStatus::Failed : StageStatus::Completed,
                 std::to_string(result->elaboration->definitions.size()) + " definitions, " +
                     std::to_string(result->elaboration->instanceCount) + " instances" +
                     (errors ? "; frontend reported errors" : "")});
        }
        else {
            sink.report(std::move(elab).error());
            pushStage({frontendStage, StageStatus::Failed, "frontend could not run"});
        }
        progress.update(frontendStage, 1.0);
    }

    // ---- Stage: RTL extraction (performed by the frontend) ----
    const std::string irStage = "RTL extraction (Design IR)";
    if (result->design) {
        const auto st = result->design->stats();
        pushStage(
            {irStage, StageStatus::Completed,
             std::to_string(st.uniqueBodies) + " module bodies, " + std::to_string(st.registers) + " registers (" +
                 std::to_string(st.registerBits) + " bits), " + std::to_string(st.latches) + " latches, " +
                 std::to_string(st.unsupported) + " unsupported construct(s)"});
    }
    else {
        pushStage({irStage, cancel.cancelled() ? StageStatus::Cancelled : StageStatus::Failed,
                                  "not available (HDL errors or frontend failure)"});
    }

    // ---- Stages: constraints + clock domain analysis ----
    const std::string conStage = "Constraint reading (SDC/XDC subset)";
    const std::string clkStage = "Clock domain analysis";
    if (result->design && !cancel.cancelled()) {
        progress.update(clkStage, 0.5);
        auto view = std::make_shared<graph::DesignView>(*result->design);
        auto cons = std::make_shared<constraints::ConstraintSet>();
        std::size_t readFiles = 0;
        if (!config.constraints.empty()) {
            constraints::DesignObjectResolver resolver(*view);
            for (const auto& f : config.constraints)
                if (constraints::readSdcFile(f, resolver, *cons, sink))
                    ++readFiles;
        }
        pushStage(
            {conStage, readFiles == config.constraints.size() ? StageStatus::Completed : StageStatus::Failed,
             std::to_string(readFiles) + "/" + std::to_string(config.constraints.size()) + " file(s), " +
                 std::to_string(cons->clocks.size()) + " clock definition(s), " +
                 std::to_string(cons->groups.size()) + " clock group constraint(s)"});

        auto clocks = clock::analyzeClocks(*view, *cons, sink);
        std::size_t unknownPairs = 0;
        for (std::size_t i = 0; i < clocks->clocks.size(); ++i)
            for (std::size_t j = i + 1; j < clocks->clocks.size(); ++j)
                if (clocks->relation(clock::ClockId{static_cast<std::uint32_t>(i)},
                                     clock::ClockId{static_cast<std::uint32_t>(j)}) == clock::ClockRelation::Unknown)
                    ++unknownPairs;
        pushStage(
            {clkStage, StageStatus::Completed,
             std::to_string(clocks->clocks.size()) + " clock(s), " + std::to_string(clocks->resolvedRegisters()) + "/" +
                 std::to_string(clocks->registers.size()) + " registers resolved, " + std::to_string(unknownPairs) +
                 " clock pair(s) with unknown relation (policy: " +
                 std::string(toString(config.options.unknownClockRelation)) + ")"});
        progress.update(clkStage, 1.0);

        // ---- Stages: CDC paths, synchronizers, rules ----
        if (!cancel.cancelled()) {
            progress.update("CDC path analysis", 0.0);
            cdc::CdcOptions opt;
            opt.minSyncStages = config.options.minSyncStages;
            opt.reconvergenceDepth = config.options.reconvergenceDepth;
            switch (config.options.unknownClockRelation) {
                case UnknownClockPolicy::Report: opt.unknownRelation = cdc::UnknownRelationPolicy::Report; break;
                case UnknownClockPolicy::AssumeAsync:
                    opt.unknownRelation = cdc::UnknownRelationPolicy::AssumeAsync;
                    break;
                case UnknownClockPolicy::AssumeSync: opt.unknownRelation = cdc::UnknownRelationPolicy::AssumeSync; break;
            }
            auto cdcResult = cdc::analyzeCdc(*view, *clocks, *cons, opt, sink);
            const auto& st = cdcResult->stats;
            pushStage(
                {"CDC path analysis", StageStatus::Completed,
                 std::to_string(st.paths) + " crossing path(s), " + std::to_string(st.unknownRelation) +
                     " with unknown clock relation; " + std::to_string(st.excludedNoDomainSources) +
                     " source(s) without clock domain not checked; " + std::to_string(st.asyncResetCrossings) +
                     " asynchronous reset connection(s) crossing domains"});
            std::map<std::string, std::size_t> byType;
            for (const auto& s : cdcResult->structures)
                ++byType[std::string(cdc::toString(s.type))];
            std::string types;
            for (const auto& [t, n] : byType)
                types += (types.empty() ? "" : ", ") + t + " x" + std::to_string(n);
            pushStage({"Synchronizer recognition", StageStatus::Completed,
                                      std::to_string(cdcResult->structures.size()) + " synchronizer structure(s)" +
                                          (types.empty() ? "" : " (" + types + ")") + "; " +
                                          std::to_string(st.synchronized) + " synchronized and " +
                                          std::to_string(st.unsynchronized) + " unsynchronized path(s)"});
            pushStage({"Advanced CDC analysis", StageStatus::Completed,
                                      std::to_string(st.resetSynchronizers) + " reset synchronizer(s), " +
                                          std::to_string(st.reconvergencePoints) +
                                          " reconvergence point(s) (depth " +
                                          std::to_string(opt.reconvergenceDepth) + ")"});
            const auto registry = rules::RuleRegistry::standard();
            rules::applyRules(registry, *cdcResult, *clocks, opt);
            std::string hits;
            for (const auto& [id, n] : rules::countHits(*cdcResult))
                hits += (hits.empty() ? "" : ", ") + id + " x" + std::to_string(n);
            std::string ids;
            for (const auto& r : registry.rules())
                ids += (ids.empty() ? "" : ", ") + r->meta().id;
            pushStage({"CDC rule checks", StageStatus::Completed,
                                      std::to_string(registry.rules().size()) + " rules (" + ids + "); hits: " +
                                          (hits.empty() ? "none" : hits)});

            // ---- Waivers ----
            std::vector<cdc::WaiverRecord> waivers;
            for (const auto& f : config.waivers)
                readWaiverFile(f, waivers, sink);
            const std::size_t defined = waivers.size();
            rules::applyWaivers(std::move(waivers), *cdcResult, todayUtc(), sink);
            std::size_t expired = 0, unused = 0;
            for (const auto& w : cdcResult->waivers) {
                expired += w.expired ? 1 : 0;
                unused += !w.expired && w.uses == 0 ? 1 : 0;
            }
            pushStage(
                {"Waiver application", StageStatus::Completed,
                 config.waivers.empty()
                     ? std::string("no waiver file in the project")
                     : std::to_string(defined) + " waiver(s) from " + std::to_string(config.waivers.size()) +
                           " file(s); " + std::to_string(cdcResult->stats.waivedHits) + " rule hit(s) waived, " +
                           std::to_string(expired) + " expired, " + std::to_string(unused) + " unused"});
            result->cdc = std::move(cdcResult);
            progress.update("CDC rule checks", 1.0);
        }
        result->view = std::move(view);
        result->constraints = std::move(cons);
        result->clocks = std::move(clocks);
    }
    else {
        const auto status = cancel.cancelled() ? StageStatus::Cancelled : StageStatus::Failed;
        pushStage({conStage, status, "not run (no Design IR)"});
        pushStage({clkStage, status, "not run (no Design IR)"});
    }
    if (!result->cdc) {
        const auto status = cancel.cancelled() ? StageStatus::Cancelled : StageStatus::Failed;
        for (const char* name : {"CDC path analysis", "Synchronizer recognition", "Advanced CDC analysis",
                                 "CDC rule checks", "Waiver application"})
            pushStage({name, status, "not run (no clock analysis)"});
    }

    for (const auto& [name, detail] : pendingStages())
        pushStage({name, StageStatus::NotImplemented, detail});

    result->diagnostics = sink.take();
    result->elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace opencdc::engine

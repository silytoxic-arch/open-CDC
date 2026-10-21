#include "engine/AnalysisSession.h"
#include "engine/ResultJson.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <vector>

using namespace opencdc;
using namespace opencdc::engine;

namespace {

ProjectConfig smokeProject() {
    auto r = loadProjectFile(test::examplesDir() / "smoke" / "smoke.ocdc.json");
    REQUIRE(r.ok());
    return r.value();
}

class RecordingProgress final : public ProgressSink {
public:
    void update(std::string_view stage, double fraction) override {
        std::lock_guard lock(mutex_);
        events.emplace_back(std::string(stage), fraction);
    }
    std::vector<std::pair<std::string, double>> events;

private:
    std::mutex mutex_;
};

/// Frontend stub used to test the session independently of slang.
class FailingFrontend final : public frontend::IFrontend {
public:
    Result<frontend::FrontendOutput> elaborate(const frontend::FrontendInput&, DiagnosticSink&) override {
        return Diagnostic{"FE-STUB", Severity::Error, "stub failure", {}, {}};
    }
};

const StageReport* findStage(const AnalysisResult& r, std::string_view prefix) {
    auto it = std::ranges::find_if(r.stages, [&](const StageReport& s) { return s.name.starts_with(prefix); });
    return it == r.stages.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Session elaborates the smoke project and marks pending stages", "[engine][session]") {
    RecordingProgress progress;
    CancellationToken cancel;
    AnalysisSession session;
    auto result = session.run(smokeProject(), progress, cancel);

    REQUIRE(result);
    REQUIRE(result->elaboration.has_value());
    CHECK_FALSE(result->hasErrors());
    CHECK(result->elaboration->instanceCount == 8);

    const auto* fe = findStage(*result, "Parse & elaborate");
    REQUIRE(fe);
    CHECK(fe->status == StageStatus::Completed);

    const auto* cdc = findStage(*result, "CDC path analysis");
    REQUIRE(cdc);
    CHECK(cdc->status == StageStatus::Completed);
    const auto* adv = findStage(*result, "Advanced CDC analysis");
    REQUIRE(adv);
    CHECK(adv->status == StageStatus::Completed);
    // Every stage of the architecture is implemented now.
    CHECK(std::ranges::none_of(result->stages,
                               [](const auto& s) { return s.status == StageStatus::NotImplemented; }));
    const auto* waivers = findStage(*result, "Waiver application");
    REQUIRE(waivers);
    CHECK(waivers->status == StageStatus::Completed);
    REQUIRE(result->cdc);
    CHECK(result->pathsWithStatus(cdc::AnalysisStatus::PotentialRisk) == 1); // rst_b_n (CDC-006)
    CHECK(result->pathsWithStatus(cdc::AnalysisStatus::NeedsReview) == 4);   // status[3:0] (CDC-008)

    REQUIRE(progress.events.size() >= 2);
    CHECK(progress.events.back().second == 1.0);
}

TEST_CASE("Session reports a missing constraint file", "[engine][session]") {
    auto cfg = smokeProject();
    cfg.constraints = {test::examplesDir() / "smoke" / "no_such.sdc"};
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = AnalysisSession().run(cfg, progress, cancel);
    auto it = std::ranges::find(result->diagnostics, std::string("CON-OPEN"), &Diagnostic::code);
    REQUIRE(it != result->diagnostics.end());
    CHECK(it->severity == Severity::Error);
    CHECK(findStage(*result, "Constraint reading")->status == StageStatus::Failed);
    REQUIRE(result->clocks); // clock analysis still runs without the file
}

TEST_CASE("Session runs clock analysis with the project constraints", "[engine][session]") {
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = AnalysisSession().run(smokeProject(), progress, cancel);
    REQUIRE(result->clocks);
    const auto& cm = *result->clocks;
    REQUIRE(cm.clocks.size() == 2);
    auto a = cm.find("clk_a");
    auto b = cm.find("clk_b");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(cm.relation(*a, *b) == clock::ClockRelation::Asynchronous);
    CHECK(cm.clock(*b).periodNs == 6.4);
    CHECK(cm.clock(*a).registers() == 4);
    CHECK(cm.clock(*b).registers() == 11);
    CHECK(findStage(*result, "Clock domain analysis")->status == StageStatus::Completed);
    CHECK(findStage(*result, "Constraint reading")->status == StageStatus::Completed);
    CHECK(result->constraints->ignoredCommands.at("set_false_path") == 1);
}

TEST_CASE("Session honours a cancelled token", "[engine][session]") {
    NullProgressSink progress;
    CancellationToken cancel;
    cancel.cancel();
    auto result = AnalysisSession().run(smokeProject(), progress, cancel);
    CHECK_FALSE(result->elaboration.has_value());
    const auto* fe = findStage(*result, "Parse & elaborate");
    REQUIRE(fe);
    CHECK(fe->status == StageStatus::Cancelled);
}

TEST_CASE("Session reports frontend failure", "[engine][session]") {
    NullProgressSink progress;
    CancellationToken cancel;
    AnalysisSession session(std::make_unique<FailingFrontend>());
    auto result = session.run(smokeProject(), progress, cancel);
    CHECK(result->hasErrors());
    CHECK_FALSE(result->elaboration.has_value());
    REQUIRE_FALSE(result->diagnostics.empty());
    CHECK(result->diagnostics.front().code == "FE-STUB");
    CHECK(findStage(*result, "Parse & elaborate")->status == StageStatus::Failed);
}

TEST_CASE("Result JSON contains the elaborated hierarchy", "[engine][json]") {
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = AnalysisSession().run(smokeProject(), progress, cancel);
    auto j = nlohmann::json::parse(toJson(*result));

    CHECK(j["format"] == kResultFormatName);
    CHECK(j["formatVersion"] == 2);
    CHECK(j["cdcAnalysisAvailable"] == true);
    // Stage-5 CDC fields.
    REQUIRE(j["cdc"]["structures"].size() == 6);
    CHECK(j["cdc"]["structures"][0]["type"] == "qualified-data");
    CHECK(j["cdc"]["stats"]["asyncResetCrossings"] == 11);
    CHECK(j["summary"]["pathsPotentialRisk"] == 1);
    CHECK(j["summary"]["pathsNeedReview"] == 4);
    bool resetPath = false;
    for (const auto& p : j["cdc"]["paths"]) {
        CHECK(p.contains("reconvergences"));
        CHECK(p.contains("splitWith"));
        if (p["pin"] == "async-reset") {
            resetPath = true;
            CHECK(p["sourceUnclocked"] == true);
            CHECK(p["groupSize"] == 11);
            CHECK(p["ruleHits"][0]["rule"] == "CDC-006");
        }
        else {
            CHECK(p["structure"].is_number());
        }
    }
    CHECK(resetPath);
    CHECK(j["project"]["name"] == "smoke");
    CHECK(j["summary"]["errors"] == 0);
    REQUIRE(j["elaboration"]["hierarchy"].size() == 1);
    const auto& top = j["elaboration"]["hierarchy"][0];
    CHECK(top["definition"] == "top");
    CHECK(top["children"].size() == 2);
    CHECK(j["elaboration"]["instanceCount"] == 8);

    // RTL section: flattened registers with clock and reset.
    REQUIRE(j["rtl"].is_object());
    CHECK(j["rtl"]["stats"]["registers"] == 15);
    CHECK(j["rtl"]["unsupported"].empty());
    bool found = false;
    for (const auto& reg : j["rtl"]["registers"]) {
        if (reg["instance"] == "top.u_rx.u_valid_sync" && reg["q"] == "meta") {
            found = true;
            CHECK(reg["clock"] == "clk");
            CHECK(reg["edge"] == "posedge");
            CHECK(reg["asyncResets"][0]["signal"] == "rst_n");
            CHECK(reg["asyncResets"][0]["activeHigh"] == false);
            CHECK(reg["enable"].is_null());
            CHECK(reg["domain"]["status"] == "resolved");
            CHECK(reg["domain"]["clocks"][0]["clock"] == "clk_b");
        }
    }
    CHECK(found);

    REQUIRE(j["clockAnalysis"].is_object());
    CHECK(j["clockAnalysis"]["clocks"].size() == 2);
    CHECK(j["clockAnalysis"]["relations"][0]["relation"] == "asynchronous");
    CHECK(j["clockAnalysis"]["unknownRelationPolicy"] == "report");
    CHECK(j["clockAnalysis"]["constraints"]["ignoredCommands"]["set_false_path"] == 1);
}

TEST_CASE("Session reports the RTL extraction stage", "[engine][session]") {
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = AnalysisSession().run(smokeProject(), progress, cancel);
    REQUIRE(result->design);
    const auto* rtl = findStage(*result, "RTL extraction");
    REQUIRE(rtl);
    CHECK(rtl->status == StageStatus::Completed);
    CHECK(rtl->detail.find("15 registers") != std::string::npos);
}

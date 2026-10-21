// Stage 7: waivers (matching, application, file format, session integration).

#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "engine/ResultJson.h"
#include "engine/WaiverFile.h"
#include "rules/Waiver.h"

#include "../TestUtil.h"
#include "../cdc/CdcTestUtil.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

using namespace opencdc;
using namespace opencdc::cdc;
using opencdc::rules::applyWaivers;
using opencdc::rules::globMatch;

namespace {

WaiverRecord waiver(std::string id, std::string rule, std::string source = {}, std::string expires = {}) {
    WaiverRecord w;
    w.id = std::move(id);
    w.rule = std::move(rule);
    w.source = std::move(source);
    w.reason = "test";
    w.expires = std::move(expires);
    return w;
}

constexpr const char* kTwoBugs = "module top(input clk_a, input clk_b, input d, input e, output reg y, output reg z);\n"
                                 "  reg a, c;\n"
                                 "  always @(posedge clk_a) begin a <= d; c <= e; end\n"
                                 "  always @(posedge clk_b) begin y <= a & e; z <= c & e; end\n"
                                 "endmodule\n";

std::filesystem::path writeFile(const std::string& name, const std::string& text) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

std::size_t countCode(const std::vector<Diagnostic>& d, std::string_view code) {
    return static_cast<std::size_t>(std::ranges::count(d, std::string(code), &Diagnostic::code));
}

} // namespace

TEST_CASE("Glob matching", "[waiver]") {
    CHECK(globMatch("*", ""));
    CHECK(globMatch("*", "top.u.a"));
    CHECK(globMatch("top.*", "top.u.a"));
    CHECK(globMatch("top.u?.a", "top.u1.a"));
    CHECK_FALSE(globMatch("top.u?.a", "top.u12.a"));
    CHECK(globMatch("CDC-00*", "CDC-007"));
    CHECK_FALSE(globMatch("CDC-00*", "CDC-010"));
    CHECK(globMatch("*a*b*", "xxaxxbxx"));
    CHECK_FALSE(globMatch("*a*b", "xxaxxbxx"));
    CHECK(globMatch("top.g[0].*", "top.g[0].s1")); // brackets are literal
    CHECK_FALSE(globMatch("top.a", "top.ab"));
}

TEST_CASE("Waivers cover hits, recompute status, and report expired/unused", "[waiver]") {
    auto r = test::cdc::analyze(kTwoBugs);
    REQUIRE(r.cdc->paths.size() == 2);
    const auto& pa = r.path("top.a", "top.y");
    REQUIRE(test::cdc::hasHit(pa, "CDC-001"));
    REQUIRE(pa.status == AnalysisStatus::Violation);

    CollectingDiagnosticSink sink;
    std::vector<WaiverRecord> ws{waiver("W-A", "CDC-001", "top.a"),          // covers a -> y
                                 waiver("W-OLD", "CDC-001", "top.c", "2020-01-01"), // expired
                                 waiver("W-NONE", "CDC-009")};                // unused
    applyWaivers(ws, *r.cdc, "2026-10-21", sink);
    const auto diags = sink.take();
    CHECK(countCode(diags, "WAIVER-EXPIRED") == 1);
    CHECK(countCode(diags, "WAIVER-UNUSED") == 1);

    const auto& a = r.path("top.a", "top.y");
    CHECK(a.hits.front().waiver == "W-A");
    CHECK(a.waived);
    CHECK(a.status == AnalysisStatus::Safe);
    const auto& c = r.path("top.c", "top.z");
    CHECK(c.hits.front().waiver.empty()); // expired waiver not applied
    CHECK_FALSE(c.waived);
    CHECK(c.status == AnalysisStatus::Violation);
    CHECK(r.cdc->stats.waivedHits == 1);
    REQUIRE(r.cdc->waivers.size() == 3);
    CHECK(r.cdc->waivers[0].uses == 1);
    CHECK(r.cdc->waivers[1].expired);
    CHECK(r.cdc->waivers[2].uses == 0);
}

TEST_CASE("Waiver by fingerprint and destination glob; partial waiver keeps the rest", "[waiver]") {
    auto r = test::cdc::analyze("module top(input clk_a, input clk_b, input clk_c, input x, input z, output y);\n"
                                "  reg a, c, s1, s2;\n"
                                "  always @(posedge clk_a) a <= x;\n"
                                "  always @(posedge clk_c) c <= z;\n"
                                "  always @(posedge clk_b) begin s1 <= a | c; s2 <= s1; end\n"
                                "  assign y = s2;\n"
                                "endmodule\n",
                                "set_clock_groups -asynchronous -group clk_a -group clk_b -group clk_c\n");
    const auto& p = r.path("top.a", "top.s1");
    REQUIRE(p.hits.size() == 2); // CDC-004 + CDC-009
    WaiverRecord byFp;
    byFp.id = "W-FP";
    byFp.fingerprint = p.fingerprint;
    byFp.rule = "CDC-009";
    byFp.reason = "test";
    WaiverRecord byDst = waiver("W-DST", "CDC-00?");
    byDst.destination = "top.s*";
    byDst.source = "top.c";
    CollectingDiagnosticSink sink;
    applyWaivers({byFp, byDst}, *r.cdc, "2026-10-21", sink);
    const auto& a = r.path("top.a", "top.s1");
    CHECK_FALSE(a.waived); // CDC-004 remains
    CHECK(a.status == AnalysisStatus::Violation);
    const auto& c = r.path("top.c", "top.s1");
    CHECK(c.waived); // both hits covered by W-DST (first matching waiver wins)
    CHECK(c.status == AnalysisStatus::Safe);
    CHECK(std::ranges::all_of(c.hits, [](const RuleHit& h) { return h.waiver == "W-DST"; }));
}

TEST_CASE("Waiver file: valid entries are read, invalid ones reported", "[waiver][engine]") {
    const auto file = writeFile("opencdc_waivers_test.json",
                                "{\n"
                                "  \"format\": \"opencdc-waivers\",\n"
                                "  \"version\": 1,\n"
                                "  \"waivers\": [\n"
                                "    {\"id\": \"OK\", \"rule\": \"CDC-001\", \"reason\": \"fine\", \"expires\": \"2030-01-01\"},\n"
                                "    {\"id\": \"NOREASON\", \"rule\": \"CDC-001\"},\n"
                                "    {\"id\": \"ALL\", \"reason\": \"everything\"},\n"
                                "    {\"id\": \"BADDATE\", \"rule\": \"CDC-002\", \"reason\": \"x\", \"expires\": \"tomorrow\"},\n"
                                "    {\"id\": \"OK\", \"rule\": \"CDC-002\", \"reason\": \"duplicate\"},\n"
                                "    {\"id\": \"EXTRA\", \"rule\": \"CDC-003\", \"reason\": \"x\", \"color\": \"red\"}\n"
                                "  ]\n"
                                "}\n");
    std::vector<WaiverRecord> out;
    CollectingDiagnosticSink sink;
    engine::readWaiverFile(file, out, sink);
    const auto diags = sink.take();
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == "OK");
    CHECK(out[0].loc.line == 5);
    CHECK(out[0].expires == "2030-01-01");
    CHECK(out[1].id == "EXTRA");
    CHECK(countCode(diags, "WAIVER-INVALID") == 4);
    CHECK(countCode(diags, "WAIVER-SCHEMA") == 1); // unknown field (warning)
    std::filesystem::remove(file);

    std::vector<WaiverRecord> none;
    engine::readWaiverFile(writeFile("opencdc_waivers_bad.json", "{\"format\": \"other\"}"), none, sink);
    CHECK(none.empty());
    CHECK(countCode(sink.take(), "WAIVER-SCHEMA") == 1);
    engine::readWaiverFile("/no/such/waivers.json", none, sink);
    CHECK(countCode(sink.take(), "WAIVER-OPEN") == 1);
}

TEST_CASE("Session applies the project's waivers (cdc_advanced_waived example)", "[waiver][engine][example]") {
    auto cfg = engine::loadProjectFile(test::examplesDir() / "cdc_advanced" / "cdc_advanced_waived.ocdc.json");
    REQUIRE(cfg);
    REQUIRE(cfg.value().waivers.size() == 1);
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = engine::AnalysisSession().run(cfg.value(), progress, cancel);
    REQUIRE(result->cdc);
    CHECK(result->waivedHits() == 3);
    CHECK(result->violations(Severity::Error) == 3);     // CDC-004 x2 and CDC-006 remain
    CHECK(result->pathsWithStatus(AnalysisStatus::PotentialRisk) == 1); // only a_kick (CDC-007)
    const auto it = std::ranges::find_if(result->stages, [](const auto& s) { return s.name == "Waiver application"; });
    REQUIRE(it != result->stages.end());
    CHECK(it->detail.find("3 rule hit(s) waived") != std::string::npos);
    auto j = nlohmann::json::parse(engine::toJson(*result));
    CHECK(j["summary"]["waivedHits"] == 3);
    REQUIRE(j["cdc"]["waivers"].size() == 2);
    CHECK(j["cdc"]["waivers"][0]["uses"] == 1);
    CHECK(j["cdc"]["waivers"][1]["uses"] == 2);
    bool waivedHit = false;
    for (const auto& p : j["cdc"]["paths"])
        for (const auto& h : p["ruleHits"])
            waivedHit = waivedHit || h["waiver"] == "W-CFG";
    CHECK(waivedHit);
}

// Stage 7: waivers (matching, application, file format, session integration).

#include "core/Progress.h"
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

TEST_CASE("Waivers cover output ports driven asynchronously (CDC-OUTPUT-CLOCK)", "[waiver][outputs]") {
    constexpr const char* kOut = "module top(input clk_a, input clk_b, input d, output qa, output qb);\n"
                                 "  reg ra, rb;\n"
                                 "  always @(posedge clk_a) ra <= d;\n"
                                 "  always @(posedge clk_b) rb <= d;\n"
                                 "  assign qa = ra;\n"
                                 "  assign qb = rb;\n"
                                 "endmodule\n";
    const std::string sdc = "create_clock -name a -period 10 [get_ports clk_a]\n"
                            "create_clock -name b -period 7 [get_ports clk_b]\n"
                            "create_clock -name c -period 9\n"
                            "set_clock_groups -asynchronous -group a -group b -group c\n"
                            "set_output_delay -clock c 1 [get_ports {qa qb}]\n";
    auto r = test::cdc::analyze(kOut, sdc);
    REQUIRE(r.cdc->outputs.size() == 2); // drivers of clock a and of clock b
    CHECK(countCode(r.diags, "CDC-OUTPUT-CLOCK") == 2);
    for (const auto& o : r.cdc->outputs)
        CHECK(o.fingerprint.size() == 16);

    // a waiver by destination port covers the record of clock b only
    CollectingDiagnosticSink sink;
    WaiverRecord w = waiver("W-OUT", "CDC-OUTPUT-CLOCK");
    w.destination = "top.qb";
    WaiverRecord other = waiver("W-RULE", "CDC-001"); // negative: another rule covers nothing here
    applyWaivers({w, other}, *r.cdc, "2026-10-27", sink);
    rules::reportOutputCrossings(*r.cdc, sink);
    const auto diags = sink.take();
    CHECK(countCode(diags, "CDC-OUTPUT-CLOCK") == 1);
    CHECK(countCode(diags, "WAIVER-UNUSED") == 1); // W-RULE
    CHECK(r.cdc->stats.outputCrossings == 1);
    REQUIRE(r.cdc->waivers.size() == 2);
    CHECK(r.cdc->waivers[0].uses == 1);
    const auto waived = std::ranges::count_if(r.cdc->outputs, [](const OutputCrossing& o) { return o.waiver == "W-OUT"; });
    CHECK(waived == 1);
    // by fingerprint
    auto r2 = test::cdc::analyze(kOut, sdc);
    WaiverRecord f = waiver("W-FP", "");
    f.fingerprint = r2.cdc->outputs.front().fingerprint;
    CollectingDiagnosticSink s2;
    applyWaivers({f}, *r2.cdc, "2026-10-27", s2);
    rules::reportOutputCrossings(*r2.cdc, s2);
    CHECK(countCode(s2.take(), "CDC-OUTPUT-CLOCK") == 1);
    // through a session: the records are in the JSON result, the waived one with its waiver
    test::TempDir dir;
    engine::ProjectConfig cfg;
    cfg.name = "out";
    cfg.top = "top";
    cfg.sources = {dir.write("t.v", kOut)};
    cfg.sourceLibraries = {""};
    cfg.constraints = {dir.write("c.sdc", sdc)};
    cfg.waivers = {dir.write("w.json", "{\"format\": \"opencdc-waivers\", \"version\": 1, \"waivers\": [{\"id\": "
                                       "\"W-OUT\", \"rule\": \"CDC-OUTPUT-CLOCK\", \"destination\": \"top.qb\", "
                                       "\"reason\": \"receiver synchronizes\"}]}")};
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto res = session.run(cfg, progress, cancel);
    REQUIRE(res);
    CHECK(countCode(res->diagnostics, "CDC-OUTPUT-CLOCK") == 1);
    CHECK(countCode(res->diagnostics, "WAIVER-UNUSED") == 0);
    const auto j = nlohmann::json::parse(engine::toJson(*res));
    REQUIRE(j["cdc"].contains("outputCrossings"));
    CHECK(j["cdc"]["outputCrossings"].size() == 2);
    CHECK(j["cdc"]["stats"]["outputCrossings"] == 1);
}

TEST_CASE("output_ports example: one output reported, one waived, synchronized and same-clock outputs not",
          "[waiver][engine][example][outputs]") {
    auto cfg = engine::loadProjectFile(test::examplesDir() / "output_ports" / "output_ports.ocdc.json");
    REQUIRE(cfg);
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = engine::AnalysisSession().run(cfg.value(), progress, cancel);
    REQUIRE(result->cdc);
    const auto& outs = result->cdc->outputs;
    REQUIRE(outs.size() == 2); // sys_flag and dbg_led; io_data and io_req are driven from clk_io
    for (const auto& o : outs) {
        REQUIRE(o.ports.size() == 1);
        CHECK(o.srcClock == "clk_sys");
        CHECK(o.outClock == "clk_io");
        CHECK(o.waiver == (o.ports.front() == "output_ports.dbg_led" ? "W-LED" : ""));
    }
    CHECK(countCode(result->diagnostics, "CDC-OUTPUT-CLOCK") == 1);
    CHECK(countCode(result->diagnostics, "WAIVER-UNUSED") == 0);
    CHECK(result->cdc->stats.outputCrossings == 1);
    CHECK(result->violations(Severity::Error) == 0); // the internal crossing is synchronized
}

TEST_CASE("appendWaiver: creates a file, appends keeping the file's entries, refuses invalid waivers",
          "[waiver][engine]") {
    test::TempDir dir;
    const auto file = dir.path() / "w.json";
    cdc::WaiverRecord w;
    w.id = "W-1";
    w.rule = "CDC-007";
    w.fingerprint = "0123456789abcdef";
    w.reason = "level";
    CHECK(engine::appendWaiver(file, w, {}).empty());
    std::vector<cdc::WaiverRecord> read;
    CollectingDiagnosticSink sink;
    engine::readWaiverFile(file, read, sink);
    CHECK(sink.take().empty());
    REQUIRE(read.size() == 1);
    CHECK(read[0].fingerprint == "0123456789abcdef");
    CHECK(read[0].source.empty()); // fields left empty are not written

    // an existing file keeps its entries, field order and extra fields
    const auto own = dir.write("own.json", "{\"format\": \"opencdc-waivers\", \"version\": 1, \"note\": \"team file\",\n"
                                           " \"waivers\": [{\"reason\": \"r\", \"id\": \"A\", \"source\": \"top.x\"}]}\n");
    w.id = "B";
    CHECK(engine::appendWaiver(own, w, {"A"}).empty());
    std::ifstream in(own);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"note\": \"team file\"") != std::string::npos);
    CHECK(text.find("\"reason\": \"r\",\n      \"id\": \"A\"") != std::string::npos); // order kept
    read.clear();
    engine::readWaiverFile(own, read, sink);
    CHECK(sink.take().empty());
    REQUIRE(read.size() == 2);
    CHECK(read[1].id == "B");

    // negatives: duplicate id (in the file, or elsewhere in the project), no
    // reason, no selector, bad date, not a waiver file
    w.id = "A";
    CHECK(engine::appendWaiver(own, w, {}).find("already exists") != std::string::npos);
    w.id = "C";
    CHECK(engine::appendWaiver(own, w, {"C"}).find("already exists") != std::string::npos);
    auto noReason = w;
    noReason.reason.clear();
    CHECK(!engine::appendWaiver(own, noReason, {}).empty());
    auto noSelector = w;
    noSelector.rule.clear();
    noSelector.fingerprint.clear();
    CHECK(!engine::appendWaiver(own, noSelector, {}).empty());
    auto badDate = w;
    badDate.expires = "2027-1-1";
    CHECK(!engine::appendWaiver(own, badDate, {}).empty());
    const auto other = dir.write("other.json", "{\"format\": \"opencdc-project\"}");
    CHECK(engine::appendWaiver(other, w, {}).find("not an OpenCDC waiver file") != std::string::npos);
    read.clear();
    engine::readWaiverFile(own, read, sink);
    CHECK(read.size() == 2); // nothing was written by the refused calls

    CHECK(engine::nextWaiverId({}) == "W-1");
    CHECK(engine::nextWaiverId({"W-1", "W-2", "X"}) == "W-3");
}

TEST_CASE("A waiver made for one rule hit covers that path only (cdc_advanced, CDC-005 twice)",
          "[waiver][engine][example]") {
    test::TempDir dir;
    auto cfg = engine::loadProjectFile(test::examplesDir() / "cdc_advanced" / "cdc_advanced.ocdc.json");
    REQUIRE(cfg);
    cfg.value().projectFile = dir.path() / "p.ocdc.json"; // waivers.json goes next to it
    REQUIRE(cfg.value().waivers.empty());
    NullProgressSink progress;
    CancellationToken cancel;
    auto before = engine::AnalysisSession().run(cfg.value(), progress, cancel);
    REQUIRE(before->cdc);
    // the two CDC-005 hits (start and mode reconverge)
    std::vector<const cdc::CdcPath*> rc;
    for (const auto& p : before->cdc->paths)
        for (const auto& h : p.hits)
            if (h.ruleId == "CDC-005")
                rc.push_back(&p);
    REQUIRE(rc.size() == 2);
    const auto hit = std::ranges::find(rc[0]->hits, std::string("CDC-005"), &cdc::RuleHit::ruleId);
    auto w = engine::waiverFor(*rc[0], *hit);
    w.reason = "configuration phases";
    const auto added = engine::addProjectWaiver(cfg.value(), w);
    REQUIRE(added.error.empty());
    CHECK(added.id == "W-1");
    CHECK(added.projectChanged);
    CHECK(added.file == (dir.path() / "waivers.json").lexically_normal());
    REQUIRE(cfg.value().waivers.size() == 1);

    auto after = engine::AnalysisSession().run(cfg.value(), progress, cancel);
    REQUIRE(after->cdc);
    int waived = 0, open = 0;
    for (const auto& p : after->cdc->paths)
        for (const auto& h : p.hits)
            if (h.ruleId == "CDC-005")
                (h.waiver == "W-1" ? waived : open) += 1;
    CHECK(waived == 1); // this path
    CHECK(open == 1);   // negative: the other path with the same rule stays open
    CHECK(countCode(after->diagnostics, "WAIVER-UNUSED") == 0);

    // a second waiver goes to the same file with the next id; the project does not change
    auto w2 = w;
    w2.fingerprint = rc[1]->fingerprint;
    const auto again = engine::addProjectWaiver(cfg.value(), w2);
    REQUIRE(again.error.empty());
    CHECK(again.id == "W-2");
    CHECK(!again.projectChanged);
}

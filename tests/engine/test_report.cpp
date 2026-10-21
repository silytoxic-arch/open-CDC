// Stage 7: HTML and CSV reports.

#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "engine/Report.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace opencdc;
using namespace opencdc::engine;

namespace {

std::shared_ptr<const AnalysisResult> run(const char* project) {
    auto cfg = loadProjectFile(test::examplesDir() / "cdc_advanced" / project);
    REQUIRE(cfg);
    NullProgressSink progress;
    CancellationToken cancel;
    return AnalysisSession().run(cfg.value(), progress, cancel);
}

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t n = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
        ++n;
    return n;
}

/// Minimal RFC 4180 reader (quoted fields, doubled quotes, CRLF).
std::vector<std::vector<std::string>> parseCsv(std::string text) {
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                ++i;
            }
            else if (c == '"') {
                quoted = false;
            }
            else {
                field += c;
            }
        }
        else if (c == '"') {
            quoted = true;
        }
        else if (c == ',') {
            rows.back().push_back(std::move(field));
            field.clear();
        }
        else if (c == '\r') {
        }
        else if (c == '\n') {
            rows.back().push_back(std::move(field));
            field.clear();
            rows.emplace_back();
        }
        else {
            field += c;
        }
    }
    if (rows.back().empty())
        rows.pop_back();
    return rows;
}

} // namespace

TEST_CASE("HTML report contains every section, path and schematic", "[report]") {
    auto r = run("cdc_advanced_waived.ocdc.json");
    REQUIRE(r->cdc);
    const std::string html = toHtmlReport(*r);
    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.ends_with("</html>\n"));
    for (const char* id : {"summary", "stages", "clocks", "rules", "paths", "structures", "details", "waivers",
                           "diagnostics", "unsupported"})
        CHECK(html.find(std::string("id=\"") + id + "\"") != std::string::npos);
    const auto paths = r->cdc->paths.size();
    CHECK(count(html, "<div class=\"path\" id=\"p") == paths);
    CHECK(count(html, "<svg ") == paths);
    CHECK(count(html, "<svg ") == count(html, "</svg>"));
    CHECK(html.find("Waived by W-CFG") != std::string::npos);
    CHECK(html.find("CDC-010") != std::string::npos); // every rule is listed
    CHECK(html.find("<script") == std::string::npos); // no scripts needed

    HtmlReportOptions noSvg;
    noSvg.schematics = false;
    CHECK(count(toHtmlReport(*r, noSvg), "<svg ") == 0);
    HtmlReportOptions limited;
    limited.maxSchematics = 3;
    const auto small = toHtmlReport(*r, limited);
    CHECK(count(small, "<svg ") == 0);
    CHECK(small.find("Schematics are left out") != std::string::npos);
    CHECK(small.find("id=\"limited\"") == std::string::npos);

    // Path limit: only paths with hits or a non-safe status are listed.
    std::size_t attention = 0;
    for (const auto& p : r->cdc->paths)
        attention += !p.hits.empty() || p.status != cdc::AnalysisStatus::Safe ? 1U : 0U;
    REQUIRE(attention < paths);
    REQUIRE(attention > 2);
    HtmlReportOptions few;
    few.maxPaths = paths - 1;
    const auto focused = toHtmlReport(*r, few);
    CHECK(focused.find("id=\"limited\"") != std::string::npos);
    CHECK(count(focused, "<div class=\"path\" id=\"p") == attention);
    CHECK(count(focused, "<svg ") == attention);
    for (const auto& p : r->cdc->paths) {
        const bool listed = focused.find("id=\"p" + std::to_string(p.id) + "\"") != std::string::npos;
        CHECK(listed == (!p.hits.empty() || p.status != cdc::AnalysisStatus::Safe));
    }
    few.maxPaths = 2;
    const auto capped = toHtmlReport(*r, few);
    CHECK(count(capped, "<div class=\"path\" id=\"p") == 2);
    CHECK(capped.find("only the 2 of " + std::to_string(attention)) != std::string::npos);
}

TEST_CASE("CSV reports: one row per path / hit, RFC 4180 quoting", "[report]") {
    auto r = run("cdc_advanced_waived.ocdc.json");
    REQUIRE(r->cdc);
    const auto paths = parseCsv(pathsCsv(*r));
    REQUIRE(paths.size() == r->cdc->paths.size() + 1);
    const auto& header = paths.front();
    for (const auto& row : paths)
        CHECK(row.size() == header.size());
    CHECK(header.front() == "id");
    const auto col = [&](std::string_view name) {
        return static_cast<std::size_t>(std::ranges::find(header, name) - header.begin());
    };
    std::size_t waivedRows = 0;
    for (std::size_t i = 1; i < paths.size(); ++i) {
        const auto& p = r->cdc->paths[i - 1];
        CHECK(paths[i][col("id")] == std::to_string(p.id));
        CHECK(paths[i][col("source")] == p.srcName);
        CHECK(paths[i][col("fingerprint")] == p.fingerprint);
        waivedRows += paths[i][col("waived")] == "yes" ? 1U : 0U;
    }
    CHECK(waivedRows == 3); // a_start, a_mode, a_cfg

    std::size_t hits = 0;
    for (const auto& p : r->cdc->paths)
        hits += p.hits.size();
    const auto viol = parseCsv(violationsCsv(*r));
    REQUIRE(viol.size() == hits + 1);
    std::size_t waived = 0;
    bool quotedMessage = false;
    for (std::size_t i = 1; i < viol.size(); ++i) {
        CHECK(viol[i].size() == viol.front().size());
        waived += viol[i][4].empty() ? 0U : 1U;
        quotedMessage = quotedMessage || viol[i][5].find(',') != std::string::npos; // survived quoting
    }
    CHECK(waived == r->waivedHits());
    CHECK(quotedMessage);
}

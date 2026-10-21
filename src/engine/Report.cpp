#include "engine/Report.h"

#include "core/PathUtil.h"
#include "core/Version.h"
#include "rules/RuleEngine.h"
#include "schematic/Schematic.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace opencdc::engine {

using namespace opencdc::cdc;

namespace {

std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string loc(const SourceLocation& l) {
    return l.file.empty() ? std::string() : toString(l);
}

const char* statusClass(AnalysisStatus s) {
    switch (s) {
        case AnalysisStatus::Safe: return "safe";
        case AnalysisStatus::NeedsReview: return "review";
        case AnalysisStatus::PotentialRisk: return "risk";
        case AnalysisStatus::Violation: return "violation";
    }
    return "";
}

std::string badge(AnalysisStatus s, bool waived = false) {
    std::string text(toString(s));
    if (waived)
        text = "waived";
    return "<span class=\"badge " + std::string(waived ? "waived" : statusClass(s)) + "\">" + esc(text) + "</span>";
}

std::string fmt(double v, const char* f) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

constexpr const char* kCss = R"(
body{font-family:-apple-system,"Segoe UI",Roboto,"Noto Sans",sans-serif;margin:24px;color:#222;background:#fff;font-size:14px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:18px;margin:28px 0 8px;border-bottom:1px solid #ddd;padding-bottom:4px}
h3{font-size:15px;margin:18px 0 6px}.nowrap{white-space:nowrap}
table{border-collapse:collapse;margin:6px 0 12px}td,th{border:1px solid #d6d6d6;padding:3px 7px;text-align:left;vertical-align:top}
th{background:#f3f3f3}tr:nth-child(even) td{background:#fafafa}
.meta{color:#666}.mono{font-family:Consolas,Menlo,monospace;font-size:12px}
.cards{display:flex;flex-wrap:wrap;gap:10px;margin:10px 0}
.card{border:1px solid #ddd;border-radius:6px;padding:8px 14px;min-width:120px}
.card b{display:block;font-size:22px}
.badge{display:inline-block;border-radius:3px;padding:0 6px;font-size:12px;color:#fff;white-space:nowrap}
.safe{background:#2e8b57}.review{background:#b38f00}.risk{background:#e07b00}.violation{background:#d02020}
.waived{background:#888}
.ok{color:#2e8b57}.fail{color:#d02020}
.path{border:1px solid #e0e0e0;border-radius:6px;padding:8px 12px;margin:12px 0}
.path svg{max-width:100%;height:auto;border:1px solid #eee}
ol.steps li{margin:1px 0}.logic{color:#b04000}
table td.rel-async{background:#ffe0c0}table td.rel-unknown{background:#fff3b8}table td.rel-same{background:#e6e6e6}
table td.rel-sync{background:#d8f0d0}table td.rel-excl{background:#e2e2f8}
@media print{.path{break-inside:avoid}}
)";

std::string relClass(clock::ClockRelation r) {
    switch (r) {
        case clock::ClockRelation::Same: return "rel-same";
        case clock::ClockRelation::Synchronous: return "rel-sync";
        case clock::ClockRelation::Asynchronous: return "rel-async";
        case clock::ClockRelation::Exclusive: return "rel-excl";
        case clock::ClockRelation::Unknown: return "rel-unknown";
    }
    return "";
}

} // namespace

std::string toHtmlReport(const AnalysisResult& r, const HtmlReportOptions& opt) {
    std::ostringstream o;
    const auto registry = rules::RuleRegistry::standard();
    o << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      << "<title>OpenCDC report: " << esc(r.project.name) << "</title><style>" << kCss << "</style></head><body>\n";

    // ---- Header ----
    o << "<h1>CDC report: " << esc(r.project.name) << "</h1>\n<p class=\"meta\">OpenCDC Analyzer " << kVersionString
      << " &middot; analyzed " << esc(r.startedAtUtc) << " &middot; "
      << (r.elapsedSeconds < 1.0 ? fmt(r.elapsedSeconds * 1000.0, "%.0f") + " ms" : fmt(r.elapsedSeconds, "%.2f") + " s");
    if (!r.project.projectFile.empty())
        o << " &middot; <span class=\"mono\">" << esc(toUtf8(r.project.projectFile)) << "</span>";
    o << "</p>\n";
    if (r.elaboration && !r.elaboration->topInstances.empty())
        o << "<p>Top: <b>" << esc(r.elaboration->topInstances.front().definition) << "</b>, "
          << r.elaboration->instanceCount << " instances, " << r.project.sources.size() << " source file(s), "
          << r.project.constraints.size() << " constraint file(s), " << r.project.waivers.size()
          << " waiver file(s).</p>\n";

    // ---- Summary ----
    o << "<h2 id=\"summary\">Summary</h2>\n<div class=\"cards\">";
    auto card = [&](const std::string& label, std::size_t n, const char* cls = "") {
        o << "<div class=\"card\"><b class=\"" << cls << "\">" << n << "</b>" << esc(label) << "</div>";
    };
    if (r.cdc) {
        card("CDC paths", r.cdc->paths.size());
        card("violation", r.pathsWithStatus(AnalysisStatus::Violation), "fail");
        card("potential risk", r.pathsWithStatus(AnalysisStatus::PotentialRisk));
        card("needs review", r.pathsWithStatus(AnalysisStatus::NeedsReview));
        card("safe", r.pathsWithStatus(AnalysisStatus::Safe), "ok");
        card("waived rule hits", r.waivedHits());
        card("synchronizer structures", r.cdc->structures.size());
    }
    card("errors (HDL/constraints)", r.count(Severity::Error) + r.count(Severity::Fatal),
         r.count(Severity::Error) + r.count(Severity::Fatal) > 0 ? "fail" : "");
    card("warnings", r.count(Severity::Warning));
    o << "</div>\n";
    if (!r.cdc)
        o << "<p class=\"fail\"><b>CDC analysis did not run</b> &mdash; see the diagnostics below.</p>\n";
    o << "<p>Error-severity violations (not waived): <b>" << r.violations(Severity::Error)
      << "</b>; warning-severity violations: <b>" << r.violations(Severity::Warning) << "</b>.</p>\n";

    // ---- Stages ----
    o << "<h2 id=\"stages\">Analysis stages</h2>\n<table><tr><th>Stage</th><th>Status</th><th>Time</th><th>Detail</th>"
         "</tr>\n";
    for (const auto& s : r.stages)
        o << "<tr><td>" << esc(s.name) << "</td><td>" << toString(s.status) << "</td><td class=\"nowrap\">"
          << fmt(s.seconds, "%.3f") << " s</td><td>" << esc(s.detail) << "</td></tr>\n";
    o << "</table>\n";

    // ---- Clocks ----
    if (r.clocks) {
        const auto& cm = *r.clocks;
        o << "<h2 id=\"clocks\">Clocks</h2>\n<table><tr><th>Clock</th><th>Kind</th><th>Source</th>"
             "<th>Derived from</th><th>Period</th><th>Frequency</th><th>Registers</th></tr>\n";
        for (const auto& c : cm.clocks) {
            const bool known = c.periodNs && *c.periodNs > 0;
            o << "<tr><td>" << esc(c.name) << "</td><td>" << toString(c.kind) << "</td><td class=\"mono\">"
              << esc(c.sourceName) << "</td><td>" << (c.master.valid() ? esc(cm.clock(c.master).name) : "&ndash;")
              << "</td><td>" << (known ? fmt(*c.periodNs, "%.4g") + " ns" : "&ndash;") << "</td><td>"
              << (known ? fmt(1000.0 / *c.periodNs, "%.4g") + " MHz" : "&ndash;") << "</td><td>" << c.registers()
              << "</td></tr>\n";
        }
        o << "</table>\n";
        if (cm.clocks.size() > 1) {
            o << "<table><tr><th></th>";
            for (const auto& c : cm.clocks)
                o << "<th>" << esc(c.name) << "</th>";
            o << "</tr>\n";
            for (std::size_t i = 0; i < cm.clocks.size(); ++i) {
                o << "<tr><th>" << esc(cm.clocks[i].name) << "</th>";
                for (std::size_t j = 0; j < cm.clocks.size(); ++j) {
                    const clock::ClockId a{static_cast<std::uint32_t>(i)}, b{static_cast<std::uint32_t>(j)};
                    const auto rel = cm.relation(a, b);
                    o << "<td class=\"" << relClass(rel) << "\" title=\"" << esc(i == j ? "" : cm.reason(a, b)) << "\">"
                      << toString(rel) << "</td>";
                }
                o << "</tr>\n";
            }
            o << "</table>\n";
        }
    }

    if (r.cdc) {
        const auto& c = *r.cdc;
        // ---- Rules ----
        std::map<std::string, std::pair<std::size_t, std::size_t>> perRule; // open, waived
        for (const auto& p : c.paths)
            for (const auto& h : p.hits)
                (h.waiver.empty() ? perRule[h.ruleId].first : perRule[h.ruleId].second)++;
        o << "<h2 id=\"rules\">Rules</h2>\n<table><tr><th>Rule</th><th>Title</th><th>Severity</th><th>Hits</th>"
             "<th>Waived</th></tr>\n";
        for (const auto& rule : registry.rules()) {
            const auto& m = rule->meta();
            const auto& n = perRule[m.id];
            o << "<tr><td>" << esc(m.id) << "</td><td>" << esc(m.title) << "</td><td>" << toString(m.defaultSeverity)
              << "</td><td>" << n.first << "</td><td>" << n.second << "</td></tr>\n";
        }
        o << "</table>\n";

        // ---- Paths table ----
        // Large designs: only the paths that need attention are listed.
        std::vector<const cdc::CdcPath*> shown;
        std::vector<bool> isShown(c.paths.size(), c.paths.size() <= opt.maxPaths);
        if (c.paths.size() <= opt.maxPaths) {
            for (const auto& p : c.paths)
                shown.push_back(&p);
        }
        else {
            std::size_t attention = 0;
            for (std::size_t i = 0; i < c.paths.size(); ++i) {
                const auto& p = c.paths[i];
                if (p.hits.empty() && p.status == AnalysisStatus::Safe)
                    continue;
                ++attention;
                if (shown.size() < opt.maxPaths) {
                    shown.push_back(&p);
                    isShown[i] = true;
                }
            }
            o << "<p class=\"meta\" id=\"limited\">" << c.paths.size() << " paths exceed the report limit of "
              << opt.maxPaths << ": only the " << shown.size() << " of " << attention
              << " path(s) with rule hits or a status other than safe are listed. The CSV and JSON outputs "
                 "contain every path.</p>\n";
        }
        auto pathRef = [&](std::size_t id) {
            return id < isShown.size() && isShown[id]
                       ? "<a href=\"#p" + std::to_string(id) + "\">#" + std::to_string(id) + "</a> "
                       : "#" + std::to_string(id) + " ";
        };
        o << "<h2 id=\"paths\">CDC paths</h2>\n<table><tr><th>#</th><th>Status</th><th>Source clock</th>"
             "<th>Dest. clock</th><th>Source</th><th>Destination</th><th>Width</th><th>Synchronizer</th>"
             "<th>Rules</th></tr>\n";
        for (const auto* pp : shown) {
            const auto& p = *pp;
            std::string rulesText;
            for (const auto& h : p.hits)
                rulesText += (rulesText.empty() ? "" : " ") + h.ruleId + (h.waiver.empty() ? "" : "&#8224;");
            o << "<tr><td><a href=\"#p" << p.id << "\">" << p.id << "</a></td><td>" << badge(p.status, p.waived)
              << "</td><td>" << esc(p.srcClockName) << "</td><td>" << esc(p.dstClockName) << "</td><td class=\"mono\">"
              << esc(p.srcName) << "</td><td class=\"mono\">" << esc(p.dstName)
              << (p.pin == Pin::Data ? "" : " (" + std::string(toString(p.pin)) + ")") << "</td><td>" << p.width
              << "</td><td>" << toString(p.syncType) << " / " << toString(p.syncStatus) << "</td><td>" << rulesText
              << "</td></tr>\n";
        }
        o << "</table>\n<p class=\"meta\">&#8224; waived</p>\n";

        // ---- Structures ----
        if (!c.structures.empty()) {
            o << "<h2 id=\"structures\">Synchronizer structures</h2>\n<table><tr><th>#</th><th>Type</th><th>Name</th>"
                 "<th>Clocks</th><th>Status</th><th>Paths</th><th>Notes</th></tr>\n";
            for (const auto& s : c.structures) {
                if (!std::ranges::any_of(s.paths, [&](std::size_t id) { return id < isShown.size() && isShown[id]; }))
                    continue;
                o << "<tr><td>S" << s.id << "</td><td>" << toString(s.type) << "</td><td class=\"mono\">" << esc(s.name)
                  << "</td><td>" << esc(s.srcClock) << " &rarr; " << esc(s.dstClock) << "</td><td>"
                  << toString(s.status) << "</td><td>";
                for (const auto id : s.paths)
                    o << pathRef(id);
                o << "</td><td>";
                for (const auto& n : s.notes)
                    o << esc(n) << "<br>";
                o << "</td></tr>\n";
            }
            o << "</table>\n";
        }

        // ---- Path details ----
        o << "<h2 id=\"details\">Path details</h2>\n";
        const bool drawSchematics = opt.schematics && shown.size() <= opt.maxSchematics;
        if (opt.schematics && !drawSchematics)
            o << "<p class=\"meta\">Schematics are left out: " << shown.size() << " paths exceed the limit of "
              << opt.maxSchematics << ".</p>\n";
        std::unique_ptr<schematic::SchematicFactory> factory;
        if (drawSchematics)
            factory = std::make_unique<schematic::SchematicFactory>(c);
        for (const auto* pp : shown) {
            const auto& p = *pp;
            o << "<div class=\"path\" id=\"p" << p.id << "\"><h3>#" << p.id << " " << badge(p.status, p.waived) << " "
              << esc(p.srcName) << " &rarr; " << esc(p.dstName) << "</h3>\n<table>";
            auto row = [&](const char* k, const std::string& v) { o << "<tr><th>" << k << "</th><td>" << v << "</td></tr>"; };
            row("Clocks", esc(p.srcClockName) + " &rarr; " + esc(p.dstClockName) + " (" +
                              std::string(clock::toString(p.relation)) + ")");
            row("Source", "<span class=\"mono\">" + esc(p.srcName) + "</span> " + esc(loc(p.srcLoc)));
            row("Destination", "<span class=\"mono\">" + esc(p.dstName) + "</span> " + esc(loc(p.dstLoc)));
            row("Pin / width / class", std::string(toString(p.pin)) + " / " + std::to_string(p.width) + " / " +
                                           std::string(toString(p.cls)));
            if (p.groupSize > 1) {
                std::string members;
                for (const auto& m : p.groupMembers)
                    members += esc(m) + " ";
                row("Grouped registers", std::to_string(p.groupSize) + ": <span class=\"mono\">" + members + "</span>");
            }
            row("Synchronizer", std::string(toString(p.syncType)) + (p.syncStages ? ", " + std::to_string(p.syncStages) + " stage(s)" : "") +
                                    " &mdash; <b>" + std::string(toString(p.syncStatus)) + "</b>");
            if (p.structure < c.structures.size())
                row("Structure", "S" + std::to_string(p.structure) + " " +
                                     std::string(toString(c.structures[p.structure].type)));
            row("Fingerprint", "<span class=\"mono\">" + esc(p.fingerprint) + "</span>");
            o << "</table>\n<ol class=\"steps\">";
            for (const auto& s : p.steps) {
                const bool logic = s.kind.starts_with("logic:");
                o << "<li" << (logic ? " class=\"logic\"" : "") << "><span class=\"mono\">" << esc(s.name) << "</span> <i>("
                  << esc(logic ? s.kind.substr(6) : s.kind) << ")</i> <span class=\"meta\">" << esc(loc(s.loc))
                  << "</span></li>";
            }
            o << "</ol>\n";
            if (!p.conditions.empty()) {
                o << "<ul>";
                for (const auto& cd : p.conditions)
                    o << "<li class=\"" << (cd.satisfied ? "ok" : "fail") << "\">" << (cd.satisfied ? "&#10004; " : "&#10008; ")
                      << esc(cd.text) << "</li>";
                o << "</ul>\n";
            }
            for (const auto& u : p.unverified)
                o << "<p><b>Not verified statically:</b> " << esc(u) << "</p>\n";
            for (const auto& n : p.notes)
                o << "<p class=\"meta\">" << esc(n) << "</p>\n";
            for (const auto& h : p.hits) {
                o << "<p><b>" << esc(h.ruleId) << "</b> " << toString(h.severity) << " "
                  << badge(h.status, !h.waiver.empty()) << " " << esc(h.message);
                if (!h.waiver.empty())
                    for (const auto& w : c.waivers)
                        if (w.id == h.waiver)
                            o << "<br><i>Waived by " << esc(w.id) << ": " << esc(w.reason) << "</i>";
                if (const auto* rule = registry.find(h.ruleId))
                    o << "<br><span class=\"meta\">Fix: " << esc(rule->meta().fixSuggestion) << "</span>";
                o << "</p>\n";
            }
            if (factory) {
                auto s = factory->build(p.id);
                schematic::layout(s);
                o << schematic::toSvg(s, c);
            }
            o << "</div>\n";
        }

        // ---- Waivers ----
        o << "<h2 id=\"waivers\">Waivers</h2>\n";
        if (c.waivers.empty()) {
            o << "<p>No waivers.</p>\n";
        }
        else {
            o << "<table><tr><th>Id</th><th>Rule</th><th>Selectors</th><th>Reason</th><th>Author</th><th>Expires</th>"
                 "<th>Hits covered</th><th>Defined at</th></tr>\n";
            for (const auto& w : c.waivers) {
                std::string sel;
                if (!w.fingerprint.empty())
                    sel += "fingerprint=" + esc(w.fingerprint) + "<br>";
                if (!w.source.empty())
                    sel += "source=" + esc(w.source) + "<br>";
                if (!w.destination.empty())
                    sel += "destination=" + esc(w.destination);
                o << "<tr><td>" << esc(w.id) << "</td><td>" << esc(w.rule) << "</td><td class=\"mono\">" << sel
                  << "</td><td>" << esc(w.reason) << "</td><td>" << esc(w.author) << "</td><td>" << esc(w.expires)
                  << (w.expired ? " <b class=\"fail\">expired</b>" : "") << "</td><td>" << w.uses
                  << (w.uses == 0 && !w.expired ? " <b class=\"fail\">unused</b>" : "") << "</td><td>" << esc(loc(w.loc))
                  << "</td></tr>\n";
            }
            o << "</table>\n";
        }
    }

    // ---- Diagnostics ----
    o << "<h2 id=\"diagnostics\">Diagnostics</h2>\n";
    if (r.diagnostics.empty()) {
        o << "<p>None.</p>\n";
    }
    else {
        o << "<table><tr><th>Severity</th><th>Code</th><th>Message</th><th>Location</th></tr>\n";
        for (const auto& d : r.diagnostics)
            o << "<tr><td>" << toString(d.severity) << "</td><td>" << esc(d.code) << "</td><td>" << esc(d.message)
              << "</td><td>" << esc(loc(d.location)) << "</td></tr>\n";
        o << "</table>\n";
    }

    // ---- Not analyzed ----
    if (r.design) {
        o << "<h2 id=\"unsupported\">Not analyzed / approximated</h2>\n";
        if (r.design->unsupported.empty()) {
            o << "<p>All constructs were modelled.</p>\n";
        }
        else {
            o << "<table><tr><th>Code</th><th>Module</th><th>What</th><th>Location</th></tr>\n";
            for (const auto& u : r.design->unsupported)
                o << "<tr><td>" << esc(u.code) << "</td><td>" << esc(u.module) << "</td><td>" << esc(u.what)
                  << "</td><td>" << esc(loc(u.loc)) << "</td></tr>\n";
            o << "</table>\n";
        }
    }
    o << "<p class=\"meta\">Generated by OpenCDC Analyzer " << kVersionString << ".</p>\n</body></html>\n";
    return o.str();
}

// ============================================================================
// CSV
// ============================================================================

namespace {

class Csv {
public:
    Csv() { out_ << "\xEF\xBB\xBF"; } // UTF-8 BOM

    Csv& field(std::string_view v) {
        if (!first_)
            out_ << ',';
        first_ = false;
        const bool quote = v.find_first_of(",\"\r\n") != std::string_view::npos || (!v.empty() && (v.front() == ' ' || v.back() == ' '));
        if (!quote) {
            out_ << v;
            return *this;
        }
        out_ << '"';
        for (const char c : v) {
            if (c == '"')
                out_ << '"';
            out_ << c;
        }
        out_ << '"';
        return *this;
    }
    Csv& field(std::size_t v) { return field(std::to_string(v)); }
    void endRow() {
        out_ << "\r\n";
        first_ = true;
    }
    [[nodiscard]] std::string str() const { return out_.str(); }

private:
    std::ostringstream out_;
    bool first_ = true;
};

} // namespace

std::string pathsCsv(const AnalysisResult& r) {
    Csv csv;
    for (const char* h : {"id", "status", "waived", "source_clock", "destination_clock", "relation", "source",
                          "destination", "pin", "width", "class", "sync_type", "sync_stages", "sync_status", "rules",
                          "waived_rules", "structure", "group_size", "fingerprint", "source_file", "source_line",
                          "destination_file", "destination_line"})
        csv.field(h);
    csv.endRow();
    if (!r.cdc)
        return csv.str();
    for (const auto& p : r.cdc->paths) {
        std::string open, waived;
        for (const auto& h : p.hits) {
            std::string& list = h.waiver.empty() ? open : waived;
            list += (list.empty() ? "" : ";") + h.ruleId;
        }
        csv.field(p.id)
            .field(toString(p.status))
            .field(p.waived ? "yes" : "no")
            .field(p.srcClockName)
            .field(p.dstClockName)
            .field(clock::toString(p.relation))
            .field(p.srcName)
            .field(p.dstName)
            .field(toString(p.pin))
            .field(std::size_t{p.width})
            .field(toString(p.cls))
            .field(toString(p.syncType))
            .field(std::size_t{p.syncStages})
            .field(toString(p.syncStatus))
            .field(open)
            .field(waived)
            .field(p.structure < r.cdc->structures.size() ? "S" + std::to_string(p.structure) : std::string())
            .field(p.groupSize)
            .field(p.fingerprint)
            .field(p.srcLoc.file)
            .field(std::size_t{p.srcLoc.line})
            .field(p.dstLoc.file)
            .field(std::size_t{p.dstLoc.line});
        csv.endRow();
    }
    return csv.str();
}

std::string violationsCsv(const AnalysisResult& r) {
    Csv csv;
    for (const char* h : {"path_id", "rule", "severity", "status", "waiver", "message", "source", "destination",
                          "source_clock", "destination_clock", "destination_file", "destination_line", "fingerprint"})
        csv.field(h);
    csv.endRow();
    if (!r.cdc)
        return csv.str();
    for (const auto& p : r.cdc->paths)
        for (const auto& h : p.hits) {
            csv.field(p.id)
                .field(h.ruleId)
                .field(toString(h.severity))
                .field(toString(h.status))
                .field(h.waiver)
                .field(h.message)
                .field(p.srcName)
                .field(p.dstName)
                .field(p.srcClockName)
                .field(p.dstClockName)
                .field(p.dstLoc.file)
                .field(std::size_t{p.dstLoc.line})
                .field(p.fingerprint);
            csv.endRow();
        }
    return csv.str();
}

} // namespace opencdc::engine

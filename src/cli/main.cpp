// opencdc - command line front end of OpenCDC Analyzer.
//
//   opencdc --version
//   opencdc analyze <project.ocdc.json> [--json <out.json>] [--dump-ir <out.txt>]
//                   [--schematic-dir <dir>] [--html <report.html>] [--csv <dir>] [--suggest-sdc <file>]
//                   [--vivado-clocks <report_clocks.txt>]... [--quiet]
//   opencdc analyze <project.xpr> ...            (a Vivado project, imported)
//   opencdc analyze <project.qpf|.qsf> ...       (a Quartus project, imported)
//   opencdc analyze <project.gprj> ...           (a Gowin project, imported)
//   opencdc import <project.xpr|.qpf|.qsf|.gprj> [-o <project.ocdc.json>] [--force]
//
// Exit codes: 0 = ran, no errors and no error-severity CDC violations;
//             1 = ran, errors reported (HDL/constraint errors);
//             2 = usage or project error;
//             3 = ran, error-severity CDC rule violations found.

#include "core/Diagnostic.h"
#include "core/PathUtil.h"
#include "core/Progress.h"
#include "core/Version.h"
#include "engine/AnalysisSession.h"
#include "engine/ClockPairs.h"
#include "engine/ProjectConfig.h"
#include "engine/Report.h"
#include "engine/SdcSuggest.h"
#include "engine/VivadoProject.h"
#include "engine/ResultJson.h"
#include "engine/WaiverFile.h"
#include "rules/Waiver.h"

#include <nlohmann/json.hpp>
#include "ir/Dump.h"
#include "schematic/Schematic.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#endif

namespace {

using namespace opencdc;

constexpr int kExitOk = 0;
constexpr int kExitAnalysisErrors = 1;
constexpr int kExitUsage = 2;
constexpr int kExitViolations = 3;
constexpr int kExitWaiverAudit = 4; ///< opencdc waivers --fail-on: a waiver is in one of the given states

void printTree(const frontend::InstanceInfo& inst, const std::string& indent, std::ostream& os) {
    os << indent << inst.name << " (" << inst.definition << ")\n";
    for (const auto& child : inst.children)
        printTree(child, indent + "  ", os);
}

struct Outputs {
    std::string json;
    std::string ir;
    std::string schematicDir;
    std::string html;
    std::string csvDir;
    std::string suggestSdc;
    std::vector<std::string> vivadoClocks; ///< extra Vivado report_clocks outputs to compare with
    bool blackBoxUnknown = false;          ///< --blackbox-unknown
};

bool writeText(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write '" << toUtf8(file) << "'\n";
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

int runAnalyze(const std::string& projectPath, const Outputs& outs, bool quiet) {
    const std::string& jsonOut = outs.json;
    const std::string& irOut = outs.ir;
    const std::string& schematicDir = outs.schematicDir;
    auto project = engine::loadProjectFile(fromUtf8(projectPath));
    if (!project) {
        std::cerr << format(project.error()) << '\n';
        return kExitUsage;
    }

    for (const auto& f : outs.vivadoClocks)
        project.value().vivadoClockReports.push_back(std::filesystem::absolute(fromUtf8(f)));
    if (outs.blackBoxUnknown)
        project.value().options.blackBoxUnknownModules = true;

    NullProgressSink progress;
    CancellationToken cancel;
    engine::AnalysisSession session;
    auto result = session.run(project.value(), progress, cancel);

    std::ostream& os = std::cout;
    os << "OpenCDC Analyzer " << kVersionString << "\n";
    os << "Project : " << result->project.name << "\n";
    os << "Language: " << result->project.languageSummary() << "\n";

    if (result->elaboration) {
        const auto& e = *result->elaboration;
        os << "Files   : " << e.parsedFiles.size() << " parsed\n";
        os << "Modules : " << e.definitions.size() << "\n";
        if (!quiet) {
            for (const auto& d : e.definitions)
                os << "  " << d.kind << ' ' << d.name << "  [" << toString(d.location) << "]\n";
        }
        for (const auto& top : e.topInstances)
            os << "Top     : " << top.definition << "\n";
        os << "Instances: " << e.instanceCount << "\n";
        if (!quiet) {
            os << "Hierarchy:\n";
            for (const auto& top : e.topInstances)
                printTree(top, "  ", os);
        }
    }

    if (result->design) {
        const auto st = result->design->stats();
        os << "RTL     : " << st.uniqueBodies << " module bodies, " << st.registers << " registers (" << st.registerBits
           << " bits), " << st.latches << " latches, " << st.cells << " cells, " << st.unsupported
           << " unsupported construct(s)\n";
    }

    if (result->clocks) {
        const auto& cm = *result->clocks;
        os << "Clocks  : " << cm.clocks.size() << " (" << cm.resolvedRegisters() << "/" << cm.registers.size()
           << " registers resolved)\n";
        for (std::size_t i = 0; i < cm.clocks.size(); ++i) {
            const auto& c = cm.clocks[i];
            os << "  " << c.name << "  [" << clock::toString(c.kind) << "]";
            for (const auto& a : c.aliases)
                os << " alias=" << a;
            if (!c.sourceName.empty() && c.sourceName != c.name)
                os << " source=" << c.sourceName;
            if (c.master.valid())
                os << " master=" << cm.clock(c.master).name;
            if (c.periodNs && *c.periodNs > 0) {
                char mhz[32];
                std::snprintf(mhz, sizeof(mhz), "%.4g", 1000.0 / *c.periodNs);
                os << " period=" << *c.periodNs << "ns (" << mhz << " MHz)";
                if (c.periodRatio && !c.periodBasis.empty())
                    os << " = " << c.periodBasis << " x " << *c.periodRatio;
            }
            os << " registers=" << c.registers();
            if (c.negedgeRegisters)
                os << " (" << c.negedgeRegisters << " negedge)";
            if (c.gatedRegisters)
                os << " gated=" << c.gatedRegisters;
            os << "\n";
        }
        bool header = false;
        for (std::size_t i = 0; i < cm.clocks.size(); ++i) {
            for (std::size_t j = i + 1; j < cm.clocks.size(); ++j) {
                const clock::ClockId a{static_cast<std::uint32_t>(i)};
                const clock::ClockId b{static_cast<std::uint32_t>(j)};
                if (!header) {
                    os << "Clock relations:\n";
                    header = true;
                }
                os << "  " << cm.clocks[i].name << " <-> " << cm.clocks[j].name << " : "
                   << clock::toString(cm.relation(a, b)) << "  (" << cm.reason(a, b) << ")\n";
            }
        }
    }

    os << "Stages:\n";
    for (const auto& s : result->stages) {
        char t[32];
        std::snprintf(t, sizeof(t), "%.3f s", s.seconds);
        os << "  [" << engine::toString(s.status) << "] " << s.name << " (" << t << ") - " << s.detail << "\n";
    }

    if (!result->diagnostics.empty()) {
        os << "Diagnostics:\n";
        for (const auto& d : result->diagnostics)
            os << "  " << format(d) << "\n";
    }
    os << "Summary : " << (result->count(Severity::Error) + result->count(Severity::Fatal)) << " error(s), "
       << result->count(Severity::Warning) << " warning(s)\n";
    if (result->cdc) {
        const auto& c = *result->cdc;
        const auto& st = c.stats;
        os << "CDC     : " << st.paths << " crossing path(s), " << st.synchronized << " synchronized, "
           << st.unsynchronized << " unsynchronized, " << c.structures.size() << " synchronizer structure(s)\n";
        os << "Rules   : " << result->violations(Severity::Error) << " error violation(s), "
           << result->violations(Severity::Warning) << " warning violation(s), "
           << result->pathsWithStatus(cdc::AnalysisStatus::PotentialRisk) << " path(s) at potential risk, "
           << result->pathsWithStatus(cdc::AnalysisStatus::NeedsReview) << " path(s) need review";
        if (!c.waivers.empty())
            os << ", " << result->waivedHits() << " hit(s) waived";
        os << "\n";
        // crossings the designer excluded from timing (set_false_path): still reported
        const auto excepted = std::ranges::count_if(c.paths, [](const cdc::CdcPath& p) {
            return !p.timingException.empty() && p.syncStatus != cdc::SyncStatus::NecessaryConditionsVerified &&
                   p.syncStatus != cdc::SyncStatus::ProtocolUnverified;
        });
        if (excepted > 0)
            os << "          " << excepted << " unsynchronized path(s) covered by a timing exception of the "
               << "constraints (set_false_path / set_max_delay: declared quasi-static by the designer)\n";
        // where the crossings are: one line per clock pair (the busiest first)
        const auto pairs = engine::summarizeClockPairs(*result);
        for (std::size_t k = 0; k < pairs.size() && k < 12; ++k) {
            const auto& cp = pairs[k];
            os << "  " << cp.srcClock << " -> " << cp.dstClock << " (" << cp.relation << "): " << cp.paths
               << " path(s), " << cp.synchronized << " synchronized";
            if (cp.violations)
                os << ", " << cp.violations << " violation(s)";
            if (cp.potentialRisk)
                os << ", " << cp.potentialRisk << " at risk";
            if (cp.needsReview)
                os << ", " << cp.needsReview << " need review";
            os << "\n";
        }
        if (pairs.size() > 12)
            os << "  ... " << pairs.size() - 12 << " more clock pair(s)\n";
        if (std::ranges::any_of(pairs, [](const engine::ClockPairSummary& cp) {
                return cp.relation == "unknown" && cp.needsReview > 0;
            }))
            os << "  (paths need review because clock relations are unknown: --suggest-sdc proposes the missing "
                  "constraints)\n";
        if (!quiet) {
            for (const auto& p : c.paths) {
                os << "  #" << p.id << " [" << cdc::toString(p.status) << "] " << p.srcName << " (" << p.srcClockName
                   << ") -> " << p.dstName << " (" << p.dstClockName << ")";
                if (p.pin != cdc::Pin::Data)
                    os << " via " << cdc::toString(p.pin);
                if (p.groupSize > 1)
                    os << " (+" << (p.groupSize - 1) << " more)";
                os << "  width=" << p.width << " sync=" << cdc::toString(p.syncType) << "/"
                   << cdc::toString(p.syncStatus);
                if (!p.hits.empty() && !p.fingerprint.empty())
                    os << " fingerprint=" << p.fingerprint; // for waivers (opencdc waive --fingerprint)
                os << "\n";
                for (const auto& h : p.hits) {
                    os << "      " << h.ruleId << " " << toString(h.severity) << " (" << cdc::toString(h.status)
                       << "): " << h.message;
                    if (!h.waiver.empty())
                        os << " [waived by " << h.waiver << "]";
                    os << "\n";
                }
                for (const auto& u : p.unverified)
                    os << "      unverified: " << u << "\n";
            }
            if (!c.structures.empty()) {
                os << "Synchronizer structures:\n";
                for (const auto& s : c.structures) {
                    os << "  S" << s.id << " " << cdc::toString(s.type) << " '" << s.name << "' " << s.srcClock
                       << " -> " << s.dstClock << " [" << cdc::toString(s.status) << "] paths:";
                    for (const auto id : s.paths)
                        os << " #" << id;
                    os << "\n";
                }
            }
        }
    }
    else {
        os << "CDC analysis: not run (see stages and diagnostics).\n";
    }

    if (!jsonOut.empty()) {
        std::ofstream out(fromUtf8(jsonOut), std::ios::binary);
        if (!out) {
            std::cerr << "error: cannot write JSON output '" << jsonOut << "'\n";
            return kExitUsage;
        }
        engine::writeJson(*result, out);
        out << '\n';
        os << "JSON    : " << jsonOut << "\n";
    }

    if (!irOut.empty()) {
        if (!result->design) {
            std::cerr << "error: no Design IR available (see diagnostics)\n";
            return kExitAnalysisErrors;
        }
        std::ofstream out(fromUtf8(irOut), std::ios::binary);
        if (!out) {
            std::cerr << "error: cannot write IR dump '" << irOut << "'\n";
            return kExitUsage;
        }
        out << ir::dumpDesign(*result->design);
        os << "IR dump : " << irOut << "\n";
    }

    if (!schematicDir.empty()) {
        if (!result->cdc) {
            std::cerr << "error: no CDC result to draw (see diagnostics)\n";
            return kExitAnalysisErrors;
        }
        const auto dir = fromUtf8(schematicDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const schematic::SchematicFactory factory(*result->cdc);
        for (const auto& p : result->cdc->paths) {
            auto s = factory.build(p.id);
            schematic::layout(s);
            const auto file = dir / ("path-" + std::to_string(p.id) + ".svg");
            std::ofstream out(file, std::ios::binary);
            if (!out) {
                std::cerr << "error: cannot write schematic '" << toUtf8(file) << "'\n";
                return kExitUsage;
            }
            out << schematic::toSvg(s, *result->cdc);
        }
        os << "Schematics: " << result->cdc->paths.size() << " SVG file(s) in " << schematicDir << "\n";
    }

    if (!outs.html.empty()) {
        if (!writeText(fromUtf8(outs.html), engine::toHtmlReport(*result)))
            return kExitUsage;
        os << "HTML    : " << outs.html << "\n";
    }
    if (!outs.csvDir.empty()) {
        const auto dir = fromUtf8(outs.csvDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!writeText(dir / "paths.csv", engine::pathsCsv(*result)) ||
            !writeText(dir / "violations.csv", engine::violationsCsv(*result)) ||
            !writeText(dir / "waivers.csv", engine::waiversCsv(*result)) ||
            !writeText(dir / "outputs.csv", engine::outputsCsv(*result)))
            return kExitUsage;
        os << "CSV     : " << outs.csvDir << " (paths.csv, violations.csv, waivers.csv, outputs.csv)\n";
    }

    if (!outs.suggestSdc.empty()) {
        if (!result->clocks) {
            os << "Suggested SDC: not written (clock analysis did not run)\n";
        }
        else {
            const auto sug = engine::suggestConstraints(*result);
            if (!writeText(fromUtf8(outs.suggestSdc), sug.text))
                return kExitUsage;
            os << "Suggested SDC: " << outs.suggestSdc << " (" << sug.clocks << " clock(s), " << sug.groups
               << " clock group(s), " << sug.todos << " TODO period(s); resolves " << sug.unknownPairs.size()
               << " unknown clock relation(s))\n";
        }
    }

    if (result->hasErrors())
        return kExitAnalysisErrors;
    return result->violations(Severity::Error) > 0 ? kExitViolations : kExitOk;
}

struct WaiveArgs {
    std::string project;
    cdc::WaiverRecord w;
    std::string file;
    std::string remove; ///< id of a waiver to remove instead
};

/// opencdc waive: adds a waiver to the project's waiver file (created and
/// added to the project when it has none).
int runWaive(WaiveArgs args) {
    auto project = engine::loadProjectFile(fromUtf8(args.project));
    if (!project) {
        std::cerr << format(project.error()) << '\n';
        return kExitUsage;
    }
    auto& cfg = project.value();
    if (!cfg.importedFrom.empty()) {
        std::cerr << "error: " << args.project
                  << " is a Vivado, Quartus or Gowin project: waivers belong to an OpenCDC project; create one with 'opencdc import "
                  << args.project << "'\n";
        return kExitUsage;
    }
    if (!args.remove.empty()) {
        for (const auto& f : cfg.waivers) {
            std::vector<cdc::WaiverRecord> records;
            CollectingDiagnosticSink ignored;
            engine::readWaiverFile(f, records, ignored);
            if (std::ranges::none_of(records, [&](const cdc::WaiverRecord& r) { return r.id == args.remove; }))
                continue;
            if (const auto err = engine::removeWaiver(f, args.remove); !err.empty()) {
                std::cerr << "error: " << err << '\n';
                return kExitUsage;
            }
            std::cout << "waiver '" << args.remove << "' removed from " << toUtf8(f) << '\n';
            return kExitOk;
        }
        std::cerr << "error: no waiver '" << args.remove << "' in the project's waiver files\n";
        return kExitUsage;
    }
    if (args.w.reason.empty()) {
        std::cerr << "error: --reason is required\n";
        return kExitUsage;
    }
    const auto added = engine::addProjectWaiver(cfg, args.w, args.file.empty() ? std::filesystem::path() : fromUtf8(args.file));
    if (!added.error.empty()) {
        std::cerr << "error: " << added.error << '\n';
        return kExitUsage;
    }
    std::cout << "waiver '" << added.id << "' added to " << toUtf8(added.file) << '\n';
    if (added.projectChanged) {
        if (auto err = engine::saveProjectFile(cfg, cfg.projectFile)) {
            std::cerr << format(*err) << '\n';
            return kExitAnalysisErrors;
        }
        std::cout << "waiver file added to project " << toUtf8(cfg.projectFile) << '\n';
    }
    return kExitOk;
}

struct ImportArgs {
    std::string project;
    std::string output;
    bool force = false;
};

/// opencdc import: writes the OpenCDC project of a Vivado, Quartus or Gowin project.
int runImport(const ImportArgs& args) {
    const auto in = fromUtf8(args.project);
    if (!engine::isVendorProjectFile(in)) {
        std::cerr << "error: " << args.project << ": not a Vivado (.xpr), Quartus (.qpf, .qsf) or Gowin (.gprj) project\n";
        return kExitUsage;
    }
    auto project = engine::loadProjectFile(in);
    if (!project) {
        std::cerr << format(project.error()) << '\n';
        return kExitUsage;
    }
    auto& cfg = project.value();
    for (const auto& d : cfg.importNotes) {
        std::cout << format(d) << '\n';
        for (const auto& n : d.notes)
            std::cout << "  note: " << n << '\n';
    }
    std::filesystem::path out = args.output.empty() ? cfg.importedFrom.parent_path() / (cfg.name + ".ocdc.json")
                                                    : std::filesystem::absolute(fromUtf8(args.output));
    std::error_code ec;
    if (std::filesystem::exists(out, ec) && !args.force) {
        std::cerr << "error: " << toUtf8(out) << " exists (--force replaces it)\n";
        return kExitUsage;
    }
    if (engine::isVendorProjectFile(out)) {
        std::cerr << "error: the output must be an OpenCDC project (.ocdc.json), not " << toUtf8(out) << '\n';
        return kExitUsage;
    }
    if (auto err = engine::saveProjectFile(cfg, out)) {
        std::cerr << format(*err) << '\n';
        return kExitAnalysisErrors;
    }
    std::cout << "OpenCDC project written: " << toUtf8(out) << '\n';
    return kExitOk;
}

struct AuditArgs {
    std::string project;
    std::vector<std::string> failOn; ///< states that fail the audit
    std::string json;
};

/// opencdc waivers: analyzes the project and lists its waivers with their
/// state and use count; fails (exit 4) when one is in a --fail-on state.
int runWaiverAudit(const AuditArgs& args) {
    static const std::vector<std::string> kStates{"unused", "expiring", "expired", "invalid"};
    for (const auto& s : args.failOn)
        if (std::ranges::find(kStates, s) == kStates.end()) {
            std::cerr << "error: --fail-on: unknown state '" << s << "' (unused, expiring, expired, invalid)\n";
            return kExitUsage;
        }
    auto project = engine::loadProjectFile(fromUtf8(args.project));
    if (!project) {
        std::cerr << format(project.error()) << '\n';
        return kExitUsage;
    }
    NullProgressSink progress;
    CancellationToken cancel;
    auto result = engine::AnalysisSession().run(project.value(), progress, cancel);
    std::cout << "OpenCDC Analyzer " << kVersionString << " - waiver audit\nProject : " << project.value().name
              << "\n";
    // waiver file problems (unreadable files, invalid entries) and expiry warnings
    std::size_t invalid = 0;
    for (const auto& d : result->diagnostics)
        if (d.code.starts_with("WAIVER-")) {
            std::cout << "  " << format(d) << '\n';
            invalid += d.severity == Severity::Error ? 1U : 0U; // WAIVER-OPEN/-SCHEMA/-INVALID errors
        }
    if (!result->cdc) {
        std::cerr << "error: the CDC analysis did not run (see 'opencdc analyze'): waiver uses are unknown\n";
        return kExitAnalysisErrors;
    }
    const auto& ws = result->cdc->waivers;
    std::map<std::string, std::size_t> counts;
    for (const auto& w : ws)
        ++counts[std::string(rules::waiverState(w))];
    std::cout << "Waivers : " << ws.size() << " from " << project.value().waivers.size() << " file(s): "
              << counts["active"] << " active, " << counts["expiring"] << " expiring, " << counts["unused"]
              << " unused, " << counts["expired"] << " expired; " << invalid << " invalid entr(y/ies) or file problem(s)\n";
    for (const auto& w : ws) {
        std::string sel;
        auto add = [&](const char* k, const std::string& v) {
            if (!v.empty())
                sel += (sel.empty() ? "" : " ") + std::string(k) + "=" + v;
        };
        add("rule", w.rule);
        add("fingerprint", w.fingerprint);
        add("source", w.source);
        add("destination", w.destination);
        add("timingException", w.timingException);
        std::cout << "  " << w.id << " [" << rules::waiverState(w) << "] uses=" << w.uses << " " << sel;
        if (!w.expires.empty())
            std::cout << " expires=" << w.expires;
        std::cout << "  (" << toString(w.loc) << ")\n      " << w.reason;
        if (!w.author.empty())
            std::cout << " -- " << w.author;
        std::cout << '\n';
    }
    if (!args.json.empty()) {
        nlohmann::json j;
        j["project"] = project.value().name;
        j["waivers"] = nlohmann::json::array();
        for (const auto& w : ws)
            j["waivers"].push_back({{"id", w.id},
                                    {"state", std::string(rules::waiverState(w))},
                                    {"uses", w.uses},
                                    {"rule", w.rule},
                                    {"fingerprint", w.fingerprint},
                                    {"source", w.source},
                                    {"destination", w.destination},
                                    {"timingException", w.timingException},
                                    {"reason", w.reason},
                                    {"author", w.author},
                                    {"expires", w.expires},
                                    {"expiresInDays", w.expiresInDays >= 0 ? nlohmann::json(w.expiresInDays)
                                                                           : nlohmann::json(nullptr)},
                                    {"file", w.loc.file},
                                    {"line", w.loc.line}});
        j["invalid"] = invalid;
        std::ofstream out(fromUtf8(args.json), std::ios::binary);
        if (!(out << j.dump(2) << '\n')) {
            std::cerr << "error: cannot write " << args.json << '\n';
            return kExitUsage;
        }
        std::cout << "JSON    : " << args.json << '\n';
    }
    std::vector<std::string> failed;
    for (const auto& s : args.failOn)
        if ((s == "invalid" && invalid > 0) || (s != "invalid" && counts[s] > 0))
            failed.push_back(s);
    if (!failed.empty()) {
        std::cout << "Audit   : FAILED (";
        for (std::size_t i = 0; i < failed.size(); ++i)
            std::cout << (i ? ", " : "") << failed[i];
        std::cout << ")\n";
        return kExitWaiverAudit;
    }
    std::cout << "Audit   : passed\n";
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    CLI::App app{"OpenCDC Analyzer - FPGA clock domain crossing static analysis", "opencdc"};
    app.set_version_flag("-v,--version", std::string("opencdc ") + opencdc::kVersionString);
    app.require_subcommand(1);

    std::string projectPath;
    Outputs outs;
    bool quiet = false;
    auto* analyze = app.add_subcommand("analyze", "Analyze a project (*.ocdc.json, a Vivado project *.xpr, a Quartus project *.qpf/*.qsf or a Gowin project *.gprj)");
    analyze->add_option("project", projectPath, "Project file")->required();
    analyze->add_option("--json", outs.json, "Write the result as JSON to this file");
    analyze->add_option("--dump-ir", outs.ir, "Write a text dump of the extracted Design IR to this file");
    analyze->add_option("--html", outs.html, "Write a self-contained HTML report (with schematics) to this file");
    analyze->add_option("--csv", outs.csvDir,
                        "Write paths.csv, violations.csv, waivers.csv and outputs.csv into this directory");
    analyze->add_option("--suggest-sdc", outs.suggestSdc,
                        "Write suggested constraints (missing clocks, clock groups for unknown relations) to this file");
    analyze->add_flag("--blackbox-unknown", outs.blackBoxUnknown,
                      "Analyze instances of modules defined nowhere (e.g. IP cores) as black boxes instead of "
                      "stopping with an error (project option unknownModules)");
    analyze->add_option("--vivado-clocks", outs.vivadoClocks,
                        "Compare the clock periods with a Vivado 'report_clocks' output (repeatable)");
    analyze->add_option("--schematic-dir", outs.schematicDir,
                        "Write the CDC schematic of every path as path-<id>.svg into this directory");
    analyze->add_flag("-q,--quiet", quiet, "Print only the summary");

    AuditArgs audit;
    auto* auditCmd = app.add_subcommand(
        "waivers", "List the project's waivers with their state (active, expiring, unused, expired) and use count "
                   "(runs the analysis); --fail-on makes the command fail (exit 4) for CI");
    auditCmd->add_option("project", audit.project, "Project file")->required();
    auditCmd->add_option("--fail-on", audit.failOn,
                         "Fail when a waiver is in one of these states: unused, expiring, expired, invalid "
                         "(comma-separated or repeated)")
        ->delimiter(',');
    auditCmd->add_option("--json", audit.json, "Write the waiver list as JSON to this file");

    WaiveArgs waive;
    auto* waiveCmd = app.add_subcommand(
        "waive", "Add a waiver to the project's waiver file (created next to the project and added to it when the "
                 "project has none); the fingerprint is printed with each path and output warning");
    waiveCmd->add_option("project", waive.project, "Project file")->required();
    waiveCmd->add_option("--fingerprint", waive.w.fingerprint, "Path or output fingerprint (exact)");
    waiveCmd->add_option("--rule", waive.w.rule, "Rule id or glob (e.g. CDC-007, CDC-OUTPUT-CLOCK)");
    waiveCmd->add_option("--source", waive.w.source, "Glob on the source name");
    waiveCmd->add_option("--destination", waive.w.destination, "Glob on the destination name");
    waiveCmd->add_option("--timing-exception", waive.w.timingException,
                         "Glob on the timing exception that covers the crossing ('*': any set_false_path / "
                         "set_max_delay of the constraints; '*sys_top.sdc:34*': that one)");
    waiveCmd->add_option("--reason", waive.w.reason, "Why the hit is acceptable (required to add a waiver)");
    waiveCmd->add_option("--author", waive.w.author, "Who waived it");
    waiveCmd->add_option("--expires", waive.w.expires, "Expiry date YYYY-MM-DD");
    waiveCmd->add_option("--id", waive.w.id, "Waiver id (default: the next free W-<n>)");
    waiveCmd->add_option("--file", waive.file, "Waiver file to write (default: the project's first waiver file)");
    waiveCmd->add_option("--remove", waive.remove, "Remove the waiver with this id (from whichever project waiver file has it)");

    ImportArgs importArgs;
    auto* importCmd = app.add_subcommand(
        "import", "Write the OpenCDC project of a Vivado project (*.xpr: the sources, IP and constraints of its "
                  "current synthesis run) or of a Quartus project (*.qpf: its first revision, or a revision *.qsf) or of a Gowin project (*.gprj)");
    importCmd->add_option("project", importArgs.project, "Vivado, Quartus or Gowin project (*.xpr, *.qpf, *.qsf, *.gprj)")
        ->required();
    importCmd->add_option("-o,--output", importArgs.output,
                          "OpenCDC project to write (default: <name>.ocdc.json next to the vendor project)");
    importCmd->add_flag("--force", importArgs.force, "Replace the output file when it exists");

    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e) {
        const int code = app.exit(e);
        return code == 0 ? kExitOk : kExitUsage;
    }

    if (analyze->parsed())
        return runAnalyze(projectPath, outs, quiet);
    if (waiveCmd->parsed())
        return runWaive(std::move(waive));
    if (auditCmd->parsed())
        return runWaiverAudit(audit);
    if (importCmd->parsed())
        return runImport(importArgs);
    return kExitUsage;
}

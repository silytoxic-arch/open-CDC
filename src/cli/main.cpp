// opencdc - command line front end of OpenCDC Analyzer.
//
//   opencdc --version
//   opencdc analyze <project.ocdc.json> [--json <out.json>] [--dump-ir <out.txt>]
//                   [--schematic-dir <dir>] [--html <report.html>] [--csv <dir>] [--quiet]
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
#include "engine/ProjectConfig.h"
#include "engine/Report.h"
#include "engine/ResultJson.h"
#include "ir/Dump.h"
#include "schematic/Schematic.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
            if (!c.sourceName.empty() && c.sourceName != c.name)
                os << " source=" << c.sourceName;
            if (c.master.valid())
                os << " master=" << cm.clock(c.master).name;
            if (c.periodNs)
                os << " period=" << *c.periodNs << "ns";
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
        if (!quiet) {
            for (const auto& p : c.paths) {
                os << "  #" << p.id << " [" << cdc::toString(p.status) << "] " << p.srcName << " (" << p.srcClockName
                   << ") -> " << p.dstName << " (" << p.dstClockName << ")";
                if (p.pin != cdc::Pin::Data)
                    os << " via " << cdc::toString(p.pin);
                if (p.groupSize > 1)
                    os << " (+" << (p.groupSize - 1) << " more)";
                os << "  width=" << p.width << " sync=" << cdc::toString(p.syncType) << "/"
                   << cdc::toString(p.syncStatus) << "\n";
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
            !writeText(dir / "violations.csv", engine::violationsCsv(*result)))
            return kExitUsage;
        os << "CSV     : " << outs.csvDir << " (paths.csv, violations.csv)\n";
    }

    if (result->hasErrors())
        return kExitAnalysisErrors;
    return result->violations(Severity::Error) > 0 ? kExitViolations : kExitOk;
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
    auto* analyze = app.add_subcommand("analyze", "Analyze a project (*.ocdc.json)");
    analyze->add_option("project", projectPath, "Project file")->required();
    analyze->add_option("--json", outs.json, "Write the result as JSON to this file");
    analyze->add_option("--dump-ir", outs.ir, "Write a text dump of the extracted Design IR to this file");
    analyze->add_option("--html", outs.html, "Write a self-contained HTML report (with schematics) to this file");
    analyze->add_option("--csv", outs.csvDir, "Write paths.csv and violations.csv into this directory");
    analyze->add_option("--schematic-dir", outs.schematicDir,
                        "Write the CDC schematic of every path as path-<id>.svg into this directory");
    analyze->add_flag("-q,--quiet", quiet, "Print only the summary");

    try {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& e) {
        const int code = app.exit(e);
        return code == 0 ? kExitOk : kExitUsage;
    }

    if (analyze->parsed())
        return runAnalyze(projectPath, outs, quiet);
    return kExitUsage;
}

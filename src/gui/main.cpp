// opencdc-gui - Qt6 desktop front end of OpenCDC Analyzer.
//
//   opencdc-gui [project.ocdc.json]
//   opencdc-gui --smoke-test <project.ocdc.json>   (headless self-test, used by CTest)
//   opencdc-gui --edit-test <project.ocdc.json>    (headless project editing self-test)

#include "core/Version.h"
#include "gui/CdcViews.h"
#include "gui/ClockView.h"
#include "gui/MainWindow.h"
#include "gui/ProjectEditor.h"
#include "core/PathUtil.h"
#include "frontend/Frontend.h"
#include "gui/SchematicView.h"
#include "gui/SourceViewer.h"

#include <QApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <QPixmap>
#include <QTextBrowser>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QImage>
#include <QTreeWidget>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDockWidget>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

namespace {

using opencdc::gui::MainWindow;

int fail(const char* what) {
    std::fprintf(stderr, "GUI smoke test FAILED: %s\n", what);
    return 1;
}

int countModules(QTreeWidget* tree) {
    auto* root = tree->topLevelItem(0);
    if (!root)
        return 0;
    for (int i = 0; i < root->childCount(); ++i) {
        if (root->child(i)->text(0) == QObject::tr("Modules"))
            return root->child(i)->childCount();
    }
    return 0;
}

/// The paths checked one by one: all of them for small designs, an evenly
/// spread sample of large ones (each check redraws the whole UI state).
std::vector<const opencdc::cdc::CdcPath*> samplePaths(const opencdc::engine::AnalysisResult& result,
                                                      std::size_t limit = 400) {
    std::vector<const opencdc::cdc::CdcPath*> out;
    const auto& paths = result.cdc->paths;
    const std::size_t step = paths.size() <= limit ? 1 : (paths.size() + limit - 1) / limit;
    for (std::size_t i = 0; i < paths.size(); i += step)
        out.push_back(&paths[i]);
    return out;
}

/// Every hit is listed; filtering by a rule shows exactly that rule's hits.
bool checkViolationFilter(MainWindow& window, const opencdc::engine::AnalysisResult& result) {
    auto* view = window.violationView();
    std::size_t total = 0;
    for (const auto& p : result.cdc->paths)
        total += p.hits.size();
    if (view->rowCount() != static_cast<int>(total))
        return false;
    for (int i = 1; i < view->ruleFilter()->count(); ++i) {
        view->ruleFilter()->setCurrentIndex(i);
        const QString rule = view->ruleFilter()->currentData().toString();
        std::size_t expected = 0;
        for (const auto& p : result.cdc->paths)
            for (const auto& h : p.hits)
                expected += QString::fromStdString(h.ruleId) == rule ? 1U : 0U;
        if (view->visibleRows() != static_cast<int>(expected))
            return false;
    }
    view->ruleFilter()->setCurrentIndex(0);
    // Waiver filter: "Waived" shows exactly the waived hits.
    auto* wf = view->waiverFilter();
    for (int i = 0; i < wf->count(); ++i)
        if (wf->itemData(i).toString() == QLatin1String("waived")) {
            wf->setCurrentIndex(i);
            const int visible = view->visibleRows();
            wf->setCurrentIndex(0);
            if (visible != static_cast<int>(result.waivedHits()))
                return false;
        }
    return true;
}

/// Every path's details render (with structure and reconvergence sections
/// where the analysis has them), and the synchronizer-type filter shows
/// exactly the paths of each type.
bool checkPathDetails(MainWindow& window, const opencdc::engine::AnalysisResult& result) {
    auto* view = window.pathView();
    for (const auto* pp : samplePaths(result)) {
        const auto& p = *pp;
        view->showPath(p.id);
        const QString html = view->details()->toHtml();
        if (!html.contains(QStringLiteral("Path #%1").arg(p.id)))
            return false;
        if (p.structure < result.cdc->structures.size() && !html.contains(QStringLiteral("S%1 ").arg(p.structure)))
            return false;
        if (!p.reconvergences.empty() && !html.contains(QStringLiteral("Reconverges at")))
            return false;
        if (p.groupSize > 1 && !html.contains(QStringLiteral("Grouped registers")))
            return false;
    }
    auto* filter = view->syncFilter();
    for (int i = 1; i < filter->count(); ++i) {
        filter->setCurrentIndex(i);
        const QString type = filter->currentData().toString();
        int expected = 0;
        for (const auto& p : result.cdc->paths)
            expected += QString::fromUtf8(opencdc::cdc::toString(p.syncType).data()) == type ? 1 : 0;
        if (view->visibleRows() != expected)
            return false;
    }
    filter->setCurrentIndex(0);
    return true;
}

/// The schematic follows the selected path and draws every node and edge of
/// the model; the hierarchy counts CDC paths per instance subtree and its
/// instance filter shows exactly those paths; the schematic exports as PNG.
bool checkSchematicAndHierarchy(MainWindow& window, const opencdc::engine::AnalysisResult& result) {
    auto* sv = window.schematicView();
    for (const auto* pp : samplePaths(result)) {
        const auto& p = *pp;
        window.pathView()->showPath(p.id);
        if (sv->currentPath() != p.id)
            return (fail("schematic does not follow the selected path"), false);
        const auto& s = sv->schematic();
        const auto real = std::ranges::count_if(s.nodes, [](const opencdc::schematic::Node& n) {
            return n.kind != opencdc::schematic::NodeKind::Dummy;
        });
        if (real < 2 || sv->drawnNodes() != static_cast<int>(real) ||
            sv->drawnEdges() != static_cast<int>(s.edges.size()) || !s.find(p.srcName) || !s.find(p.dstName))
            return (fail("schematic drawing does not match its model"), false);
    }
    if (!result.cdc->paths.empty()) {
        // Own directory per process: the GUI tests run in parallel.
        const QTemporaryDir dir;
        const QString png = dir.filePath(QStringLiteral("schematic.png"));
        if (!dir.isValid() || !sv->exportPng(png) || QImage(png).isNull())
            return (fail("schematic PNG export failed"), false);
        const QString svg = dir.filePath(QStringLiteral("schematic.svg"));
        if (!sv->exportSvg(svg) || QFileInfo(svg).size() < 200)
            return (fail("schematic SVG export failed"), false);
        const QString html = dir.filePath(QStringLiteral("report.html"));
        if (!window.exportHtmlReport(html) || QFileInfo(html).size() < 2000 || !window.exportCsv(dir.path()) ||
            !QFileInfo::exists(dir.filePath(QStringLiteral("violations.csv"))))
            return (fail("report export failed"), false);
    }
    auto* top = window.hierarchyTree()->topLevelItem(0);
    if (!top)
        return false;
    const auto total = static_cast<int>(result.cdc->paths.size());
    if (total > 0 && top->text(3) != QString::number(total))
        return (fail("hierarchy CDC path count of the top instance is wrong"), false);
    // Filter by every instance with paths (the first 200 in breadth-first
    // order for large designs): the visible rows include all its paths.
    std::vector<QTreeWidgetItem*> items{top};
    for (std::size_t i = 0; i < items.size(); ++i)
        for (int c = 0; c < items[i]->childCount(); ++c)
            items.push_back(items[i]->child(c));
    int checked = 0;
    for (auto* item : items) {
        if (item->text(3).isEmpty())
            continue;
        if (++checked > 200)
            break;
        if (!window.showInstancePaths(item))
            return (fail("instance filter failed"), false);
        if (window.pathView()->visibleRows() < item->text(3).toInt())
            return (fail("instance filter hides paths of the instance"), false);
    }
    window.pathView()->setTextFilter(QString());
    return true;
}

/// Exercises the real GUI flow: open project -> background analysis ->
/// populate views -> jump to source. Exit code 0 means every check passed.
int runSmokeTest(MainWindow& window, const QString& project, const QString& screenshot) {
    if (!window.openProject(project))
        return fail("project could not be loaded");

    int exitCode = -1;
    QElapsedTimer clock;
    QObject::connect(window.controller(), &opencdc::gui::AnalysisController::finished, &window,
                     [&](const opencdc::gui::AnalysisController::ResultPtr& result) {
                         const auto loadedMs = clock.elapsed(); // analysis + view population
                         exitCode = 0;
                         if (!result || !result->elaboration) {
                             exitCode = fail("no elaboration result");
                         }
                         else if (window.hierarchyTree()->topLevelItemCount() < 1 ||
                                  window.hierarchyTree()->topLevelItem(0)->childCount() < 1) {
                             exitCode = fail("hierarchy view is empty");
                         }
                         else if (countModules(window.projectTree()) !=
                                  static_cast<int>(result->elaboration->definitions.size())) {
                             exitCode = fail("module list does not match the elaboration result");
                         }
                         else if (result->design &&
                                  window.hierarchyTree()->topLevelItem(0)->text(2).isEmpty()) {
                             exitCode = fail("register column is empty although the Design IR exists");
                         }
                         else if (result->clocks &&
                                  window.clockView()->matrix()->rowCount() !=
                                      static_cast<int>(result->clocks->clocks.size())) {
                             exitCode = fail("clock relation matrix does not match the clock model");
                         }
                         else if (result->cdc && window.pathView()->rowCount() !=
                                                     static_cast<int>(result->cdc->paths.size())) {
                             exitCode = fail("CDC path table does not match the analysis result");
                         }
                         else if (result->cdc && !checkViolationFilter(window, *result)) {
                             exitCode = fail("rule violation table or its filter does not match the analysis result");
                         }
                         else if (result->cdc && !checkPathDetails(window, *result)) {
                             exitCode = fail("CDC path details or the synchronizer filter do not match the result");
                         }
                         else if (result->cdc && !checkSchematicAndHierarchy(window, *result)) {
                             exitCode = 1;
                         }
                         else if (window.messageTable()->rowCount() !=
                                  static_cast<int>(result->diagnostics.size())) {
                             exitCode = fail("message view does not match diagnostics");
                         }
                         else {
                             // Double-click equivalent on the first child instance.
                             auto* inst = window.hierarchyTree()->topLevelItem(0)->child(0);
                             if (!window.jumpToItemLocation(inst))
                                 exitCode = fail("jump to source failed");
                             else if (window.sourceViewer()->highlightedLine() !=
                                      inst->data(0, Qt::UserRole + 1).toInt())
                                 exitCode = fail("source viewer highlighted the wrong line");
                         }
                         if (exitCode == 0 && !screenshot.isEmpty()) {
                             for (const char* dock : {"designHierarchy", "cdcPaths", "cdcSchematic"})
                                 if (auto* d = window.findChild<QDockWidget*>(QString::fromLatin1(dock)))
                                     d->raise();
                             window.hierarchyTree()->expandAll();
                             // Show a protocol synchronizer in the details pane if there is one.
                             if (result->cdc)
                                 for (const auto type : {opencdc::cdc::SyncType::AsyncFifo,
                                                         opencdc::cdc::SyncType::Handshake})
                                     if (auto it = std::ranges::find(result->cdc->paths, type,
                                                                     &opencdc::cdc::CdcPath::syncType);
                                         it != result->cdc->paths.end()) {
                                         window.pathView()->showPath(it->id);
                                         break;
                                     }
                             window.resize(1600, 1150);
                             if (auto* paths = window.findChild<QDockWidget*>(QStringLiteral("cdcPaths")))
                                 window.resizeDocks({paths}, {560}, Qt::Vertical);
                             if (auto* sch = window.findChild<QDockWidget*>(QStringLiteral("cdcSchematic"))) {
                                 window.resizeDocks({sch}, {900}, Qt::Horizontal);
                                 window.schematicView()->showPath(*window.pathView()->currentPath());
                             }
                             window.show();
                             if (!window.grab().save(screenshot))
                                 exitCode = fail("could not save screenshot");
                         }
                         if (exitCode == 0) {
                             std::printf("GUI smoke test passed: %d top instance(s), %d module(s), "
                                         "%d message(s), %d CDC path(s), %d rule hit(s), jumped to %s:%d; "
                                         "analysis and views ready in %lld ms\n",
                                         window.hierarchyTree()->topLevelItemCount(),
                                         countModules(window.projectTree()), window.messageTable()->rowCount(),
                                         window.pathView()->rowCount(),
                                         window.violationView()->rowCount(),
                                         qPrintable(window.sourceViewer()->currentFile()),
                                         window.sourceViewer()->highlightedLine(),
                                         static_cast<long long>(loadedMs));
                         }
                         QApplication::exit(exitCode);
                     });

    QTimer::singleShot(0, &window, [&window, &clock] {
        clock.start();
        window.runAnalysis();
    });
    // Safety net: never hang CI.
    QTimer::singleShot(600000, &window, [] {
        std::fprintf(stderr, "GUI smoke test FAILED: timeout\n");
        QApplication::exit(1);
    });
    return QApplication::exec();
}

/// Project editing: the editor shows the project as loaded and gives it back
/// unchanged; removing, re-adding and re-labelling a source, adding a
/// parameter and a constraint, saving under a new name and reopening keeps
/// every edit; the saved project analyzes like the original.
int runEditTest(MainWindow& window, const QString& project) {
    if (!window.openProject(project))
        return fail("project could not be loaded");
    const auto orig = *window.project();
    opencdc::gui::ProjectEditor editor(orig, QString());
    if (editor.sourceTable()->rowCount() != static_cast<int>(orig.sources.size()) ||
        editor.constraintList()->count() != static_cast<int>(orig.constraints.size()))
        return fail("project editor does not show the project");
    auto same = editor.config();
    if (same.sources != orig.sources || same.sourceLibraries != orig.sourceLibraries || same.name != orig.name ||
        same.top != orig.top || same.language != orig.language || same.constraints != orig.constraints ||
        same.parameters != orig.parameters || same.includeDirs != orig.includeDirs ||
        same.waivers != orig.waivers || same.defines.size() != orig.defines.size() ||
        same.options.minSyncStages != orig.options.minSyncStages)
        return fail("project editor changes a project it did not edit");
    if (orig.sources.empty())
        return fail("project has no sources");

    const auto last = static_cast<int>(orig.sources.size()) - 1;
    const QString lastFile = editor.sourceTable()->item(last, 0)->text();
    editor.removeSource(last);
    if (editor.validationError().isEmpty() != (orig.sources.size() > 1))
        return fail("validation of the source list is wrong");
    editor.addSources({lastFile, lastFile}); // duplicates are ignored
    editor.setSourceLibrary(last, QStringLiteral("edit_lib"));
    editor.setParameter(QStringLiteral("OPENCDC_EDIT_TEST"), QStringLiteral("1"));
    editor.removeSource(-1);              // no-op
    editor.moveSource(0, -1);             // no-op
    if (!orig.constraints.empty())
        editor.addConstraints({QString::fromStdString(opencdc::toUtf8(orig.constraints.front()))}); // duplicate
    auto edited = editor.config();
    if (edited.sources != orig.sources || edited.sourceLibraries.back() != "edit_lib" ||
        edited.constraints != orig.constraints || edited.parameters.size() != orig.parameters.size() + 1)
        return fail("edits are not reflected in the configuration");

    // Keep the library only if it does not matter (Verilog file) for the analysis check below.
    if (opencdc::frontend::isVhdlFile(edited.sources.back()))
        edited.sourceLibraries.back() = orig.sourceLibraries.back();
    edited.parameters = orig.parameters;
    const QTemporaryDir dir;
    const QString saved = dir.filePath(QStringLiteral("edited.ocdc.json"));
    window.setProject(edited);
    if (!dir.isValid() || !window.saveProjectAs(saved))
        return fail("project could not be saved");
    if (!window.openProject(saved))
        return fail("saved project could not be reopened");
    const auto& re = *window.project();
    if (re.sources != edited.sources || re.sourceLibraries != edited.sourceLibraries ||
        re.constraints != edited.constraints || re.name != edited.name || re.language != edited.language)
        return fail("reopened project differs from the saved one");
    if (window.saveProjectAs(dir.filePath(QStringLiteral("missing/dir/x.ocdc.json"))))
        return fail("saving into a missing directory did not fail");

    int exitCode = -1;
    QObject::connect(window.controller(), &opencdc::gui::AnalysisController::finished, &window,
                     [&](const opencdc::gui::AnalysisController::ResultPtr& result) {
                         exitCode = result && result->cdc ? 0 : fail("saved project does not analyze");
                         if (exitCode == 0)
                             std::printf("GUI edit test passed: %zu source(s), %zu constraint file(s), "
                                         "%zu CDC path(s) after save and reopen\n",
                                         re.sources.size(), re.constraints.size(), result->cdc->paths.size());
                         QApplication::exit(exitCode);
                     });
    QTimer::singleShot(0, &window, [&window] { window.runAnalysis(); });
    QTimer::singleShot(600000, &window, [] {
        std::fprintf(stderr, "GUI edit test FAILED: timeout\n");
        QApplication::exit(1);
    });
    return QApplication::exec();
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("OpenCDC Analyzer");
    QApplication::setOrganizationName("OpenCDC");
    QApplication::setApplicationVersion(QString::fromLatin1(opencdc::kVersionString));

    QCommandLineParser parser;
    parser.setApplicationDescription("OpenCDC Analyzer GUI");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeOption("smoke-test", "Run a headless GUI self-test on <project> and exit.", "project");
    parser.addOption(smokeOption);
    QCommandLineOption screenshotOption("screenshot", "With --smoke-test: save a window screenshot to <png>.", "png");
    parser.addOption(screenshotOption);
    QCommandLineOption editOption("edit-test", "Run a headless project editing self-test on <project> and exit.",
                                  "project");
    parser.addOption(editOption);
    parser.addPositionalArgument("project", "Project file to open (*.ocdc.json).", "[project]");
    parser.process(app);

    MainWindow window;

    if (parser.isSet(editOption))
        return runEditTest(window, parser.value(editOption));
    if (parser.isSet(smokeOption))
        return runSmokeTest(window, parser.value(smokeOption), parser.value(screenshotOption));

    window.show();
    if (!parser.positionalArguments().isEmpty()) {
        const QString project = parser.positionalArguments().first();
        QTimer::singleShot(0, &window, [&window, project] {
            if (window.openProject(project))
                window.runAnalysis();
        });
    }
    return QApplication::exec();
}

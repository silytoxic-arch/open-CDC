// opencdc-gui - Qt6 desktop front end of OpenCDC Analyzer.
//
//   opencdc-gui [project.ocdc.json]
//   opencdc-gui --smoke-test <project.ocdc.json>   (headless self-test, used by CTest)
//   opencdc-gui --edit-test <project.ocdc.json>    (headless project editing self-test)
//   opencdc-gui --waive-test <project.ocdc.json>   (headless waiver self-test, on a copy of the project)

#include "core/Version.h"
#include "gui/CdcViews.h"
#include "gui/ClockView.h"
#include "gui/MainWindow.h"
#include "gui/ProjectEditor.h"

#include <QDir>
#include "core/PathUtil.h"
#include "frontend/Frontend.h"
#include "gui/SchematicView.h"
#include "gui/SourceViewer.h"
#include "gui/WaiverDialog.h"
#include "engine/WaiverFile.h"

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
#include <QHeaderView>
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

/// Items that show a clock (clock role set), at any depth.
int countClockItems(QTreeWidget* tree) {
    int count = 0;
    std::vector<QTreeWidgetItem*> work;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        work.push_back(tree->topLevelItem(i));
    while (!work.empty()) {
        auto* it = work.back();
        work.pop_back();
        if (it->data(0, Qt::UserRole + 2).isValid())
            ++count;
        for (int c = 0; c < it->childCount(); ++c)
            work.push_back(it->child(c));
    }
    return count;
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
bool checkWaiverView(MainWindow& window, const opencdc::engine::AnalysisResult& result);

/// Output Ports view: one row per output crossing, the waiver filter splits
/// waived and open ones, the waiver column and details match the record, and
/// a double-click opens the set_output_delay command.
bool checkOutputPorts(MainWindow& window, const opencdc::engine::AnalysisResult& result) {
    auto* view = window.outputView();
    const auto& outs = result.cdc->outputs;
    if (view->rowCount() != static_cast<int>(outs.size()))
        return false;
    const auto waived = std::ranges::count_if(outs, [](const auto& o) { return !o.waiver.empty(); });
    auto* f = view->waiverFilter();
    f->setCurrentIndex(f->findData(QStringLiteral("open")));
    const int open = view->visibleRows();
    f->setCurrentIndex(f->findData(QStringLiteral("waived")));
    const int closed = view->visibleRows();
    f->setCurrentIndex(0);
    if (open != static_cast<int>(outs.size() - static_cast<std::size_t>(waived)) || closed != waived ||
        view->visibleRows() != view->rowCount())
        return false;
    for (int r = 0; r < view->rowCount(); ++r) {
        const auto& o = outs[static_cast<std::size_t>(r)];
        if (view->table()->item(r, 7)->text() != QString::fromStdString(o.waiver) ||
            view->table()->item(r, 3)->text() != QString::fromStdString(o.outClock))
            return false;
        view->table()->setCurrentCell(r, 0);
        const QString details = view->details()->toPlainText();
        if (!details.contains(QString::fromStdString(o.outClock)) ||
            (!o.waiver.empty() && !details.contains(QStringLiteral("Waived by ") + QString::fromStdString(o.waiver))))
            return false;
        emit view->table()->cellDoubleClicked(r, 0);
        if (window.sourceViewer()->highlightedLine() != static_cast<int>(o.loc.line) ||
            !QFileInfo(window.sourceViewer()->currentFile()).fileName().endsWith(
                QFileInfo(QString::fromStdString(o.loc.file)).fileName()))
            return false;
    }
    return true;
}

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
            // (waivedHits() also counts waived output crossings: they are in the Output Ports view)
            std::size_t waivedPathHits = 0;
            for (const auto& p : result.cdc->paths)
                waivedPathHits += static_cast<std::size_t>(
                    std::ranges::count_if(p.hits, [](const auto& h) { return !h.waiver.empty(); }));
            if (visible != static_cast<int>(waivedPathHits))
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
        const QString sdc = dir.filePath(QStringLiteral("suggested.sdc"));
        if (!window.exportSuggestedSdc(sdc) || QFileInfo(sdc).size() < 100)
            return (fail("suggested constraints export failed"), false);
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
                         else if (result->clocks && countClockItems(window.clockView()->tree()) !=
                                                        static_cast<int>(result->clocks->clocks.size())) {
                             exitCode = fail("clock tree does not show every clock exactly once");
                         }
                         else if (result->cdc && window.pathView()->rowCount() !=
                                                     static_cast<int>(result->cdc->paths.size())) {
                             exitCode = fail("CDC path table does not match the analysis result");
                         }
                         else if (result->cdc && !checkWaiverView(window, *result)) {
                             exitCode = fail("the Waivers view or its state filter does not match the result");
                         }
                         else if (result->cdc && !checkOutputPorts(window, *result)) {
                             exitCode = fail("output port table, its filter or its details do not match the result");
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
                         if (exitCode == 0) {
                             // every data column can be resized by dragging its divider
                             for (auto* h : window.findChildren<QHeaderView*>()) {
                                 if (h->orientation() != Qt::Horizontal)
                                     continue;
                                 for (int c = 0; c + 1 < h->count(); ++c)
                                     if (h->sectionResizeMode(c) != QHeaderView::Interactive) {
                                         exitCode = fail("a table column cannot be resized by dragging");
                                         break;
                                     }
                             }
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
                                         "%d message(s), %d CDC path(s), %d rule hit(s), %d output crossing(s), "
                                         "jumped to %s:%d; "
                                         "analysis and views ready in %lld ms\n",
                                         window.hierarchyTree()->topLevelItemCount(),
                                         countModules(window.projectTree()), window.messageTable()->rowCount(),
                                         window.pathView()->rowCount(),
                                         window.violationView()->rowCount(), window.outputView()->rowCount(),
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
        editor.constraintTable()->rowCount() != static_cast<int>(orig.constraints.size()))
        return fail("project editor does not show the project");
    auto same = editor.config();
    if (same.sources != orig.sources || same.sourceLibraries != orig.sourceLibraries || same.name != orig.name ||
        same.top != orig.top || same.language != orig.language || same.constraints != orig.constraints ||
        same.constraintScopes != orig.constraintScopes ||
        same.parameters != orig.parameters || same.includeDirs != orig.includeDirs ||
        same.waivers != orig.waivers || same.vivadoClockReports != orig.vivadoClockReports ||
        same.ipRepositories != orig.ipRepositories || same.tclSourceDirs != orig.tclSourceDirs ||
        same.defines.size() != orig.defines.size() ||
        same.options.minSyncStages != orig.options.minSyncStages ||
        same.options.waiverExpiryWarningDays != orig.options.waiverExpiryWarningDays)
        return fail("project editor changes a project it did not edit");
    if (orig.sources.empty())
        return fail("project has no sources");

    // the module of a scoped constraint file is edited in the constraints
    // table and kept through save and reopen; a name that is no module is refused
    if (!orig.constraints.empty()) {
        const QString before = editor.constraintTable()->item(0, 1) ? editor.constraintTable()->item(0, 1)->text()
                                                                     : QString();
        editor.setConstraintScope(0, QStringLiteral("not a module"));
        if (editor.validationError().isEmpty())
            return fail("a module name with spaces is accepted for scoped constraints");
        editor.setConstraintScope(0, QStringLiteral("opencdc_scope_test"));
        if (!editor.validationError().isEmpty())
            return fail("a scoped constraint module is refused");
        const auto scoped = editor.config();
        if (scoped.constraintScopes.size() != orig.constraints.size() ||
            scoped.constraintScopes.front() != "opencdc_scope_test" || scoped.constraints != orig.constraints)
            return fail("the scoped constraint module is not in the configuration");
        const QTemporaryDir sdir;
        const QString sfile = sdir.filePath(QStringLiteral("scoped.ocdc.json"));
        window.setProject(scoped);
        if (!sdir.isValid() || !window.saveProjectAs(sfile) || !window.openProject(sfile))
            return fail("project with a scoped constraint file could not be saved and reopened");
        if (window.project()->constraintScopes != scoped.constraintScopes)
            return fail("the scoped constraint module is lost on save and reopen");
        opencdc::gui::ProjectEditor reEdit(*window.project(), QString());
        if (!reEdit.constraintTable()->item(0, 1) ||
            reEdit.constraintTable()->item(0, 1)->text() != QStringLiteral("opencdc_scope_test"))
            return fail("the editor does not show the module of a scoped constraint file");
        editor.setConstraintScope(0, before);
        if (editor.config().constraintScopes != orig.constraintScopes)
            return fail("clearing the scoped constraint module does not restore the project");
    }

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
    editor.setWaiverExpiryWarningDays(7); // saved as an option, kept on reopen
    if (!orig.constraints.empty())
        editor.addConstraints({QString::fromStdString(opencdc::toUtf8(orig.constraints.front()))}); // duplicate
    // an IP repository is added once (a duplicate is ignored) and saved with the project
    const QString repo = QDir::tempPath();
    editor.addIpRepository(repo);
    editor.addIpRepository(repo);
    if (editor.ipRepositoryList()->count() != static_cast<int>(orig.ipRepositories.size()) + 1)
        return fail("IP repository list is wrong");
    auto edited = editor.config();
    if (edited.ipRepositories.size() != orig.ipRepositories.size() + 1)
        return fail("IP repositories are not reflected in the configuration");
    edited.ipRepositories = orig.ipRepositories;
    if (edited.sources != orig.sources || edited.sourceLibraries.back() != "edit_lib" ||
        edited.constraints != orig.constraints || edited.parameters.size() != orig.parameters.size() + 1 ||
        edited.options.waiverExpiryWarningDays != 7)
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
        re.constraints != edited.constraints || re.name != edited.name || re.language != edited.language ||
        re.options.waiverExpiryWarningDays != 7)
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

/// Waivers view: one row per waiver; the state filter counts match the
/// result (unused = no use, expired).
bool checkWaiverView(MainWindow& window, const opencdc::engine::AnalysisResult& result) {
    auto* view = window.waiverView();
    const auto& ws = result.cdc->waivers;
    if (view->rowCount() != static_cast<int>(ws.size()))
        return false;
    auto* f = view->stateFilter();
    for (const char* state : {"active", "unused", "expiring", "expired"}) {
        f->setCurrentIndex(f->findData(QString::fromLatin1(state)));
        const auto expected = std::ranges::count_if(ws, [&](const auto& w) {
            return opencdc::gui::WaiverView::stateOf(w) == QString::fromLatin1(state);
        });
        if (view->visibleRows() != static_cast<int>(expected))
            return false;
    }
    f->setCurrentIndex(0);
    return view->visibleRows() == view->rowCount();
}

/// Waivers from the GUI: on a copy of the project (saved into a temporary
/// directory), the first open rule hit is waived through the waiver dialog
/// into a new waiver file (added to the project, which is saved); the
/// analysis runs again and shows it waived; the waiver is edited and then
/// removed in the Waivers view, the hit is open again. Then the first open
/// output crossing is waived. Negative: no reason, a malformed date, a
/// duplicate id when adding and when editing.
int runWaiveTest(MainWindow& window, const QString& project) {
    if (!window.openProject(project))
        return fail("project could not be loaded");
    static QTemporaryDir dir;
    if (!dir.isValid() || !window.saveProjectAs(dir.filePath(QStringLiteral("copy.ocdc.json"))))
        return fail("project copy could not be saved");
    static QString file;
    file = dir.filePath(QStringLiteral("new-waivers.json"));
    enum Stage { Start, HitWaived, Edited, Removed, OutputWaived };
    struct State {
        Stage stage = Start;
        std::string hitRule, hitFingerprint, hitId, outFingerprint, outId;
        std::size_t waivedBefore = 0;
    };
    static State st;
    static MainWindow* win = &window;
    struct Helpers {
        static std::size_t waivedHits(const opencdc::engine::AnalysisResult& r) {
            std::size_t n = 0;
            for (const auto& p : r.cdc->paths)
                n += static_cast<std::size_t>(
                    std::ranges::count_if(p.hits, [](const auto& h) { return !h.waiver.empty(); }));
            return n;
        }
        static const opencdc::cdc::RuleHit* hit(const opencdc::engine::AnalysisResult& r) {
            for (const auto& p : r.cdc->paths)
                if (p.fingerprint == st.hitFingerprint)
                    for (const auto& h : p.hits)
                        if (h.ruleId == st.hitRule)
                            return &h;
            return nullptr;
        }
        // fill and check an add dialog, then add its waiver; "" on success
        static const char* waiveThrough(std::unique_ptr<opencdc::gui::WaiverDialog> dlg, std::string& id) {
            if (!dlg)
                return "no waiver dialog";
            if (dlg->validationError().isEmpty())
                return "a waiver without a reason is accepted";
            dlg->setReason(QStringLiteral("self-test: acceptable"));
            dlg->setExpires(QStringLiteral("2099-1-1"));
            if (dlg->validationError().isEmpty())
                return "a malformed expiry date is accepted";
            dlg->setExpires(QStringLiteral("2099-12-31"));
            if (!dlg->validationError().isEmpty())
                return "a complete waiver is refused";
            auto w = dlg->waiver();
            id = w.id;
            if (!win->addWaiver(w, file).isEmpty())
                return "the waiver could not be added";
            return "";
        }
        // waive the first open output crossing; false: none
        static bool waiveOutput(const opencdc::cdc::CdcResult& c, const char*& err) {
            for (std::size_t i = 0; i < c.outputs.size(); ++i)
                if (c.outputs[i].waiver.empty()) {
                    st.outFingerprint = c.outputs[i].fingerprint;
                    st.stage = OutputWaived;
                    err = waiveThrough(win->waiverDialogForOutput(i), st.outId);
                    return true;
                }
            return false;
        }
    };
    auto finish = [](int code) { QApplication::exit(code); };
    auto done = [finish] {
        if (st.hitRule.empty() && st.outFingerprint.empty())
            return finish(fail("nothing to waive in this project"));
        std::printf("GUI waive test passed: rule hit %s %s, output %s %s\n",
                    st.hitRule.empty() ? "-" : st.hitRule.c_str(),
                    st.hitId.empty() ? "" : (st.hitId + " (waived, edited, removed)").c_str(),
                    st.outFingerprint.empty() ? "-" : st.outFingerprint.c_str(), st.outId.c_str());
        finish(0);
    };
    QObject::connect(
        window.controller(), &opencdc::gui::AnalysisController::finished, &window,
        [&window, finish, done](const opencdc::gui::AnalysisController::ResultPtr& result) {
            if (!result || !result->cdc)
                return finish(fail("no CDC result"));
            const auto& c = *result->cdc;
            if (!checkWaiverView(window, *result))
                return finish(fail("the Waivers view or its state filter does not match the result"));
            const char* err = "";
            switch (st.stage) {
                case Start: {
                    st.waivedBefore = Helpers::waivedHits(*result);
                    for (const auto& p : c.paths)
                        for (std::size_t h = 0; h < p.hits.size(); ++h)
                            if (p.hits[h].waiver.empty()) {
                                st.hitRule = p.hits[h].ruleId;
                                st.hitFingerprint = p.fingerprint;
                                st.stage = HitWaived;
                                if (*(err = Helpers::waiveThrough(window.waiverDialogForHit(p.id, h), st.hitId)))
                                    return finish(fail(err));
                                return; // analyzed again
                            }
                    if (!Helpers::waiveOutput(c, err))
                        return done();
                    if (*err)
                        return finish(fail(err));
                    return;
                }
                case HitWaived: {
                    const auto* h = Helpers::hit(*result);
                    if (!h || h->waiver != st.hitId || Helpers::waivedHits(*result) <= st.waivedBefore)
                        return finish(fail("the waived rule hit is not waived after the analysis"));
                    if (!window.project() || std::ranges::none_of(window.project()->waivers, [](const auto& f) {
                            return QFileInfo(QString::fromStdString(opencdc::toUtf8(f))) == QFileInfo(file);
                        }))
                        return finish(fail("the new waiver file is not in the project"));
                    auto reloaded = opencdc::engine::loadProjectFile(window.project()->projectFile);
                    if (!reloaded || reloaded.value().waivers != window.project()->waivers)
                        return finish(fail("the project file does not list the new waiver file"));
                    // the Waivers view shows it, used once
                    const QString id = QString::fromStdString(st.hitId);
                    if (!window.waiverView()->selectWaiver(id) || window.waiverView()->currentId() != id)
                        return finish(fail("the new waiver is not in the Waivers view"));
                    // edit: a new reason; negative: an id another waiver has
                    auto dlg = window.waiverDialogForEdit(id);
                    if (!dlg)
                        return finish(fail("no edit dialog"));
                    for (const auto& w : c.waivers)
                        if (w.id != st.hitId) {
                            dlg->setId(QString::fromStdString(w.id));
                            if (dlg->validationError().isEmpty())
                                return finish(fail("editing a waiver to another waiver's id is accepted"));
                            break;
                        }
                    dlg->setId(id);
                    dlg->setReason(QStringLiteral("self-test: edited reason"));
                    if (!dlg->validationError().isEmpty())
                        return finish(fail("an edited waiver is refused"));
                    st.stage = Edited;
                    if (!window.updateWaiver(id, dlg->waiver()).isEmpty())
                        return finish(fail("the waiver could not be updated"));
                    return;
                }
                case Edited: {
                    const auto it = std::ranges::find(c.waivers, st.hitId, &opencdc::cdc::WaiverRecord::id);
                    const auto* h = Helpers::hit(*result);
                    if (it == c.waivers.end() || it->reason != "self-test: edited reason" || !h ||
                        h->waiver != st.hitId)
                        return finish(fail("the edited waiver is not in the result"));
                    st.stage = Removed;
                    if (!window.removeWaiver(QString::fromStdString(st.hitId)).isEmpty())
                        return finish(fail("the waiver could not be removed"));
                    return;
                }
                case Removed: {
                    const auto* h = Helpers::hit(*result);
                    if (!h || !h->waiver.empty() ||
                        std::ranges::any_of(c.waivers, [](const auto& w) { return w.id == st.hitId; }))
                        return finish(fail("the removed waiver still applies"));
                    if (!Helpers::waiveOutput(c, err))
                        return done();
                    if (*err)
                        return finish(fail(err));
                    return;
                }
                case OutputWaived: {
                    const auto it =
                        std::ranges::find(c.outputs, st.outFingerprint, &opencdc::cdc::OutputCrossing::fingerprint);
                    if (it == c.outputs.end() || it->waiver != st.outId)
                        return finish(fail("the waived output crossing is not waived after the analysis"));
                    return done();
                }
            }
        });
    QTimer::singleShot(0, &window, [&window] { window.runAnalysis(); });
    QTimer::singleShot(600000, &window, [] {
        std::fprintf(stderr, "GUI smoke test FAILED: timeout\n");
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
    QCommandLineOption waiveOption("waive-test", "Run a headless waiver self-test on a copy of <project> and exit.",
                                   "project");
    parser.addOption(waiveOption);
    parser.addPositionalArgument("project", "Project file to open (*.ocdc.json).", "[project]");
    parser.process(app);

    MainWindow window;

    if (parser.isSet(editOption))
        return runEditTest(window, parser.value(editOption));
    if (parser.isSet(waiveOption))
        return runWaiveTest(window, parser.value(waiveOption));
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

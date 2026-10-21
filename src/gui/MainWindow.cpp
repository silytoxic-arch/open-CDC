#include "gui/MainWindow.h"

#include "core/PathUtil.h"
#include "core/Version.h"
#include "engine/Report.h"
#include "rules/RuleEngine.h"
#include "gui/CdcViews.h"
#include "gui/ClockView.h"
#include "gui/SchematicView.h"
#include "gui/SourceViewer.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>
#include <set>
#include <unordered_map>

namespace opencdc::gui {

namespace {

constexpr int kFileRole = Qt::UserRole;
constexpr int kLineRole = Qt::UserRole + 1;
constexpr int kHierRole = Qt::UserRole + 3; ///< hierarchy tree: hierarchical path of the instance

QString qs(const std::string& s) {
    return QString::fromStdString(s); // all OpenCDC strings are UTF-8
}

QString qs(const std::filesystem::path& p) {
    return QString::fromStdString(toUtf8(p));
}

void setLocation(QTreeWidgetItem* item, const SourceLocation& loc) {
    if (!loc.file.empty()) {
        item->setData(0, kFileRole, qs(loc.file));
        item->setData(0, kLineRole, static_cast<int>(loc.line));
        item->setToolTip(0, qs(toString(loc)));
    }
}

/// Register statistics of one instance (from the Design IR), keyed by hierarchical path.
using RegisterCounts = std::unordered_map<std::string, std::pair<std::size_t, std::uint64_t>>;

RegisterCounts registerCounts(const ir::Design* design) {
    RegisterCounts counts;
    if (!design)
        return counts;
    for (const auto& inst : design->instances) {
        const auto& b = design->body(inst.body);
        std::uint64_t bits = 0;
        for (const auto& r : b.registers)
            bits += r.q.width();
        counts[inst.hierPath] = {b.registers.size(), bits};
    }
    return counts;
}

/// Per instance subtree: CDC paths whose destination register lies in it,
/// and how many of them are violations or potential risks.
struct CdcCount {
    std::size_t paths = 0;
    std::size_t issues = 0;
};
using CdcCounts = std::unordered_map<std::string, CdcCount>;

CdcCounts cdcCounts(const cdc::CdcResult* cdc) {
    CdcCounts counts;
    if (!cdc)
        return counts;
    for (const auto& p : cdc->paths) {
        const bool issue = p.status == cdc::AnalysisStatus::Violation || p.status == cdc::AnalysisStatus::PotentialRisk;
        // Every enclosing instance: the prefixes of the register name up to a '.'
        // outside brackets (generate names such as g[0] contain no dots).
        int depth = 0;
        for (std::size_t i = 0; i < p.dstName.size(); ++i) {
            const char c = p.dstName[i];
            if (c == '[')
                ++depth;
            else if (c == ']')
                --depth;
            else if (c == '.' && depth == 0) {
                auto& n = counts[p.dstName.substr(0, i)];
                ++n.paths;
                n.issues += issue ? 1 : 0;
            }
        }
    }
    return counts;
}

void addInstance(QTreeWidgetItem* parent, QTreeWidget* tree, const frontend::InstanceInfo& inst,
                 const RegisterCounts& counts, const CdcCounts& cdc) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, qs(inst.name));
    item->setText(1, qs(inst.definition));
    item->setToolTip(1, qs(inst.hierPath));
    if (auto it = counts.find(inst.hierPath); it != counts.end()) {
        item->setText(2, QStringLiteral("%1 (%2 bits)").arg(it->second.first).arg(it->second.second));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
    if (auto it = cdc.find(inst.hierPath); it != cdc.end()) {
        item->setText(3, QString::number(it->second.paths));
        item->setText(4, QString::number(it->second.issues));
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        if (it->second.issues > 0)
            item->setForeground(4, QColor(0xd0, 0x20, 0x20));
    }
    item->setData(0, kHierRole, qs(inst.hierPath));
    setLocation(item, inst.location);
    for (const auto& child : inst.children)
        addInstance(item, tree, child, counts, cdc);
}

QString severityText(Severity s) {
    switch (s) {
        case Severity::Info: return QObject::tr("Info");
        case Severity::Warning: return QObject::tr("Warning");
        case Severity::Error: return QObject::tr("Error");
        case Severity::Fatal: return QObject::tr("Fatal");
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      controller_(new AnalysisController(this)),
      sourceViewer_(new SourceViewer(this)),
      projectTree_(new QTreeWidget(this)),
      hierarchyTree_(new QTreeWidget(this)),
      messageTable_(new QTableWidget(this)),
      reportView_(new QTextBrowser(this)),
      statusLabel_(new QLabel(this)),
      progressBar_(new QProgressBar(this)) {
    setWindowTitle(tr("OpenCDC Analyzer"));
    resize(1400, 900);
    setCentralWidget(sourceViewer_);

    createActions();
    createDocks();

    statusBar()->addWidget(statusLabel_, 1);
    progressBar_->setRange(0, 100);
    progressBar_->setMaximumWidth(220);
    progressBar_->setVisible(false);
    statusBar()->addPermanentWidget(progressBar_);
    statusLabel_->setText(tr("No project loaded"));

    connect(controller_, &AnalysisController::started, this, &MainWindow::onAnalysisStarted);
    connect(controller_, &AnalysisController::progress, this, &MainWindow::onAnalysisProgress);
    connect(controller_, &AnalysisController::finished, this, &MainWindow::onAnalysisFinished);
    connect(sourceViewer_, &SourceViewer::fileChanged, this, [this](const QString& file) {
        setWindowTitle(tr("OpenCDC Analyzer - %1").arg(QFileInfo(file).fileName()));
    });
    updateActions();
}

void MainWindow::createActions() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    openAction_ = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton),
                                      tr("&Open Project..."), this, &MainWindow::onOpenProject);
    openAction_->setShortcut(QKeySequence::Open);
    fileMenu->addSeparator();
    exportHtmlAction_ = fileMenu->addAction(tr("Export &HTML Report..."), this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export HTML report"),
                                                          QStringLiteral("cdc-report.html"), tr("HTML files (*.html)"));
        if (!file.isEmpty() && !exportHtmlReport(file))
            QMessageBox::warning(this, tr("Export"), tr("Could not write %1").arg(file));
    });
    exportCsvAction_ = fileMenu->addAction(tr("Export &CSV..."), this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Directory for paths.csv and violations.csv"));
        if (!dir.isEmpty() && !exportCsv(dir))
            QMessageBox::warning(this, tr("Export"), tr("Could not write CSV files into %1").arg(dir));
    });
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    auto* analysisMenu = menuBar()->addMenu(tr("&Analysis"));
    runAction_ = analysisMenu->addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("&Run"),
                                         this, &MainWindow::runAnalysis);
    runAction_->setShortcut(Qt::Key_F5);
    cancelAction_ = analysisMenu->addAction(style()->standardIcon(QStyle::SP_MediaStop), tr("&Cancel"),
                                            controller_, &AnalysisController::cancel);

    viewMenu_ = menuBar()->addMenu(tr("&View"));

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About OpenCDC"), this, &MainWindow::onAbout);

    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName("mainToolbar");
    toolbar->addAction(openAction_);
    toolbar->addAction(runAction_);
    toolbar->addAction(cancelAction_);
}

QDockWidget* MainWindow::addDock(const QString& title, const QString& objectName, QWidget* widget,
                                 Qt::DockWidgetArea area) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setWidget(widget);
    addDockWidget(area, dock);
    viewMenu_->addAction(dock->toggleViewAction());
    return dock;
}

void MainWindow::createDocks() {
    // Project Explorer: files and modules.
    projectTree_->setHeaderLabels({tr("Item"), tr("Details")});
    projectTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    auto* projectDock = addDock(tr("Project Explorer"), "projectExplorer", projectTree_, Qt::LeftDockWidgetArea);

    // Design Hierarchy: elaborated instance tree.
    hierarchyTree_->setHeaderLabels({tr("Instance"), tr("Module"), tr("Registers"), tr("CDC paths"), tr("Issues")});
    hierarchyTree_->headerItem()->setToolTip(3, tr("CDC paths whose destination register is in this instance subtree"));
    hierarchyTree_->headerItem()->setToolTip(4, tr("Of those, paths with status violation or potential risk"));
    hierarchyTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    hierarchyTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    auto* hierDock = addDock(tr("Design Hierarchy"), "designHierarchy", hierarchyTree_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(projectDock, hierDock);
    projectDock->raise();
    hierarchyTree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    resizeDocks({projectDock}, {420}, Qt::Horizontal);

    connect(projectTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { jumpToItemLocation(item); });
    connect(hierarchyTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { jumpToItemLocation(item); });
    connect(hierarchyTree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = hierarchyTree_->itemAt(pos);
        if (!item || !lastResult_ || !lastResult_->cdc)
            return;
        QMenu menu(this);
        menu.addAction(tr("Show CDC paths in this instance"), this, [this, item] { showInstancePaths(item); });
        menu.addAction(tr("Go to source"), this, [this, item] { jumpToItemLocation(item); });
        menu.exec(hierarchyTree_->viewport()->mapToGlobal(pos));
    });

    clockView_ = new ClockView(this);
    connect(clockView_, &ClockView::locationActivated, this,
            [this](const QString& file, int line) { sourceViewer_->showLocation(file, line); });
    auto* clockDock = addDock(tr("Clock Domains"), "clockDomains", clockView_, Qt::RightDockWidgetArea);
    schematicView_ = new SchematicView(this);
    connect(schematicView_, &SchematicView::locationActivated, this,
            [this](const QString& file, int line) { sourceViewer_->showLocation(file, line); });
    auto* schematicDock = addDock(tr("CDC Schematic"), "cdcSchematic", schematicView_, Qt::RightDockWidgetArea);
    schematicDock_ = schematicDock;
    tabifyDockWidget(clockDock, schematicDock);
    clockDock->raise();
    resizeDocks({clockDock}, {460}, Qt::Horizontal);

    // Messages: diagnostics with double-click jump to source.
    messageTable_->setColumnCount(4);
    messageTable_->setHorizontalHeaderLabels({tr("Severity"), tr("Code"), tr("Message"), tr("Location")});
    messageTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    messageTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    messageTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    messageTable_->verticalHeader()->setVisible(false);
    connect(messageTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = messageTable_->item(row, 0);
        if (!item)
            return;
        const QString file = item->data(kFileRole).toString();
        if (!file.isEmpty())
            sourceViewer_->showLocation(file, item->data(kLineRole).toInt());
    });
    auto* msgDock = addDock(tr("Messages"), "messages", messageTable_, Qt::BottomDockWidgetArea);

    pathView_ = new CdcPathView(this);
    violationView_ = new ViolationView(this);
    auto jump = [this](const QString& file, int line) { sourceViewer_->showLocation(file, line); };
    connect(pathView_, &CdcPathView::locationActivated, this, jump);
    connect(violationView_, &ViolationView::locationActivated, this, jump);
    auto* pathDock = addDock(tr("CDC Paths"), "cdcPaths", pathView_, Qt::BottomDockWidgetArea);
    auto* violationDock = addDock(tr("Rule Violations"), "ruleViolations", violationView_, Qt::BottomDockWidgetArea);
    pathDock_ = pathDock;
    violationDock_ = violationDock;
    connect(violationView_, &ViolationView::pathActivated, this, [this](std::size_t id) {
        pathDock_->raise();
        pathView_->showPath(id);
    });
    // The schematic follows the selected path (from the path table, a
    // violation, a structure link or a hierarchy filter).
    connect(pathView_, &CdcPathView::pathSelected, schematicView_, &SchematicView::showPath);
    reportView_->setOpenLinks(false);
    auto* reportDock = addDock(tr("Report"), "report", reportView_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(msgDock, pathDock);
    tabifyDockWidget(pathDock, violationDock);
    tabifyDockWidget(violationDock, reportDock);
    msgDock->raise();
}

bool MainWindow::exportHtmlReport(const QString& file) const {
    if (!lastResult_)
        return false;
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const std::string html = engine::toHtmlReport(*lastResult_);
    return out.write(html.data(), static_cast<qint64>(html.size())) == static_cast<qint64>(html.size());
}

bool MainWindow::exportCsv(const QString& dir) const {
    if (!lastResult_)
        return false;
    for (const auto& [name, text] : {std::pair{QStringLiteral("paths.csv"), engine::pathsCsv(*lastResult_)},
                                      std::pair{QStringLiteral("violations.csv"), engine::violationsCsv(*lastResult_)}}) {
        QFile out(QDir(dir).filePath(name));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size()))
            return false;
    }
    return true;
}

bool MainWindow::showInstancePaths(const QTreeWidgetItem* item) {
    if (!item || !lastResult_ || !lastResult_->cdc)
        return false;
    const QString hier = item->data(0, kHierRole).toString();
    if (hier.isEmpty())
        return false;
    pathDock_->raise();
    pathView_->setTextFilter(hier + QLatin1Char('.'));
    return true;
}

bool MainWindow::openProject(const QString& path) {
    auto loaded = engine::loadProjectFile(fromUtf8(path.toStdString()));
    messageTable_->setRowCount(0);
    if (!loaded) {
        appendMessage(loaded.error());
        statusLabel_->setText(tr("Failed to load project: %1").arg(path));
        return false;
    }
    project_ = std::move(loaded).value();
    lastResult_.reset();
    hierarchyTree_->clear();
    reportView_->clear();
    clockView_->clear();
    pathView_->clear();
    violationView_->clear();
    schematicView_->clear();
    showProjectInfo();
    statusLabel_->setText(tr("Project '%1' loaded").arg(qs(project_->name)));
    updateActions();
    return true;
}

void MainWindow::showProjectInfo() {
    projectTree_->clear();
    if (!project_)
        return;
    auto* root = new QTreeWidgetItem(projectTree_, {qs(project_->name), qs(project_->projectFile)});
    root->setExpanded(true);

    auto* sources = new QTreeWidgetItem(root, {tr("Sources"), QString::number(project_->sources.size())});
    for (const auto& s : project_->sources) {
        auto* item = new QTreeWidgetItem(sources, {qs(s.filename()), qs(s)});
        setLocation(item, {toUtf8(s), 1, 1});
    }
    sources->setExpanded(true);

    if (!project_->includeDirs.empty()) {
        auto* incs = new QTreeWidgetItem(root, {tr("Include directories"), QString()});
        for (const auto& d : project_->includeDirs)
            new QTreeWidgetItem(incs, {qs(d.filename()), qs(d)});
    }
    if (!project_->constraints.empty()) {
        auto* cons = new QTreeWidgetItem(root, {tr("Constraints"), QString::number(project_->constraints.size())});
        for (const auto& c : project_->constraints) {
            auto* item = new QTreeWidgetItem(cons, {qs(c.filename()), qs(c)});
            setLocation(item, {toUtf8(c), 1, 1});
        }
    }
}

void MainWindow::runAnalysis() {
    if (!project_ || controller_->isRunning())
        return;
    controller_->start(*project_);
}

void MainWindow::onOpenProject() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open OpenCDC Project"), QString(),
                                                      tr("OpenCDC projects (*.ocdc.json *.json)"));
    if (!path.isEmpty() && openProject(path))
        runAnalysis();
}

void MainWindow::onAnalysisStarted() {
    statusLabel_->setText(tr("Analyzing..."));
    progressBar_->setValue(0);
    progressBar_->setVisible(true);
    updateActions();
}

void MainWindow::onAnalysisProgress(const QString& stage, double fraction) {
    statusLabel_->setText(stage);
    progressBar_->setValue(static_cast<int>(fraction * 100.0));
}

void MainWindow::onAnalysisFinished(AnalysisController::ResultPtr result) {
    progressBar_->setVisible(false);
    lastResult_ = std::move(result);
    if (lastResult_)
        populateResult(*lastResult_);
    updateActions();
}

void MainWindow::populateResult(const engine::AnalysisResult& result) {
    messageTable_->setRowCount(0);
    for (const auto& d : result.diagnostics)
        appendMessage(d);
    messageTable_->resizeColumnToContents(0);
    messageTable_->resizeColumnToContents(1);

    hierarchyTree_->clear();
    showProjectInfo();
    clockView_->setResult(result);
    schematicView_->setResult(lastResult_);
    pathView_->setResult(lastResult_);
    violationView_->setResult(lastResult_);
    if (result.elaboration) {
        const auto& e = *result.elaboration;
        const auto counts = registerCounts(result.design.get());
        const auto cdc = cdcCounts(result.cdc.get());
        for (const auto& top : e.topInstances)
            addInstance(nullptr, hierarchyTree_, top, counts, cdc);
        hierarchyTree_->expandToDepth(1);

        auto* root = projectTree_->topLevelItem(0);
        auto* modules = new QTreeWidgetItem(root, {tr("Modules"), QString::number(e.definitions.size())});
        for (const auto& d : e.definitions) {
            auto* item = new QTreeWidgetItem(modules, {qs(d.name), qs(d.kind)});
            setLocation(item, d.location);
        }
        modules->setExpanded(true);

        // Files pulled in through `include are listed separately.
        std::set<std::string> sources;
        for (const auto& s : result.project.sources)
            sources.insert(toUtf8(s.lexically_normal()));
        QList<QTreeWidgetItem*> includes;
        for (const auto& f : e.parsedFiles) {
            if (!sources.contains(toUtf8(f.lexically_normal()))) {
                auto* item = new QTreeWidgetItem({qs(f.filename()), qs(f)});
                setLocation(item, {toUtf8(f), 1, 1});
                includes.append(item);
            }
        }
        if (!includes.isEmpty()) {
            auto* inc = new QTreeWidgetItem(root, {tr("Included files"), QString::number(includes.size())});
            inc->addChildren(includes);
        }
    }

    // Report view: run summary (File > Export HTML Report writes the full report).
    QString html = tr("<h3>Analysis run</h3><p>Project <b>%1</b>, started %2 (UTC), %3 s</p>")
                       .arg(qs(result.project.name).toHtmlEscaped(), qs(result.startedAtUtc))
                       .arg(result.elapsedSeconds, 0, 'f', 2);
    html += "<table border='1' cellspacing='0' cellpadding='3'><tr><th>" + tr("Stage") + "</th><th>" +
            tr("Status") + "</th><th>" + tr("Detail") + "</th></tr>";
    for (const auto& s : result.stages) {
        html += "<tr><td>" + qs(s.name).toHtmlEscaped() + "</td><td>" + qs(std::string(engine::toString(s.status))) +
                "</td><td>" + qs(s.detail).toHtmlEscaped() + "</td></tr>";
    }
    html += "</table>";
    if (result.design) {
        const auto st = result.design->stats();
        html += tr("<h3>RTL extraction</h3><p>%1 module bodies, %2 instances, %3 registers (%4 bits), "
                   "%5 latches, %6 combinational cells.</p>")
                    .arg(st.uniqueBodies)
                    .arg(st.instances)
                    .arg(st.registers)
                    .arg(st.registerBits)
                    .arg(st.latches)
                    .arg(st.cells);
        if (result.design->unsupported.empty()) {
            html += "<p>" + tr("All constructs were modelled; nothing was left unanalyzed.") + "</p>";
        }
        else {
            html += "<p><b>" + tr("Not analyzed / approximated (%1):").arg(result.design->unsupported.size()) +
                    "</b></p><table border='1' cellspacing='0' cellpadding='3'><tr><th>" + tr("Code") + "</th><th>" +
                    tr("Module") + "</th><th>" + tr("What") + "</th><th>" + tr("Location") + "</th></tr>";
            for (const auto& u : result.design->unsupported) {
                html += "<tr><td>" + qs(u.code).toHtmlEscaped() + "</td><td>" + qs(u.module).toHtmlEscaped() +
                        "</td><td>" + qs(u.what).toHtmlEscaped() + "</td><td>" + qs(toString(u.loc)).toHtmlEscaped() +
                        "</td></tr>";
            }
            html += "</table>";
        }
    }
    if (result.cdc) {
        const auto& st = result.cdc->stats;
        html += tr("<h3>CDC analysis</h3><p>%1 crossing path(s): %2 synchronized, %3 unsynchronized; %4 "
                   "synchronizer structure(s), %5 reset synchronizer(s), %6 reconvergence point(s); %7 path(s) "
                   "between clocks with an unknown relation.</p>")
                    .arg(st.paths)
                    .arg(st.synchronized)
                    .arg(st.unsynchronized)
                    .arg(result.cdc->structures.size())
                    .arg(st.resetSynchronizers)
                    .arg(st.reconvergencePoints)
                    .arg(st.unknownRelation);
        html += "<p>" + tr("Paths by status: %1 violation, %2 potential risk, %3 needs review, %4 safe.")
                            .arg(result.pathsWithStatus(cdc::AnalysisStatus::Violation))
                            .arg(result.pathsWithStatus(cdc::AnalysisStatus::PotentialRisk))
                            .arg(result.pathsWithStatus(cdc::AnalysisStatus::NeedsReview))
                            .arg(result.pathsWithStatus(cdc::AnalysisStatus::Safe)) +
                "</p>";
        html += "<table border='1' cellspacing='0' cellpadding='3'><tr><th>" + tr("Rule") + "</th><th>" +
                tr("Hits") + "</th></tr>";
        for (const auto& [id, n] : rules::countHits(*result.cdc))
            html += "<tr><td>" + qs(id) + "</td><td>" + QString::number(n) + "</td></tr>";
        html += "</table>";
        const auto& structures = result.cdc->structures;
        constexpr std::size_t kMaxListedStructures = 300; // QTextEdit tables get slow beyond this
        if (!structures.empty() && structures.size() > kMaxListedStructures) {
            std::map<std::string, std::size_t> byType;
            for (const auto& sv : structures)
                ++byType[std::string(cdc::toString(sv.type))];
            html += "<p><b>" + tr("Synchronizer structures") + "</b> (" +
                    tr("%1 in total; the path table lists them per path").arg(structures.size()) +
                    ")</p><table border='1' cellspacing='0' cellpadding='3'><tr><th>" + tr("Type") + "</th><th>" +
                    tr("Count") + "</th></tr>";
            for (const auto& [type, n] : byType)
                html += "<tr><td>" + qs(type) + "</td><td>" + QString::number(n) + "</td></tr>";
            html += "</table>";
        }
        else if (!structures.empty()) {
            html += "<p><b>" + tr("Synchronizer structures") + "</b></p><table border='1' cellspacing='0' "
                    "cellpadding='3'><tr><th>#</th><th>" + tr("Type") + "</th><th>" + tr("Name") + "</th><th>" +
                    tr("Clocks") + "</th><th>" + tr("Status") + "</th><th>" + tr("Paths") + "</th></tr>";
            for (const auto& sv : structures) {
                QStringList ids;
                for (const auto id : sv.paths)
                    ids << QStringLiteral("#%1").arg(id);
                html += "<tr><td>S" + QString::number(sv.id) + "</td><td>" + qs(cdc::toString(sv.type)) + "</td><td>" +
                        qs(sv.name).toHtmlEscaped() + "</td><td>" + qs(sv.srcClock).toHtmlEscaped() + " &rarr; " +
                        qs(sv.dstClock).toHtmlEscaped() + "</td><td>" + qs(cdc::toString(sv.status)) + "</td><td>" +
                        ids.join(QLatin1Char(' ')) + "</td></tr>";
            }
            html += "</table>";
        }
        html += "<p>" +
                tr("Not checked: %1 source(s) without a clock domain (inputs need set_input_delay -clock).")
                    .arg(st.excludedNoDomainSources) +
                "</p>";
    }
    html += "<p><i>" + tr("The full report with path details and schematics: File > Export HTML Report; tables: "
                          "File > Export CSV.") +
            "</i></p>";
    reportView_->setHtml(html);

    const auto errors = result.count(Severity::Error) + result.count(Severity::Fatal);
    const auto violations = result.violations(Severity::Error) + result.violations(Severity::Warning);
    statusLabel_->setText(tr("Analysis finished: %1 error(s), %2 warning(s); %3 CDC path(s), %4 rule violation(s), "
                             "%5 potential risk(s), %6 need review")
                              .arg(errors)
                              .arg(result.count(Severity::Warning))
                              .arg(result.cdc ? result.cdc->paths.size() : 0)
                              .arg(violations)
                              .arg(result.pathsWithStatus(cdc::AnalysisStatus::PotentialRisk))
                              .arg(result.pathsWithStatus(cdc::AnalysisStatus::NeedsReview)));
    if (violations > 0)
        violationDock_->raise();
    else if (result.cdc && !result.cdc->paths.empty())
        pathDock_->raise();
    if (result.cdc && !result.cdc->paths.empty())
        schematicDock_->raise();
}

void MainWindow::appendMessage(const Diagnostic& diag) {
    const int row = messageTable_->rowCount();
    messageTable_->insertRow(row);
    auto* sev = new QTableWidgetItem(severityText(diag.severity));
    if (!diag.location.file.empty()) {
        sev->setData(kFileRole, qs(diag.location.file));
        sev->setData(kLineRole, static_cast<int>(diag.location.line));
    }
    messageTable_->setItem(row, 0, sev);
    messageTable_->setItem(row, 1, new QTableWidgetItem(qs(diag.code)));
    QString message = qs(diag.message);
    for (const auto& note : diag.notes)
        message += "\n  note: " + qs(note);
    auto* msgItem = new QTableWidgetItem(message);
    msgItem->setToolTip(message);
    messageTable_->setItem(row, 2, msgItem);
    messageTable_->setItem(row, 3, new QTableWidgetItem(qs(toString(diag.location))));
}

bool MainWindow::jumpToItemLocation(const QTreeWidgetItem* item) {
    if (!item)
        return false;
    const QString file = item->data(0, kFileRole).toString();
    if (file.isEmpty())
        return false;
    return sourceViewer_->showLocation(file, item->data(0, kLineRole).toInt());
}

void MainWindow::updateActions() {
    const bool running = controller_->isRunning();
    openAction_->setEnabled(!running);
    runAction_->setEnabled(project_.has_value() && !running);
    cancelAction_->setEnabled(running);
    exportHtmlAction_->setEnabled(lastResult_ != nullptr && !running);
    exportCsvAction_->setEnabled(lastResult_ != nullptr && !running);
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About OpenCDC Analyzer"),
                       tr("<h3>OpenCDC Analyzer %1</h3>"
                          "<p>FPGA clock domain crossing static analysis.</p>"
                          "<p>Implemented: project loading, Verilog parsing and elaboration (slang), "
                          "RTL extraction (registers, resets, enables, latches), "
                          "SDC/XDC clock constraints, clock domain analysis, CDC paths, flop-chain, qualified-data, "
                          "toggle, Gray, handshake, asynchronous FIFO and reset synchronizers, reconvergence, "
                          "rules CDC-001..CDC-010. "
                          "CDC schematic with SVG/PNG export, waivers, HTML/CSV reports.</p>"
                          "<p>Uses Qt %2 (LGPLv3) and slang (MIT).</p>")
                           .arg(QString::fromLatin1(kVersionString), QString::fromLatin1(qVersion())));
}

} // namespace opencdc::gui

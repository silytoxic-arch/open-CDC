#pragma once

#include "engine/ProjectConfig.h"
#include "gui/AnalysisController.h"

#include <QMainWindow>
#include <QString>

#include <optional>

class QAction;
class QDockWidget;
class QLabel;
class QProgressBar;
class QTableWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace opencdc::gui {

class CdcPathView;
class ClockView;
class SchematicView;
class ViolationView;
class SourceViewer;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    /// Loads a project file; shows errors in the message view. Returns success.
    bool openProject(const QString& path);
    void runAnalysis();

    // Accessors used by the headless self-test (--smoke-test).
    [[nodiscard]] QTreeWidget* projectTree() const { return projectTree_; }
    [[nodiscard]] QTreeWidget* hierarchyTree() const { return hierarchyTree_; }
    [[nodiscard]] QTableWidget* messageTable() const { return messageTable_; }
    [[nodiscard]] SourceViewer* sourceViewer() const { return sourceViewer_; }
    [[nodiscard]] ClockView* clockView() const { return clockView_; }
    [[nodiscard]] CdcPathView* pathView() const { return pathView_; }
    [[nodiscard]] ViolationView* violationView() const { return violationView_; }
    [[nodiscard]] SchematicView* schematicView() const { return schematicView_; }
    [[nodiscard]] AnalysisController* controller() const { return controller_; }
    [[nodiscard]] const AnalysisController::ResultPtr& lastResult() const { return lastResult_; }

    /// Jumps the source viewer to the location stored on an item (if any).
    bool jumpToItemLocation(const QTreeWidgetItem* item);
    /// Filters the CDC path table to paths whose registers are inside the
    /// hierarchy-tree instance `item`.
    bool showInstancePaths(const QTreeWidgetItem* item);
    /// Writes the HTML report / the CSV tables of the last result.
    bool exportHtmlReport(const QString& file) const;
    bool exportCsv(const QString& dir) const;

private slots:
    void onOpenProject();
    void onAnalysisStarted();
    void onAnalysisProgress(const QString& stage, double fraction);
    void onAnalysisFinished(opencdc::gui::AnalysisController::ResultPtr result);
    void onAbout();

private:
    void createActions();
    void createDocks();
    QDockWidget* addDock(const QString& title, const QString& objectName, QWidget* widget,
                         Qt::DockWidgetArea area);
    void showProjectInfo();
    void populateResult(const engine::AnalysisResult& result);
    void appendMessage(const Diagnostic& diag);
    void updateActions();

    std::optional<engine::ProjectConfig> project_;
    AnalysisController* controller_;
    AnalysisController::ResultPtr lastResult_;

    SourceViewer* sourceViewer_;
    QTreeWidget* projectTree_;
    QTreeWidget* hierarchyTree_;
    QTableWidget* messageTable_;
    QTextBrowser* reportView_;
    ClockView* clockView_ = nullptr;
    CdcPathView* pathView_ = nullptr;
    ViolationView* violationView_ = nullptr;
    SchematicView* schematicView_ = nullptr;
    QDockWidget* schematicDock_ = nullptr;
    QDockWidget* pathDock_ = nullptr;
    QDockWidget* violationDock_ = nullptr;
    QLabel* statusLabel_;
    QProgressBar* progressBar_;
    QMenu* viewMenu_ = nullptr;

    QAction* openAction_ = nullptr;
    QAction* runAction_ = nullptr;
    QAction* cancelAction_ = nullptr;
    QAction* exportHtmlAction_ = nullptr;
    QAction* exportCsvAction_ = nullptr;
};

} // namespace opencdc::gui

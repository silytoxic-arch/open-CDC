#pragma once

#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <memory>

namespace opencdc::gui {

/// Runs the analysis engine on a worker thread and reports back on the GUI
/// thread through Qt signals. The engine itself knows nothing about Qt.
class AnalysisController final : public QObject {
    Q_OBJECT

public:
    using ResultPtr = std::shared_ptr<const engine::AnalysisResult>;

    explicit AnalysisController(QObject* parent = nullptr);
    ~AnalysisController() override;

    [[nodiscard]] bool isRunning() const;

public slots:
    void start(const opencdc::engine::ProjectConfig& config);
    void cancel();

signals:
    void started();
    void progress(const QString& stage, double fraction);
    void finished(opencdc::gui::AnalysisController::ResultPtr result);

private:
    QFutureWatcher<ResultPtr> watcher_;
    std::shared_ptr<CancellationToken> cancel_;
};

} // namespace opencdc::gui

Q_DECLARE_METATYPE(opencdc::gui::AnalysisController::ResultPtr)

#include "gui/AnalysisController.h"

#include <QMetaObject>
#include <QtConcurrent/QtConcurrentRun>

namespace opencdc::gui {

namespace {

/// Forwards engine progress (worker thread) to the controller (GUI thread).
class QueuedProgressSink final : public ProgressSink {
public:
    explicit QueuedProgressSink(AnalysisController* target) : target_(target) {}

    void update(std::string_view stage, double fraction) override {
        // The controller outlives every worker (its destructor waits), and the
        // queued call is dropped by Qt if the context object is gone.
        auto* target = target_;
        const QString text = QString::fromUtf8(stage.data(), static_cast<qsizetype>(stage.size()));
        QMetaObject::invokeMethod(
            target, [target, text, fraction] { emit target->progress(text, fraction); },
            Qt::QueuedConnection);
    }

private:
    AnalysisController* target_;
};

} // namespace

AnalysisController::AnalysisController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ResultPtr>();
    connect(&watcher_, &QFutureWatcher<ResultPtr>::finished, this, [this] {
        ResultPtr result = watcher_.result();
        cancel_.reset();
        emit finished(std::move(result));
    });
}

AnalysisController::~AnalysisController() {
    // Never leave a worker running against a destroyed controller.
    if (watcher_.isRunning()) {
        if (cancel_)
            cancel_->cancel();
        watcher_.waitForFinished();
    }
}

bool AnalysisController::isRunning() const {
    return watcher_.isRunning();
}

void AnalysisController::start(const engine::ProjectConfig& config) {
    if (watcher_.isRunning())
        return;
    cancel_ = std::make_shared<CancellationToken>();
    auto token = cancel_;
    emit started();
    watcher_.setFuture(QtConcurrent::run([this, config, token]() -> ResultPtr {
        QueuedProgressSink sink(this);
        engine::AnalysisSession session;
        return session.run(config, sink, *token);
    }));
}

void AnalysisController::cancel() {
    if (cancel_)
        cancel_->cancel();
}

} // namespace opencdc::gui

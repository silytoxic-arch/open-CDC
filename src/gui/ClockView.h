#pragma once

#include "engine/AnalysisSession.h"

#include <QWidget>

class QLabel;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace opencdc::gui {

/// Clock Domain Viewer: clocks with their registers, unresolved clock pins,
/// and the clock relation matrix (with the reason for every relation).
class ClockView final : public QWidget {
    Q_OBJECT

public:
    explicit ClockView(QWidget* parent = nullptr);

    void clear();
    void setResult(const engine::AnalysisResult& result);

    [[nodiscard]] QTreeWidget* tree() const { return tree_; }
    [[nodiscard]] QTableWidget* matrix() const { return matrix_; }

signals:
    /// Emitted on double-click of an item that has a source location.
    void locationActivated(const QString& file, int line);

private:
    QLabel* summary_;
    QTreeWidget* tree_;
    QTableWidget* matrix_;
};

} // namespace opencdc::gui

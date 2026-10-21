#pragma once

#include "engine/AnalysisSession.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTextBrowser;

namespace opencdc::gui {

namespace detail {
class PathModel;
class PathFilter;
class HitModel;
class HitFilter;
} // namespace detail

/// CDC Path Viewer: every crossing path with its clocks, registers,
/// synchronizer verdict and status; the details pane shows the full
/// hierarchical path (source clock -> crossing logic -> destination clock),
/// the checked synchronizer conditions, unverified protocol conditions,
/// rule hits and fix suggestions. The table is a model/view pair (no item
/// per cell), so that 10^5..10^6 paths stay responsive.
class CdcPathView final : public QWidget {
    Q_OBJECT

public:
    explicit CdcPathView(QWidget* parent = nullptr);

    void clear();
    void setResult(std::shared_ptr<const engine::AnalysisResult> result);
    /// Selects the row of path `id` (if visible) and shows its details.
    void showPath(std::size_t id);

    [[nodiscard]] QTableView* table() const { return table_; }
    [[nodiscard]] QTextBrowser* details() const { return details_; }
    /// Paths in the table (all of them, filtered or not).
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] QComboBox* syncFilter() const { return syncFilter_; }

    /// Sets the text filter (matches source, destination or crossing signal).
    void setTextFilter(const QString& text);
    [[nodiscard]] int visibleRows() const;

    /// Path whose details are shown (none before the first selection).
    [[nodiscard]] std::optional<std::size_t> currentPath() const { return current_; }

signals:
    void locationActivated(const QString& file, int line);
    /// The details pane now shows path `pathId` (selection or showPath()).
    void pathSelected(std::size_t pathId);

private:
    void applyFilter();
    void showDetails(std::size_t id);

    std::optional<std::size_t> current_;

    std::shared_ptr<const engine::AnalysisResult> result_;
    QComboBox* clockFilter_;
    QComboBox* statusFilter_;
    QComboBox* syncFilter_;
    QLineEdit* textFilter_;
    QLabel* countLabel_;
    detail::PathModel* model_;
    detail::PathFilter* filter_;
    QTableView* table_;
    QTextBrowser* details_;
};

/// Rule Violation Viewer: one row per rule hit, filterable by rule, severity,
/// status and clock; shows the rule's purpose and fix suggestion.
class ViolationView final : public QWidget {
    Q_OBJECT

public:
    explicit ViolationView(QWidget* parent = nullptr);

    void clear();
    void setResult(std::shared_ptr<const engine::AnalysisResult> result);

    [[nodiscard]] QTableView* table() const { return table_; }
    /// Rule hits in the table (all of them, filtered or not).
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] int visibleRows() const;
    [[nodiscard]] QComboBox* ruleFilter() const { return ruleFilter_; }
    [[nodiscard]] QComboBox* severityFilter() const { return severityFilter_; }
    [[nodiscard]] QComboBox* waiverFilter() const { return waiverFilter_; }

signals:
    void locationActivated(const QString& file, int line);
    void pathActivated(std::size_t pathId);

private:
    void applyFilter();
    void showRule(std::size_t pathId, std::size_t hit);

    std::shared_ptr<const engine::AnalysisResult> result_;
    QComboBox* ruleFilter_;
    QComboBox* severityFilter_;
    QComboBox* statusFilter_;
    QComboBox* clockFilter_;
    QComboBox* waiverFilter_;
    QLabel* countLabel_;
    detail::HitModel* model_;
    detail::HitFilter* filter_;
    QTableView* table_;
    QTextBrowser* ruleInfo_;
};

} // namespace opencdc::gui

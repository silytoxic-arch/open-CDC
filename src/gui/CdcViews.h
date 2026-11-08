#pragma once

#include "engine/AnalysisSession.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTableWidget;
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
    [[nodiscard]] QComboBox* exceptionFilter() const { return exceptionFilter_; }

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
    QComboBox* exceptionFilter_;
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
    [[nodiscard]] QComboBox* exceptionFilter() const { return exceptionFilter_; }
    /// The selected rule hit (path id, hit index), if any.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> currentHit() const;
    /// "Waive...": enabled for a selected hit that no waiver covers.
    [[nodiscard]] QPushButton* waiveButton() const { return waiveButton_; }
    /// "Waive by timing exception...": enabled for such a hit whose crossing a
    /// timing exception of the constraints covers.
    [[nodiscard]] QPushButton* waiveExceptionButton() const { return waiveExceptionButton_; }

signals:
    void locationActivated(const QString& file, int line);
    void pathActivated(std::size_t pathId);
    /// The user asked to waive hit `hit` of path `pathId`.
    void waiveRequested(std::size_t pathId, std::size_t hit);
    /// The user asked to waive hit `hit`'s rule on every crossing the timing
    /// exception of path `pathId` covers.
    void waiveExceptionRequested(std::size_t pathId, std::size_t hit);

private:
    void applyFilter();
    void showRule(std::size_t pathId, std::size_t hit);

    std::shared_ptr<const engine::AnalysisResult> result_;
    QComboBox* ruleFilter_;
    QComboBox* severityFilter_;
    QComboBox* statusFilter_;
    QComboBox* clockFilter_;
    QComboBox* waiverFilter_;
    QComboBox* exceptionFilter_;
    QPushButton* waiveButton_;
    QPushButton* waiveExceptionButton_;
    QLabel* countLabel_;
    detail::HitModel* model_;
    detail::HitFilter* filter_;
    QTableView* table_;
    QTextBrowser* ruleInfo_;
};

/// Output Ports: outputs driven asynchronously to the clock their
/// set_output_delay names (rule CDC-OUTPUT-CLOCK), one row per constraint and
/// driver clock, waived ones with their waiver; double-click opens the
/// set_output_delay command.
class OutputPortView final : public QWidget {
    Q_OBJECT

public:
    explicit OutputPortView(QWidget* parent = nullptr);

    void clear();
    void setResult(std::shared_ptr<const engine::AnalysisResult> result);

    [[nodiscard]] QTableWidget* table() const { return table_; }
    [[nodiscard]] QComboBox* waiverFilter() const { return waiverFilter_; }
    /// Records in the table (all of them, filtered or not).
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] int visibleRows() const;
    [[nodiscard]] QTextBrowser* details() const { return details_; }
    /// Index (in CdcResult::outputs) of the selected record, if any.
    [[nodiscard]] std::optional<std::size_t> currentOutput() const;
    /// "Waive...": enabled for a selected record that no waiver covers.
    [[nodiscard]] QPushButton* waiveButton() const { return waiveButton_; }

signals:
    void locationActivated(const QString& file, int line);
    /// The user asked to waive output crossing `index` (CdcResult::outputs).
    void waiveRequested(std::size_t index);

private:
    void applyFilter();
    void showDetails(int row);

    std::shared_ptr<const engine::AnalysisResult> result_;
    QComboBox* waiverFilter_;
    QPushButton* waiveButton_;
    QLabel* countLabel_;
    QTableWidget* table_;
    QTextBrowser* details_;
};

/// Waivers: every waiver of the project with its selectors, reason, use
/// count and state (active, unused, expired); Edit... and Remove change its
/// waiver file; double-click opens the waiver in its file.
class WaiverView final : public QWidget {
    Q_OBJECT

public:
    explicit WaiverView(QWidget* parent = nullptr);

    void clear();
    void setResult(std::shared_ptr<const engine::AnalysisResult> result);

    [[nodiscard]] QTableWidget* table() const { return table_; }
    [[nodiscard]] QComboBox* stateFilter() const { return stateFilter_; }
    [[nodiscard]] QPushButton* editButton() const { return editButton_; }
    [[nodiscard]] QPushButton* removeButton() const { return removeButton_; }
    [[nodiscard]] int rowCount() const;
    [[nodiscard]] int visibleRows() const;
    /// Id of the selected waiver ("" if none).
    [[nodiscard]] QString currentId() const;
    /// Selects the row of waiver `id`; false if it is not in the table.
    bool selectWaiver(const QString& id);
    /// "active", "unused", "expiring" (used, expires within the warning
    /// period) or "expired".
    [[nodiscard]] static QString stateOf(const cdc::WaiverRecord& w);

signals:
    void locationActivated(const QString& file, int line);
    void editRequested(const QString& id);
    void removeRequested(const QString& id);

private:
    void applyFilter();

    std::shared_ptr<const engine::AnalysisResult> result_;
    QComboBox* stateFilter_;
    QPushButton* editButton_;
    QPushButton* removeButton_;
    QLabel* countLabel_;
    QTableWidget* table_;
};

} // namespace opencdc::gui

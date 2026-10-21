#include "gui/CdcViews.h"

#include "rules/RuleEngine.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace opencdc::gui {

namespace {

constexpr int kIdRole = Qt::UserRole;       ///< path id
constexpr int kSortRole = Qt::UserRole + 2;  ///< numbers sort as numbers

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}
QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}
QString esc(const std::string& s) {
    return qs(s).toHtmlEscaped();
}

QColor statusColor(cdc::AnalysisStatus s) {
    switch (s) {
        case cdc::AnalysisStatus::Safe: return QColor(200, 235, 200);
        case cdc::AnalysisStatus::NeedsReview: return QColor(255, 235, 160);
        case cdc::AnalysisStatus::PotentialRisk: return QColor(255, 210, 160);
        case cdc::AnalysisStatus::Violation: return QColor(255, 180, 180);
    }
    return {};
}

QComboBox* combo(QWidget* parent, const QString& all) {
    auto* c = new QComboBox(parent);
    c->addItem(all, QString());
    c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    return c;
}

/// Plain clock name without the "(negedge)" suffix.
QString baseClock(const std::string& name) {
    const auto n = qs(name);
    const auto p = n.indexOf(QLatin1String(" ("));
    return p < 0 ? n : n.left(p);
}

QString locText(const SourceLocation& l) {
    return qs(toString(l));
}

} // namespace

// ============================================================================
// Table models (one row per path / per rule hit; nothing is copied)
// ============================================================================

namespace detail {

/// Re-runs a proxy's row filter (the API changed in Qt 6.10).
template <typename Proxy>
void refilter(Proxy* proxy) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    proxy->beginFilterChange();
    proxy->endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    proxy->invalidateRowsFilter();
#endif
}

class PathModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;

    void setResult(std::shared_ptr<const engine::AnalysisResult> r) {
        beginResetModel();
        result_ = std::move(r);
        endResetModel();
    }
    [[nodiscard]] const cdc::CdcPath& path(int row) const { return result_->cdc->paths[static_cast<std::size_t>(row)]; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() || !result_ || !result_->cdc ? 0 : static_cast<int>(result_->cdc->paths.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 12; }

    [[nodiscard]] QVariant headerData(int section, Qt::Orientation o, int role) const override {
        static const QStringList kHeaders{CdcPathView::tr("#"),           CdcPathView::tr("Status"),
                                          CdcPathView::tr("Source clock"), CdcPathView::tr("Dest. clock"),
                                          CdcPathView::tr("Source"),       CdcPathView::tr("Destination"),
                                          CdcPathView::tr("Crossing signal"), CdcPathView::tr("Width"),
                                          CdcPathView::tr("Pin"),          CdcPathView::tr("Synchronizer"),
                                          CdcPathView::tr("Sync status"),  CdcPathView::tr("Rules")};
        if (o == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < kHeaders.size())
            return kHeaders[section];
        return {};
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rowCount())
            return {};
        const auto& p = path(index.row());
        switch (role) {
            case Qt::DisplayRole:
            case Qt::ToolTipRole: return text(p, index.column());
            case kIdRole: return static_cast<qulonglong>(p.id);
            case kSortRole:
                if (index.column() == 0)
                    return static_cast<qulonglong>(p.id);
                if (index.column() == 7)
                    return p.width;
                return text(p, index.column());
            case Qt::BackgroundRole:
                return index.column() == 1 ? QVariant(statusColor(p.status)) : QVariant();
            case Qt::ForegroundRole:
                return index.column() == 1 ? QVariant(QColor(20, 20, 20)) : QVariant();
            default: return {};
        }
    }

private:
    static QString text(const cdc::CdcPath& p, int column) {
        switch (column) {
            case 0: return QString::number(p.id);
            case 1: return qs(cdc::toString(p.status));
            case 2: return qs(p.srcClockName);
            case 3: return qs(p.dstClockName);
            case 4: return qs(p.srcName);
            case 5: return qs(p.dstName);
            case 6: return qs(p.crossingSignal);
            case 7: return QString::number(p.width);
            case 8: return qs(cdc::toString(p.pin));
            case 9: return qs(cdc::toString(p.syncType));
            case 10: return qs(cdc::toString(p.syncStatus));
            case 11: {
                QStringList rules;
                for (const auto& h : p.hits)
                    rules << qs(h.ruleId);
                return rules.join(QLatin1String(", "));
            }
            default: return {};
        }
    }

    std::shared_ptr<const engine::AnalysisResult> result_;
};

class PathFilter final : public QSortFilterProxyModel {
public:
    explicit PathFilter(PathModel* model, QObject* parent) : QSortFilterProxyModel(parent), model_(model) {
        setSourceModel(model);
        setSortRole(kSortRole);
    }
    void set(QString clock, QString status, QString sync, QString text) {
        clock_ = std::move(clock);
        status_ = std::move(status);
        sync_ = std::move(sync);
        text_ = std::move(text);
        refilter(this);
    }
    friend void refilter<PathFilter>(PathFilter*);

protected:
    [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex&) const override {
        const auto& p = model_->path(row);
        if (!clock_.isEmpty() && baseClock(p.srcClockName) != clock_ && baseClock(p.dstClockName) != clock_)
            return false;
        if (!status_.isEmpty() && qs(cdc::toString(p.status)) != status_)
            return false;
        if (!sync_.isEmpty() && qs(cdc::toString(p.syncType)) != sync_)
            return false;
        return text_.isEmpty() || qs(p.srcName).contains(text_, Qt::CaseInsensitive) ||
               qs(p.dstName).contains(text_, Qt::CaseInsensitive) ||
               qs(p.crossingSignal).contains(text_, Qt::CaseInsensitive);
    }

private:
    PathModel* model_;
    QString clock_, status_, sync_, text_;
};

class HitModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    struct Row {
        std::size_t path;
        std::size_t hit;
    };

    void setResult(std::shared_ptr<const engine::AnalysisResult> r) {
        beginResetModel();
        result_ = std::move(r);
        rows_.clear();
        if (result_ && result_->cdc)
            for (const auto& p : result_->cdc->paths)
                for (std::size_t k = 0; k < p.hits.size(); ++k)
                    rows_.push_back({p.id, k});
        endResetModel();
    }
    [[nodiscard]] const Row& row(int r) const { return rows_[static_cast<std::size_t>(r)]; }
    [[nodiscard]] const cdc::CdcPath& path(int r) const { return result_->cdc->paths[row(r).path]; }
    [[nodiscard]] const cdc::RuleHit& hit(int r) const { return path(r).hits[row(r).hit]; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 9; }

    [[nodiscard]] QVariant headerData(int section, Qt::Orientation o, int role) const override {
        static const QStringList kHeaders{ViolationView::tr("Rule"),         ViolationView::tr("Severity"),
                                          ViolationView::tr("Status"),       ViolationView::tr("Message"),
                                          ViolationView::tr("Source clock"), ViolationView::tr("Dest. clock"),
                                          ViolationView::tr("Destination"),  ViolationView::tr("Location"),
                                          ViolationView::tr("Waiver")};
        if (o == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < kHeaders.size())
            return kHeaders[section];
        return {};
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rowCount())
            return {};
        const auto& p = path(index.row());
        const auto& h = hit(index.row());
        switch (role) {
            case Qt::DisplayRole:
            case Qt::ToolTipRole:
            case kSortRole: return text(p, h, index.column());
            case kIdRole: return static_cast<qulonglong>(p.id);
            case Qt::BackgroundRole:
                return index.column() == 2 && h.waiver.empty() ? QVariant(statusColor(h.status)) : QVariant();
            case Qt::ForegroundRole:
                if (index.column() != 2)
                    return {};
                return h.waiver.empty() ? QColor(20, 20, 20) : QColor(0x70, 0x70, 0x70);
            default: return {};
        }
    }

private:
    static QString text(const cdc::CdcPath& p, const cdc::RuleHit& h, int column) {
        switch (column) {
            case 0: return qs(h.ruleId);
            case 1: return qs(toString(h.severity));
            case 2:
                return h.waiver.empty() ? qs(cdc::toString(h.status))
                                        : ViolationView::tr("waived (%1)").arg(qs(cdc::toString(h.status)));
            case 3: return qs(h.message);
            case 4: return qs(p.srcClockName);
            case 5: return qs(p.dstClockName);
            case 6: return qs(p.dstName);
            case 7: return locText(p.dstLoc);
            case 8: return qs(h.waiver);
            default: return {};
        }
    }

    std::shared_ptr<const engine::AnalysisResult> result_;
    std::vector<Row> rows_;
};

class HitFilter final : public QSortFilterProxyModel {
public:
    explicit HitFilter(HitModel* model, QObject* parent) : QSortFilterProxyModel(parent), model_(model) {
        setSourceModel(model);
        setSortRole(kSortRole);
    }
    void set(QString rule, QString severity, QString status, QString clock, QString waiver) {
        rule_ = std::move(rule);
        severity_ = std::move(severity);
        status_ = std::move(status);
        clock_ = std::move(clock);
        waiver_ = std::move(waiver);
        refilter(this);
    }
    friend void refilter<HitFilter>(HitFilter*);

protected:
    [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex&) const override {
        const auto& p = model_->path(row);
        const auto& h = model_->hit(row);
        if (waiver_ == QLatin1String("open") && !h.waiver.empty())
            return false;
        if (waiver_ == QLatin1String("waived") && h.waiver.empty())
            return false;
        return (rule_.isEmpty() || qs(h.ruleId) == rule_) &&
               (severity_.isEmpty() || qs(toString(h.severity)) == severity_) &&
               (status_.isEmpty() || qs(cdc::toString(h.status)) == status_) &&
               (clock_.isEmpty() || baseClock(p.srcClockName) == clock_ || baseClock(p.dstClockName) == clock_);
    }

private:
    HitModel* model_;
    QString rule_, severity_, status_, clock_, waiver_;
};

} // namespace detail

namespace {

void setupTable(QTableView* t) {
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(t->fontMetrics().height() + 6); // uniform rows: no per-row sizing
    t->horizontalHeader()->setResizeContentsPrecision(500);                    // sample rows when fitting columns
}

} // namespace

// ============================================================================
// CdcPathView
// ============================================================================

CdcPathView::CdcPathView(QWidget* parent)
    : QWidget(parent), clockFilter_(combo(this, tr("All clocks"))), statusFilter_(combo(this, tr("All statuses"))),
      syncFilter_(combo(this, tr("All synchronizers"))), textFilter_(new QLineEdit(this)), countLabel_(new QLabel(this)),
      model_(new detail::PathModel(this)), filter_(new detail::PathFilter(model_, this)), table_(new QTableView(this)),
      details_(new QTextBrowser(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout();
    textFilter_->setPlaceholderText(tr("Filter by register or signal name"));
    for (const auto s : {cdc::AnalysisStatus::Violation, cdc::AnalysisStatus::NeedsReview,
                         cdc::AnalysisStatus::PotentialRisk, cdc::AnalysisStatus::Safe})
        statusFilter_->addItem(qs(cdc::toString(s)), qs(cdc::toString(s)));
    for (const auto s : {cdc::SyncType::None, cdc::SyncType::SingleFlop, cdc::SyncType::TwoFlop, cdc::SyncType::ThreeFlop,
                         cdc::SyncType::MultiFlop, cdc::SyncType::QualifiedData, cdc::SyncType::Toggle,
                         cdc::SyncType::Gray, cdc::SyncType::Handshake, cdc::SyncType::AsyncFifo,
                         cdc::SyncType::ResetSync})
        syncFilter_->addItem(qs(cdc::toString(s)), qs(cdc::toString(s)));
    bar->addWidget(clockFilter_);
    bar->addWidget(statusFilter_);
    bar->addWidget(syncFilter_);
    bar->addWidget(textFilter_, 1);
    bar->addWidget(countLabel_);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal, this);
    table_->setModel(filter_);
    setupTable(table_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    split->addWidget(table_);
    details_->setOpenLinks(false);
    split->addWidget(details_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split, 1);

    connect(clockFilter_, &QComboBox::currentIndexChanged, this, &CdcPathView::applyFilter);
    connect(statusFilter_, &QComboBox::currentIndexChanged, this, &CdcPathView::applyFilter);
    connect(syncFilter_, &QComboBox::currentIndexChanged, this, &CdcPathView::applyFilter);
    connect(textFilter_, &QLineEdit::textChanged, this, &CdcPathView::applyFilter);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current) {
                if (current.isValid())
                    showDetails(current.data(kIdRole).toULongLong());
            });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!result_ || !result_->cdc || !index.isValid())
            return;
        const auto id = index.data(kIdRole).toULongLong();
        const auto& p = result_->cdc->paths.at(id);
        const int column = index.column();
        const auto& loc = (column == 2 || column == 4) ? p.srcLoc : p.dstLoc;
        if (!loc.file.empty())
            emit locationActivated(qs(loc.file), static_cast<int>(loc.line));
    });
    connect(details_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        // "loc:<line>:<file>" or "path:<id>"
        const QString s = url.toString();
        if (s.startsWith(QLatin1String("path:"))) {
            showPath(s.mid(5).toULongLong());
            return;
        }
        if (!s.startsWith(QLatin1String("loc:")))
            return;
        const auto rest = s.mid(4);
        const auto colon = rest.indexOf(QLatin1Char(':'));
        emit locationActivated(rest.mid(colon + 1), rest.left(colon).toInt());
    });
    clear();
}

void CdcPathView::clear() {
    result_.reset();
    current_.reset();
    model_->setResult(nullptr);
    details_->setHtml(tr("<p>No CDC analysis result.</p>"));
    countLabel_->clear();
    clockFilter_->blockSignals(true);
    while (clockFilter_->count() > 1)
        clockFilter_->removeItem(1);
    clockFilter_->blockSignals(false);
}

void CdcPathView::setResult(std::shared_ptr<const engine::AnalysisResult> result) {
    clear();
    result_ = std::move(result);
    if (!result_ || !result_->cdc) {
        details_->setHtml(tr("<p>CDC analysis did not run (see Messages).</p>"));
        return;
    }
    if (result_->clocks) {
        clockFilter_->blockSignals(true);
        for (const auto& c : result_->clocks->clocks)
            clockFilter_->addItem(qs(c.name), qs(c.name));
        clockFilter_->blockSignals(false);
    }
    const auto& paths = result_->cdc->paths;
    model_->setResult(result_);
    table_->resizeColumnsToContents();
    for (int c : {4, 5, 6})
        table_->setColumnWidth(c, std::min(table_->columnWidth(c), 260));
    applyFilter();
    if (!paths.empty())
        table_->setCurrentIndex(filter_->index(0, 0));
    else
        details_->setHtml(tr("<p><b>No clock domain crossings were found.</b></p>"));
}

void CdcPathView::setTextFilter(const QString& text) {
    textFilter_->setText(text);
    applyFilter();
}

int CdcPathView::rowCount() const {
    return model_->rowCount();
}

int CdcPathView::visibleRows() const {
    return filter_->rowCount();
}

void CdcPathView::applyFilter() {
    if (!result_ || !result_->cdc)
        return;
    filter_->set(clockFilter_->currentData().toString(), statusFilter_->currentData().toString(),
                 syncFilter_->currentData().toString(), textFilter_->text().trimmed());
    countLabel_->setText(tr("%1 of %2 paths").arg(filter_->rowCount()).arg(model_->rowCount()));
}

void CdcPathView::showPath(std::size_t id) {
    if (result_ && result_->cdc && id < result_->cdc->paths.size()) {
        auto index = filter_->mapFromSource(model_->index(static_cast<int>(id), 0));
        if (!index.isValid()) { // hidden by a filter: show everything
            for (auto* c : {clockFilter_, statusFilter_, syncFilter_}) {
                c->blockSignals(true);
                c->setCurrentIndex(0);
                c->blockSignals(false);
            }
            textFilter_->blockSignals(true);
            textFilter_->clear();
            textFilter_->blockSignals(false);
            applyFilter();
            index = filter_->mapFromSource(model_->index(static_cast<int>(id), 0));
        }
        if (index.isValid()) {
            table_->setCurrentIndex(index);
            table_->scrollTo(index);
        }
    }
    showDetails(id);
}

void CdcPathView::showDetails(std::size_t id) {
    if (!result_ || !result_->cdc || id >= result_->cdc->paths.size())
        return;
    current_ = id;
    emit pathSelected(id);
    const auto& p = result_->cdc->paths[id];
    auto locLink = [](const SourceLocation& l) -> QString {
        if (l.file.empty())
            return {};
        return QStringLiteral(" <a href=\"loc:%1:%2\">%3</a>")
            .arg(l.line)
            .arg(qs(l.file).toHtmlEscaped(), locText(l).toHtmlEscaped());
    };
    QString h = QStringLiteral("<h3>Path #%1 &mdash; <span style='background:%2'>&nbsp;%3&nbsp;</span></h3>")
                    .arg(p.id)
                    .arg(statusColor(p.status).name(), qs(cdc::toString(p.status)));
    QString relReason;
    if (p.srcUnclocked)
        relReason = tr("the source is an input without clock domain: asynchronous to every clock");
    else if (result_->clocks)
        relReason = qs(result_->clocks->reason(p.srcClock.clock, p.dstClock.clock));
    h += "<table cellspacing='0' cellpadding='2'>";
    auto row = [&](const QString& k, const QString& v) { h += "<tr><td><b>" + k + "</b></td><td>" + v + "</td></tr>"; };
    row(tr("Source clock"), esc(p.srcClockName));
    row(tr("Destination clock"), esc(p.dstClockName));
    row(tr("Clock relation"), qs(clock::toString(p.relation)) + " &mdash; " + relReason.toHtmlEscaped());
    row(tr("Source"), esc(p.srcName) + locLink(p.srcLoc));
    row(tr("Destination"), esc(p.dstName) + locLink(p.dstLoc));
    row(tr("Pin"), qs(cdc::toString(p.pin)));
    row(tr("Crossing signal"), esc(p.crossingSignal));
    row(tr("Width / class"), QString::number(p.width) + " / " + qs(cdc::toString(p.cls)));
    row(tr("Fingerprint"), qs(p.fingerprint));
    if (p.groupSize > 1) {
        QStringList m;
        for (const auto& g : p.groupMembers)
            m << esc(g);
        if (p.groupSize > p.groupMembers.size())
            m << QStringLiteral("&hellip;");
        row(tr("Grouped registers"), QString::number(p.groupSize) + ": " + m.join(QLatin1String(", ")));
    }
    if (p.structure < result_->cdc->structures.size()) {
        const auto& st = result_->cdc->structures[p.structure];
        QStringList ids;
        for (const auto other : st.paths)
            ids << QStringLiteral("<a href=\"path:%1\">#%1</a>").arg(other);
        row(tr("Structure"), QStringLiteral("S%1 %2 '%3' &mdash; paths ").arg(st.id).arg(qs(cdc::toString(st.type)), esc(st.name)) +
                                 ids.join(QLatin1String(" ")));
    }
    h += "</table>";

    h += "<h4>" + tr("Path: source clock &rarr; crossing logic &rarr; destination clock") + "</h4><ol>";
    for (std::size_t i = 0; i < p.steps.size(); ++i) {
        const auto& s = p.steps[i];
        QString text = esc(s.name);
        const bool reg = s.kind == "register";
        const bool logic = s.kind.starts_with("logic:");
        if (reg)
            text = "<b>" + text + "</b>";
        if (logic)
            text = "<span style='color:#b04000'><b>" + qs(s.kind.substr(6)) + "</b> " + text + "</span>";
        QString clockTag;
        if (i == 0)
            clockTag = " <i>[" + esc(p.srcClockName) + "]</i>";
        if (i + 1 == p.steps.size())
            clockTag = " <i>[" + esc(p.dstClockName) + "]</i>";
        h += "<li>" + text + " <small>(" + qs(logic ? std::string("logic") : s.kind) + ")</small>" + clockTag +
             locLink(s.loc) + "</li>";
    }
    h += "</ol>";

    h += "<h4>" + tr("Synchronizer") + "</h4><p>" + qs(cdc::toString(p.syncType));
    if (p.syncStages > 0)
        h += tr(", %1 stage(s)").arg(p.syncStages);
    h += " &mdash; <b>" + qs(cdc::toString(p.syncStatus)) + "</b></p>";
    if (!p.syncRegisters.empty()) {
        QStringList regs;
        for (const auto& r : p.syncRegisters)
            regs << esc(r);
        h += "<p>" + tr("Registers: ") + regs.join(QLatin1String(" &rarr; ")) + "</p>";
    }
    if (!p.conditions.empty()) {
        h += "<ul>";
        for (const auto& c : p.conditions)
            h += QStringLiteral("<li><span style='color:%1'>%2</span> %3</li>")
                     .arg(c.satisfied ? "#007000" : "#c00000", c.satisfied ? "&#10004;" : "&#10008;", esc(c.text));
        h += "</ul>";
    }
    if (!p.unverified.empty()) {
        h += "<p><b>" + tr("Not verified statically:") + "</b></p><ul>";
        for (const auto& u : p.unverified)
            h += "<li>" + esc(u) + "</li>";
        h += "</ul>";
    }
    for (const auto& n : p.notes)
        h += "<p><i>" + esc(n) + "</i></p>";

    auto pathLink = [&](std::size_t other) {
        const auto& o = result_->cdc->paths.at(other);
        return QStringLiteral("<a href=\"path:%1\">#%1</a> ").arg(other) + esc(o.srcName);
    };
    if (!p.reconvergences.empty() || !p.mixedClocks.empty() || !p.splitWith.empty() || p.sourceMayPulse) {
        h += "<h4>" + tr("Advanced checks") + "</h4><ul>";
        for (const auto& rv : p.reconvergences) {
            QStringList others;
            for (const auto other : rv.paths)
                if (other < result_->cdc->paths.size())
                    others << pathLink(other);
            h += "<li>" + tr("Reconverges at <b>%1</b> (%2 register level(s) after the synchronizers) with: ")
                              .arg(esc(rv.at))
                              .arg(rv.depth) +
                 others.join(QLatin1String(", ")) + locLink(rv.loc) + "</li>";
        }
        if (!p.splitWith.empty()) {
            QStringList others;
            for (const auto other : p.splitWith)
                if (other < result_->cdc->paths.size())
                    others << pathLink(other);
            h += "<li>" + tr("Other bits of the same source synchronized separately: ") +
                 others.join(QLatin1String(", ")) +
                 (p.bitsCombined ? tr(" &mdash; read together") : tr(" &mdash; not read together")) + "</li>";
        }
        if (!p.mixedClocks.empty()) {
            QStringList c;
            for (const auto& m : p.mixedClocks)
                c << esc(m);
            h += "<li>" + tr("Logic also combines signals from: ") + c.join(QLatin1String(", ")) + "</li>";
        }
        if (p.sourceMayPulse)
            h += "<li>" + tr("Source can pulse for a single cycle (no hold path)") + "</li>";
        h += "</ul>";
    }

    h += "<h4>" + tr("Rule hits") + "</h4>";
    if (p.hits.empty()) {
        h += "<p>" + tr("None.") + "</p>";
    }
    else {
        const auto registry = rules::RuleRegistry::standard();
        for (const auto& hit : p.hits) {
            h += "<p><b>" + esc(hit.ruleId) + "</b> (" + qs(toString(hit.severity)) + ", " +
                 qs(cdc::toString(hit.status)) + "): " + esc(hit.message) + "</p>";
            if (const auto* rule = registry.find(hit.ruleId))
                h += "<p style='margin-left:16px'><i>" + tr("Fix: ") + "</i>" + esc(rule->meta().fixSuggestion) +
                     "</p>";
        }
    }
    details_->setHtml(h);
}

// ============================================================================
// ViolationView
// ============================================================================

ViolationView::ViolationView(QWidget* parent)
    : QWidget(parent), ruleFilter_(combo(this, tr("All rules"))), severityFilter_(combo(this, tr("All severities"))),
      statusFilter_(combo(this, tr("All statuses"))), clockFilter_(combo(this, tr("All clocks"))),
      waiverFilter_(combo(this, tr("Waived and not waived"))), countLabel_(new QLabel(this)),
      model_(new detail::HitModel(this)), filter_(new detail::HitFilter(model_, this)), table_(new QTableView(this)),
      ruleInfo_(new QTextBrowser(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout();
    const auto registry = rules::RuleRegistry::standard();
    for (const auto& r : registry.rules())
        ruleFilter_->addItem(qs(r->meta().id), qs(r->meta().id));
    for (const auto s : {Severity::Error, Severity::Warning, Severity::Info})
        severityFilter_->addItem(qs(toString(s)), qs(toString(s)));
    for (const auto s : {cdc::AnalysisStatus::Violation, cdc::AnalysisStatus::NeedsReview,
                         cdc::AnalysisStatus::PotentialRisk})
        statusFilter_->addItem(qs(cdc::toString(s)), qs(cdc::toString(s)));
    bar->addWidget(ruleFilter_);
    bar->addWidget(severityFilter_);
    bar->addWidget(statusFilter_);
    bar->addWidget(clockFilter_);
    waiverFilter_->addItem(tr("Not waived"), QStringLiteral("open"));
    waiverFilter_->addItem(tr("Waived"), QStringLiteral("waived"));
    bar->addWidget(waiverFilter_);
    bar->addStretch(1);
    bar->addWidget(countLabel_);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal, this);
    table_->setModel(filter_);
    setupTable(table_);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    split->addWidget(table_);
    ruleInfo_->setOpenLinks(false);
    split->addWidget(ruleInfo_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    for (auto* c : {ruleFilter_, severityFilter_, statusFilter_, clockFilter_, waiverFilter_})
        connect(c, &QComboBox::currentIndexChanged, this, &ViolationView::applyFilter);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current) {
                if (!current.isValid())
                    return;
                const auto& r = model_->row(filter_->mapToSource(current).row());
                showRule(r.path, r.hit);
            });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!result_ || !result_->cdc || !index.isValid())
            return;
        const auto id = index.data(kIdRole).toULongLong();
        const auto& p = result_->cdc->paths.at(id);
        emit pathActivated(id);
        if (!p.dstLoc.file.empty())
            emit locationActivated(qs(p.dstLoc.file), static_cast<int>(p.dstLoc.line));
    });
    clear();
}

void ViolationView::clear() {
    result_.reset();
    model_->setResult(nullptr);
    ruleInfo_->setHtml(tr("<p>No rule results.</p>"));
    countLabel_->clear();
    clockFilter_->blockSignals(true);
    while (clockFilter_->count() > 1)
        clockFilter_->removeItem(1);
    clockFilter_->blockSignals(false);
}

void ViolationView::setResult(std::shared_ptr<const engine::AnalysisResult> result) {
    clear();
    result_ = std::move(result);
    if (!result_ || !result_->cdc) {
        ruleInfo_->setHtml(tr("<p>CDC analysis did not run (see Messages).</p>"));
        return;
    }
    if (result_->clocks) {
        clockFilter_->blockSignals(true);
        for (const auto& c : result_->clocks->clocks)
            clockFilter_->addItem(qs(c.name), qs(c.name));
        clockFilter_->blockSignals(false);
    }
    model_->setResult(result_);
    const int rows = model_->rowCount();
    table_->resizeColumnToContents(0);
    table_->resizeColumnToContents(1);
    table_->resizeColumnToContents(2);
    applyFilter();
    if (rows > 0)
        table_->setCurrentIndex(filter_->index(0, 0));
    else
        ruleInfo_->setHtml(tr("<p><b>No rule violations.</b></p>"));
}

int ViolationView::rowCount() const {
    return model_->rowCount();
}

int ViolationView::visibleRows() const {
    return filter_->rowCount();
}

void ViolationView::applyFilter() {
    if (!result_ || !result_->cdc)
        return;
    filter_->set(ruleFilter_->currentData().toString(), severityFilter_->currentData().toString(),
                 statusFilter_->currentData().toString(), clockFilter_->currentData().toString(),
                 waiverFilter_->currentData().toString());
    countLabel_->setText(tr("%1 of %2 hits").arg(filter_->rowCount()).arg(model_->rowCount()));
}

void ViolationView::showRule(std::size_t pathId, std::size_t hit) {
    if (!result_ || !result_->cdc)
        return;
    const auto& h = result_->cdc->paths.at(pathId).hits.at(hit);
    const auto registry = rules::RuleRegistry::standard();
    QString html = "<h3>" + esc(h.ruleId) + "</h3><p>" + esc(h.message) + "</p>";
    if (!h.waiver.empty()) {
        for (const auto& w : result_->cdc->waivers)
            if (w.id == h.waiver) {
                html += "<p style='background:#eeeeee'><b>" + tr("Waived by %1").arg(esc(w.id)) + "</b>: " +
                        esc(w.reason);
                if (!w.author.empty())
                    html += " &mdash; " + esc(w.author);
                if (!w.expires.empty())
                    html += tr(" (expires %1)").arg(esc(w.expires));
                html += "<br/><small>" + esc(toString(w.loc)) + "</small></p>";
            }
    }
    if (const auto* r = registry.find(h.ruleId)) {
        const auto& m = r->meta();
        html += "<p><b>" + esc(m.title) + "</b></p>";
        html += "<p><b>" + tr("Purpose: ") + "</b>" + esc(m.purpose) + "</p>";
        html += "<p><b>" + tr("Applies to: ") + "</b>" + esc(m.applicability) + "</p>";
        html += "<p><b>" + tr("Limits: ") + "</b>" + esc(m.falsePositiveNotes) + "</p>";
        html += "<p><b>" + tr("Fix: ") + "</b>" + esc(m.fixSuggestion) + "</p>";
    }
    html += "<p><i>" + tr("Double-click the row to open the path and the source.") + "</i></p>";
    ruleInfo_->setHtml(html);
}

} // namespace opencdc::gui

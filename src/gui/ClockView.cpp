#include "gui/ClockView.h"

#include "ir/Dump.h"

#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace opencdc::gui {

namespace {

constexpr int kFileRole = Qt::UserRole;
constexpr int kLineRole = Qt::UserRole + 1;
constexpr std::size_t kMaxRegistersPerClock = 500;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

void setLocation(QTreeWidgetItem* item, const SourceLocation& loc) {
    if (loc.file.empty())
        return;
    item->setData(0, kFileRole, qs(loc.file));
    item->setData(0, kLineRole, static_cast<int>(loc.line));
}

QString relationShort(clock::ClockRelation r) {
    switch (r) {
        case clock::ClockRelation::Same: return QStringLiteral("=");
        case clock::ClockRelation::Synchronous: return QStringLiteral("sync");
        case clock::ClockRelation::Asynchronous: return QStringLiteral("async");
        case clock::ClockRelation::Exclusive: return QStringLiteral("excl");
        case clock::ClockRelation::Unknown: return QStringLiteral("?");
    }
    return {};
}

QColor relationColor(clock::ClockRelation r) {
    switch (r) {
        case clock::ClockRelation::Same: return QColor(225, 225, 225);
        case clock::ClockRelation::Synchronous: return QColor(200, 235, 200);
        case clock::ClockRelation::Asynchronous: return QColor(255, 215, 170);
        case clock::ClockRelation::Exclusive: return QColor(210, 220, 245);
        case clock::ClockRelation::Unknown: return QColor(255, 180, 180);
    }
    return {};
}

} // namespace

ClockView::ClockView(QWidget* parent)
    : QWidget(parent), summary_(new QLabel(this)), tree_(new QTreeWidget(this)), matrix_(new QTableWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    auto* split = new QSplitter(Qt::Vertical, this);
    tree_->setHeaderLabels({tr("Clock / register"), tr("Kind"), tr("Source"), tr("Period"), tr("Registers")});
    tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    split->addWidget(tree_);
    matrix_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    matrix_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    split->addWidget(matrix_);
    layout->addWidget(split, 1);

    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        const QString file = item->data(0, kFileRole).toString();
        if (!file.isEmpty())
            emit locationActivated(file, item->data(0, kLineRole).toInt());
    });
    clear();
}

void ClockView::clear() {
    tree_->clear();
    matrix_->clear();
    matrix_->setRowCount(0);
    matrix_->setColumnCount(0);
    summary_->setText(tr("No clock analysis result."));
}

void ClockView::setResult(const engine::AnalysisResult& result) {
    clear();
    if (!result.clocks || !result.design || !result.view) {
        summary_->setText(tr("Clock analysis did not run (see Messages)."));
        return;
    }
    const auto& cm = *result.clocks;
    const auto& d = *result.design;
    const std::size_t n = cm.clocks.size();

    // Registers per clock (and unresolved ones).
    std::vector<QTreeWidgetItem*> clockItems(n, nullptr);
    std::vector<std::size_t> shown(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& c = cm.clocks[i];
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, qs(c.name));
        item->setText(1, qs(std::string(clock::toString(c.kind))));
        item->setText(2, qs(c.sourceName));
        item->setText(3, c.periodNs ? QStringLiteral("%1 ns").arg(*c.periodNs) : QStringLiteral("-"));
        QString regs = QString::number(c.registers());
        if (c.negedgeRegisters)
            regs += tr(" (%1 negedge)").arg(c.negedgeRegisters);
        if (c.gatedRegisters)
            regs += tr(", %1 gated").arg(c.gatedRegisters);
        item->setText(4, regs);
        QString tip = tr("%1 clock").arg(qs(std::string(clock::toString(c.kind))));
        if (c.master.valid())
            tip += tr(", derived from '%1'").arg(qs(cm.clock(c.master).name));
        item->setToolTip(0, tip);
        setLocation(item, c.definedAt);
        clockItems[i] = item;
    }
    auto* unresolved = new QTreeWidgetItem(tree_);
    unresolved->setText(0, tr("(unresolved / constant clock)"));
    std::size_t unresolvedCount = 0;

    for (std::size_t i = 0; i < d.instances.size(); ++i) {
        const auto& node = d.instances[i];
        const auto& body = d.body(node.body);
        for (std::size_t r = 0; r < body.registers.size(); ++r) {
            const auto& reg = body.registers[r];
            const auto& rc = cm.registers.at(static_cast<std::size_t>(node.registerBase) + r);
            const QString name = qs(node.hierPath + "." + ir::formatSig(body, reg.q));
            if (rc.clocks.empty()) {
                if (unresolvedCount++ < kMaxRegistersPerClock) {
                    auto* it = new QTreeWidgetItem(unresolved, {name});
                    it->setText(2, qs(rc.reason));
                    it->setToolTip(0, qs(rc.reason));
                    setLocation(it, reg.origin);
                }
                continue;
            }
            for (const auto& ce : rc.clocks) {
                const auto k = ce.clock.index();
                if (shown[k]++ >= kMaxRegistersPerClock)
                    continue;
                auto* it = new QTreeWidgetItem(clockItems[k], {name});
                QString attr = ce.edge == ir::Edge::Pos ? QStringLiteral("posedge") : QStringLiteral("negedge");
                if (rc.gated)
                    attr += tr(", gated");
                if (rc.muxed)
                    attr += tr(", clock mux");
                it->setText(1, attr);
                setLocation(it, reg.origin);
            }
        }
    }
    for (std::size_t k = 0; k < n; ++k)
        if (shown[k] > kMaxRegistersPerClock)
            new QTreeWidgetItem(clockItems[k],
                                {tr("... %1 more").arg(shown[k] - kMaxRegistersPerClock)});
    if (unresolvedCount == 0)
        delete unresolved;
    else
        unresolved->setText(4, QString::number(unresolvedCount));

    // Relation matrix.
    matrix_->setRowCount(static_cast<int>(n));
    matrix_->setColumnCount(static_cast<int>(n));
    QStringList names;
    for (const auto& c : cm.clocks)
        names << qs(c.name);
    matrix_->setHorizontalHeaderLabels(names);
    matrix_->setVerticalHeaderLabels(names);
    std::size_t unknown = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const clock::ClockId a{static_cast<std::uint32_t>(i)};
            const clock::ClockId b{static_cast<std::uint32_t>(j)};
            const auto rel = cm.relation(a, b);
            if (i < j && rel == clock::ClockRelation::Unknown)
                ++unknown;
            auto* cell = new QTableWidgetItem(relationShort(rel));
            cell->setBackground(relationColor(rel));
            cell->setForeground(QColor(20, 20, 20));
            cell->setTextAlignment(Qt::AlignCenter);
            cell->setToolTip(qs(std::string(clock::toString(rel))) + ": " + qs(cm.reason(a, b)));
            matrix_->setItem(static_cast<int>(i), static_cast<int>(j), cell);
        }
    }

    summary_->setText(tr("%1 clock(s); %2 of %3 registers resolved; %4 clock pair(s) with unknown relation "
                         "(policy: %5). Hover a matrix cell for the reason.")
                          .arg(n)
                          .arg(cm.resolvedRegisters())
                          .arg(cm.registers.size())
                          .arg(unknown)
                          .arg(qs(std::string(engine::toString(result.project.options.unknownClockRelation)))));
}

} // namespace opencdc::gui

#include "gui/SchematicView.h"

#include <QFile>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace opencdc::gui {

using namespace opencdc::schematic;

namespace {

constexpr int kNodeIdRole = 0;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

QColor statusColor(cdc::AnalysisStatus s) {
    switch (s) {
        case cdc::AnalysisStatus::Safe: return QColor(0x2e, 0x8b, 0x57);
        case cdc::AnalysisStatus::NeedsReview: return QColor(0xc9, 0xa2, 0x00);
        case cdc::AnalysisStatus::PotentialRisk: return QColor(0xe0, 0x7b, 0x00);
        case cdc::AnalysisStatus::Violation: return QColor(0xd0, 0x20, 0x20);
    }
    return Qt::black;
}

/// Pastel fills for clock domains, assigned in clock-model order.
const QColor kClockPalette[] = {QColor(0xcf, 0xe2, 0xff), QColor(0xd8, 0xf0, 0xd0), QColor(0xff, 0xe0, 0xc0),
                                QColor(0xec, 0xd8, 0xf5), QColor(0xff, 0xf3, 0xb8), QColor(0xc8, 0xee, 0xee),
                                QColor(0xf8, 0xd0, 0xdc), QColor(0xe2, 0xe2, 0xc8)};

/// "clk_a (negedge)" -> "clk_a"
std::string baseClock(const std::string& name) {
    const auto sp = name.find(' ');
    return sp == std::string::npos ? name : name.substr(0, sp);
}

QPainterPath edgePath(const std::vector<Point>& pts) {
    QPainterPath path(QPointF(pts.front().x, pts.front().y));
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const QPointF a(pts[i - 1].x, pts[i - 1].y);
        const QPointF b(pts[i].x, pts[i].y);
        const double dx = (b.x() - a.x()) * 0.5;
        path.cubicTo(QPointF(a.x() + dx, a.y()), QPointF(b.x() - dx, b.y()), b);
    }
    return path;
}

} // namespace

// ============================================================================
// SchematicCanvas
// ============================================================================

SchematicCanvas::SchematicCanvas(QWidget* parent) : QGraphicsView(parent) {
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::Antialiasing);
    setRenderHint(QPainter::TextAntialiasing);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setBackgroundBrush(QColor(0xfb, 0xfb, 0xfb));
}

void SchematicCanvas::zoomBy(double factor) {
    autoFit_ = false;
    const double current = transform().m11();
    const double next = std::clamp(current * factor, 0.1, 4.0);
    scale(next / current, next / current);
}

void SchematicCanvas::fit() {
    autoFit_ = true;
    applyFit();
}

void SchematicCanvas::applyFit() {
    if (!scene() || scene()->items().isEmpty())
        return;
    resetTransform();
    fitInView(scene()->itemsBoundingRect().adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
    if (transform().m11() > 1.25) {
        const double f = 1.25 / transform().m11();
        scale(f, f);
    }
}

void SchematicCanvas::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (autoFit_)
        applyFit();
}

void SchematicCanvas::wheelEvent(QWheelEvent* event) {
    zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15);
    event->accept();
}

void SchematicCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    for (auto* item = itemAt(event->pos()); item; item = item->parentItem()) {
        const QVariant v = item->data(kNodeIdRole);
        if (v.isValid()) {
            emit nodeDoubleClicked(static_cast<std::size_t>(v.toULongLong()));
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

// ============================================================================
// SchematicView
// ============================================================================

SchematicView::SchematicView(QWidget* parent)
    : QWidget(parent), title_(new QLabel(this)), legend_(new QLabel(this)), fitButton_(new QToolButton(this)),
      exportButton_(new QToolButton(this)), svgButton_(new QToolButton(this)), canvas_(new SchematicCanvas(this)), scene_(new QGraphicsScene(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout();
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addWidget(title_, 1);
    auto* zoomIn = new QToolButton(this);
    zoomIn->setText(QStringLiteral("+"));
    zoomIn->setToolTip(tr("Zoom in (mouse wheel)"));
    auto* zoomOut = new QToolButton(this);
    zoomOut->setText(QStringLiteral("−"));
    zoomOut->setToolTip(tr("Zoom out (mouse wheel)"));
    fitButton_->setText(tr("Fit"));
    fitButton_->setToolTip(tr("Fit the schematic into the view"));
    exportButton_->setText(tr("PNG…"));
    exportButton_->setToolTip(tr("Export the schematic as a PNG image"));
    bar->addWidget(zoomIn);
    bar->addWidget(zoomOut);
    bar->addWidget(fitButton_);
    bar->addWidget(exportButton_);
    svgButton_->setText(tr("SVG\u2026"));
    svgButton_->setToolTip(tr("Export the schematic as an SVG drawing"));
    bar->addWidget(svgButton_);
    layout->addLayout(bar);
    legend_->setTextFormat(Qt::RichText);
    layout->addWidget(legend_);
    canvas_->setScene(scene_);
    layout->addWidget(canvas_, 1);

    connect(zoomIn, &QToolButton::clicked, this, [this] { canvas_->zoomBy(1.25); });
    connect(zoomOut, &QToolButton::clicked, this, [this] { canvas_->zoomBy(1.0 / 1.25); });
    connect(fitButton_, &QToolButton::clicked, canvas_, &SchematicCanvas::fit);
    connect(exportButton_, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export schematic"), QStringLiteral("schematic.png"),
                                                          tr("PNG images (*.png)"));
        if (!file.isEmpty())
            exportPng(file);
    });
    connect(svgButton_, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export schematic"), QStringLiteral("schematic.svg"),
                                                          tr("SVG drawings (*.svg)"));
        if (!file.isEmpty())
            exportSvg(file);
    });
    connect(canvas_, &SchematicCanvas::nodeDoubleClicked, this, [this](std::size_t id) {
        if (id >= schematic_.nodes.size())
            return;
        const auto& loc = schematic_.nodes[id].loc;
        if (!loc.file.empty())
            emit locationActivated(qs(loc.file), static_cast<int>(loc.line));
    });
    clear();
}

void SchematicView::clear() {
    factory_.reset();
    result_.reset();
    current_.reset();
    schematic_ = {};
    clockColors_.clear();
    scene_->clear();
    drawnNodes_ = drawnEdges_ = 0;
    title_->setText(tr("<i>Select a CDC path to show its schematic.</i>"));
    legend_->clear();
    exportButton_->setEnabled(false);
    svgButton_->setEnabled(false);
}

void SchematicView::setResult(std::shared_ptr<const engine::AnalysisResult> result) {
    clear();
    result_ = std::move(result);
    if (result_ && result_->cdc)
        factory_ = std::make_unique<SchematicFactory>(*result_->cdc);
    if (result_ && result_->clocks) {
        // Alphabetical, like the SVG output (schematic::toSvg).
        std::vector<std::string> names;
        for (const auto& c : result_->clocks->clocks)
            names.push_back(c.name);
        std::ranges::sort(names);
        for (const auto& n : names)
            (void)clockColor(n);
    }
    if (!result_ || !result_->cdc || result_->cdc->paths.empty())
        title_->setText(tr("<i>No CDC paths: nothing to draw.</i>"));
}

QColor SchematicView::clockColor(const std::string& clock) {
    if (clock.empty())
        return QColor(0xee, 0xee, 0xee);
    const std::string base = baseClock(clock);
    auto it = clockColors_.find(base);
    if (it == clockColors_.end()) {
        const auto n = clockColors_.size() % std::size(kClockPalette);
        it = clockColors_.emplace(base, kClockPalette[n]).first;
    }
    return it->second;
}

void SchematicView::showPath(std::size_t id) {
    if (!result_ || !result_->cdc || id >= result_->cdc->paths.size())
        return;
    current_ = id;
    schematic_ = factory_->build(id);
    LayoutOptions opt;
    const QFontMetricsF fm(font());
    opt.charWidth = std::max(5.0, fm.averageCharWidth() * 1.05);
    opt.nodeHeight = std::max(34.0, fm.height() * 2.3);
    schematic::layout(schematic_, opt);
    draw();
    canvas_->fit();
}

void SchematicView::draw() {
    scene_->clear();
    drawnNodes_ = drawnEdges_ = 0;
    const auto& s = schematic_;
    const auto& paths = result_->cdc->paths;
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.85));

    // Edges first, so boxes are drawn on top of the line ends.
    for (const auto& e : s.edges) {
        cdc::AnalysisStatus worst = cdc::AnalysisStatus::Safe;
        bool focus = false;
        for (const auto pid : e.paths) {
            worst = std::max(worst, paths[pid].status);
            focus = focus || pid == s.focusPath;
        }
        QPen pen(QColor(0x44, 0x44, 0x44), 1.5);
        switch (e.kind) {
            case EdgeKind::Path:
                pen.setColor(statusColor(worst));
                pen.setWidthF(e.crossesClocks ? 3.0 : 2.2);
                break;
            case EdgeKind::Chain: break;
            case EdgeKind::Qualifier:
                pen.setColor(QColor(0x1f, 0x5f, 0xbf));
                pen.setStyle(Qt::DashLine);
                break;
            case EdgeKind::Reconvergence:
                pen.setColor(QColor(0x8e, 0x3f, 0xb0));
                pen.setStyle(Qt::DashDotLine);
                break;
        }
        if (!focus)
            pen.setWidthF(pen.widthF() * 0.7);
        auto* item = scene_->addPath(edgePath(e.points), pen);
        item->setZValue(focus ? 1 : 0);
        QString tip = e.kind == EdgeKind::Path ? tr("crossing path") : e.kind == EdgeKind::Chain ? tr("synchronizer stage")
                      : e.kind == EdgeKind::Qualifier                                     ? tr("synchronized qualifier")
                                                                                          : tr("reconvergence");
        if (e.crossesClocks)
            tip += tr(" (clock domain crossing)");
        if (!e.label.empty())
            tip += "\n" + qs(e.label);
        QStringList ids;
        for (const auto pid : e.paths)
            ids << QStringLiteral("#%1 %2").arg(pid).arg(qs(std::string(cdc::toString(paths[pid].status))));
        tip += "\n" + tr("paths: ") + ids.join(QStringLiteral(", "));
        item->setToolTip(tip);

        // Arrow head at the target end.
        const auto& tip0 = e.points.back();
        const auto& prev = e.points[e.points.size() - 2];
        const double dir = tip0.x >= prev.x ? 1.0 : -1.0;
        QPolygonF head;
        head << QPointF(tip0.x, tip0.y) << QPointF(tip0.x - dir * 9, tip0.y - 4.5)
             << QPointF(tip0.x - dir * 9, tip0.y + 4.5);
        auto* arrow = scene_->addPolygon(head, QPen(pen.color(), 1), QBrush(pen.color()));
        arrow->setZValue(item->zValue());
        if (!e.label.empty()) {
            auto* text = scene_->addSimpleText(qs(e.label), small);
            text->setBrush(QColor(0x55, 0x55, 0x55));
            const auto& p0 = e.points.front();
            text->setPos(p0.x + (dir > 0 ? 6 : -6 - text->boundingRect().width()), p0.y - text->boundingRect().height() - 2);
            text->setZValue(2);
        }
        ++drawnEdges_;
    }

    for (const auto& n : s.nodes) {
        if (n.kind == NodeKind::Dummy)
            continue;
        const QRectF r(n.x, n.y, n.w, n.h);
        const QColor fill = n.kind == NodeKind::Logic ? QColor(0xf0, 0xf0, 0xf0) : clockColor(n.clock);
        QPen pen(QColor(0x50, 0x50, 0x50), 1.2);
        if (n.roles & kDestination) {
            pen = QPen(statusColor(n.status), 3.0);
        }
        else if (n.roles & kReconvergence) {
            pen = QPen(QColor(0x8e, 0x3f, 0xb0), 2.5, Qt::DashLine);
        }
        QGraphicsItem* box = nullptr;
        switch (n.kind) {
            case NodeKind::Logic: {
                QPainterPath p;
                p.addRoundedRect(r, n.h / 2.5, n.h / 2.5);
                box = scene_->addPath(p, pen, fill);
                break;
            }
            case NodeKind::Input:
            case NodeKind::BlackBox: {
                QPolygonF poly;
                poly << r.topLeft() << QPointF(r.right() - 12, r.top()) << QPointF(r.right(), r.center().y())
                     << QPointF(r.right() - 12, r.bottom()) << r.bottomLeft();
                box = scene_->addPolygon(poly, pen, fill);
                break;
            }
            case NodeKind::Memory: {
                auto* back = scene_->addRect(r.translated(5, -5), QPen(QColor(0x50, 0x50, 0x50), 1.0), fill.darker(108));
                back->setZValue(3);
                box = scene_->addRect(r, pen, fill);
                break;
            }
            default: {
                box = scene_->addRect(r, pen, fill);
                // Clock pin triangle of a flop.
                QPolygonF clk;
                clk << QPointF(r.left(), r.bottom() - 13) << QPointF(r.left() + 8, r.bottom() - 7)
                    << QPointF(r.left(), r.bottom() - 1);
                auto* tri = scene_->addPolygon(clk, QPen(QColor(0x50, 0x50, 0x50), 1.0));
                tri->setParentItem(box);
                break;
            }
        }
        box->setZValue(4);
        box->setData(kNodeIdRole, QVariant::fromValue<qulonglong>(n.id));
        box->setToolTip(qs(n.detail));
        auto* text = scene_->addSimpleText(qs(n.label), font());
        const QRectF tb = text->boundingRect();
        text->setPos(r.center().x() - tb.width() / 2, r.center().y() - tb.height() / 2 - (n.clock.empty() ? 0 : 5));
        text->setParentItem(box);
        text->setPos(r.center().x() - tb.width() / 2, r.center().y() - tb.height() / 2 - (n.clock.empty() ? 0 : 5));
        if (!n.clock.empty() && n.kind != NodeKind::Logic) {
            auto* clk = scene_->addSimpleText(qs(n.clock), small);
            clk->setBrush(QColor(0x40, 0x40, 0x40));
            const QRectF cb = clk->boundingRect();
            clk->setParentItem(box);
            clk->setPos(r.center().x() - cb.width() / 2, r.bottom() - cb.height() - 2);
        }
        ++drawnNodes_;
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30));

    QString title = qs(s.title).toHtmlEscaped();
    if (s.truncated)
        title += tr(" <i>(more related paths exist; only the first %1 are drawn)</i>").arg(s.paths.size());
    title_->setText("<b>" + title + "</b>");
    updateLegend();
    exportButton_->setEnabled(true);
    svgButton_->setEnabled(true);
}

void SchematicView::updateLegend() {
    QString html;
    std::vector<std::string> shown;
    for (const auto& n : schematic_.nodes)
        if (!n.clock.empty() && std::ranges::find(shown, baseClock(n.clock)) == shown.end())
            shown.push_back(baseClock(n.clock));
    for (const auto& c : shown)
        html += QStringLiteral("<span style='background:%1'>&nbsp;%2&nbsp;</span>&nbsp; ")
                    .arg(clockColor(c).name(), qs(c).toHtmlEscaped());
    html += tr("&nbsp;edges: <span style='color:%1'><b>&#8212;</b> crossing (path status)</span>, "
               "<b>&#8212;</b> synchronizer stage, <span style='color:#1f5fbf'>- - qualifier</span>, "
               "<span style='color:#8e3fb0'>-&middot;- reconvergence</span>; boxes: destination border = path "
               "status")
                .arg(statusColor(cdc::AnalysisStatus::Violation).name());
    legend_->setText(html);
}

bool SchematicView::exportSvg(const QString& file) {
    if (!current_ || !result_ || !result_->cdc)
        return false;
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const std::string svg = toSvg(schematic_, *result_->cdc);
    return out.write(svg.data(), static_cast<qint64>(svg.size())) == static_cast<qint64>(svg.size());
}

bool SchematicView::exportPng(const QString& file) {
    if (scene_->items().isEmpty())
        return false;
    const QRectF src = scene_->sceneRect();
    QImage image((src.size() * 2).toSize(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    scene_->render(&painter, QRectF(QPointF(0, 0), src.size() * 2), src);
    painter.end();
    return image.save(file, "PNG");
}

} // namespace opencdc::gui

#pragma once

#include "engine/AnalysisSession.h"
#include "schematic/Schematic.h"

#include <QGraphicsView>
#include <QWidget>

#include <map>
#include <memory>
#include <optional>

class QGraphicsScene;
class QLabel;
class QToolButton;

namespace opencdc::gui {

/// Graphics view with wheel zoom and drag panning.
class SchematicCanvas final : public QGraphicsView {
    Q_OBJECT

public:
    explicit SchematicCanvas(QWidget* parent = nullptr);
    /// Zooms by `factor`; a user zoom stops automatic fitting.
    void zoomBy(double factor);
    /// Fits the drawing into the view and keeps fitting on resize until the user zooms.
    void fit();

signals:
    void nodeDoubleClicked(std::size_t nodeId);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyFit();
    bool autoFit_ = true;
};

/// CDC Schematic Viewer: the registers, logic and synchronizer stages around
/// the selected CDC path, laid out left to right (schematic::layout), colored
/// by clock domain. Crossing edges are drawn in the color of the path status;
/// the destination register's border shows the path status. Double-clicking
/// a node jumps to its source.
class SchematicView final : public QWidget {
    Q_OBJECT

public:
    explicit SchematicView(QWidget* parent = nullptr);

    void clear();
    void setResult(std::shared_ptr<const engine::AnalysisResult> result);
    /// Builds, lays out and draws the schematic of path `id`.
    void showPath(std::size_t id);
    /// Writes the current schematic as a PNG image.
    bool exportPng(const QString& file);
    /// Writes the current schematic as SVG (schematic::toSvg, no QtSvg needed).
    bool exportSvg(const QString& file);

    [[nodiscard]] std::optional<std::size_t> currentPath() const { return current_; }
    [[nodiscard]] const schematic::Schematic& schematic() const { return schematic_; }
    [[nodiscard]] QGraphicsScene* scene() const { return scene_; }
    /// Number of drawn register/logic/input boxes (dummy nodes excluded).
    [[nodiscard]] int drawnNodes() const { return drawnNodes_; }
    [[nodiscard]] int drawnEdges() const { return drawnEdges_; }
    [[nodiscard]] QColor clockColor(const std::string& clock);

signals:
    void locationActivated(const QString& file, int line);

private:
    void draw();
    void updateLegend();

    std::shared_ptr<const engine::AnalysisResult> result_;
    std::unique_ptr<schematic::SchematicFactory> factory_;
    std::optional<std::size_t> current_;
    schematic::Schematic schematic_;
    std::map<std::string, QColor> clockColors_;
    QLabel* title_;
    QLabel* legend_;
    QToolButton* fitButton_;
    QToolButton* exportButton_;
    QToolButton* svgButton_;
    SchematicCanvas* canvas_;
    QGraphicsScene* scene_;
    int drawnNodes_ = 0;
    int drawnEdges_ = 0;
};

} // namespace opencdc::gui

#pragma once

#include <QPlainTextEdit>
#include <QString>

class QLabel;

namespace opencdc::gui {

/// Read-only HDL source viewer with a line-number gutter and line highlight.
class SourceViewer final : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit SourceViewer(QWidget* parent = nullptr);

    /// Loads `file` (if not already shown) and highlights `line` (1-based).
    /// Returns false and shows an explanation if the file cannot be read.
    bool showLocation(const QString& file, int line);

    [[nodiscard]] const QString& currentFile() const noexcept { return currentFile_; }
    [[nodiscard]] int highlightedLine() const noexcept { return highlightedLine_; }

    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent* event);

signals:
    void fileChanged(const QString& file);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);

    QWidget* lineNumberArea_;
    QString currentFile_;
    int highlightedLine_ = 0;
};

} // namespace opencdc::gui

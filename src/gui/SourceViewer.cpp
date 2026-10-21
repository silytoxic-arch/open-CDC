#include "gui/SourceViewer.h"

#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QTextBlock>
#include <QTextStream>

namespace opencdc::gui {

namespace {

class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(SourceViewer* viewer) : QWidget(viewer), viewer_(viewer) {}
    QSize sizeHint() const override { return {viewer_->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { viewer_->paintLineNumbers(event); }

private:
    SourceViewer* viewer_;
};

} // namespace

SourceViewer::SourceViewer(QWidget* parent)
    : QPlainTextEdit(parent), lineNumberArea_(new LineNumberArea(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setPlaceholderText(tr("Double-click a module, instance or message to show its source."));
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateLineNumberAreaWidth(); });
    connect(this, &QPlainTextEdit::updateRequest, this, &SourceViewer::updateLineNumberArea);
    updateLineNumberAreaWidth();
}

bool SourceViewer::showLocation(const QString& file, int line) {
    if (file != currentFile_) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            setPlainText(tr("Cannot open %1: %2").arg(file, f.errorString()));
            currentFile_.clear();
            highlightedLine_ = 0;
            return false;
        }
        QTextStream in(&f);
        in.setEncoding(QStringConverter::Utf8);
        setPlainText(in.readAll());
        currentFile_ = file;
        emit fileChanged(file);
    }

    highlightedLine_ = 0;
    QList<QTextEdit::ExtraSelection> selections;
    if (line > 0 && line <= blockCount()) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        QTextCursor cursor(block);
        setTextCursor(cursor);
        centerCursor();

        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(palette().highlight().color().lighter(160));
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = cursor;
        selections.append(sel);
        highlightedLine_ = line;
    }
    setExtraSelections(selections);
    return true;
}

int SourceViewer::lineNumberAreaWidth() const {
    int digits = 1;
    for (int max = std::max(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void SourceViewer::updateLineNumberAreaWidth() {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void SourceViewer::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy != 0)
        lineNumberArea_->scroll(0, dy);
    else
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void SourceViewer::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    lineNumberArea_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void SourceViewer::paintLineNumbers(QPaintEvent* event) {
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), palette().alternateBase());
    painter.setPen(palette().placeholderText().color());

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.drawText(0, top, lineNumberArea_->width() - 4, fontMetrics().height(),
                             Qt::AlignRight, QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

} // namespace opencdc::gui

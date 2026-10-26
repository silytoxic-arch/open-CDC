#pragma once

#include <QHeaderView>
#include <QTableView>
#include <QTreeView>

namespace opencdc::gui {

/// Every column can be resized by dragging its divider; the last column
/// fills the remaining width. (Stretch / ResizeToContents sections cannot be
/// dragged, so they are not used for data tables.)
inline void makeResizable(QHeaderView* h) {
    h->setSectionResizeMode(QHeaderView::Interactive);
    h->setStretchLastSection(true);
    h->setMinimumSectionSize(40);
}

/// Initial widths from the contents (capped), once per data load; the user
/// can still drag the dividers afterwards.
template <class View>
void fitColumns(View* v, int maxWidth = 600) {
    auto* h = [&] {
        if constexpr (std::is_base_of_v<QTableView, View>)
            return v->horizontalHeader();
        else
            return v->header();
    }();
    const int last = h->count() - 1;
    for (int c = 0; c < last; ++c) {
        v->resizeColumnToContents(c);
        if (h->sectionSize(c) > maxWidth)
            h->resizeSection(c, maxWidth);
    }
}

} // namespace opencdc::gui

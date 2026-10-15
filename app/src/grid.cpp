#include "grid.h"

#include "audiobox.h"
#include "editbox.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <string_view>
#include <QScrollBar>
#include <QSettings>
#include <numeric>

namespace {
struct GridTheme {
    QColor background{0x1f, 0x22, 0x27}, dialogue{0x27, 0x2b, 0x31}, comment{0x3b, 0x33, 0x2e};
    QColor selection{80, 140, 220, 90}, onVideo{70, 140, 70, 60}, collision{0xe0, 0x6c, 0x6c}, compare{0x9a, 0x5a, 0xd0, 80},
        compareText{0xb0, 0x70, 0xf0, 150};
    QColor text{0xdc, 0xdc, 0xdc}, lines{0x3a, 0x3f, 0x46}, header{0x17, 0x19, 0x1d}, headerText{0xb0, 0xb6, 0xbe};
    QColor labelNormal{0x2d, 0x3e, 0x57}, labelModified{0x55, 0x70, 0x2c}, labelSaved{0x4a, 0x4e, 0x56},
        labelDoubtful{0xb8, 0x64, 0x1a}, active{0xf0, 0xb0, 0x30}, error{0x8a, 0x25, 0x25};
};
const GridTheme T;
constexpr int Pad = 4;

// Calls draw(x, width) for each (start, length) span of the raw text as the cell shows it: with
// tags hidden (mode 2) or simplified to one character (mode 1)
template <class F>
void eachSpan(const std::vector<size_t> &spans, const QString &raw, const QString &shown, int mode,
              const QRect &rect, const QFontMetrics &fm, F draw) {
    const auto shownAt = [&](size_t at) {
        int out = 0;
        bool block = false;
        for (size_t k = 0; k < at && k < size_t(raw.size()); ++k) {
            if (block) block = raw[k] != '}';
            else if (mode && raw[k] == '{') {
                block = true;
                out += mode == 1;
            } else {
                ++out;
            }
        }
        return out;
    };
    for (size_t k = 0; k + 1 < spans.size(); k += 2) {
        const int s = shownAt(spans[k]), e = shownAt(spans[k] + spans[k + 1]);
        if (s >= shown.size()) break;
        draw(rect.left() + fm.horizontalAdvance(shown.left(s)), fm.horizontalAdvance(shown.mid(s, e - s)));
    }
}
} // namespace

Grid::Grid(TabPanel *tab) : QAbstractScrollArea(tab), m_tab(tab) {
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    QSettings st;
    setFont(st.value("grid/font", QFont("Tahoma", 10)).value<QFont>());
    for (const auto &v : st.value("grid/hiddenColumns").toList()) m_userHidden.insert(v.toInt());
    m_tagMode = st.value("grid/tagMode").toInt();
    // the last filter's options, as Kainote keeps them (the TL-only ones stay per tab)
    for (const QString &x : st.value("grid/filterStyles").toStringList()) filterSpec.hide_styles.push_back(fs::rs(x));
    filterSpec.hide_comments = st.value("grid/filterComments").toBool();
    filterSpec.invert = st.value("grid/filterInvert").toBool();
}

void Grid::resetFilter() {
    filterSpec.hide_rows.clear();
    m_expanded.clear();
    filterOn = QSettings().value("grid/filterAfterLoad").toBool() &&
               (!filterSpec.hide_styles.empty() || filterSpec.hide_comments);
    applyFilter();
}

QString Grid::colName(int c) {
    static const char *names[] = {"#", QT_TR_NOOP("L."), QT_TR_NOOP("Start"), QT_TR_NOOP("End"), QT_TR_NOOP("Style"),
        QT_TR_NOOP("Actor"), QT_TR_NOOP("Left"), QT_TR_NOOP("Right"), QT_TR_NOOP("Vert."), QT_TR_NOOP("Effect"),
        QT_TR_NOOP("CPS"), QT_TR_NOOP("Wraps"), QT_TR_NOOP("Original text"), QT_TR_NOOP("Text")};
    return tr(names[c]);
}

int Grid::rowHeight() const { return fontMetrics().height() + 4; }

void Grid::setColumnHidden(int c, bool hidden) {
    if (hidden) m_userHidden.insert(c); else m_userHidden.erase(c);
    QVariantList l;
    for (int x : m_userHidden) l << x;
    QSettings().setValue("grid/hiddenColumns", l);
    layoutColumns();
    viewport()->update();
}

void Grid::setTagMode(int mode) {
    m_tagMode = mode;
    viewport()->update();
}

void Grid::refresh() {
    auto &s = m_tab->s();
    if (s.generation() != m_gen) {
        m_gen = s.generation();
        m_colInfo = s.column_info();
        if (s.row_count() != m_rowCount) m_expanded.clear();
        m_rowCount = s.row_count();
        applyFilter();
        layoutColumns();
    }
    m_collisions.clear();
    if (m_tab->active() >= 0)
        for (size_t r : s.overlapping(size_t(m_tab->active()))) m_collisions.insert(r);
    updateScroll();
    viewport()->update();
}

const fsb::ColumnInfo &Grid::columnInfo() {
    if (m_tab->s().generation() != m_gen) refresh();
    return m_colInfo;
}

void Grid::applyFilter() {
    m_vis.clear();
    if (filterOn) {
        m_vis = fs::vec(m_tab->s().filter(filterSpec));
        if (!m_expanded.empty()) {
            std::vector<size_t> all;
            std::ranges::set_union(m_vis, m_expanded, std::back_inserter(all));
            m_vis = std::move(all);
        }
    } else {
        m_expanded.clear();
        m_vis.resize(m_tab->s().row_count());
        std::iota(m_vis.begin(), m_vis.end(), size_t(0));
    }
    m_folded = fs::vec(m_tab->s().tree_hidden());
    if (!m_folded.empty()) {
        std::vector<size_t> shown;
        std::ranges::set_difference(m_vis, m_folded, std::back_inserter(shown));
        m_vis = std::move(shown);
    }
    updateScroll();
    viewport()->update();
}

void Grid::setCompare(std::map<size_t, std::vector<size_t>> rows) {
    m_compare = std::move(rows);
    viewport()->update();
}

void Grid::setLinked(std::set<size_t> rows) {
    m_linked = std::move(rows);
    viewport()->update();
}

void Grid::setVideoTime(qint64 ms) {
    if (ms == m_videoMs) return;
    m_videoMs = ms;
    m_onVideo.clear();
    if (ms >= 0)
        for (size_t r : m_tab->s().rows_at(ms)) m_onVideo.insert(r);
    viewport()->update();
}

void Grid::layoutColumns() {
    const auto info = m_tab->s().file_info();
    const QString fmt = fs::q(info.format);
    const bool ass = fmt == "ASS" || fmt == "SSA";
    const QFontMetrics fm(font());
    m_cols.clear();
    m_widths.clear();
    auto add = [&](int c, int w) {
        if (c != Num && c != Text && c != Original && m_userHidden.contains(c)) return;
        m_cols.push_back(c);
        m_widths.push_back(w);
    };
    const int timeW = fm.horizontalAdvance("0:00:00.00") + 2 * Pad;
    add(Num, fm.horizontalAdvance(QString::number(m_tab->s().row_count() + 1)) + 3 * Pad + 4);
    if (fmt == "TMP") {
        add(Start, timeW);
        add(Wraps, fm.horizontalAdvance("00/00/00") + 2 * Pad);
    } else if (!ass) {
        add(Start, timeW);
        add(End, timeW);
        add(Cps, fm.horizontalAdvance("CPS") + 3 * Pad);
        add(Wraps, fm.horizontalAdvance("00/00/00") + 2 * Pad);
    } else {
        const auto &ci = m_colInfo;
        auto maxw = [&](const rust::Vec<rust::String> &values, int cap) {
            int w = 40;
            for (const auto &v : values) w = std::max(w, fm.horizontalAdvance(fs::q(v)));
            return std::min(w, cap) + 2 * Pad;
        };
        if (ci.any_layer) add(Layer, fm.horizontalAdvance("00") + 3 * Pad);
        add(Start, timeW);
        add(End, timeW);
        add(Style, maxw(ci.styles, 160));
        if (!ci.actors.empty()) add(Actor, maxw(ci.actors, 140));
        if (ci.any_margin_l) add(MarginL, fm.horizontalAdvance("0000") + 2 * Pad);
        if (ci.any_margin_r) add(MarginR, fm.horizontalAdvance("0000") + 2 * Pad);
        if (ci.any_margin_v) add(MarginV, fm.horizontalAdvance("0000") + 2 * Pad);
        if (!ci.effects.empty()) add(Effect, maxw(ci.effects, 120));
        add(Cps, fm.horizontalAdvance("CPS") + 3 * Pad);
        add(Wraps, fm.horizontalAdvance("00/00/00") + 2 * Pad);
    }
    const int used = std::accumulate(m_widths.begin(), m_widths.end(), 0);
    const int rest = std::max(100, viewport()->width() - used);
    if (info.tl_mode) {
        add(Original, rest / 2);
        add(Text, rest - rest / 2);
    } else {
        add(Text, rest);
    }
}

void Grid::resizeEvent(QResizeEvent *e) {
    QAbstractScrollArea::resizeEvent(e);
    layoutColumns();
    updateScroll();
}

void Grid::updateScroll() {
    const int page = std::max(1, viewport()->height() / rowHeight() - 1);
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setRange(0, std::max(0, int(m_vis.size()) - page));
}

int Grid::rowAt(int y) const {
    if (y < rowHeight()) return -1;
    const int vi = verticalScrollBar()->value() + (y - rowHeight()) / rowHeight();
    return vi < int(m_vis.size()) ? vi : -1;
}

int Grid::colAt(int x) const {
    int left = 0;
    for (size_t k = 0; k < m_cols.size(); ++k) {
        if (x < left + m_widths[k]) return m_cols[k];
        left += m_widths[k];
    }
    return -1;
}

QString Grid::cell(const fsb::Row &r, int col, size_t index) const {
    // Aegisub's text column: tag blocks simplified to ☀ or hidden, and at most 512 characters kept, so a long
    // drawing is not converted and shaped in full on every paint
    auto text = [&](const rust::String &t) {
        constexpr qsizetype Cap = 512;
        const std::string_view v(t.data(), t.size());
        QString s;
        for (size_t at = 0; at < v.size() && s.size() <= Cap;) {
            const size_t end = std::min(m_tagMode ? v.find('{', at) : v.npos, v.size());
            s += QString::fromUtf8(v.data() + at, qsizetype(std::min<size_t>(end - at, 4 * Cap)));
            if (end == v.size()) break;
            if (m_tagMode == 1) s += QChar(0x2600);
            const size_t close = v.find('}', end);
            at = close == v.npos ? v.size() : close + 1;
        }
        if (s.size() > Cap) s = s.left(Cap) + QChar(0x2026);
        return s;
    };
    const auto frames = [this] { return m_tab->edit() && m_tab->edit()->framesMode() && m_tab->video()->hasVideo(); };
    switch (col) {
    case Num: return QString::number(index + 1);
    case Layer: return QString::number(r.layer);
    case Start: return frames() ? QString::number(m_tab->video()->frameAtStart(r.start)) : fs::fmtTime(r.start);
    case End: return frames() ? QString::number(m_tab->video()->frameAtEnd(r.end)) : fs::fmtTime(r.end);
    case Style: return fs::q(r.style);
    case Actor: return fs::q(r.actor);
    case MarginL: return QString::number(r.margin_l);
    case MarginR: return QString::number(r.margin_r);
    case MarginV: return QString::number(r.margin_v);
    case Effect: return fs::q(r.effect);
    case Cps: return QString::number(int(r.cps + 0.5));
    case Wraps: return fs::q(r.wraps);
    case Original: return text(r.original);
    case Text: return text(r.text);
    }
    return {};
}

// Kainote's spell check in the grid: the text column's misspelled words get a red line. Words come
// from the full line (a drawing after \p1 is no words), mapped onto the shown text, whose tag blocks
// may be a ☀ or gone.
void Grid::underlineMisspelled(QPainter &p, const fsb::Row &r, const QString &shown, const QRect &rect) {
    auto *sp = spell::get();
    if (!sp) return;
    if (m_spellGen != spell::generation()) {
        m_misspelled.clear();
        m_spellGen = spell::generation();
    }
    const QString raw = fs::q(r.text);
    auto it = m_misspelled.constFind(raw);
    if (it == m_misspelled.cend()) {
        if (m_misspelled.size() > 20000) m_misspelled.clear();
        it = m_misspelled.insert(raw, fs::vec(sp->misspelled(r.text)));
    }
    if (it->empty()) return;
    const QFontMetrics fm = p.fontMetrics();
    p.save();
    p.setPen(QPen(QColor(0xe0, 0x40, 0x40), 1));
    p.setClipRect(rect);
    const int y = rect.center().y() + (fm.ascent() - fm.descent()) / 2 + 2; // under the baseline
    eachSpan(*it, raw, shown, m_tagMode, rect, fm, [&](int x, int w) { p.drawLine(x, y, x + w, y); });
    p.restore();
}

void Grid::paintEvent(QPaintEvent *) {
    QPainter p(viewport());
    const int rh = rowHeight(), w = viewport()->width();
    p.fillRect(viewport()->rect(), T.background);
    p.setFont(font());
    const auto styles = m_tab->s().styles();
    QSet<QString> styleNames;
    for (const auto &st : styles) styleNames.insert(fs::q(st.name));

    // header
    p.fillRect(0, 0, w, rh, T.header);
    int x = 0;
    for (size_t k = 0; k < m_cols.size(); ++k) {
        p.setPen(T.headerText);
        p.drawText(QRect(x + Pad, 0, m_widths[k] - 2 * Pad, rh), Qt::AlignVCenter | Qt::AlignLeft,
                   colName(m_cols[k]));
        p.setPen(T.lines);
        p.drawLine(x + m_widths[k] - 1, 0, x + m_widths[k] - 1, rh);
        x += m_widths[k];
    }

    const int first = verticalScrollBar()->value();
    for (int vi = first, y = rh; vi < int(m_vis.size()) && y < viewport()->height(); ++vi, y += rh) {
        const size_t i = m_vis[vi];
        const fsb::Row r = m_tab->s().row(i);
        p.fillRect(0, y, w, rh, r.comment ? T.comment : T.dialogue);
        if (m_onVideo.contains(i)) p.fillRect(0, y, w, rh, T.onVideo);
        const auto cmp = m_compare.find(i);
        if (cmp != m_compare.end()) p.fillRect(0, y, w, rh, T.compare);
        // a gap in the visible rows marks a filtered block, "+" to expand; "-" folds it again
        const char *fold = !filterOn ? nullptr : gapBefore(vi) ? "+" : expandedStart(i) ? "-" : nullptr;
        const QString marks = fs::q(r.markers);
        x = 0;
        for (size_t k = 0; k < m_cols.size(); ++k) {
            const int c = m_cols[k];
            const QRect cr(x, y, m_widths[k], rh);
            if (c != Num && marks.contains("[tree_description]")) {
                // Kainote: a tree's description fills the row behind the arrow that folds it
                const QRect rest = QRect(x, y, w - x, rh).adjusted(Pad, 0, -Pad, 0);
                const bool closed = std::ranges::binary_search(m_folded, i + 1);
                p.fillRect(QRect(x, y, w - x, rh), T.comment);
                p.setPen(T.text);
                p.drawText(rest, Qt::AlignVCenter | Qt::AlignLeft,
                           p.fontMetrics().elidedText(QString(closed ? "▶  " : "▼  ") + fs::q(r.text), Qt::ElideRight,
                                                      rest.width()));
                break;
            }
            if (c == Num) {
                QColor lc = r.doubtful ? T.labelDoubtful
                          : r.state == 1 ? T.labelModified
                          : r.state == 2 ? T.labelSaved
                                         : T.labelNormal;
                p.fillRect(cr, lc);
                if (fold) {
                    p.fillRect(cr.left(), cr.top() + 2, 8, 8, T.text);
                    p.setPen(T.background);
                    p.drawText(QRect(cr.left(), cr.top() + 2, 8, 8), Qt::AlignCenter, fold);
                }
                if (int(i) == m_marker) p.fillRect(cr.right() - 4, cr.top(), 4, rh, T.collision);
            } else if ((c == Cps && int(r.cps + 0.5) > limits::cps()) ||
                       (c == Style && !styleNames.isEmpty() && !styleNames.contains(fs::q(r.style))) ||
                       (c == Wraps && limits::wrapsOver(fs::q(r.wraps)))) {
                p.fillRect(cr, T.error);
            }
            const bool collide = (c == Text || c == Original) && m_collisions.contains(i);
            p.setPen(collide ? T.collision : T.text);
            const QString shown = cell(r, c, i);
            if (c == Text && cmp != m_compare.end()) {
                const QRect tx = cr.adjusted(Pad, 0, -Pad, 0);
                p.save();
                p.setClipRect(tx);
                eachSpan(cmp->second, fs::q(r.text), shown, m_tagMode, tx, p.fontMetrics(),
                         [&](int x, int w) { p.fillRect(x, cr.top() + 2, std::max(w, 2), rh - 4, T.compareText); });
                p.restore();
            }
            p.drawText(cr.adjusted(Pad, 0, -Pad, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(shown, Qt::ElideRight, cr.width() - 2 * Pad));
            if (c == Text) underlineMisspelled(p, r, shown, cr.adjusted(Pad, 0, -Pad, 0));
            p.setPen(T.lines);
            p.drawLine(cr.right(), cr.top(), cr.right(), cr.bottom());
            x += m_widths[k];
        }
        p.setPen(T.lines);
        p.drawLine(0, y + rh - 1, w, y + rh - 1);
        if (marks.contains("[tree_opened]") && !m_widths.empty()) { // an open tree's lines, framed
            p.setPen(T.text);
            p.drawLine(m_widths[0], y, m_widths[0], y + rh - 1);
            p.drawLine(w - 1, y, w - 1, y + rh - 1);
        }
        if (m_linked.contains(i)) p.fillRect(0, y + 1, 3, rh - 2, T.active);
        if (m_tab->isSelected(i)) p.fillRect(0, y, w, rh, T.selection);
        if (int(i) == m_tab->active()) {
            p.setPen(QPen(T.active, 2));
            p.drawRect(QRect(1, y + 1, m_widths.empty() ? 0 : m_widths[0] - 2, rh - 2));
        }
    }
}

void Grid::ensureVisible(int row) {
    auto it = std::find(m_vis.begin(), m_vis.end(), size_t(row));
    if (it == m_vis.end()) return;
    const int vi = int(it - m_vis.begin());
    auto *sb = verticalScrollBar();
    if (vi < sb->value()) sb->setValue(vi);
    else if (vi >= sb->value() + sb->pageStep()) sb->setValue(vi - sb->pageStep() + 1);
}

void Grid::clickRow(int vi, Qt::KeyboardModifiers mods) {
    const size_t row = m_vis[vi];
    if (mods & Qt::ShiftModifier) {
        const int a = std::min(m_anchor, vi), b = std::max(m_anchor, vi);
        std::vector<size_t> rows;
        for (int k = a; k <= b; ++k) rows.push_back(m_vis[k]);
        m_tab->setSelection(rows, int(row));
        return;
    }
    m_anchor = vi;
    if (mods & Qt::ControlModifier) {
        m_tab->toggleSelected(row);
        m_tab->setActive(int(row), false);
    } else {
        m_tab->setActive(int(row));
    }
}

void Grid::mousePressEvent(QMouseEvent *e) {
    setFocus();
    const int vi = rowAt(int(e->position().y()));
    const int col = colAt(int(e->position().x()));
    if (vi < 0) return;
    const size_t row = m_vis[vi];
    if (e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && isTreeHead(row))
        return toggleTree(row); // Kainote: a click on the description opens or closes the tree
    if (e->button() == Qt::LeftButton) {
        if (col == Num && e->position().x() < 10 && filterOn && (gapBefore(vi) || expandedStart(row))) {
            if (gapBefore(vi))
                for (size_t r = vi > 0 ? m_vis[vi - 1] + 1 : 0; r < row; ++r) m_expanded.insert(r);
            else
                for (size_t r = row; m_expanded.erase(r); ++r) {}
            applyFilter();
            return;
        }
        if ((e->modifiers() & Qt::AltModifier) && m_tab->isSelected(row)) {
            m_dragFrom = vi;
            return;
        }
        clickRow(vi, e->modifiers());
        const auto r = m_tab->s().row(row);
        if (col == Num) m_tab->seekVideo(e->modifiers() & Qt::ShiftModifier ? r.end : r.start);
        else if (col == Start || col == End) m_tab->seekVideo(col == Start ? r.start : r.end);
    } else if (e->button() == Qt::MiddleButton) {
        if (e->modifiers() & Qt::ControlModifier) m_tab->setActive(int(row), false);
        else m_tab->playLine(int(row));
    } else if (e->button() == Qt::RightButton && col == Num) {
        m_marker = int(row); // sync marker for Shift Times "by video/audio"
        viewport()->update();
    }
}

void Grid::mouseMoveEvent(QMouseEvent *e) {
    if (m_dragFrom < 0 || !(e->buttons() & Qt::LeftButton)) return;
    const int vi = rowAt(int(e->position().y()));
    if (vi < 0 || vi == m_dragFrom) return;
    const auto moved = m_tab->s().move_rows(fs::slice(m_tab->selection()), vi - m_dragFrom);
    m_dragFrom = vi;
    m_tab->changed();
    const auto rows = fs::vec(moved);
    if (!rows.empty()) m_tab->setSelection(rows, int(rows.front()));
}

void Grid::mouseReleaseEvent(QMouseEvent *) { m_dragFrom = -1; }

void Grid::mouseDoubleClickEvent(QMouseEvent *e) {
    const int vi = rowAt(int(e->position().y()));
    if (vi < 0) return;
    const size_t row = m_vis[vi];
    if (e->modifiers() == Qt::NoModifier && isTreeHead(row)) return; // its press toggled the tree
    const int col = colAt(int(e->position().x()));
    const auto r = m_tab->s().row(row);
    m_tab->seekVideo(col == End ? r.end : r.start);
}

bool Grid::isTreeHead(size_t row) { return fs::q(m_tab->s().row(row).markers).contains("[tree_description]"); }

void Grid::toggleTree(size_t row) {
    m_tab->s().toggle_tree(row);
    m_tab->changed();
    // a line the tree folded away gives the active mark to the description (Kainote)
    if (const int a = m_tab->active(); a > int(row) && std::ranges::binary_search(m_folded, size_t(a)))
        m_tab->setActive(int(row));
}

// Kainote's menu on a tree's description line
void Grid::treeMenu(const QPoint &global, size_t row) {
    QMenu m(this);
    m.addAction(tr("Copy tree"), this, [this, row] {
        auto rows = fs::vec(m_tab->s().tree_members(row));
        rows.insert(rows.begin(), row);
        QApplication::clipboard()->setText(fs::q(m_tab->s().copy_rows(fs::slice(rows))));
    });
    m.addAction(tr("Change description"), this, [this, row] {
        auto r = m_tab->s().row(row);
        bool ok = false;
        const QString d = QInputDialog::getText(this, tr("Tree"), tr("Description:"), QLineEdit::Normal, fs::q(r.text), &ok);
        if (!ok) return;
        r.text = fs::rs(d);
        m_tab->s().set_row(row, r, "");
        m_tab->changed();
    });
    m.addAction(tr("Select tree lines"), this, [this, row] {
        const auto rows = fs::vec(m_tab->s().tree_members(row));
        if (rows.empty()) return;
        if (std::ranges::binary_search(m_folded, rows.front())) toggleTree(row); // shown to be selected
        m_tab->setSelection(rows, int(rows.front()));
    });
    m.addAction(tr("Remove tree"), this, [this, row] {
        m_tab->s().remove_tree(row);
        m_tab->changed();
    });
    m.exec(global);
}

void Grid::wheelEvent(QWheelEvent *e) {
    verticalScrollBar()->setValue(verticalScrollBar()->value() - e->angleDelta().y() / 120 * 3);
}

void Grid::moveSelection(int delta) {
    const auto moved = fs::vec(m_tab->s().move_rows(fs::slice(m_tab->selection()), delta));
    m_tab->changed();
    if (!moved.empty()) {
        const int act = m_tab->active() + int(moved.front()) - int(m_tab->selection().front());
        m_tab->setSelection(moved, act);
    }
}

void Grid::keyPressEvent(QKeyEvent *e) {
    if (m_vis.empty()) return;
    auto pos = std::find(m_vis.begin(), m_vis.end(), size_t(std::max(0, m_tab->active())));
    int vi = pos == m_vis.end() ? 0 : int(pos - m_vis.begin());
    const int page = verticalScrollBar()->pageStep();
    const bool alt = e->modifiers() & Qt::AltModifier;
    int to = vi;
    switch (e->key()) {
    case Qt::Key_Up:
        if (alt) return moveSelection(-1);
        to = vi - 1;
        break;
    case Qt::Key_Down:
        if (alt) return moveSelection(1);
        to = vi + 1;
        break;
    case Qt::Key_PageUp: to = vi - page; break;
    case Qt::Key_PageDown: to = vi + page; break;
    case Qt::Key_Home: to = 0; break;
    case Qt::Key_End: to = int(m_vis.size()) - 1; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: m_tab->edit()->focusText(); return;
    default:
        if (e->matches(QKeySequence::SelectAll)) {
            m_tab->setSelection(m_vis, m_tab->active());
            return;
        }
        if (e->matches(QKeySequence::Copy)) return copy();
        if (e->matches(QKeySequence::Cut)) return cut();
        if (e->matches(QKeySequence::Paste)) return paste();
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }
    to = std::clamp(to, 0, int(m_vis.size()) - 1);
    if (e->modifiers() & Qt::ShiftModifier) clickRow(to, Qt::ShiftModifier);
    else {
        m_anchor = to;
        m_tab->setActive(int(m_vis[to]));
    }
    ensureVisible(int(m_vis[to]));
}

// ---- actions ---------------------------------------------------------------

void Grid::copy() {
    QApplication::clipboard()->setText(fs::q(m_tab->s().copy_rows(fs::slice(m_tab->selection()))));
}

void Grid::cut() {
    copy();
    deleteRows();
}

void Grid::paste() {
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;
    const int at = m_tab->active() + 1;
    const auto rows = fs::vec(m_tab->s().paste_rows(size_t(at), fs::Utf8(text)));
    m_tab->changed();
    if (!rows.empty()) m_tab->setSelection(rows, int(rows.front()));
}

void Grid::duplicate() {
    const auto rows = fs::vec(m_tab->s().duplicate_rows(fs::slice(m_tab->selection())));
    m_tab->changed();
    if (!rows.empty()) m_tab->setSelection(rows, int(rows.front()));
}

void Grid::deleteRows() {
    const auto sel = m_tab->selection();
    if (sel.empty()) return;
    m_tab->s().delete_rows(fs::slice(sel));
    m_tab->changed();
    m_tab->setActive(std::min<int>(int(sel.front()), int(m_tab->s().row_count()) - 1));
}

void Grid::deleteText() {
    m_tab->s().clear_text(fs::slice(m_tab->selection()));
    m_tab->changed();
}

// timeMode: 0 from neighbours, 1 video time, 2 video frame
void Grid::insertRow(bool after, int timeMode) {
    auto &s = m_tab->s();
    const int act = std::max(0, m_tab->active());
    const bool empty = s.row_count() == 0;
    const fsb::Row r = empty ? fsb::Row{} : s.row(size_t(act));
    qint64 start, end;
    if (timeMode == 1) {
        start = m_tab->videoStartTime();
        end = start + 5000;
    } else if (timeMode == 2) {
        start = m_tab->videoStartTime();
        end = m_tab->videoEndTime();
    } else if (after) {
        start = r.end;
        end = start + 5000;
    } else {
        end = r.start;
        start = std::max<qint64>(0, end - 5000);
    }
    const size_t at = empty ? 0 : size_t(act + (after ? 1 : 0));
    const size_t row = s.insert_row(at, start, end);
    m_tab->changed();
    m_tab->setActive(int(row));
}

void Grid::selectVisibleOnVideo() {
    const auto rows = fs::vec(m_tab->s().rows_at(m_tab->videoTime()));
    if (!rows.empty()) m_tab->setSelection(rows, int(rows.front()));
}

// how: 0 at video time, 1 into frames, 2 characters, 3 words, 4 wraps
void Grid::splitRows(int how) {
    auto &s = m_tab->s();
    // every line's parts, split in one go
    std::vector<size_t> rows, counts;
    QStringList all;
    std::vector<std::int64_t> starts, ends;
    for (const size_t row : m_tab->selection()) {
        const fsb::Row r = s.row(row);
        QStringList parts;
        std::vector<std::int64_t> st, en;
        const QString text = fs::q(r.text);
        if (how == 0) {
            const qint64 t = m_tab->videoStartTime();
            if (t <= r.start || t >= r.end) continue;
            parts = {text, text};
            st = {r.start, t};
            en = {t, r.end};
        } else if (how == 1) {
            auto *v = m_tab->video();
            if (!v->hasVideo()) return;
            // one line per frame the line is on, cut halfway between frames (Aegisub's START/END)
            for (qint64 f = v->frameAtStart(r.start), last = v->frameAtEnd(r.end); f <= last; ++f) {
                parts << text;
                st.push_back(std::max<std::int64_t>(r.start, v->timeAtStart(f)));
                en.push_back(std::min<std::int64_t>(r.end, v->timeAtEnd(f)));
            }
        } else {
            if (how == 2) {
                for (QChar c : fs::q(fsb::strip_tags(r.text))) if (!c.isSpace()) parts << QString(c);
            } else if (how == 3) {
                parts = fs::q(fsb::strip_tags(r.text)).replace("\\N", " ").split(' ', Qt::SkipEmptyParts);
            } else {
                parts = text.split("\\N", Qt::SkipEmptyParts);
            }
            // times proportional to the characters of each part
            qint64 total = 0;
            for (const auto &p : parts) total += std::max<qsizetype>(1, p.size());
            qint64 t = r.start, acc = 0;
            for (const auto &p : parts) {
                acc += std::max<qsizetype>(1, p.size());
                st.push_back(t);
                t = r.start + (r.end - r.start) * acc / std::max<qint64>(1, total);
                en.push_back(t);
            }
        }
        if (parts.size() < 2) continue;
        rows.push_back(row);
        counts.push_back(size_t(parts.size()));
        all << parts;
        starts.insert(starts.end(), st.begin(), st.end());
        ends.insert(ends.end(), en.begin(), en.end());
    }
    if (rows.empty()) return;
    const auto texts = fs::rvec(all);
    const auto made = fs::vec(s.explode_rows(fs::slice(rows), fs::slice(counts), {texts.data(), texts.size()},
                                             {starts.data(), starts.size()},
                                             {ends.data(), ends.size()}));
    m_tab->changed();
    if (!made.empty()) {
        m_tab->setSelection(made, int(made.front()));
    }
}

void Grid::contextMenuEvent(QContextMenuEvent *e) {
    if (const int vi = rowAt(e->pos().y()); e->reason() == QContextMenuEvent::Mouse && vi >= 0 && isTreeHead(m_vis[vi]))
        return treeMenu(e->globalPos(), m_vis[vi]);
    if (colAt(e->pos().x()) == Num && e->reason() == QContextMenuEvent::Mouse) return; // sync marker
    contextMenu(e->globalPos());
}

void Grid::contextMenu(const QPoint &global) {
    auto &s = m_tab->s();
    auto *mw = m_tab->main();
    const auto sel = m_tab->selection();
    const bool tl = s.file_info().tl_mode;
    QMenu m(this);
    auto *ins = m.addMenu(tr("Insert lines"));
    ins->addAction(tr("Insert before"), this, [this] { insertRow(false, 0); });
    ins->addAction(tr("Insert after"), this, [this] { insertRow(true, 0); });
    ins->addAction(tr("Insert before with video time"), this, [this] { insertRow(false, 1); });
    ins->addAction(tr("Insert after with video time"), this, [this] { insertRow(true, 1); });
    ins->addAction(tr("Insert before with video frame time"), this, [this] { insertRow(false, 2); });
    ins->addAction(tr("Insert after with video frame time"), this, [this] { insertRow(true, 2); });
    m.addAction(mw->action("duplicate"));
    m.addAction(tr("Swap lines"), this, [this, sel] {
        m_tab->s().swap_rows(sel[0], sel[1]);
        m_tab->changed();
    })->setEnabled(sel.size() == 2);
    const bool canJoin = sel.size() >= 2 && sel.size() <= 20 && !tl;
    auto join = [this](uint8_t mode) {
        const auto r = m_tab->s().join_rows(fs::slice(m_tab->selection()), mode);
        m_tab->changed();
        if (r >= 0) m_tab->setActive(int(r));
    };
    m.addAction(tr("Join lines"), this, [join] { join(0); })->setEnabled(canJoin);
    m.addAction(tr("Join and keep first"), this, [join] { join(1); })->setEnabled(canJoin);
    m.addAction(tr("Join and keep last"), this, [join] { join(2); })->setEnabled(canJoin);
    m.addAction(tr("Join as karaoke"), this, [join] { join(3); })->setEnabled(canJoin);
    m.addAction(tr("Set times as continuous (previous line)"), this, [this] {
        m_tab->s().make_continuous(fs::slice(m_tab->selection()), false);
        m_tab->changed();
    });
    m.addAction(tr("Set times as continuous (next line)"), this, [this] {
        m_tab->s().make_continuous(fs::slice(m_tab->selection()), true);
        m_tab->changed();
    });
    auto *split = m.addMenu(tr("Split lines"));
    split->setEnabled(!tl);
    split->addAction(tr("at video time"), this, [this] { splitRows(0); });
    split->addAction(tr("into frames"), this, [this] { splitRows(1); });
    split->addAction(tr("into characters"), this, [this] { splitRows(2); });
    split->addAction(tr("into words"), this, [this] { splitRows(3); });
    split->addAction(tr("by wraps (\\N)"), this, [this] { splitRows(4); });
    m.addAction(tr("Select all lines visible on video"), this, [this] { selectVisibleOnVideo(); });
    m.addSeparator();
    m.addAction(tr("Copy"), QKeySequence::Copy, this, [this] { copy(); });
    m.addAction(tr("Cut"), QKeySequence::Cut, this, [this] { cut(); });
    m.addAction(tr("Paste"), QKeySequence::Paste, this, [this] { paste(); });
    m.addAction(mw->action("copyColumns"));
    m.addAction(mw->action("pasteOver"));
    m.addSeparator();
    auto *cols = m.addMenu(tr("Hide columns"));
    for (int c : {Layer, Start, End, Actor, Style, MarginL, MarginR, MarginV, Effect, Cps, Wraps}) {
        auto *a = cols->addAction(colName(c));
        a->setCheckable(true);
        a->setChecked(columnHidden(c));
        connect(a, &QAction::toggled, this, [this, c](bool on) { setColumnHidden(c, on); });
    }
    m.addAction(tr("Make tree"), this, [this, sel] {
        bool ok = false;
        const QString d = QInputDialog::getText(this, tr("Make tree"), tr("Description:"), QLineEdit::Normal, {}, &ok);
        if (!ok) return;
        const auto heads = fs::vec(m_tab->s().make_tree(fs::slice(sel), fs::Utf8(d)));
        m_tab->changed();
        if (!heads.empty()) m_tab->setActive(int(heads.front()));
    })->setEnabled(!sel.empty() && !tl);
    m.addAction(tr("Hide selected lines"), this, [this, sel] {
        filterSpec.hide_rows.clear();
        for (size_t r : sel) filterSpec.hide_rows.push_back(r);
        filterOn = true;
        m_expanded.clear();
        applyFilter();
    });
    auto *flt = m.addMenu(tr("Filtering"));
    auto check = [flt](const QString &t, bool v, auto f) {
        auto *a = flt->addAction(t);
        a->setCheckable(true);
        a->setChecked(v);
        QObject::connect(a, &QAction::toggled, f);
    };
    check(tr("Reverse filtering"), filterSpec.invert, [this](bool v) { filterSpec.invert = v; });
    auto *byStyle = flt->addMenu(tr("Hide lines with styles"));
    for (const auto &st : s.styles()) {
        const QString name = fs::q(st.name);
        auto *a = byStyle->addAction(name);
        a->setCheckable(true);
        a->setChecked(std::any_of(filterSpec.hide_styles.begin(), filterSpec.hide_styles.end(),
                                  [&](auto &x) { return fs::q(x) == name; }));
        connect(a, &QAction::toggled, this, [this, name](bool on) {
            // rust::Vec has no erase: rebuild without the style
            rust::Vec<rust::String> v;
            for (const auto &x : filterSpec.hide_styles)
                if (fs::q(x) != name) v.push_back(x);
            if (on) v.push_back(fs::rs(name));
            filterSpec.hide_styles = std::move(v);
        });
    }
    check(tr("Hide comments"), filterSpec.hide_comments, [this](bool v) { filterSpec.hide_comments = v; });
    if (tl) {
        check(tr("Show unconfirmed"), filterSpec.only_doubtful, [this](bool v) { filterSpec.only_doubtful = v; });
        check(tr("Show untranslated"), filterSpec.only_untranslated,
              [this](bool v) { filterSpec.only_untranslated = v; });
    }
    check(tr("Filter after loading subtitles"), QSettings().value("grid/filterAfterLoad").toBool(),
          [](bool v) { QSettings().setValue("grid/filterAfterLoad", v); });
    flt->addSeparator();
    flt->addAction(tr("Filter"), this, [this] {
        filterOn = true;
        m_expanded.clear();
        applyFilter();
        QStringList styles;
        for (const auto &x : filterSpec.hide_styles) styles << fs::q(x);
        QSettings st;
        st.setValue("grid/filterStyles", styles);
        st.setValue("grid/filterComments", filterSpec.hide_comments);
        st.setValue("grid/filterInvert", filterSpec.invert);
    });
    flt->addAction(tr("Turn off filtering"), this, [this] {
        filterOn = false;
        filterSpec.hide_rows.clear(); // the options stay for the next filter (Kainote)
        applyFilter();
    });
    auto *ign = m.addAction(tr("Ignore filtering in some actions"));
    ign->setCheckable(true);
    ign->setChecked(ignoreFilterInActions);
    connect(ign, &QAction::toggled, this, [this](bool v) { ignoreFilterInActions = v; });
    if (tl) {
        m.addSeparator();
        m.addAction(tr("Paste translation text"), mw, [mw] { mw->pasteTranslation(); });
        m.addAction(mw->action("translationMatching"));
    }
    m.addSeparator();
    m.addAction(mw->action("subsFromVideo"));
    m.addSeparator();
    m.addAction(mw->action("deleteText"));
    m.addAction(mw->action("deleteLines"));
    m.exec(global);
}

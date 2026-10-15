// The subtitle grid (Kainote layout): custom-painted rows with number-cell
// states, collisions, lines visible on video, filtering and tag hiding.
#pragma once

#include "bridge.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <algorithm>
#include <map>
#include <set>

class TabPanel;

class Grid : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum Col { Num, Layer, Start, End, Style, Actor, MarginL, MarginR, MarginV, Effect, Cps, Wraps, Original, Text, ColCount };

    explicit Grid(TabPanel *tab);

    void refresh();
    const fsb::ColumnInfo &columnInfo(); // distinct styles/actors/effects, current with the document
    void ensureVisible(int row);
    void setAnchor(size_t row); // where Shift+click selects from (follows the active line, Aegisub)
    void setVideoTime(qint64 ms);
    // rows that differ from the compared tab (Kainote subtitle comparison)
    // rows that differ from the compared tab, with the differing parts of their text (Kainote)
    void setCompare(std::map<size_t, std::vector<size_t>> rows);
    // rows timed with the other split-view tab's active line, marked at the left edge
    void setLinked(std::set<size_t> rows);
    void setTagMode(int mode); // Aegisub's: 0 show tags, 1 simplify them to ☀, 2 hide them
    static QString colName(int c);
    void setColumnHidden(int c, bool hidden);
    bool columnHidden(int c) const { return m_userHidden.contains(c); }

    // filtering
    fsb::FilterSpec filterSpec;
    bool filterOn = false;
    bool ignoreFilterInActions = false;
    void applyFilter();
    // a newly loaded file: drops the old rows' filter, refilters with the options when
    // "Filter after loading subtitles" is on (Kainote)
    void resetFilter();
    std::vector<size_t> visibleRows() const { return m_vis; }
    int syncMarker() const { return m_marker; }

    // edit actions shared with the main menu
    void copy();
    void cut();
    void paste();
    void duplicate();
    void deleteRows();
    void deleteText();
    void insertRow(bool after, int timeMode);
    void moveSelection(int delta);
    void selectVisibleOnVideo();
    void splitRows(int how);
    void swapRows();
    void joinRows(int mode);
    void makeContinuous(bool next);
    void contextMenu(const QPoint &global);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    void layoutColumns();
    void updateScroll();
    int rowAt(int y) const;      // index into m_vis, -1 outside
    int colAt(int x) const;
    int rowHeight() const;
    QString cell(const fsb::Row &r, int col, size_t index) const;
    void underlineMisspelled(QPainter &p, const fsb::Row &r, const QString &shown, const QRect &rect);
    void clickRow(int vi, Qt::KeyboardModifiers mods);

    TabPanel *m_tab;
    fsb::ColumnInfo m_colInfo{}; // rows are fetched when painted, not copied (100k lines took 400 ms a change)
    std::vector<size_t> m_vis;
    std::vector<size_t> m_folded; // rows closed trees fold away (Kainote), sorted
    // filtered-out blocks the user expanded with "+" (Kainote), until the filter changes or rows
    // are added or removed (their numbers would point elsewhere)
    std::set<size_t> m_expanded;
    size_t m_rowCount = 0;
    // a block the filter hid before the row (the lines a closed tree folds are none)
    bool gapBefore(int vi) const {
        const size_t from = vi > 0 ? m_vis[vi - 1] + 1 : 0, to = m_vis[vi];
        const auto folded = std::lower_bound(m_folded.begin(), m_folded.end(), to) -
                            std::lower_bound(m_folded.begin(), m_folded.end(), from);
        return to - from > size_t(folded);
    }
    bool isTreeHead(size_t row);
    void toggleTree(size_t row);
    void treeMenu(const QPoint &global, size_t row);
    bool expandedStart(size_t row) const { return m_expanded.contains(row) && !(row && m_expanded.contains(row - 1)); }
    std::vector<int> m_cols;     // visible columns in order
    std::vector<int> m_widths;
    std::set<int> m_userHidden;
    std::set<size_t> m_collisions, m_onVideo, m_linked;
    std::map<size_t, std::vector<size_t>> m_compare;
    quint64 m_gen = ~0ull;
    qint64 m_videoMs = -1;
    int m_tagMode = 0;
    int m_marker = -1;
    int m_anchor = 0;
    int m_dragFrom = -1;         // Alt+drag moving rows
    int m_selectTo = -1;         // the row a left-button drag selects to
    QHash<QString, std::vector<size_t>> m_misspelled; // by line text: (start, length) pairs
    int m_spellGen = -1;
};

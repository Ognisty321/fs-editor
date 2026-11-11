// MainWindow's Edit menu: undo, find, sorting and the commands on lines, and the tab commands
// that are keys only.
#include "mainwindow_p.h"

#include "editbox.h"
#include "findreplace.h"
#include "grid.h"
#include "historydialog.h"
#include "pasteover.h"
#include "selectlines.h"
#include "tabpanel.h"

#include <QApplication>
#include <QClipboard>
#include <QMenu>

#include <algorithm>
#include <utility>
#include <vector>

void MainWindow::buildEditMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *edit = menus.edit = barMenu(tr("&Edit"));
    add("undo", tr("Undo"), QKeySequence("Ctrl+Z"), edit, [=] { tab()->undo(); });
    add("undoToSave", tr("Undo to last save"), {}, edit, [=] {
        for (int guard = 0; tab()->modified() && tab()->s().can_undo() && guard < 100000; ++guard) tab()->s().undo();
        tab()->reselect();
    });
    add("redo", tr("Redo"), QKeySequence("Ctrl+Y"), edit, [=] { tab()->redo(); });
    add("history", tr("History"), QKeySequence("Ctrl+Shift+H"), edit, [=] { HistoryDialog(tab()).exec(); });
    edit->addSeparator();
    // Aegisub's edit/line/copy, cut and paste: the grid's, as commands with keys of their own (the grid
    // keeps Ctrl+C/X/V, which text boxes need everywhere else)
    add("copyLines", tr("Copy lines"), {}, edit, [=] { tab()->grid()->copy(); });
    add("cutLines", tr("Cut lines"), {}, edit, [=] { tab()->grid()->cut(); });
    add("pasteLines", tr("Paste lines"), {}, edit, [=] { tab()->grid()->paste(); });
    edit->addSeparator();
    auto findDlg = [this] {
        if (!m_find) m_find = new FindDialog(this);
        return m_find;
    };
    add("findReplace", tr("Find and replace"), QKeySequence("Ctrl+H"), edit, [=] { findDlg()->showTab(1); });
    add("find", tr("Find"), QKeySequence("Ctrl+F"), edit, [=] { findDlg()->showTab(0); });
    add("findNext", tr("Find next"), QKeySequence("F3"), edit, [=, this] {
        if (m_find) m_find->findNext();
        else findDlg()->showTab(0);
    });
    add("correctErrors", tr("Correct minor errors..."), {}, edit, [this] { correctErrors(); });
    edit->addSeparator();
    const QString sortNames[] = {tr("Start time"), tr("End time"), tr("Styles"), tr("Actor"), tr("Effect"), tr("Layer")};
    for (bool selected : {false, true}) {
        auto *sm = subMenu(edit, selected ? tr("Sort selected lines") : tr("Sort all lines"));
        for (int k = 0; k < 6; ++k)
            add(QString("sort%1%2").arg(selected ? "Sel" : "All").arg(k), sortNames[k], {}, sm, [=] {
                tab()->s().sort_rows(uint8_t(k), selected, fs::slice(tab()->selection()));
                tab()->reselect(); // the same lines, where the sort put them
            });
    }
    add("selectLines", tr("Select lines"), {}, edit, [this] { SelectLinesDialog(this).exec(); });
    edit->addSeparator();
    // Line commands from the Kainote global hotkey table.
    auto *lines = menus.lines = subMenu(edit, tr("Lines"));
    add("duplicate", tr("Duplicate lines"), QKeySequence("Ctrl+D"), lines, [=] { tab()->grid()->duplicate(); });
    // the grid's context menu commands (Aegisub's subtitle/insert, grid/swap, edit/line/join, …)
    auto *insert = subMenu(lines, tr("Insert lines"));
    const char *insertNames[] = {QT_TR_NOOP("Insert before"),
                                 QT_TR_NOOP("Insert after"),
                                 QT_TR_NOOP("Insert before with video time"),
                                 QT_TR_NOOP("Insert after with video time"),
                                 QT_TR_NOOP("Insert before with video frame time"),
                                 QT_TR_NOOP("Insert after with video frame time")};
    const char *insertIds[] = {"insertBefore",     "insertAfter",       "insertBeforeVideo",
                               "insertAfterVideo", "insertBeforeFrame", "insertAfterFrame"};
    for (int k = 0; k < 6; ++k)
        add(insertIds[k], tr(insertNames[k]), {}, insert, [=] { tab()->grid()->insertRow(k % 2, k / 2); });
    add("swapLines", tr("Swap lines"), {}, lines, [=] { tab()->grid()->swapRows(); });
    // Aegisub's grid/move/up|down and their keys
    add("moveLinesUp", tr("Move lines up"), QKeySequence("Alt+Up"), lines, [=] { tab()->grid()->moveSelection(-1); });
    add("moveLinesDown", tr("Move lines down"), QKeySequence("Alt+Down"), lines,
        [=] { tab()->grid()->moveSelection(1); });
    const char *joinNames[] = {QT_TR_NOOP("Join lines"), QT_TR_NOOP("Join and keep first"),
                               QT_TR_NOOP("Join and keep last"), QT_TR_NOOP("Join as karaoke")};
    const char *joinIds[] = {"joinLines", "joinKeepFirst", "joinKeepLast", "joinKaraoke"};
    for (int k = 0; k < 4; ++k) add(joinIds[k], tr(joinNames[k]), {}, lines, [=] { tab()->grid()->joinRows(k); });
    add("continuousPrev", tr("Set times as continuous (previous line)"), {}, lines,
        [=] { tab()->grid()->makeContinuous(false); });
    add("continuousNext", tr("Set times as continuous (next line)"), {}, lines,
        [=] { tab()->grid()->makeContinuous(true); });
    auto *splitMenu = subMenu(lines, tr("Split lines"));
    const char *splitNames[] = {QT_TR_NOOP("Split lines before current frame"), QT_TR_NOOP("Split lines after current frame"),
                                QT_TR_NOOP("Split lines into frames"), QT_TR_NOOP("Split lines into characters"),
                                QT_TR_NOOP("Split lines into words"), QT_TR_NOOP("Split lines by wraps (\\N)")};
    const std::pair<const char *, int> splitIds[] = {{"splitBefore", 0}, {"splitAfter", 5}, {"splitFrames", 1},
                                                     {"splitChars", 2},  {"splitWords", 3}, {"splitWraps", 4}};
    for (int k = 0; k < 6; ++k)
        add(splitIds[k].first, tr(splitNames[k]), {}, splitMenu, [=] { tab()->grid()->splitRows(splitIds[k].second); });
    add("selectOnVideo", tr("Select all lines visible on video"), {}, lines,
        [=] { tab()->grid()->selectVisibleOnVideo(); });
    add("deleteText", tr("Delete text"), QKeySequence("Alt+Del"), lines, [=] { tab()->grid()->deleteText(); });
    add("deleteTextKeepTags", tr("Delete text, keep tags"), {}, lines, [=] {
        tab()->s().clear_text_keep_tags(fs::slice(tab()->selection()));
        tab()->changed();
    });
    add("deleteLines", tr("Delete lines"), QKeySequence("Shift+Del"), lines, [=] { tab()->grid()->deleteRows(); });
    // Aegisub edit/line/paste/over: onto the selection, or downwards from the active line
    add("pasteOver", tr("Paste lines over..."), QKeySequence("Ctrl+Shift+V"), lines, [=, this] {
        const QString text = QApplication::clipboard()->text();
        if (text.isEmpty() || tab()->active() < 0) return;
        const int fields = chooseFields(this);
        if (fields <= 0) return;
        std::vector<size_t> rows = tab()->selection();
        if (rows.size() < 2) {
            rows.clear();
            for (size_t r = size_t(tab()->active()); r < tab()->s().row_count(); ++r) rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end());
        const auto done = fs::vec(tab()->s().paste_over(fs::slice(rows), fs::Utf8(text), uint16_t(fields)));
        tab()->changed();
        if (!done.empty()) tab()->setSelection(done, int(done.front()));
    });
    // Kainote's copy columns: the chosen fields of the selected lines, comma separated, text last
    add("copyColumns", tr("Copy columns..."), QKeySequence("Ctrl+Shift+C"), lines, [=, this] {
        const int f = chooseFields(this, true);
        if (f <= 0) return;
        QStringList out;
        for (size_t i : tab()->selection()) {
            const auto r = tab()->s().row(i);
            QStringList l;
            const std::pair<int, QString> fields[] = {
                {1, r.comment ? "Comment" : "Dialogue"}, {2, QString::number(r.layer)},
                {4, fs::fmtTime(r.start)}, {8, fs::fmtTime(r.end)}, {16, fs::q(r.style)}, {32, fs::q(r.actor)},
                {64, QString::number(r.margin_l)}, {128, QString::number(r.margin_r)},
                {256, QString::number(r.margin_v)}, {512, fs::q(r.effect)}};
            for (const auto &[bit, value] : fields)
                if (f & bit) l << value;
            if (f & 2048) l << fs::q(fsb::strip_tags(r.text));
            else if (f & 1024) l << fs::q(r.text);
            out << l.join(',');
        }
        QApplication::clipboard()->setText(out.join('\n'));
    });
    add("prevLine", tr("Previous line"), QKeySequence("Ctrl+Up"), lines, [=] { tab()->setActive(tab()->active() - 1); });
    add("nextLine", tr("Next line"), QKeySequence("Ctrl+Down"), lines, [=] { tab()->setActive(tab()->active() + 1); });
    auto join = [=, this](int other) {
        const int a = tab()->active();
        const int b = a + other;
        if (a < 0 || b < 0 || size_t(b) >= tab()->s().row_count()) return;
        const auto r = tab()->s().join_rows(fs::slice({size_t(std::min(a, b)), size_t(std::max(a, b))}), 0);
        if (r < 0) return status(tr("These lines cannot be joined"));
        tab()->changed();
        tab()->setActive(int(r));
    };
    add("joinPrev", tr("Join with previous line"), QKeySequence("F4"), lines, [=] { join(-1); });
    add("joinNext", tr("Join with next line"), QKeySequence("F5"), lines, [=] { join(1); });
    // Aegisub edit/line/recombine and edit/line/split/by_karaoke
    add("recombine", tr("Recombine lines"), {}, lines, [=] {
        auto *t = tab();
        const auto left = t->s().recombine_rows(fs::slice(t->selection()));
        const std::vector<size_t> rows(left.begin(), left.end());
        t->changed();
        if (!rows.empty()) t->setSelection(rows, int(rows.front()));
    });
    add("splitKaraoke", tr("Split lines by karaoke"), {}, lines, [=] {
        auto *t = tab();
        const auto made = t->s().split_by_karaoke(fs::slice(t->selection()));
        if (made.empty()) return;
        const std::vector<size_t> rows(made.begin(), made.end());
        t->changed();
        t->setSelection(rows, int(rows.front()));
    });
    add("selectFromVideo", tr("Select line from video time"), QKeySequence("F2"), lines, [=] {
        const auto rows = tab()->s().rows_at(tab()->videoTime());
        if (!rows.empty()) tab()->setActive(int(rows[0]));
    });

    // Tabs (hotkeys only, like Kainote)
    add("newTab", tr("New tab"), QKeySequence("Ctrl+T"), nullptr, [this] { newTab(); });
    add("closeTab", tr("Close tab"), QKeySequence("Ctrl+W"), nullptr, [this] { closeTab(m_tabs->currentIndex()); });
    add("nextTab", tr("Next tab"), QKeySequence("Ctrl+PgDown"), nullptr,
        [this] { m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count()); });
    add("prevTab", tr("Previous tab"), QKeySequence("Ctrl+PgUp"), nullptr,
        [this] { m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count()); });

    addPanelKeys(subMenu(edit, tr("Edit box keys")), EditBox::keys(), "EditBox",
              [=](const char *id) { tab()->edit()->runCommand(id); });
}

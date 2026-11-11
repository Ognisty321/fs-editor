// Kainote's corrector of minor errors: the multireplacer's rules and their window.
#include "mainwindow_p.h"

#include "editbox.h"
#include "findreplace.h"
#include "grid.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Aegisub's Automation Manager: G(lobal, autoload) / L(oaded by hand) scripts, failed ones in red.
// Kainote's multireplacer (its minor error corrector): the regex rules ticked replaced in the lines
// chosen of this tab or every tab, one undo step per tab, or their matches listed first to tick the
// ones to replace. Rules live in corrector/rules, each with the case it writes its replacement in.
void MainWindow::correctErrors() {
    static QPointer<QDialog> open; // one, left open beside the editor
    if (open) {
        open->raise();
        open->activateWindow();
        return;
    }
    struct Rule {
        QString description, find, replace;
    };
    // Kainote's defaults; \b is a word boundary, $1 a group. All start unchecked.
    std::vector<Rule> defaults = {
        {tr("Remove spaces before punctuation"), " +([,.!?%])", "${1}"},
        {tr("Remove double spaces"), "  +", " "},
        {tr("Change four or more dots to an ellipsis"), "\\.{4,}", "..."},
        {tr("Change two dots to an ellipsis"), "(^|[^.])\\.\\.($|[^.])", "${1}...${2}"},
        {tr("Add a missing space after punctuation"), "([^.]\\.|[,!?%])([^\\s,.!?%\"'’”»)\\]}\\\\0-9-])",
         "${1} ${2}"},
        {tr("Remove Japanese honorifics"), " ?- ?(san|chan|kun|sama|nee|dono|senpai|sensei)\\b", ""},
    };
    const QString lang = QSettings().value("main/language").toString();
    if ((lang.isEmpty() ? QLocale() : QLocale(lang)).language() == QLocale::Polish)
        defaults.insert(defaults.end(),
                        {{tr("Change \"sie\" to \"się\""), "\\bsie\\b", "się"},
                         {tr("Change \"nie możliwe\" to \"niemożliwe\""), "\\bnie możliwe\\b", "niemożliwe"},
                         {tr("Change \"nie ważne\" to \"nieważne\""), "\\bnie ważne\\b", "nieważne"},
                         {tr("Fix misspellings of \"w ogóle\""), "\\b(w ?og[uo]le|wogóle)\\b", "w ogóle"},
                         {tr("Fix misspellings of \"będę\""), "\\b(bed[eę]|będe)\\b", "będę"}});

    auto *d = new QDialog(this);
    open = d;
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(tr("Correct minor errors"));
    d->resize(1000, 640);
    auto *v = new QVBoxLayout(d);
    auto *list = new QTreeWidget(d);
    list->setHeaderLabels({tr("Description"), tr("Find (regular expression)"), tr("Replace with"), tr("Match case"),
                           tr("Only text"), tr("Only tags"), tr("Case")});
    list->setRootIsDecorated(false);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(list, 2);
    // Kainote's case options; the replacement's case as FindSpec::case_rule, from 1
    const QStringList cases = {tr("Like the match"), tr("As written"), tr("Lower case"), tr("Upper case")};
    auto addRule = [list, cases](const QString &desc, const QString &find, const QString &repl, bool on,
                                 bool matchCase, bool onlyText, bool onlyTags, int caseRule) {
        auto *it = new QTreeWidgetItem(list, {desc, find, repl});
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        it->setCheckState(3, matchCase ? Qt::Checked : Qt::Unchecked);
        it->setCheckState(4, onlyText ? Qt::Checked : Qt::Unchecked);
        it->setCheckState(5, onlyTags ? Qt::Checked : Qt::Unchecked);
        it->setData(6, Qt::UserRole, std::clamp(caseRule, 1, 4));
        it->setText(6, cases[std::clamp(caseRule, 1, 4) - 1]);
        return it;
    };
    {
        QSettings st;
        const int n = st.beginReadArray("corrector/rules");
        for (int i = 0; i < n; ++i) {
            st.setArrayIndex(i);
            const bool matchCase = st.value("matchCase").toBool();
            addRule(st.value("description").toString(), st.value("find").toString(), st.value("replace").toString(),
                    st.value("enabled").toBool(), matchCase, st.value("onlyText", true).toBool(),
                    st.value("onlyTags").toBool(), st.value("case", matchCase ? 2 : 1).toInt());
        }
        st.endArray();
        if (!n)
            for (const Rule &r : defaults) addRule(r.description, r.find, r.replace, false, false, true, false, 1);
    }
    auto restore = [list, defaults, addRule] {
        for (const Rule &r : defaults)
            if (list->findItems(r.find, Qt::MatchExactly, 1).isEmpty())
                addRule(r.description, r.find, r.replace, false, false, true, false, 1);
    };
    for (int c = 1; c < 7; ++c) list->resizeColumnToContents(c);
    // text columns edit on a double click, the case cycles; only text and only tags exclude each other
    connect(list, &QTreeWidget::itemDoubleClicked, d, [list, cases](QTreeWidgetItem *it, int col) {
        if (col < 3) list->editItem(it, col);
        if (col != 6) return;
        const int c = it->data(6, Qt::UserRole).toInt() % 4 + 1;
        it->setData(6, Qt::UserRole, c);
        it->setText(6, cases[c - 1]);
    });
    connect(list, &QTreeWidget::itemChanged, d, [](QTreeWidgetItem *it, int col) {
        if ((col == 4 || col == 5) && it->checkState(col) == Qt::Checked) it->setCheckState(9 - col, Qt::Unchecked);
    });
    // the rules saved as they change, so closing the editor with the window open keeps them
    const auto save = [list] {
        QSettings st;
        st.beginWriteArray("corrector/rules", list->topLevelItemCount());
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            const auto *it = list->topLevelItem(i);
            st.setArrayIndex(i);
            st.setValue("description", it->text(0));
            st.setValue("find", it->text(1));
            st.setValue("replace", it->text(2));
            st.setValue("enabled", it->checkState(0) == Qt::Checked);
            st.setValue("matchCase", it->checkState(3) == Qt::Checked);
            st.setValue("onlyText", it->checkState(4) == Qt::Checked);
            st.setValue("onlyTags", it->checkState(5) == Qt::Checked);
            st.setValue("case", it->data(6, Qt::UserRole).toInt());
        }
        st.endArray();
    };
    connect(list->model(), &QAbstractItemModel::dataChanged, d, save);
    connect(list->model(), &QAbstractItemModel::rowsInserted, d, save);
    connect(list->model(), &QAbstractItemModel::rowsRemoved, d, save);

    auto row = [&] {
        auto *h = new QHBoxLayout;
        v->addLayout(h);
        return h;
    };
    auto button = [d](QHBoxLayout *h, const QString &text, auto fn) {
        auto *b = new QPushButton(text, d);
        connect(b, &QPushButton::clicked, d, fn);
        h->addWidget(b);
        return b;
    };
    auto *edits = row();
    button(edits, tr("Add"), [list, addRule] {
        auto *it = addRule(tr("New rule"), {}, {}, true, false, true, false, 1);
        list->setCurrentItem(it);
        list->editItem(it, 1);
    });
    button(edits, tr("Remove"), [list] { delete list->currentItem(); });
    button(edits, tr("Restore defaults"), restore);
    edits->addStretch();

    // Kainote's "Which lines": all, the selected, from the first selected on, or of the styles named
    auto *which = row();
    which->addWidget(new QLabel(tr("Lines:"), d));
    auto *scope = new QComboBox(d);
    scope->addItems({tr("All lines"), tr("Selected lines"), tr("From the selected line"), tr("Lines of the styles")});
    which->addWidget(scope);
    auto *styles = new QLineEdit(d);
    styles->setPlaceholderText(tr("Styles, separated by commas"));
    auto *pick = new QToolButton(d);
    pick->setText("+");
    pick->setToolTip(tr("Choose styles"));
    which->addWidget(pick);
    which->addWidget(styles, 1);
    const auto byStyles = [styles, pick](int i) {
        styles->setEnabled(i == 3);
        pick->setEnabled(i == 3);
    };
    byStyles(0);
    connect(scope, &QComboBox::currentIndexChanged, d, byStyles);
    connect(pick, &QToolButton::clicked, d, [this, d, styles] {
        if (!current()) return;
        QDialog pickD(d);
        pickD.setWindowTitle(tr("Choose styles"));
        auto *pv = new QVBoxLayout(&pickD);
        auto *names = new QListWidget(&pickD);
        const QStringList chosen = styles->text().split(',', Qt::SkipEmptyParts);
        for (const auto &style : current()->s().styles()) {
            auto *it = new QListWidgetItem(fs::q(style.name), names);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(chosen.contains(it->text()) ? Qt::Checked : Qt::Unchecked);
        }
        pv->addWidget(names);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &pickD);
        connect(bb, &QDialogButtonBox::accepted, &pickD, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &pickD, &QDialog::reject);
        pv->addWidget(bb);
        if (pickD.exec() != QDialog::Accepted) return;
        QStringList out;
        for (int i = 0; i < names->count(); ++i)
            if (names->item(i)->checkState() == Qt::Checked) out << names->item(i)->text();
        styles->setText(out.join(','));
    });
    // the rows of a tab the scope chooses (empty: every line); none when it chooses no line. As in
    // Kainote, the lines the grid's filter hides are left
    const auto rowsOf = [scope, styles](TabPanel *t) -> std::optional<std::vector<size_t>> {
        std::vector<size_t> rows;
        const size_t count = t->s().row_count();
        const auto vis = t->grid()->visibleRows();
        switch (scope->currentIndex()) {
        case 0:
            if (vis.size() == count) return rows;
            rows = vis;
            break;
        case 1:
            rows = t->selection();
            break;
        case 2: {
            const auto sel = t->selection();
            const size_t first = sel.empty() ? size_t(std::max(0, t->active())) : std::ranges::min(sel);
            for (size_t r = first; r < count; ++r) rows.push_back(r);
            break;
        }
        default: {
            QSet<QString> names;
            for (const QString &n : styles->text().split(',', Qt::SkipEmptyParts)) names.insert(n.trimmed());
            for (size_t r = 0; r < count; ++r)
                if (names.contains(fs::q(t->s().row(r).style))) rows.push_back(r);
        }
        }
        if (scope->currentIndex() != 0 && vis.size() < count) {
            std::vector<char> shown(count);
            for (size_t r : vis) shown[r] = 1;
            std::erase_if(rows, [&shown](size_t r) { return r >= shown.size() || !shown[r]; });
        }
        if (rows.empty()) return std::nullopt;
        return rows;
    };
    // the rules ticked, as searches over the rows; `ruleRows` gets the list row of each
    const auto specsFor = [list](const std::vector<size_t> &rows, std::vector<int> *ruleRows) {
        std::vector<fsb::FindSpec> specs;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            const auto *it = list->topLevelItem(i);
            if (it->checkState(0) != Qt::Checked || it->text(1).isEmpty()) continue;
            fsb::FindSpec &s = specs.emplace_back();
            s.pattern = fs::rs(it->text(1));
            s.replacement = fs::rs(it->text(2));
            s.regex = true;
            s.match_case = it->checkState(3) == Qt::Checked;
            s.skip_tags = it->checkState(4) == Qt::Checked;
            s.skip_text = it->checkState(5) == Qt::Checked;
            s.case_rule = uint8_t(it->data(6, Qt::UserRole).toInt());
            for (size_t r : rows) s.rows.push_back(r);
            if (ruleRows) ruleRows->push_back(i);
        }
        return specs;
    };

    auto *runs = row();
    auto *results = new QTreeWidget(d);
    results->setHeaderLabels({tr("Line"), tr("Text"), tr("Rule")});
    results->setUniformRowHeights(true);
    results->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    results->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    results->setItemDelegateForColumn(1, matchDelegate(results));
    v->addWidget(results, 3);
    auto *resultRow = row();
    // per file in the results: its tab and the searches its matches come from
    struct Found {
        QPointer<TabPanel> tab;
        std::vector<fsb::FindSpec> specs;
    };
    auto found = std::make_shared<std::vector<Found>>();
    // Kainote's Find errors: every match of the rules ticked, listed to tick the ones to replace
    const auto findIn = [=, this](const QList<TabPanel *> &tabs) {
        results->clear();
        found->clear();
        size_t total = 0;
        bool chosen = false;
        for (auto *t : tabs) {
            const auto rows = rowsOf(t);
            if (!rows) continue;
            chosen = true;
            std::vector<int> ruleRows;
            auto specs = specsFor(*rows, &ruleRows);
            if (specs.empty()) return status(tr("No rule is checked"));
            struct Hit {
                fsb::FindMatch m;
                size_t rule;
            };
            std::vector<Hit> hits;
            try {
                for (size_t k = 0; k < specs.size() && total + hits.size() < 20000; ++k)
                    for (auto &m : t->s().find_matches(specs[k], 20000 - total - hits.size())) hits.push_back({m, k});
            } catch (const rust::Error &e) {
                return error(QString::fromUtf8(e.what()));
            }
            if (hits.empty()) continue;
            std::ranges::stable_sort(hits, {}, [](const Hit &h) { return std::pair(h.m.row, h.m.start); });
            auto *file = new QTreeWidgetItem(results, {t->title()});
            file->setFirstColumnSpanned(true);
            file->setFlags(file->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            file->setData(0, Qt::UserRole, int(found->size()));
            file->setToolTip(0, t->path());
            for (const auto &h : hits) {
                auto *it = new QTreeWidgetItem(file, {QString::number(h.m.row + 1), fs::q(h.m.text),
                                                      list->topLevelItem(ruleRows[h.rule])->text(0)});
                it->setData(0, Qt::UserRole, qulonglong(h.m.row));
                it->setData(0, Qt::UserRole + 1, h.m.original);
                it->setData(1, Qt::UserRole, QPoint(int(h.m.start), int(h.m.end)));
                it->setData(2, Qt::UserRole, qulonglong(h.rule));
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(0, Qt::Checked);
            }
            file->setExpanded(true);
            found->push_back({t, std::move(specs)});
            total += hits.size();
        }
        if (!chosen) return status(tr("No lines to correct"));
        status(total ? tr("Errors found: %1").arg(total) : tr("Nothing found"));
    };
    // the rules ticked replaced in the lines chosen, one undo step per tab
    const auto replaceIn = [=, this](const QList<TabPanel *> &tabs) {
        size_t count = 0;
        bool chosen = false;
        for (auto *t : tabs) {
            const auto rows = rowsOf(t);
            if (!rows) continue;
            chosen = true;
            const auto specs = specsFor(*rows, nullptr);
            if (specs.empty()) return status(tr("No rule is checked"));
            try {
                const size_t n = t->s().replace_rules({specs.data(), specs.size()});
                if (n) t->changed();
                count += n;
            } catch (const rust::Error &e) {
                return error(QString::fromUtf8(e.what()));
            }
        }
        if (!chosen) return status(tr("No lines to correct"));
        results->clear(); // its places are stale now
        found->clear();
        status(tr("Replacements: %1").arg(count));
    };
    button(runs, tr("Find errors"), [=, this] { if (current()) findIn({current()}); });
    button(runs, tr("Find errors in all tabs"), [=, this] { findIn(tabs()); });
    button(runs, tr("Replace"), [=, this] { if (current()) replaceIn({current()}); })->setDefault(true);
    button(runs, tr("Replace in all tabs"), [=, this] { replaceIn(tabs()); });
    runs->addStretch();
    button(runs, tr("Close"), [d] { d->close(); });

    connect(results, &QTreeWidget::itemActivated, d, [this, found](QTreeWidgetItem *it) {
        auto *file = it->parent();
        if (!file) return;
        TabPanel *t = (*found)[file->data(0, Qt::UserRole).toInt()].tab;
        const auto r = it->data(0, Qt::UserRole).toULongLong();
        if (!t || r >= t->s().row_count()) return;
        showPanel(t);
        t->setActive(int(r));
        const QPoint at = it->data(1, Qt::UserRole).toPoint();
        t->edit()->selectText(at.x(), at.y() - at.x(), it->data(0, Qt::UserRole + 1).toBool());
    });
    const auto tickAll = [results](Qt::CheckState state) {
        for (int f = 0; f < results->topLevelItemCount(); ++f) results->topLevelItem(f)->setCheckState(0, state);
    };
    button(resultRow, tr("Check all"), [tickAll] { tickAll(Qt::Checked); });
    button(resultRow, tr("Uncheck all"), [tickAll] { tickAll(Qt::Unchecked); });
    // each match ticked replaced by its rule where it was found; lines changed since are left
    button(resultRow, tr("Replace ticked"), [=, this] {
        size_t replaced = 0, ticked = 0;
        for (int f = 0; f < results->topLevelItemCount(); ++f) {
            auto *file = results->topLevelItem(f);
            const Found &in = (*found)[file->data(0, Qt::UserRole).toInt()];
            std::vector<fsb::FindMatch> matches;
            std::vector<size_t> rules;
            for (int i = 0; i < file->childCount(); ++i) {
                auto *it = file->child(i);
                if (it->checkState(0) != Qt::Checked) continue;
                const QPoint at = it->data(1, Qt::UserRole).toPoint();
                fsb::FindMatch m;
                m.row = size_t(it->data(0, Qt::UserRole).toULongLong());
                m.original = it->data(0, Qt::UserRole + 1).toBool();
                m.start = size_t(at.x());
                m.end = size_t(at.y());
                m.text = fs::rs(it->text(1));
                matches.push_back(std::move(m));
                rules.push_back(size_t(it->data(2, Qt::UserRole).toULongLong()));
            }
            ticked += matches.size();
            if (matches.empty() || !in.tab) continue;
            try {
                const size_t n = in.tab->s().replace_rule_matches({in.specs.data(), in.specs.size()},
                                                                  {matches.data(), matches.size()},
                                                                  {rules.data(), rules.size()});
                if (n) in.tab->changed();
                replaced += n;
            } catch (const rust::Error &e) {
                error(QString::fromUtf8(e.what()));
            }
        }
        if (!ticked) return status(tr("No matches ticked"));
        results->clear(); // the places found are stale now: a new search before the next replacing
        found->clear();
        const QString done = tr("Replaced %1 of %2 matches ticked").arg(replaced).arg(ticked);
        status(replaced < ticked ? done + tr("; the lines changed since the search were left") : done);
    });
    resultRow->addStretch();
    d->show();
}

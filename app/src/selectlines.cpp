#include "selectlines.h"

#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

SelectLinesDialog::SelectLinesDialog(MainWindow *main) : QDialog(main), m_main(main) {
    setWindowTitle(tr("Select lines"));
    auto *v = new QVBoxLayout(this);
    auto *row = new QHBoxLayout;
    m_with = new QRadioButton(tr("With"), this);
    m_without = new QRadioButton(tr("Without"), this);
    QSettings st; // the last search, as Aegisub and Kainote keep it
    (st.value("select/with", true).toBool() ? m_with : m_without)->setChecked(true);
    m_find = new QComboBox(this); // with the recent searches (Kainote)
    m_find->setEditable(true);
    m_find->setInsertPolicy(QComboBox::NoInsert);
    m_find->addItems(st.value("select/recent").toStringList());
    m_find->setCurrentText(st.value("select/text").toString());
    auto *styles = new QToolButton(this);
    styles->setText("+");
    styles->setToolTip(tr("Choose styles"));
    connect(styles, &QToolButton::clicked, this, &SelectLinesDialog::chooseStyles);
    row->addWidget(m_with);
    row->addWidget(m_without);
    row->addWidget(m_find, 1);
    row->addWidget(styles);
    v->addLayout(row);
    auto *f = new QFormLayout;
    m_field = new QComboBox(this);
    // the times as written in the grid (Kainote's)
    m_field->addItems({tr("Text"), tr("Styles"), tr("Actor"), tr("Effect"), tr("Start time"), tr("End time")});
    m_field->setCurrentIndex(st.value("select/field", 0).toInt());
    f->addRow(tr("Field"), m_field);
    m_case = new QCheckBox(tr("Match case"), this);
    m_regex = new QCheckBox(tr("Regular expressions"), this);
    m_case->setChecked(st.value("select/case", false).toBool());
    m_regex->setChecked(st.value("select/regex", false).toBool());
    f->addRow(m_case, m_regex);
    m_exact = new QCheckBox(tr("Exact match"), this); // the whole field (Aegisub); empty: the empty ones
    m_exact->setChecked(st.value("select/exact", false).toBool());
    f->addRow(m_exact);
    m_dialogue = new QCheckBox(tr("Dialogue"), this);
    m_comments = new QCheckBox(tr("Comments"), this);
    m_dialogue->setChecked(st.value("select/dialogue", true).toBool());
    m_comments->setChecked(st.value("select/comments", false).toBool());
    f->addRow(m_dialogue, m_comments);
    m_mode = new QButtonGroup(this);
    const char *modes[] = {QT_TR_NOOP("Set selection"), QT_TR_NOOP("Add to selection"), QT_TR_NOOP("Deselect"),
                           QT_TR_NOOP("Intersect with selection")};
    QRadioButton *mode[4];
    for (int k = 0; k < 4; ++k) m_mode->addButton(mode[k] = new QRadioButton(tr(modes[k]), this), k);
    if (auto *b = m_mode->button(st.value("select/mode", 0).toInt())) b->setChecked(true);
    f->addRow(mode[0], mode[1]);
    f->addRow(mode[2], mode[3]);
    m_action = new QComboBox(this);
    m_action->addItems({tr("Do nothing"), tr("Copy"), tr("Cut"), tr("Move to beginning"), tr("Move to end"),
                        tr("Set as comment"), tr("Delete")});
    m_action->setCurrentIndex(st.value("select/action", 0).toInt());
    f->addRow(tr("Action"), m_action);
    v->addLayout(f);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    auto *allTabs = bb->addButton(tr("Select on all tabs"), QDialogButtonBox::ActionRole);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        apply({m_main->current()});
        accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply({m_main->current()}); });
    connect(allTabs, &QPushButton::clicked, this, [this] { apply(m_main->tabs()); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
}

void SelectLinesDialog::chooseStyles() {
    QDialog d(this);
    d.setWindowTitle(tr("Choose styles"));
    auto *v = new QVBoxLayout(&d);
    auto *list = new QListWidget(&d);
    for (const auto &style : m_main->current()->s().styles()) {
        auto *it = new QListWidgetItem(fs::q(style.name), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
    }
    v->addWidget(list);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return;
    QStringList names;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() != Qt::Checked) continue;
        QString n;
        for (const QChar c : list->item(i)->text()) {
            if (QStringLiteral("\\.+*?()|[]{}^$#&-~").contains(c)) n += '\\';
            n += c;
        }
        names << n;
    }
    if (names.isEmpty()) return;
    m_find->setCurrentText("^(" + names.join('|') + ")$");
    m_field->setCurrentIndex(1);
    m_regex->setChecked(true);
}

void SelectLinesDialog::apply(const QList<TabPanel *> &tabs) {
    if (tabs.isEmpty() || !tabs.front()) return;
    QSettings st;
    // the recent searches, the newest first (Kainote keeps 20)
    const QString text = m_find->currentText();
    QStringList recent = st.value("select/recent").toStringList();
    recent.removeAll(text);
    if (!text.isEmpty()) recent.prepend(text);
    while (recent.size() > 20) recent.removeLast();
    st.setValue("select/recent", recent);
    m_find->blockSignals(true);
    m_find->clear();
    m_find->addItems(recent);
    m_find->setCurrentText(text);
    m_find->blockSignals(false);
    size_t n = 0;
    for (auto *tab : tabs) n += applyTo(tab);
    m_main->status(tr("Selected %1 lines").arg(n));
}

size_t SelectLinesDialog::applyTo(TabPanel *tab) {
    auto &s = tab->s();
    const int field = m_field->currentIndex();
    fsb::FindSpec spec;
    spec.pattern = fs::rs(m_find->currentText());
    spec.regex = m_regex->isChecked();
    spec.match_case = m_case->isChecked();
    spec.field = uint8_t(std::min(field, 3));
    spec.include_comments = true;
    spec.at_start = spec.at_end = m_exact->isChecked();
    QSettings st;
    st.setValue("select/text", m_find->currentText());
    st.setValue("select/with", m_with->isChecked());
    st.setValue("select/field", m_field->currentIndex());
    st.setValue("select/case", m_case->isChecked());
    st.setValue("select/regex", m_regex->isChecked());
    st.setValue("select/exact", m_exact->isChecked());
    st.setValue("select/dialogue", m_dialogue->isChecked());
    st.setValue("select/comments", m_comments->isChecked());
    st.setValue("select/mode", m_mode->checkedId());
    st.setValue("select/action", m_action->currentIndex());
    // Aegisub's matching: lines of the checked kinds, with or without the text
    const auto spans = s.row_spans();
    std::vector<bool> hit(spans.size());
    if (field >= 4) {
        // the start or end time, as text (Kainote)
        // shortcut: Qt's regex, not the Rust one the other fields use; the same for anything a time holds
        const QString find = m_find->currentText();
        const QString pattern = m_regex->isChecked() ? find : QRegularExpression::escape(find);
        const QRegularExpression re(m_exact->isChecked() ? QRegularExpression::anchoredPattern(pattern) : pattern,
                                    m_case->isChecked() ? QRegularExpression::NoPatternOption
                                                        : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            m_main->error(re.errorString());
            return 0;
        }
        for (size_t i = 0; i < spans.size(); ++i) {
            const QString t = fs::fmtTime(field == 4 ? spans[i].start : spans[i].end);
            hit[i] = re.match(t).hasMatch();
        }
    } else {
        try {
            for (size_t r : s.find_all(spec)) hit[r] = true;
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
            return 0;
        }
    }
    // lines hidden by the filter or a folded tree are left alone (Kainote): no action reaches what is not shown
    const auto vis = tab->grid()->visibleRows();
    std::vector<bool> shown(spans.size(), vis.size() == spans.size());
    for (size_t r : vis)
        if (r < shown.size()) shown[r] = true;
    std::vector<bool> match(spans.size());
    for (size_t i = 0; i < spans.size(); ++i)
        match[i] = shown[i] && (spans[i].comment ? m_comments : m_dialogue)->isChecked() && hit[i] == m_with->isChecked();
    const auto cur = tab->selection();
    std::vector<size_t> v;
    if (m_mode->checkedId() == 0 || m_mode->checkedId() == 1) {
        for (size_t i = 0; i < spans.size(); ++i)
            if (match[i]) v.push_back(i);
        if (m_mode->checkedId() == 1) {
            std::vector<size_t> both;
            std::ranges::set_union(cur, v, std::back_inserter(both));
            v = std::move(both);
        }
    } else {
        const bool keep = m_mode->checkedId() == 3; // intersect keeps the matches, subtract the rest
        for (size_t r : cur)
            if (r < match.size() && match[r] == keep) v.push_back(r);
    }
    if (v.empty()) return 0;
    const int active = tab->active();
    tab->setSelection(v, active >= 0 && std::ranges::binary_search(v, size_t(active)) ? active : int(v.front()));
    switch (m_action->currentIndex()) {
    case 1: tab->grid()->copy(); break;
    case 2: tab->grid()->cut(); break;
    case 3: tab->grid()->moveSelection(-int(v.front())); break;
    case 4: tab->grid()->moveSelection(int(s.row_count() - v.size() - v.front())); break;
    case 5:
        s.set_rows_field(fs::slice(v), "comment", "1");
        tab->changed();
        break;
    case 6: tab->grid()->deleteRows(); break;
    }
    return v.size();
}

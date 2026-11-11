#include "spellcheckdialog.h"

#include "editbox.h"
#include "mainwindow.h"
#include "tabpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

SpellCheckDialog::SpellCheckDialog(MainWindow *main, TabPanel *tab) : QDialog(main), m_main(main), m_tab(tab) {
    setWindowTitle(tr("Spell checker"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *g = new QGridLayout(this);
    m_word = new QLineEdit(this);
    m_word->setReadOnly(true);
    m_with = new QLineEdit(this);
    m_suggest = new QListWidget(this);
    auto *lang = new QComboBox(this);
    spell::fillLanguages(lang, QSettings().value("spell/lang", "pl_PL").toString());
    m_skipComments = new QCheckBox(tr("Skip comments"), this);
    m_skipUpper = new QCheckBox(tr("Ignore UPPERCASE words"), this);
    m_skipComments->setChecked(QSettings().value("spell/skipComments", true).toBool());
    m_skipUpper->setChecked(QSettings().value("spell/skipUpper", false).toBool());
    g->addWidget(new QLabel(tr("Misspelled word:"), this), 0, 0);
    g->addWidget(m_word, 0, 1);
    g->addWidget(new QLabel(tr("Replace with:"), this), 1, 0);
    g->addWidget(m_with, 1, 1);
    g->addWidget(m_suggest, 2, 0, 1, 2);
    g->addWidget(new QLabel(tr("Language:"), this), 3, 0);
    g->addWidget(lang, 3, 1);
    g->addWidget(m_skipComments, 4, 0, 1, 2);
    g->addWidget(m_skipUpper, 5, 0, 1, 2);
    auto *buttons = new QVBoxLayout;
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, this);
        b->setAutoDefault(false);
        connect(b, &QPushButton::clicked, this, fn);
        buttons->addWidget(b);
        return b;
    };
    button(tr("Replace"), [this] { replace(false); })->setDefault(true);
    button(tr("Replace all"), [this] { replace(true); });
    button(tr("Ignore"), [this] {
        m_pos = m_start + int(m_word->text().size());
        findNext();
    });
    button(tr("Ignore all"), [this] {
        m_ignored << m_word->text();
        findNext();
    });
    button(tr("Add to dictionary"), [this] {
        spell::addWord(m_word->text());
        findNext();
    });
    // Aegisub: the word in "Replace with", when the user dictionary has it
    m_remove = button(tr("Remove from dictionary"), [this] {
        spell::removeWord(m_with->text());
        showWord(m_row, m_start, m_word->text());
        m_remove->setEnabled(false);
    });
    m_remove->setEnabled(false);
    connect(m_with, &QLineEdit::textChanged, this, [this](const QString &w) { m_remove->setEnabled(spell::userWords().contains(w)); });
    // Kainote's: the words added to the dictionary, those ticked removed
    button(tr("Words added to dictionary..."), [this] {
        QDialog d(this);
        d.setWindowTitle(tr("Words added to dictionary"));
        auto *v = new QVBoxLayout(&d);
        auto *list = new QListWidget(&d);
        for (const QString &w : spell::userWords()) {
            auto *it = new QListWidgetItem(w, list);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Unchecked);
        }
        v->addWidget(list);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Remove ticked"));
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        v->addWidget(bb);
        if (d.exec() != QDialog::Accepted) return;
        QStringList gone;
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->checkState() == Qt::Checked) gone << list->item(i)->text();
        spell::removeWords(gone);
        m_remove->setEnabled(spell::userWords().contains(m_with->text()));
    });
    buttons->addStretch();
    button(tr("Close"), [this] { close(); });
    g->addLayout(buttons, 0, 2, 6, 1);

    connect(m_suggest, &QListWidget::currentTextChanged, m_with, &QLineEdit::setText);
    connect(m_suggest, &QListWidget::itemDoubleClicked, this, [this] { replace(false); });
    connect(lang, &QComboBox::currentIndexChanged, this, [this, lang] {
        QSettings().setValue("spell/lang", lang->currentData());
        spell::reload();
        m_pos = m_start; // re-check the current word with the new dictionary
        findNext();
    });
    connect(m_skipComments, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("spell/skipComments", on); });
    connect(m_skipUpper, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("spell/skipUpper", on); });

    m_row = m_startRow = std::max(0, tab->active());
}

bool SpellCheckDialog::findNext() {
    auto *sp = spell::get();
    if (!m_tab || !sp) {
        QMessageBox::information(this, windowTitle(), tr("No dictionary is loaded. Pick one in the settings."));
        close();
        return false;
    }
    // Aegisub: a line picked in the grid meanwhile is where the check goes on, and round to
    if (const int a = m_tab->active(); a >= 0 && a != m_row) {
        m_row = m_startRow = a;
        m_pos = 0;
        m_wrapped = false;
    }
    auto &s = m_tab->s();
    const int n = int(s.row_count());
    while (true) {
        for (; m_row < n; ++m_row, m_pos = 0) {
            if (m_wrapped && m_row > m_startRow) break;
            const fsb::Row r = s.row(size_t(m_row));
            if (r.comment && m_skipComments->isChecked()) continue;
            const QString text = fs::q(r.text);
            const auto bad = sp->misspelled(rust::Str(r.text));
            for (size_t k = 0; k + 1 < bad.size(); k += 2) {
                const int start = int(bad[k]);
                const QString word = text.mid(start, int(bad[k + 1]));
                if (start < m_pos || m_ignored.contains(word)) continue;
                if (m_skipUpper->isChecked() && word == word.toUpper()) continue;
                showWord(m_row, start, word);
                return true;
            }
        }
        if (m_wrapped || m_startRow == 0) break;
        m_wrapped = true; // Aegisub: continue from the top up to where we started
        m_row = 0;
        m_pos = 0;
    }
    QMessageBox::information(this, windowTitle(), tr("Spell check of this script is complete."));
    close();
    return false;
}

void SpellCheckDialog::showWord(int row, int start, const QString &word) {
    m_row = row;
    m_start = start;
    m_word->setText(word);
    m_suggest->clear();
    for (const auto &w : spell::get()->suggest(fs::Utf8(word))) m_suggest->addItem(fs::q(w));
    if (m_suggest->count()) m_suggest->setCurrentRow(0);
    else m_with->setText(word);
    m_tab->setActive(row);
    m_tab->edit()->selectText(start, int(word.size()));
}

void SpellCheckDialog::replace(bool all) {
    if (!m_tab || m_word->text().isEmpty()) return;
    auto &s = m_tab->s();
    const QString word = m_word->text(), with = m_with->text();
    std::vector<size_t> rows;
    QStringList texts;
    auto fix = [&](size_t i, int only) {
        QString text = fs::q(s.row(i).text);
        const auto bad = spell::get()->misspelled(fs::Utf8(text));
        bool changed = false;
        for (size_t k = bad.size(); k >= 2; k -= 2) { // from the end: earlier offsets stay valid
            const int start = int(bad[k - 2]), len = int(bad[k - 1]);
            if ((only >= 0 && start != only) || text.mid(start, len) != word) continue;
            text.replace(start, len, with);
            changed = true;
        }
        if (!changed) return;
        rows.push_back(i);
        texts << text;
    };
    if (all) {
        // only the lines with the word go through the checker, and all of them change in one edit
        fsb::FindSpec f{};
        f.pattern = fs::rs(word);
        f.match_case = true;
        f.include_comments = !m_skipComments->isChecked();
        for (size_t i : s.find_all(f)) fix(i, -1);
    } else {
        fix(size_t(m_row), m_start);
    }
    const auto t = fs::rvec(texts);
    s.set_texts(fs::slice(rows), {t.data(), t.size()}, "Spell check");
    m_tab->changed();
    m_pos = m_start + int(with.size());
    if (all) m_ignored << word; // nothing left to find, unless the replacement is itself unknown
    findNext();
}

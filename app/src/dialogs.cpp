#include "dialogs.h"

#include "audiobox.h"
#include "editbox.h"
#include "grid.h"
#include "icons.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QtConcurrentRun>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <array>
#include <memory>
#include <numeric>

// ---- tag buttons --------------------------------------------------------------

TagButtonDialog::TagButtonDialog(const QString &name, const QString &tag, int mode, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Tag button"));
    auto *f = new QFormLayout(this);
    m_tag = new QLineEdit(tag, this);
    m_name = new QLineEdit(name, this);
    m_mode = new QComboBox(this);
    m_mode->addItems({tr("Tag inserted in place of cursor"), tr("Insert tag at text beginning"), tr("Plain text")});
    m_mode->setCurrentIndex(mode);
    f->addRow(tr("Enter ASS tag"), m_tag);
    f->addRow(tr("Button name"), m_name);
    f->addRow(tr("Mode"), m_mode);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto *del = bb->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(del, &QPushButton::clicked, this, [this] { done(Delete); });
    f->addRow(bb);
}
QString TagButtonDialog::name() const { return m_name->text().remove('|'); }
QString TagButtonDialog::tag() const { return m_tag->text().remove('|'); }
int TagButtonDialog::mode() const { return m_mode->currentIndex(); }

// ---- find / replace -----------------------------------------------------------

FindDialog::FindDialog(MainWindow *main) : QDialog(main), m_main(main) {
    setWindowTitle(tr("Find and replace"));
    auto *v = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(new QWidget, tr("Find"));
    m_tabs->addTab(new QWidget, tr("Find and replace"));
    m_tabs->addTab(new QWidget, tr("Find in files"));
    v->addWidget(m_tabs);
    auto *f = new QFormLayout;
    m_find = new QComboBox(this);
    m_find->setEditable(true);
    m_replace = new QComboBox(this);
    m_replace->setEditable(true);
    QSettings st;
    m_find->addItems(st.value("find/recent").toStringList());
    m_replace->addItems(st.value("find/recentReplace").toStringList());
    f->addRow(tr("Search text"), m_find);
    auto *replLabel = new QLabel(tr("Replace with"), this);
    f->addRow(replLabel, m_replace);
    v->addLayout(f);
    auto *filesBox = new QWidget(this);
    auto *ff = new QFormLayout(filesBox);
    ff->setContentsMargins(0, 0, 0, 0);
    auto *dirRow = new QHBoxLayout;
    m_dir = new QLineEdit(st.value("find/dir").toString(), filesBox);
    auto *browse = new QToolButton(filesBox);
    browse->setText("...");
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder"), m_dir->text());
        if (!d.isEmpty()) m_dir->setText(QDir::toNativeSeparators(d));
    });
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    ff->addRow(tr("Folder"), dirRow);
    m_filter = new QLineEdit(st.value("find/filter", "*.ass *.ssa *.srt").toString(), filesBox);
    ff->addRow(tr("Files"), m_filter);
    m_subdirs = new QCheckBox(tr("Include subfolders"), filesBox);
    m_subdirs->setChecked(st.value("find/subdirs", true).toBool());
    ff->addRow(m_subdirs);
    v->addWidget(filesBox);
    auto *checks = new QGridLayout;
    m_case = new QCheckBox(tr("Match case"), this);
    m_regex = new QCheckBox(tr("Regular expressions"), this);
    m_comments = new QCheckBox(tr("Include comments"), this);
    m_skipTags = new QCheckBox(tr("Skip tags"), this);
    m_skipText = new QCheckBox(tr("Skip text"), this);
    m_skipText->setToolTip(tr("Search only inside the {} blocks"));
    m_atStart = new QCheckBox(tr("Start of text"), this);
    m_atEnd = new QCheckBox(tr("End of text"), this);
    for (int i = 0; auto *c : {m_case, m_regex, m_comments, m_skipTags, m_skipText, m_atStart, m_atEnd}) {
        checks->addWidget(c, i / 4, i % 4);
        ++i;
    }
    connect(m_skipTags, &QCheckBox::toggled, m_skipText, [this](bool on) { if (on) m_skipText->setChecked(false); });
    connect(m_skipText, &QCheckBox::toggled, m_skipTags, [this](bool on) { if (on) m_skipTags->setChecked(false); });
    v->addLayout(checks);
    auto *groups = new QHBoxLayout;
    auto *fg = new QGroupBox(tr("In field"), this);
    auto *fl = new QVBoxLayout(fg);
    const char *fields[] = {QT_TR_NOOP("Text"), QT_TR_NOOP("Styles"), QT_TR_NOOP("Actor"), QT_TR_NOOP("Effect")};
    for (int k = 0; k < 4; ++k) fl->addWidget(m_field[k] = new QRadioButton(tr(fields[k]), fg));
    m_field[0]->setChecked(true);
    auto *lg = new QGroupBox(tr("Lines"), this);
    auto *ll = new QVBoxLayout(lg);
    const char *lines[] = {QT_TR_NOOP("All"), QT_TR_NOOP("Selected"), QT_TR_NOOP("From selected")};
    for (int k = 0; k < 3; ++k) ll->addWidget(m_lines[k] = new QRadioButton(tr(lines[k]), lg));
    m_lines[0]->setChecked(true);
    groups->addWidget(fg);
    groups->addWidget(lg);
    v->addLayout(groups);
    auto *bb = new QHBoxLayout;
    auto *find = new QPushButton(tr("Find"), this);
    auto *findAll = new QPushButton(tr("Find all"), this);
    auto *findOpen = new QPushButton(tr("Find in all open subtitles"), this);
    auto *findFiles = m_findFiles = new QPushButton(tr("Find in files"), this);
    auto *replNext = new QPushButton(tr("Replace next"), this);
    auto *replAll = new QPushButton(tr("Replace all"), this);
    auto *replOpen = new QPushButton(tr("Replace in all open subtitles"), this);
    auto *close = new QPushButton(tr("Close"), this);
    for (auto *b : {find, findAll, findOpen, findFiles, replNext, replAll, replOpen, close}) bb->addWidget(b);
    v->addLayout(bb);
    connect(find, &QPushButton::clicked, this, &FindDialog::findNext);
    connect(findAll, &QPushButton::clicked, this, [this] { this->findAll({m_main->current()}); });
    connect(findOpen, &QPushButton::clicked, this, [this] { this->findAll(m_main->tabs()); });
    connect(findFiles, &QPushButton::clicked, this, &FindDialog::findInFiles);
    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    auto remember = [this] {
        QSettings st;
        for (auto [box, key] : {std::pair{m_find, "find/recent"}, std::pair{m_replace, "find/recentReplace"}}) {
            QStringList l = st.value(key).toStringList();
            l.removeAll(box->currentText());
            l.prepend(box->currentText());
            st.setValue(key, l.mid(0, 20));
        }
    };
    connect(replNext, &QPushButton::clicked, this, [this, remember] {
        remember();
        auto *tab = m_main->current();
        auto spec = this->spec();
        const int act = tab->active();
        // replace in the active line if it matches, then move to the next match
        spec.rows = {size_t(act)};
        try {
            if (tab->s().replace_all(spec)) tab->changed();
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
            return;
        }
        findNext();
    });
    connect(replAll, &QPushButton::clicked, this, [this, remember] {
        remember();
        auto *tab = m_main->current();
        try {
            const size_t n = tab->s().replace_all(spec());
            tab->changed();
            m_main->status(tr("Replacements: %1").arg(n));
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
        }
    });
    connect(replOpen, &QPushButton::clicked, this, [this, remember] {
        remember();
        size_t n = 0;
        for (auto *tab : m_main->tabs()) {
            auto s = spec();
            s.rows.clear();
            try {
                n += tab->s().replace_all(s);
            } catch (const rust::Error &e) {
                m_main->error(QString::fromUtf8(e.what()));
                return;
            }
            tab->changed();
        }
        m_main->status(tr("Replacements: %1").arg(n));
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [=, this](int i) {
        for (QWidget *w : std::initializer_list<QWidget *>{replLabel, m_replace, replNext, replAll, replOpen})
            w->setVisible(i == 1);
        for (QWidget *w : std::initializer_list<QWidget *>{find, findAll, findOpen, lg}) w->setVisible(i < 2);
        for (QWidget *w : std::initializer_list<QWidget *>{filesBox, findFiles}) w->setVisible(i == 2);
        (i < 2 ? find : findFiles)->setDefault(true);
        adjustSize();
    });
    showTab(0);
}

void FindDialog::showTab(int tab) {
    m_tabs->setCurrentIndex(tab);
    emit m_tabs->currentChanged(tab);
    show();
    raise();
    m_find->setFocus();
    m_find->lineEdit()->selectAll();
}

fsb::FindSpec FindDialog::spec() const {
    fsb::FindSpec s;
    s.pattern = fs::rs(m_find->currentText());
    s.replacement = fs::rs(m_replace->currentText());
    s.regex = m_regex->isChecked();
    s.match_case = m_case->isChecked();
    s.skip_tags = m_skipTags->isChecked();
    s.skip_text = m_skipText->isChecked();
    for (int k = 0; k < 4; ++k)
        if (m_field[k]->isChecked()) s.field = uint8_t(k);
    s.include_comments = m_comments->isChecked();
    s.at_start = m_atStart->isChecked();
    s.at_end = m_atEnd->isChecked();
    auto *tab = m_main->current();
    if (m_lines[1]->isChecked())
        for (size_t r : tab->selection()) s.rows.push_back(r);
    else if (m_lines[2]->isChecked())
        for (size_t r = tab->selection().empty() ? 0 : tab->selection().front(); r < tab->s().row_count(); ++r)
            s.rows.push_back(r);
    return s;
}

void FindDialog::findAll(const QList<TabPanel *> &tabs) {
    if (m_find->currentText().isEmpty()) return;
    clearResults();
    size_t lines = 0;
    int files = 0;
    for (auto *tab : tabs) {
        auto s = spec(); // "Selected" and "From selected" are the current tab's rows
        if (tab != m_main->current()) s.rows.clear();
        try {
            const auto rows = tab->s().find_all(s);
            if (rows.empty()) continue;
            Lines found;
            for (size_t r : rows) found.append({r, fs::q(tab->s().row(r).text)});
            addResults(tab, tab->title(), tab->path(), found);
            lines += rows.size();
            ++files;
        } catch (const rust::Error &e) {
            return m_main->error(QString::fromUtf8(e.what()));
        }
    }
    showResults(tr("Lines found: %1, in files: %2").arg(lines).arg(files));
}

void FindDialog::findInFiles() {
    const QString dir = m_dir->text().trimmed();
    if (m_find->currentText().isEmpty() || !QFileInfo(dir).isDir()) return;
    QSettings st;
    st.setValue("find/dir", dir);
    st.setValue("find/filter", m_filter->text());
    st.setValue("find/subdirs", m_subdirs->isChecked());
    auto s = spec();
    s.rows.clear();
    try {
        (void)fsb::new_session()->find_all(s); // a bad regex fails here once, not for every file
    } catch (const rust::Error &e) {
        return m_main->error(QString::fromUtf8(e.what()));
    }
    // the files are read on a worker, each into its own session
    struct Found {
        QList<std::pair<QString, Lines>> files; // path, matching lines
        int searched = 0;
    };
    const QStringList filters = m_filter->text().split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
    const auto flags = m_subdirs->isChecked() ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    m_findFiles->setEnabled(false);
    m_main->status(tr("Searching in %1...").arg(dir));
    auto *watch = new QFutureWatcher<Found>(this);
    connect(watch, &QFutureWatcher<Found>::finished, this, [this, watch, dir] {
        const Found f = watch->result();
        watch->deleteLater();
        m_findFiles->setEnabled(true);
        clearResults();
        size_t lines = 0;
        for (const auto &[path, found] : f.files) {
            addResults(nullptr, QDir(dir).relativeFilePath(path), path, found);
            lines += size_t(found.size());
        }
        showResults(tr("Lines found: %1, in files: %2 (searched: %3)").arg(lines).arg(f.files.size()).arg(f.searched));
    });
    watch->setFuture(QtConcurrent::run([dir, filters, flags, s] {
        Found f;
        QDirIterator it(dir, filters, QDir::Files, flags);
        while (it.hasNext()) {
            const QString path = it.next();
            auto file = fsb::new_session();
            try {
                file->open(fs::Utf8(path));
            } catch (const rust::Error &) {
                continue; // not subtitles after all
            }
            ++f.searched;
            const auto rows = file->find_all(s);
            if (rows.empty()) continue;
            Lines found;
            for (size_t r : rows) found.append({r, fs::q(file->row(r).text)});
            f.files.append({path, std::move(found)});
        }
        return f;
    }));
}

void FindDialog::clearResults() {
    if (!m_results) {
        m_results = new QDialog(m_main);
        m_results->setWindowTitle(tr("Search results"));
        auto *v = new QVBoxLayout(m_results);
        m_tree = new QTreeWidget(m_results);
        m_tree->setHeaderLabels({tr("Line"), tr("Text")});
        m_tree->setRootIsDecorated(true);
        m_tree->setUniformRowHeights(true);
        m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        v->addWidget(m_tree);
        m_summary = new QLabel(m_results);
        v->addWidget(m_summary);
        connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) { goToResult(it); });
        m_results->resize(700, 400);
    }
    m_tree->clear();
    m_resultTabs.clear();
}

void FindDialog::addResults(TabPanel *tab, const QString &title, const QString &path, const Lines &lines) {
    auto *file = new QTreeWidgetItem(m_tree, {title});
    file->setFirstColumnSpanned(true);
    file->setData(0, Qt::UserRole, int(m_resultTabs.size()));
    file->setData(0, Qt::UserRole + 1, path);
    file->setToolTip(0, path);
    m_resultTabs.append(tab);
    for (const auto &[r, text] : lines) {
        auto *it = new QTreeWidgetItem(file, {QString::number(r + 1), text});
        it->setData(0, Qt::UserRole, qulonglong(r));
    }
    file->setExpanded(true);
}

void FindDialog::showResults(const QString &summary) {
    m_summary->setText(summary);
    m_results->show();
    m_results->raise();
    m_main->status(m_tree->topLevelItemCount() ? summary : tr("Nothing found"));
}

void FindDialog::goToResult(QTreeWidgetItem *item) {
    auto *file = item->parent();
    if (!file) return;
    const int k = file->data(0, Qt::UserRole).toInt();
    QPointer<TabPanel> &tab = m_resultTabs[k];
    if (!tab) { // a file from the folder, or a tab closed since: open it, once
        const QString path = file->data(0, Qt::UserRole + 1).toString();
        if (path.isEmpty()) return;
        for (auto *t : m_main->tabs())
            if (!t->path().isEmpty() && QFileInfo(t->path()) == QFileInfo(path)) tab = t;
        if (!tab) {
            m_main->openFile(path);
            if (QFileInfo(m_main->current()->path()) == QFileInfo(path)) tab = m_main->current();
        }
        if (!tab) return;
    }
    m_main->showPanel(tab);
    tab->setActive(int(item->data(0, Qt::UserRole).toULongLong()));
}

void FindDialog::findNext() {
    auto *tab = m_main->current();
    if (!tab || m_find->currentText().isEmpty()) return;
    try {
        const auto r = tab->s().find(spec(), size_t(std::max(0, tab->active())));
        if (r < 0) m_main->status(tr("Nothing found"));
        else tab->setActive(int(r));
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
    }
}

// ---- select lines -------------------------------------------------------------

SelectLinesDialog::SelectLinesDialog(MainWindow *main) : QDialog(main), m_main(main) {
    setWindowTitle(tr("Select lines"));
    auto *v = new QVBoxLayout(this);
    auto *row = new QHBoxLayout;
    m_with = new QRadioButton(tr("With"), this);
    m_without = new QRadioButton(tr("Without"), this);
    QSettings st; // the last search, as Aegisub and Kainote keep it
    (st.value("select/with", true).toBool() ? m_with : m_without)->setChecked(true);
    m_find = new QLineEdit(st.value("select/text").toString(), this);
    row->addWidget(m_with);
    row->addWidget(m_without);
    row->addWidget(m_find, 1);
    v->addLayout(row);
    auto *f = new QFormLayout;
    m_field = new QComboBox(this);
    m_field->addItems({tr("Text"), tr("Styles"), tr("Actor"), tr("Effect")});
    m_field->setCurrentIndex(st.value("select/field", 0).toInt());
    f->addRow(tr("Field"), m_field);
    m_case = new QCheckBox(tr("Match case"), this);
    m_regex = new QCheckBox(tr("Regular expressions"), this);
    m_case->setChecked(st.value("select/case", false).toBool());
    m_regex->setChecked(st.value("select/regex", false).toBool());
    f->addRow(m_case, m_regex);
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
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SelectLinesDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
}

void SelectLinesDialog::apply() {
    auto *tab = m_main->current();
    auto &s = tab->s();
    fsb::FindSpec spec;
    spec.pattern = fs::rs(m_find->text());
    spec.regex = m_regex->isChecked();
    spec.match_case = m_case->isChecked();
    spec.field = uint8_t(m_field->currentIndex());
    spec.include_comments = true;
    QSettings st;
    st.setValue("select/text", m_find->text());
    st.setValue("select/with", m_with->isChecked());
    st.setValue("select/field", m_field->currentIndex());
    st.setValue("select/case", m_case->isChecked());
    st.setValue("select/regex", m_regex->isChecked());
    st.setValue("select/dialogue", m_dialogue->isChecked());
    st.setValue("select/comments", m_comments->isChecked());
    st.setValue("select/mode", m_mode->checkedId());
    st.setValue("select/action", m_action->currentIndex());
    rust::Vec<size_t> found;
    try {
        found = s.find_all(spec);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return;
    }
    // Aegisub's matching: lines of the checked kinds, with or without the text
    const auto spans = s.row_spans();
    std::vector<bool> hit(spans.size());
    for (size_t r : found) hit[r] = true;
    std::vector<bool> match(spans.size());
    for (size_t i = 0; i < spans.size(); ++i)
        match[i] = (spans[i].comment ? m_comments : m_dialogue)->isChecked() && hit[i] == m_with->isChecked();
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
    if (v.empty()) return m_main->status(tr("Selected %1 lines").arg(0));
    const int active = tab->active();
    tab->setSelection(v, active >= 0 && std::ranges::binary_search(v, size_t(active)) ? active : int(v.front()));
    switch (m_action->currentIndex()) {
    case 1: tab->grid()->copy(); break;
    case 2: tab->grid()->cut(); break;
    case 3: tab->grid()->moveSelection(-int(v.front())); break;
    case 4: tab->grid()->moveSelection(int(s.row_count()) - 1 - int(v.back())); break;
    case 5:
        s.set_rows_field(fs::slice(v), "comment", "1");
        tab->changed();
        break;
    case 6: tab->grid()->deleteRows(); break;
    }
    m_main->status(tr("Selected %1 lines").arg(v.size()));
}

// ---- style editor -------------------------------------------------------------

QColor assColor(const QString &s) {
    QString h = s;
    h.remove("&H", Qt::CaseInsensitive).remove('&');
    const uint v = h.toUInt(nullptr, 16);
    return QColor(int(v & 0xff), int(v >> 8 & 0xff), int(v >> 16 & 0xff), 255 - int(v >> 24 & 0xff));
}

namespace {
QString assColorString(const QColor &c) {
    return QString::asprintf("&H%02X%02X%02X%02X", 255 - c.alpha(), c.blue(), c.green(), c.red());
}
QString styleLine(const fsb::StyleData &s) {
    return QString("Style: %1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18,%19,%20,%21,%22,%23")
        .arg(fs::q(s.name), fs::q(s.font)).arg(s.size).arg(fs::q(s.primary), fs::q(s.secondary),
                                                              fs::q(s.outline_color), fs::q(s.back))
        .arg(s.bold ? -1 : 0).arg(s.italic ? -1 : 0).arg(s.underline ? -1 : 0).arg(s.strikeout ? -1 : 0)
        .arg(s.scale_x).arg(s.scale_y).arg(s.spacing).arg(s.angle).arg(s.border_style).arg(s.outline)
        .arg(s.shadow).arg(s.alignment).arg(s.margin_l).arg(s.margin_r).arg(s.margin_v).arg(s.encoding);
}
QVariantMap toMap(const fsb::StyleData &s) {
    return {{"line", styleLine(s)}};
}
} // namespace

// Laid out like Kainote's style panel (two columns, colours with alpha under them, big preview);
// the Apply / rename / name-clash logic follows Aegisub's DialogStyleEditor.
StyleEditor::StyleEditor(const fsb::StyleData &style, QWidget *parent) : QDialog(parent), m_style(style) {
    setWindowTitle(tr("Style editor"));
    auto *v = new QVBoxLayout(this);
    auto *cols = new QHBoxLayout;
    auto *left = new QVBoxLayout, *right = new QVBoxLayout;
    cols->addLayout(left, 3);
    cols->addLayout(right, 2);
    v->addLayout(cols);
    auto group = [this](QBoxLayout *col, const QString &title, QLayout *content) {
        auto *g = new QGroupBox(title, this);
        g->setLayout(content);
        col->addWidget(g);
    };
    auto dspin = [this](double v, double lo, double hi) {
        auto *s = new QDoubleSpinBox(this);
        s->setRange(lo, hi);
        s->setDecimals(2);
        s->setValue(v);
        return s;
    };
    auto ispin = [this](int v, int hi) {
        auto *s = new QSpinBox(this);
        s->setRange(0, hi);
        s->setValue(v);
        return s;
    };
    // a caption above each field, Kainote style
    auto labelled = [this](QGridLayout *g, int row, int col, const QString &text, QWidget *w) {
        g->addWidget(new QLabel(text, this), row * 2, col);
        g->addWidget(w, row * 2 + 1, col);
    };

    auto *nameL = new QHBoxLayout;
    m_name = new QLineEdit(fs::q(style.name), this);
    nameL->addWidget(m_name);
    group(left, tr("Style name"), nameL);

    auto *fontL = new QGridLayout;
    m_font = new QComboBox(this);
    m_font->setEditable(true);
    m_font->addItems(QFontDatabase::families());
    m_font->completer()->setCompletionMode(QCompleter::PopupCompletion);
    m_font->completer()->setFilterMode(Qt::MatchContains);
    m_font->setCurrentText(fs::q(style.font));
    if (!QFontDatabase::families().contains(m_font->currentText())) m_font->setToolTip(tr("Font is not installed"));
    m_size = dspin(style.size, 1, 1000);
    fontL->addWidget(m_font, 0, 0, 1, 3);
    fontL->addWidget(m_size, 0, 3);
    m_bold = new QCheckBox(tr("Bold"), this);
    m_italic = new QCheckBox(tr("Italic"), this);
    m_underline = new QCheckBox(tr("Underline"), this);
    m_strike = new QCheckBox(tr("Strikeout"), this);
    m_bold->setChecked(style.bold);
    m_italic->setChecked(style.italic);
    m_underline->setChecked(style.underline);
    m_strike->setChecked(style.strikeout);
    int fc = 0;
    for (auto *c : {m_bold, m_italic, m_underline, m_strike}) fontL->addWidget(c, 1, fc++);
    group(left, tr("Font and size"), fontL);

    auto *colorL = new QGridLayout;
    const char *names[] = {QT_TR_NOOP("Primary"), QT_TR_NOOP("Secondary"), QT_TR_NOOP("Outline"), QT_TR_NOOP("Shadow")};
    const rust::String *src[] = {&style.primary, &style.secondary, &style.outline_color, &style.back};
    for (int k = 0; k < 4; ++k) {
        m_colors[k] = fs::q(*src[k]);
        auto *b = m_colorButtons[k] = new QToolButton(this);
        b->setText(tr(names[k]));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_alpha[k] = new QSpinBox(this);
        m_alpha[k]->setRange(0, 255);
        m_alpha[k]->setValue(255 - assColor(m_colors[k]).alpha());
        m_alpha[k]->setToolTip(tr("Transparency (0 = opaque)"));
        auto paint = [this, k] {
            QColor c = assColor(m_colors[k]);
            c.setAlpha(255);
            m_colorButtons[k]->setStyleSheet(QString("QToolButton{background:%1;color:%2;border:1px solid palette(mid);padding:3px}")
                                                 .arg(c.name(), c.lightness() > 140 ? "black" : "white"));
        };
        paint();
        connect(b, &QToolButton::clicked, this, [this, k, paint] {
            QColor c = QColorDialog::getColor(assColor(m_colors[k]), this);
            if (!c.isValid()) return;
            c.setAlpha(255 - m_alpha[k]->value());
            m_colors[k] = assColorString(c);
            paint();
            preview();
        });
        colorL->addWidget(b, 0, k);
        colorL->addWidget(m_alpha[k], 1, k);
    }
    group(left, tr("Colors and transparency"), colorL);

    auto *prevL = new QVBoxLayout;
    m_previewText = new QLineEdit(QSettings().value("styles/previewText", tr("Sample text 0123 ĄĘŻŹ")).toString(), this);
    m_preview = new QLabel(this);
    m_preview->setMinimumSize(420, 110);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); // the pixmap must not dictate the size
    prevL->addWidget(m_previewText);
    prevL->addWidget(m_preview, 1);
    group(left, tr("Preview"), prevL);
    connect(m_previewText, &QLineEdit::textChanged, this, &StyleEditor::preview);

    auto *borderL = new QGridLayout;
    m_outline = dspin(style.outline, 0, 100);
    m_shadow = dspin(style.shadow, 0, 100);
    m_box = new QCheckBox(tr("Opaque box"), this);
    m_box->setChecked(style.border_style == 3);
    labelled(borderL, 0, 0, tr("Outline:"), m_outline);
    labelled(borderL, 0, 1, tr("Shadow:"), m_shadow);
    borderL->addWidget(m_box, 2, 0, 1, 2);
    group(right, tr("Outline"), borderL);

    auto *geoL = new QGridLayout;
    m_scaleX = dspin(style.scale_x, 0, 1000);
    m_scaleY = dspin(style.scale_y, 0, 1000);
    m_angle = dspin(style.angle, -360, 360);
    m_spacing = dspin(style.spacing, -100, 100);
    labelled(geoL, 0, 0, tr("Scale X:"), m_scaleX);
    labelled(geoL, 0, 1, tr("Scale Y:"), m_scaleY);
    labelled(geoL, 1, 0, tr("Angle:"), m_angle);
    labelled(geoL, 1, 1, tr("Spacing:"), m_spacing);
    group(right, tr("Miscellaneous"), geoL);

    auto *marginL = new QGridLayout;
    m_ml = ispin(style.margin_l, 9999);
    m_mr = ispin(style.margin_r, 9999);
    m_mv = ispin(style.margin_v, 9999);
    labelled(marginL, 0, 0, tr("Left:"), m_ml);
    labelled(marginL, 0, 1, tr("Right:"), m_mr);
    labelled(marginL, 0, 2, tr("Vertical:"), m_mv);
    group(right, tr("Margins"), marginL);

    auto *alignL = new QGridLayout;
    auto *ag = new QButtonGroup(this);
    for (int k = 0; k < 9; ++k) {
        m_align[k] = new QRadioButton(QString::number(k + 1), this);
        ag->addButton(m_align[k]);
        alignL->addWidget(m_align[k], 2 - k / 3, k % 3);
    }
    m_align[std::clamp(style.alignment, 1, 9) - 1]->setChecked(true);
    connect(ag, &QButtonGroup::buttonToggled, this, &StyleEditor::preview);
    group(right, tr("Alignment"), alignL);

    // Aegisub's encoding list; the number before " - " is what gets stored
    auto *encL = new QHBoxLayout;
    m_encodingBox = new QComboBox(this);
    m_encodingBox->setEditable(true);
    m_encodingBox->addItems({"0 - ANSI", "1 - Default", "2 - Symbol", "77 - Mac", "128 - Shift_JIS", "129 - Hangeul",
                             "130 - Johab", "134 - GB2312", "136 - Chinese BIG5", "161 - Greek", "162 - Turkish",
                             "163 - Vietnamese", "177 - Hebrew", "178 - Arabic", "186 - Baltic", "204 - Russian",
                             "222 - Thai", "238 - East European", "255 - OEM"});
    const int enc = m_encodingBox->findText(QString::number(style.encoding) + " - ", Qt::MatchStartsWith);
    if (enc >= 0) m_encodingBox->setCurrentIndex(enc);
    else m_encodingBox->setCurrentText(QString::number(style.encoding));
    encL->addWidget(m_encodingBox);
    group(right, tr("Encoding"), encL);
    right->addStretch();

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
        if (onApply) onApply(this->style());
    });
    v->addWidget(bb);
    for (auto *w : findChildren<QDoubleSpinBox *>()) connect(w, &QDoubleSpinBox::valueChanged, this, &StyleEditor::preview);
    for (auto *w : findChildren<QSpinBox *>()) connect(w, &QSpinBox::valueChanged, this, &StyleEditor::preview);
    for (auto *w : findChildren<QCheckBox *>()) connect(w, &QCheckBox::toggled, this, &StyleEditor::preview);
    connect(m_font, &QComboBox::currentTextChanged, this, &StyleEditor::preview);
    resize(900, sizeHint().height());
    preview();
}

void StyleEditor::accept() {
    if (onApply && !onApply(style())) return;
    QSettings().setValue("styles/previewText", m_previewText->text());
    QDialog::accept();
}

void StyleEditor::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    QTimer::singleShot(0, this, &StyleEditor::preview); // once the layout gave the preview its new size
}

fsb::StyleData StyleEditor::style() const {
    fsb::StyleData s = m_style;
    s.name = fs::rs(m_name->text().replace(',', ';'));
    s.font = fs::rs(m_font->currentText());
    s.size = m_size->value();
    rust::String *dst[] = {&s.primary, &s.secondary, &s.outline_color, &s.back};
    for (int k = 0; k < 4; ++k) {
        QColor c = assColor(m_colors[k]);
        c.setAlpha(255 - m_alpha[k]->value());
        *dst[k] = fs::rs(assColorString(c));
    }
    s.bold = m_bold->isChecked();
    s.italic = m_italic->isChecked();
    s.underline = m_underline->isChecked();
    s.strikeout = m_strike->isChecked();
    s.scale_x = m_scaleX->value();
    s.scale_y = m_scaleY->value();
    s.spacing = m_spacing->value();
    s.angle = m_angle->value();
    s.border_style = m_box->isChecked() ? 3 : 1;
    s.outline = m_outline->value();
    s.shadow = m_shadow->value();
    for (int k = 0; k < 9; ++k)
        if (m_align[k]->isChecked()) s.alignment = k + 1;
    s.margin_l = m_ml->value();
    s.margin_r = m_mr->value();
    s.margin_v = m_mv->value();
    s.encoding = m_encodingBox->currentText().section(' ', 0, 0).toInt();
    return s;
}

// Render the preview text with libass on a checkerboard.
void StyleEditor::preview() {
    if (!m_preview) return;
    const int w = std::max(420, m_preview->width()), h = std::max(110, m_preview->height());
    QImage img(w, h, QImage::Format_RGBA8888);
    {
        QPainter p(&img);
        for (int y = 0; y < h; y += 10)
            for (int x = 0; x < w; x += 10)
                p.fillRect(x, y, 10, 10, ((x + y) / 10) % 2 ? QColor(0x99, 0x99, 0x99) : QColor(0x66, 0x66, 0x66));
    }
    auto st = style();
    st.name = "Preview";
    const QString script =
        QString("[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\n\n[V4+ Styles]\nFormat: Name, Fontname, "
                "Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, "
                "StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
                "MarginR, MarginV, Encoding\n%3\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, "
                "MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:01:00.00,Preview,,0,0,0,,{\\an5\\q2}%4\n")
            .arg(w).arg(h).arg(styleLine(st), m_previewText->text());
    try {
        static std::optional<rust::Box<fsb::Renderer>> r;
        if (!r) r.emplace(fsb::new_renderer());
        (*r)->set_script(fs::Utf8(script));
        (*r)->blend(1000, w, h, w, h, {img.bits(), size_t(img.sizeInBytes())});
    } catch (const rust::Error &) {
        // no preview without libass; the editor still works
    }
    m_preview->setPixmap(QPixmap::fromImage(img));
}

// ---- style manager ------------------------------------------------------------

namespace {
// Aegisub's do_move; false when the block is already there
template <class V> bool moveBlock(V &v, int type, int &first, int &last) {
    const auto b = v.begin();
    const int n = int(v.size()), len = last - first + 1;
    if ((type < 2 && first == 0) || (type >= 2 && last == n - 1)) return false;
    if (type == 0) std::rotate(b, b + first, b + last + 1);
    if (type == 1) std::rotate(b + first - 1, b + first, b + last + 1);
    if (type == 2) std::rotate(b + first, b + last + 1, b + last + 2);
    if (type == 3) std::rotate(b + first, b + last + 1, v.end());
    first = type == 0 ? 0 : type == 1 ? first - 1 : type == 2 ? first + 1 : n - len;
    last = first + len - 1;
    return true;
}
} // namespace

StyleManager::StyleManager(MainWindow *main) : QDialog(main), m_main(main) {
    setWindowTitle(tr("Style manager"));
    auto *g = new QGridLayout(this);
    auto *catRow = new QHBoxLayout;
    m_catalog = new QComboBox(this);
    QSettings st;
    QStringList cats = st.value("styles/catalogs", QStringList{"Default"}).toStringList();
    m_catalog->addItems(cats);
    auto *newCat = new QPushButton(tr("New"), this);
    auto *delCat = new QPushButton(tr("Delete"), this);
    catRow->addWidget(new QLabel(tr("Catalog:"), this));
    catRow->addWidget(m_catalog, 1);
    catRow->addWidget(newCat);
    catRow->addWidget(delCat);
    g->addLayout(catRow, 0, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles stored"), this), 1, 0);
    m_storage = new QListWidget(this);
    m_storage->setSelectionMode(QAbstractItemView::ExtendedSelection);
    g->addWidget(m_storage, 2, 0);
    auto *sb = new QVBoxLayout;
    auto *sNew = new QPushButton(tr("New"), this), *sEdit = new QPushButton(tr("Edit"), this),
         *sDel = new QPushButton(tr("Delete"), this), *sSort = new QPushButton(tr("Sort"), this);
    for (auto *b : {sNew, sEdit, sDel, sSort}) sb->addWidget(b);
    sb->addStretch();
    g->addLayout(sb, 2, 1);
    auto *mid = new QHBoxLayout;
    auto *toStore = new QPushButton(tr("Add to storage ▲"), this);
    auto *toFile = new QPushButton(tr("Add to ASS ▼"), this);
    auto *toAll = new QPushButton(tr("Add to all open ASS files"), this);
    for (auto *b : {toStore, toFile, toAll}) mid->addWidget(b);
    g->addLayout(mid, 3, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles in ASS file"), this), 4, 0);
    m_file = new QListWidget(this);
    m_file->setSelectionMode(QAbstractItemView::ExtendedSelection);
    g->addWidget(m_file, 5, 0);
    auto *fb = new QVBoxLayout;
    auto *fNew = new QPushButton(tr("New"), this), *fCopy = new QPushButton(tr("Copy"), this),
         *fEdit = new QPushButton(tr("Edit"), this), *fDel = new QPushButton(tr("Delete"), this),
         *fClear = new QPushButton(tr("Clear"), this), *fClean = new QPushButton(tr("Clean"), this),
         *fSort = new QPushButton(tr("Sort"), this), *fImport = new QPushButton(tr("Import..."), this);
    fClean->setToolTip(tr("Delete unused styles"));
    fImport->setToolTip(tr("Import styles from another subtitle file"));
    for (auto *b : {fNew, fCopy, fEdit, fDel, fClear, fClean, fSort, fImport}) fb->addWidget(b);
    fb->addStretch();
    g->addLayout(fb, 5, 1);
    const auto moveButtons = [this](QBoxLayout *column, bool storage) {
        static const char *const names[] = {"arrow-bar-to-up", "arrow-up", "arrow-down", "arrow-bar-to-down"};
        static const char *const tips[] = {QT_TR_NOOP("Move to top"), QT_TR_NOOP("Move up"), QT_TR_NOOP("Move down"),
                                           QT_TR_NOOP("Move to bottom")};
        auto *row = new QHBoxLayout;
        for (int k = 0; k < 4; ++k) {
            auto *b = new QToolButton(this);
            b->setIcon(icon(names[k]));
            b->setToolTip(tr(tips[k]));
            connect(b, &QToolButton::clicked, this, [=, this] { moveStyles(storage, k); });
            row->addWidget(b);
        }
        column->insertLayout(column->count() - 1, row); // above the stretch
    };
    moveButtons(sb, true);
    moveButtons(fb, false);
    auto *close = new QPushButton(tr("Close"), this);
    g->addWidget(close, 6, 1);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto session = [this]() -> fsb::Session & { return m_main->current()->s(); };
    auto edited = [this] {
        m_main->current()->changed();
        reload();
    };
    auto defaultStyle = [] {
        fsb::StyleData d;
        d.name = "Default";
        d.font = "Arial";
        d.size = 48;
        d.primary = "&H00FFFFFF";
        d.secondary = "&H000000FF";
        d.outline_color = "&H00000000";
        d.back = "&H00000000";
        d.scale_x = d.scale_y = 100;
        d.border_style = 1;
        d.outline = 2;
        d.shadow = 2;
        d.alignment = 2;
        d.margin_l = d.margin_r = d.margin_v = 10;
        d.encoding = 1;
        return d;
    };
    connect(fNew, &QPushButton::clicked, this, [=, this] {
        int idx = -1;
        StyleEditor e(defaultStyle(), this);
        e.onApply = [&](const fsb::StyleData &s) { return applyToFile(idx, s); };
        e.exec();
    });
    connect(fCopy, &QPushButton::clicked, this, [=, this] {
        const int i = m_file->currentRow();
        if (i < 0) return;
        auto s = session().styles()[size_t(i)];
        s.name = fs::rs(fs::q(s.name) + tr(" - copy"));
        session().set_style(-1, s);
        edited();
    });
    auto editFile = [=, this] {
        const int i = m_file->currentRow();
        if (i < 0) return;
        int idx = i;
        StyleEditor e(session().styles()[size_t(i)], this);
        e.onApply = [&](const fsb::StyleData &s) { return applyToFile(idx, s); };
        e.exec();
    };
    connect(fEdit, &QPushButton::clicked, this, editFile);
    connect(m_file, &QListWidget::itemDoubleClicked, this, editFile);
    connect(fDel, &QPushButton::clicked, this, [=, this] {
        auto rows = m_file->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.row() > b.row(); });
        for (const auto &r : rows) session().delete_style(size_t(r.row()));
        edited();
    });
    connect(fClean, &QPushButton::clicked, this, [=, this] {
        QStringList gone;
        for (const auto &n : session().clean_styles()) gone << fs::q(n);
        edited();
        QMessageBox::information(this, tr("Delete unused styles"),
                                 gone.isEmpty() ? tr("Every style is in use.")
                                                : tr("Deleted styles:\n%1").arg(gone.join("\n")));
    });
    connect(fClear, &QPushButton::clicked, this, [=, this] {
        if (!m_main->confirm(tr("Delete all styles from the file?"))) return;
        for (size_t n = session().styles().size(); n > 0; --n) session().delete_style(n - 1);
        edited();
    });
    const auto selectedStored = [this] {
        std::vector<fsb::StyleData> v;
        for (int r = 0; r < m_storage->count(); ++r)
            if (m_storage->item(r)->isSelected()) v.push_back(m_stored[size_t(r)]);
        return v;
    };
    connect(toFile, &QPushButton::clicked, this, [=, this] {
        const auto chosen = selectedStored();
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    });
    connect(toAll, &QPushButton::clicked, this, [=, this] {
        const auto chosen = selectedStored();
        if (chosen.empty()) return;
        for (auto *tab : m_main->tabs()) {
            tab->s().add_styles({chosen.data(), chosen.size()});
            tab->changed();
        }
        reload();
    });
    connect(fSort, &QPushButton::clicked, this, [=, this] {
        const auto styles = session().styles();
        std::vector<size_t> order(styles.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return fs::q(styles[a].name) < fs::q(styles[b].name); });
        if (std::is_sorted(order.begin(), order.end())) return;
        session().reorder_styles({order.data(), order.size()});
        edited();
    });
    // Aegisub's "Import from script": pick styles of another file, asking before overwriting
    connect(fImport, &QPushButton::clicked, this, [=, this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import styles"),
                                                          QSettings().value("main/lastDir").toString(),
                                                          tr("ASS/SSA subtitles (*.ass *.ssa);;All files (*)"));
        if (path.isEmpty()) return;
        auto other = fsb::new_session();
        try {
            other->open(fs::Utf8(path));
        } catch (const rust::Error &e) {
            return (void)QMessageBox::warning(this, tr("Import styles"), QString::fromUtf8(e.what()));
        }
        const auto styles = other->styles();
        if (styles.empty())
            return (void)QMessageBox::information(this, tr("Import styles"),
                                                  tr("The selected file has no available styles."));
        QDialog pick(this);
        pick.setWindowTitle(tr("Import styles"));
        auto *l = new QVBoxLayout(&pick);
        l->addWidget(new QLabel(tr("Choose styles to import:"), &pick));
        auto *list = new QListWidget(&pick);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        for (const auto &s : styles) list->addItem(fs::q(s.name));
        list->selectAll();
        l->addWidget(list);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &pick);
        connect(bb, &QDialogButtonBox::accepted, &pick, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &pick, &QDialog::reject);
        l->addWidget(bb);
        if (pick.exec() != QDialog::Accepted) return;
        const auto current = session().styles();
        std::vector<fsb::StyleData> chosen;
        for (int r = 0; r < list->count(); ++r) {
            if (!list->item(r)->isSelected()) continue;
            const QString name = fs::q(styles[size_t(r)].name);
            const bool taken =
                std::any_of(current.begin(), current.end(), [&](const auto &s) { return fs::q(s.name) == name; });
            if (taken && QMessageBox::question(this, tr("Style name collision"),
                                               tr("There is already a style with the name \"%1\" in the current "
                                                  "script. Overwrite?")
                                                   .arg(name)) != QMessageBox::Yes)
                continue;
            chosen.push_back(styles[size_t(r)]);
        }
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    });
    connect(toStore, &QPushButton::clicked, this, [=, this] {
        const auto styles = session().styles();
        for (auto *it : m_file->selectedItems()) {
            const auto &st = styles[size_t(m_file->row(it))];
            auto same = std::find_if(m_stored.begin(), m_stored.end(), [&](auto &x) { return fs::q(x.name) == fs::q(st.name); });
            if (same != m_stored.end()) *same = st;
            else m_stored.push_back(st);
        }
        saveCatalog();
        reload();
    });
    connect(sNew, &QPushButton::clicked, this, [=, this] {
        int idx = -1;
        StyleEditor e(defaultStyle(), this);
        e.onApply = [&](const fsb::StyleData &s) { return applyToStorage(idx, s); };
        e.exec();
    });
    auto editStored = [this] {
        const int i = m_storage->currentRow();
        if (i < 0) return;
        int idx = i;
        StyleEditor e(m_stored[size_t(i)], this);
        e.onApply = [&](const fsb::StyleData &s) { return applyToStorage(idx, s); };
        e.exec();
    };
    connect(sEdit, &QPushButton::clicked, this, editStored);
    connect(m_storage, &QListWidget::itemDoubleClicked, this, editStored);
    connect(sDel, &QPushButton::clicked, this, [this] {
        auto rows = m_storage->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.row() > b.row(); });
        for (const auto &r : rows) m_stored.erase(m_stored.begin() + r.row());
        saveCatalog();
        reload();
    });
    connect(sSort, &QPushButton::clicked, this, [this] {
        std::sort(m_stored.begin(), m_stored.end(), [](auto &a, auto &b) { return fs::q(a.name) < fs::q(b.name); });
        saveCatalog();
        reload();
    });
    connect(newCat, &QPushButton::clicked, this, [this] {
        bool ok;
        const QString n = QInputDialog::getText(this, tr("Catalog"), tr("Name"), QLineEdit::Normal, {}, &ok);
        if (!ok || n.isEmpty()) return;
        m_catalog->addItem(n);
        QStringList all;
        for (int k = 0; k < m_catalog->count(); ++k) all << m_catalog->itemText(k);
        QSettings().setValue("styles/catalogs", all);
        m_catalog->setCurrentText(n);
    });
    connect(delCat, &QPushButton::clicked, this, [this] {
        if (m_catalog->count() <= 1) return;
        QSettings().remove("styles/catalog/" + m_catalog->currentText());
        m_catalog->removeItem(m_catalog->currentIndex());
        QStringList all;
        for (int k = 0; k < m_catalog->count(); ++k) all << m_catalog->itemText(k);
        QSettings().setValue("styles/catalogs", all);
    });
    connect(m_catalog, &QComboBox::currentTextChanged, this, [this] {
        loadCatalog();
        reload();
        m_main->current()->s().set_info_value("Last Style Storage", fs::Utf8(m_catalog->currentText()), "");
    });
    const QString last = fs::q(m_main->current()->s().info_value("Last Style Storage"));
    if (!last.isEmpty() && cats.contains(last)) m_catalog->setCurrentText(last);
    loadCatalog();
    reload();
    resize(560, 640);
}

void StyleManager::loadCatalog() {
    m_stored.clear();
    const QStringList lines = QSettings().value("styles/catalog/" + m_catalog->currentText()).toStringList();
    // stored as ASS "Style:" lines; parse them through a throwaway session
    if (lines.isEmpty()) return;
    const QString script = "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, "
                           "PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, "
                           "StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
                           "MarginL, MarginR, MarginV, Encoding\n" + lines.join('\n') + "\n";
    const QString tmp = QDir::temp().filePath(QString("fsed-styles-%1.ass").arg(QCoreApplication::applicationPid()));
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(script.toUtf8());
    f.close();
    auto s = fsb::new_session();
    try {
        s->open(fs::Utf8(tmp));
        for (const auto &st : s->styles()) m_stored.push_back(st);
    } catch (const rust::Error &) {
    }
    QFile::remove(tmp);
}

void StyleManager::saveCatalog() {
    QStringList lines;
    for (const auto &s : m_stored) lines << styleLine(s);
    QSettings().setValue("styles/catalog/" + m_catalog->currentText(), lines);
}

void StyleManager::moveStyles(bool storage, int type) {
    QListWidget *list = storage ? m_storage : m_file;
    int first = list->count(), last = -1;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->isSelected()) first = std::min(first, r), last = r;
    if (last < 0) return;
    if (storage) {
        if (!moveBlock(m_stored, type, first, last)) return;
        saveCatalog();
    } else {
        std::vector<size_t> order(size_t(list->count()));
        std::iota(order.begin(), order.end(), size_t(0));
        if (!moveBlock(order, type, first, last)) return;
        m_main->current()->s().reorder_styles({order.data(), order.size()});
        m_main->current()->changed();
    }
    reload();
    for (int r = first; r <= last; ++r) list->item(r)->setSelected(true);
    list->setCurrentRow(first, QItemSelectionModel::NoUpdate);
}

void StyleManager::reload() {
    m_storage->clear();
    for (const auto &s : m_stored) m_storage->addItem(fs::q(s.name));
    m_file->clear();
    for (const auto &s : m_main->current()->s().styles()) m_file->addItem(fs::q(s.name));
}

void StyleManager::editStyle(const QString &name) {
    const auto items = m_file->findItems(name, Qt::MatchExactly);
    if (items.isEmpty()) return;
    m_file->setCurrentItem(items.first());
    int idx = m_file->currentRow();
    StyleEditor e(m_main->current()->s().styles()[size_t(idx)], this);
    e.onApply = [&](const fsb::StyleData &s) { return applyToFile(idx, s); };
    e.exec();
}

bool StyleManager::applyToFile(int &index, const fsb::StyleData &s) {
    auto &session = m_main->current()->s();
    const auto styles = session.styles();
    for (size_t k = 0; k < styles.size(); ++k)
        if (int(k) != index && fs::q(styles[k].name) == fs::q(s.name)) {
            QMessageBox::warning(this, tr("Style name conflict"),
                                 tr("There is already a style with this name. Please choose another name."));
            return false;
        }
    // a rename also moves the lines and \r tags that used the old name (Aegisub asks; this always does)
    session.set_style(index, s);
    if (index < 0) index = int(styles.size());
    m_main->current()->changed();
    reload();
    return true;
}

bool StyleManager::applyToStorage(int &index, const fsb::StyleData &s) {
    for (size_t k = 0; k < m_stored.size(); ++k)
        if (int(k) != index && fs::q(m_stored[k].name) == fs::q(s.name)) {
            QMessageBox::warning(this, tr("Style name conflict"),
                                 tr("There is already a style with this name. Please choose another name."));
            return false;
        }
    if (index < 0) {
        index = int(m_stored.size());
        m_stored.push_back(s);
    } else {
        m_stored[size_t(index)] = s;
    }
    saveCatalog();
    reload();
    return true;
}

// ---- properties ---------------------------------------------------------------

PropertiesDialog::PropertiesDialog(TabPanel *tab) : QDialog(tab), m_tab(tab) {
    setWindowTitle(tr("ASS file properties"));
    auto &s = tab->s();
    auto *f = new QFormLayout(this);
    const std::pair<const char *, const char *> info[] = {
        {QT_TR_NOOP("Title"), "Title"}, {QT_TR_NOOP("Author"), "Original Script"},
        {QT_TR_NOOP("Translator"), "Original Translation"}, {QT_TR_NOOP("Proofreading"), "Original Editing"},
        {QT_TR_NOOP("Timer"), "Original Timing"}, {QT_TR_NOOP("Editing"), "Script Updated By"}};
    for (auto [label, key] : info) {
        auto *e = new QLineEdit(fs::q(s.info_value(key)), this);
        f->addRow(tr(label), e);
        m_fields.emplace_back(key, e);
    }
    auto spin = [this](const QString &v) {
        auto *sp = new QSpinBox(this);
        sp->setRange(0, 16384);
        sp->setValue(v.toInt());
        return sp;
    };
    const auto fi = s.file_info();
    m_w = spin(QString::number(fi.play_res_x));
    m_h = spin(QString::number(fi.play_res_y));
    auto *res = new QHBoxLayout;
    res->addWidget(m_w);
    res->addWidget(new QLabel("x", this));
    res->addWidget(m_h);
    auto *fromVideo = new QPushButton(tr("From video"), this);
    fromVideo->setEnabled(tab->video()->hasVideo());
    connect(fromVideo, &QPushButton::clicked, this, [this] {
        m_w->setValue(m_tab->video()->info().width);
        m_h->setValue(m_tab->video()->info().height);
    });
    res->addWidget(fromVideo);
    f->addRow(tr("Resolution"), res);
    m_lw = spin(fs::q(s.info_value("LayoutResX")));
    m_lh = spin(fs::q(s.info_value("LayoutResY")));
    auto *lres = new QHBoxLayout;
    lres->addWidget(m_lw);
    lres->addWidget(new QLabel("x", this));
    lres->addWidget(m_lh);
    f->addRow(tr("Layout resolution"), lres);
    m_matrix = new QComboBox(this);
    m_matrix->addItems({"None", "TV.601", "PC.601", "TV.709", "PC.709", "TV.FCC", "PC.FCC", "TV.240M", "PC.240M"});
    m_matrix->setCurrentText(fs::q(s.info_value("YCbCr Matrix")).isEmpty() ? "None" : fs::q(s.info_value("YCbCr Matrix")));
    f->addRow(tr("YCbCr matrix"), m_matrix);
    m_wrap = new QComboBox(this);
    m_wrap->addItems({tr("0: Smart wrapping, top line is wider"), tr("1: End-of-line word wrapping, only \\N breaks"),
                      tr("2: No word wrapping, \\n and \\N break"), tr("3: Smart wrapping, bottom line is wider")});
    m_wrap->setCurrentIndex(std::clamp(fs::q(s.info_value("WrapStyle")).toInt(), 0, 3));
    f->addRow(tr("Wrap style"), m_wrap);
    m_collisions = new QComboBox(this);
    m_collisions->addItems({"Normal", "Reverse"});
    m_collisions->setCurrentText(fs::q(s.info_value("Collisions")).isEmpty() ? "Normal" : fs::q(s.info_value("Collisions")));
    f->addRow(tr("Colliding lines"), m_collisions);
    m_scaled = new QCheckBox(tr("Scale border and shadow"), this);
    m_scaled->setChecked(fs::q(s.info_value("ScaledBorderAndShadow")).compare("yes", Qt::CaseInsensitive) == 0);
    f->addRow(m_scaled);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}

void PropertiesDialog::accept() {
    auto &s = m_tab->s();
    static int edits = 0;
    const QString amend = QString("properties-%1").arg(++edits); // one undo step
    auto set = [&](const QString &k, const QString &v) {
        if (fs::q(s.info_value(fs::Utf8(k))) != v) s.set_info_value(fs::Utf8(k), fs::Utf8(v), fs::Utf8(amend));
    };
    for (auto &[k, e] : m_fields) set(k, e->text());
    const auto fi = s.file_info();
    if (m_w->value() != fi.play_res_x) set("PlayResX", QString::number(m_w->value()));
    if (m_h->value() != fi.play_res_y) set("PlayResY", QString::number(m_h->value()));
    set("LayoutResX", m_lw->value() ? QString::number(m_lw->value()) : QString());
    set("LayoutResY", m_lh->value() ? QString::number(m_lh->value()) : QString());
    set("YCbCr Matrix", m_matrix->currentText());
    set("WrapStyle", QString::number(m_wrap->currentIndex()));
    set("Collisions", m_collisions->currentText());
    set("ScaledBorderAndShadow", m_scaled->isChecked() ? "yes" : "no");
    m_tab->changed();
    QDialog::accept();
}

// ---- history ------------------------------------------------------------------

HistoryDialog::HistoryDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("History"));
    auto *v = new QVBoxLayout(this);
    auto *list = new QListWidget(this);
    list->addItem(tr("(current state)"));
    for (const auto &h : tab->s().history()) list->addItem(fs::q(h));
    v->addWidget(list);
    v->addWidget(new QLabel(tr("Click a step to undo back to before it."), this));
    connect(list, &QListWidget::itemClicked, this, [this, tab, list](QListWidgetItem *it) {
        const int steps = list->row(it);
        if (steps <= 0) return;
        tab->s().undo_to(size_t(steps));
        tab->changed();
        accept();
    });
    resize(360, 420);
}

// ---- resample -----------------------------------------------------------------

// Aegisub's Resample Resolution: both resolutions and colour matrices, what a changed aspect ratio
// does, and the source margins of the manual mode.
ResampleDialog::ResampleDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("Resample subtitles"));
    auto *v = new QVBoxLayout(this);
    const auto fi = tab->s().file_info();
    // blank: colours stay; "None" has no fixed meaning, so it is not offered (Aegisub)
    const QStringList matrices{"", "TV.601", "PC.601", "TV.709", "PC.709", "TV.FCC", "PC.FCC", "TV.240M", "PC.240M"};
    QString scriptMatrix = fs::q(tab->s().info_value("YCbCr Matrix"));
    if (scriptMatrix != "None" && matrices.indexOf(scriptMatrix) < 1) scriptMatrix = "TV.601"; // how it renders
    const QString videoMatrix = tab->video()->hasVideo() ? fs::q(tab->video()->info().matrix) : QString();
    const auto resolution = [&](const QString &title, QSpinBox *&x, QSpinBox *&y, QComboBox *&matrix,
                                QPushButton *from) {
        auto *box = new QGroupBox(title, this);
        auto *g = new QGridLayout(box);
        x = new QSpinBox(box);
        y = new QSpinBox(box);
        for (auto *s : {x, y}) s->setRange(1, 99999);
        matrix = new QComboBox(box);
        matrix->addItems(matrices);
        g->addWidget(x, 0, 0);
        g->addWidget(new QLabel("×", box), 0, 1);
        g->addWidget(y, 0, 2);
        g->addWidget(from, 0, 3);
        g->addWidget(new QLabel(tr("YCbCr matrix:"), box), 1, 0);
        g->addWidget(matrix, 1, 1, 1, 3);
        v->addWidget(box);
    };
    QSpinBox *sx, *sy, *dx, *dy;
    QComboBox *sm, *dm;
    auto *fromScript = new QPushButton(tr("From script"), this);
    auto *fromVideo = new QPushButton(tr("From video"), this);
    resolution(tr("Source resolution"), sx, sy, sm, fromScript);
    resolution(tr("Destination resolution"), dx, dy, dm, fromVideo);
    // Aegisub proposes converting the colours when the script was made for another matrix than the video's
    if (matrices.indexOf(scriptMatrix) > 0 && matrices.indexOf(videoMatrix) > 0 && scriptMatrix != videoMatrix) {
        sm->setCurrentText(scriptMatrix);
        dm->setCurrentText(videoMatrix);
    }
    const bool video = tab->video()->hasVideo();
    const int vw = video ? tab->video()->info().width : fi.play_res_x;
    const int vh = video ? tab->video()->info().height : fi.play_res_y;
    sx->setValue(fi.play_res_x);
    sy->setValue(fi.play_res_y);
    dx->setValue(vw);
    dy->setValue(vh);

    auto *arBox = new QGroupBox(tr("Aspect ratio handling"), this);
    auto *arLayout = new QVBoxLayout(arBox);
    auto *ar = new QButtonGroup(this);
    for (const char *label : {QT_TR_NOOP("Stretch"), QT_TR_NOOP("Add borders"), QT_TR_NOOP("Remove borders"),
                              QT_TR_NOOP("Manual")}) {
        auto *b = new QRadioButton(tr(label), arBox);
        ar->addButton(b, int(ar->buttons().size()));
        arLayout->addWidget(b);
    }
    ar->button(std::clamp(QSettings().value("resample/arMode", 0).toInt(), 0, 3))->setChecked(true);
    v->addWidget(arBox);

    auto *marginBox = new QGroupBox(tr("Margin offset"), this);
    auto *mg = new QGridLayout(marginBox);
    std::array<QSpinBox *, 4> m; // left, right, top, bottom
    for (auto *&s : m) {
        s = new QSpinBox(marginBox);
        s->setRange(-9999, 9999);
    }
    auto *symmetrical = new QCheckBox(tr("Symmetrical"), marginBox);
    symmetrical->setChecked(true);
    mg->addWidget(m[2], 0, 1);
    mg->addWidget(m[0], 1, 0);
    mg->addWidget(symmetrical, 1, 1);
    mg->addWidget(m[1], 1, 2);
    mg->addWidget(m[3], 2, 1);
    v->addWidget(marginBox);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    v->addWidget(bb);

    const auto update = [=] {
        fromVideo->setEnabled(video && (dx->value() != vw || dy->value() != vh));
        fromScript->setEnabled(sx->value() != fi.play_res_x || sy->value() != fi.play_res_y);
        const double from = double(sx->value()) / sy->value(), to = double(dx->value()) / dy->value();
        const bool changed = std::abs(from - to) / to > .01;
        arBox->setEnabled(changed);
        marginBox->setEnabled(changed && ar->checkedId() == 3);
        m[1]->setEnabled(!symmetrical->isChecked());
        m[3]->setEnabled(!symmetrical->isChecked());
    };
    for (auto *s : {sx, sy, dx, dy}) connect(s, &QSpinBox::valueChanged, this, update);
    connect(ar, &QButtonGroup::idToggled, this, update);
    connect(symmetrical, &QCheckBox::toggled, this, [=](bool on) {
        if (on) {
            m[1]->setValue(m[0]->value());
            m[3]->setValue(m[2]->value());
        }
        update();
    });
    for (int k : {0, 2})
        connect(m[k], &QSpinBox::valueChanged, this, [=](int value) {
            if (symmetrical->isChecked()) m[k + 1]->setValue(value);
        });
    connect(fromScript, &QPushButton::clicked, this, [=] {
        sx->setValue(fi.play_res_x);
        sy->setValue(fi.play_res_y);
        sm->setCurrentIndex(std::max(0, int(matrices.indexOf(scriptMatrix))));
    });
    connect(fromVideo, &QPushButton::clicked, this, [=] {
        dx->setValue(vw);
        dy->setValue(vh);
        dm->setCurrentIndex(std::max(0, int(matrices.indexOf(videoMatrix))));
    });
    update();
    connect(bb, &QDialogButtonBox::accepted, this, [=, this] {
        fsb::ResampleSpec r;
        r.from_x = sx->value();
        r.from_y = sy->value();
        r.to_x = dx->value();
        r.to_y = dy->value();
        r.margin_l = m[0]->value();
        r.margin_r = m[1]->value();
        r.margin_t = m[2]->value();
        r.margin_b = m[3]->value();
        r.ar_mode = uint8_t(ar->checkedId());
        r.from_matrix = fs::rs(sm->currentText());
        r.to_matrix = fs::rs(dm->currentText());
        QSettings().setValue("resample/arMode", ar->checkedId());
        tab->s().resample(r);
        tab->changed();
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---- font collector -----------------------------------------------------------

FontCollectorDialog::FontCollectorDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("Fonts collector"));
    resize(760, 460);
    QSettings st;
    auto *f = new QFormLayout(this);
    auto folderRow = [this](QLineEdit *e) {
        auto *row = new QHBoxLayout;
        auto *b = new QPushButton(tr("Browse..."), this);
        connect(b, &QPushButton::clicked, this, [this, e] {
            const QString d = QFileDialog::getExistingDirectory(this, {}, e->text());
            if (!d.isEmpty()) e->setText(QDir::toNativeSeparators(d));
        });
        row->addWidget(e, 1);
        row->addWidget(b);
        return row;
    };
    auto *extra = new QLineEdit(st.value("fonts/extraDir").toString(), this);
    f->addRow(tr("Also search folder"), folderRow(extra));
    const QString media = tab->video()->hasVideo() ? tab->video()->path() : QString();
    auto *attached = new QCheckBox(tr("Fonts attached to the video"), this);
    attached->setEnabled(!media.isEmpty());
    attached->setChecked(!media.isEmpty());
    f->addRow(QString(), attached);
    auto *dest = new QLineEdit(st.value("fonts/destDir").toString(), this);
    f->addRow(tr("Copy fonts to"), folderRow(dest));
    auto *report = new QPlainTextEdit(this);
    report->setReadOnly(true);
    report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    f->addRow(report);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *check = bb->addButton(tr("Check"), QDialogButtonBox::ActionRole);
    auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    // the script's fonts are read here, the font files on a worker (the first scan of the system's takes seconds)
    auto run = [=, this](bool doCopy) {
        if (doCopy && dest->text().isEmpty()) return report->setPlainText(tr("Choose a destination folder first."));
        QSettings s;
        s.setValue("fonts/extraDir", extra->text());
        s.setValue("fonts/destDir", dest->text());
        auto uses = std::make_shared<rust::Box<fsb::FontUses>>(tab->s().font_uses());
        const QString dir = extra->text(), from = attached->isChecked() ? media : QString(),
                      to = doCopy ? dest->text() : QString();
        check->setEnabled(false);
        copy->setEnabled(false);
        report->setPlainText(tr("Searching for fonts..."));
        using Result = std::pair<bool, QString>; // ok, report or error
        auto *watch = new QFutureWatcher<Result>(this);
        connect(watch, &QFutureWatcher<Result>::finished, this, [=, this] {
            const auto [ok, r] = watch->result();
            const int missing = r.count("MISSING") + r.count("missing glyphs");
            report->setPlainText(!ok ? r
                                     : r + "\n" + (missing ? tr("%n problem(s) found.", nullptr, missing) : tr("All fonts found.")) +
                                           (to.isEmpty() ? QString() : "\n" + tr("Copied to %1").arg(to)));
            check->setEnabled(true);
            copy->setEnabled(true);
            watch->deleteLater();
        });
        watch->setFuture(QtConcurrent::run([uses, dir, from, to]() -> Result {
            rust::Vec<rust::String> dirs;
            if (!dir.isEmpty()) dirs.push_back(fs::rs(dir));
            try {
                return {true, fs::q(fsb::collect_fonts(**uses, std::move(dirs), fs::Utf8(from), fs::Utf8(to)))};
            } catch (const std::exception &e) {
                return {false, QString::fromUtf8(e.what())};
            }
        }));
    };
    connect(check, &QPushButton::clicked, this, [run] { run(false); });
    connect(copy, &QPushButton::clicked, this, [run] { run(true); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}

// ---- settings -----------------------------------------------------------------

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    // shortcut: Kainote's themes and file associations pages are not built yet.
    QSettings st;
    // Kainote's layout: the pages listed on the left
    auto *list = new QListWidget(this);
    auto *pages = new QStackedWidget(this);
    const auto page = [list, pages](const QString &name) {
        list->addItem(name);
        auto *w = new QWidget(pages);
        pages->addWidget(w);
        auto *form = new QFormLayout(w);
        form->setContentsMargins(0, 0, 0, 0);
        return form;
    };
    QFormLayout *editor = page(tr("Editor")), *subs = page(tr("Subtitles")), *video = page(tr("Video")),
                *audio = page(tr("Audio"));
    struct Check { const char *key; const char *label; bool def; QFormLayout *page; };
    const Check checks[] = {
        {"main/askSession", QT_TR_NOOP("Load the last session at startup"), true, editor},
        {"subs/cpsIgnoreWhitespace", QT_TR_NOOP("Ignore whitespace in characters per second"), true, subs},
        {"subs/cpsIgnorePunctuation", QT_TR_NOOP("Ignore punctuation in characters per second"), true, subs},
        {"video/openAudio", QT_TR_NOOP("Open audio together with video"), true, video},
        {"video/openWithSubs", QT_TR_NOOP("Open video and audio named in the subtitles"), true, video},
        {"video/askResolution", QT_TR_NOOP("Ask to resample the subtitles to a video of another resolution"), true,
         video},
        {"audio/snapKeyframes", QT_TR_NOOP("Snap audio boundaries to keyframes"), false, audio},
        {"audio/snapLines", QT_TR_NOOP("Snap audio boundaries to other lines"), false, audio},
        {"audio/inactiveComments", QT_TR_NOOP("Show inactive comments in the audio"), false, audio},
        {"audio/wheelZooms", QT_TR_NOOP("Mouse wheel zooms the audio (Shift scrolls)"), false, audio},
    };
    std::vector<std::pair<QString, QCheckBox *>> boxes;
    for (const auto &c : checks) {
        auto *b = new QCheckBox(tr(c.label), this);
        b->setChecked(st.value(c.key, c.def).toBool());
        c.page->addRow(b);
        boxes.emplace_back(c.key, b);
    }
    auto *inactive = new QComboBox(this);
    inactive->addItems({tr("None"), tr("Previous line"), tr("Previous and next line"), tr("All lines")});
    inactive->setCurrentIndex(std::clamp(st.value("audio/inactiveLines", 3).toInt(), 0, 3));
    audio->addRow(tr("Inactive lines in the audio"), inactive);
    auto *leadIn = new QSpinBox(this), *leadOut = new QSpinBox(this);
    for (auto *s : {leadIn, leadOut}) {
        s->setRange(0, 5000);
        s->setSuffix(" ms");
    }
    leadIn->setValue(st.value("audio/leadIn", 200).toInt());
    leadOut->setValue(st.value("audio/leadOut", 300).toInt());
    audio->addRow(tr("Audio lead-in"), leadIn);
    audio->addRow(tr("Audio lead-out"), leadOut);
    auto *autosave = new QSpinBox(this);
    autosave->setRange(0, 3600);
    autosave->setSuffix(" s");
    autosave->setSpecialValueText(tr("off"));
    autosave->setValue(st.value("main/autosaveSeconds", 60).toInt());
    editor->addRow(tr("Autosave every"), autosave);
    // the character counter's limits, red in the grid and the edit box (Aegisub's, Kainote's)
    const std::pair<const char *, int> limitKeys[] = {
        {"subs/maxCps", 15}, {"subs/maxLineLength", 43}, {"subs/maxLines", 2}};
    const char *limitLabels[] = {QT_TR_NOOP("Maximum characters per second"), QT_TR_NOOP("Maximum line length"),
                                 QT_TR_NOOP("Maximum lines")};
    std::vector<std::pair<QString, QSpinBox *>> spins;
    for (int k = 0; k < 3; ++k) {
        auto *s = new QSpinBox(this);
        s->setRange(1, k == 2 ? 20 : 1000);
        s->setValue(st.value(limitKeys[k].first, limitKeys[k].second).toInt());
        subs->addRow(tr(limitLabels[k]), s);
        spins.emplace_back(limitKeys[k].first, s);
    }
    const auto fontName = [](const QFont &f) { return QString("%1, %2 pt").arg(f.family()).arg(f.pointSize()); };
    auto *font = new QPushButton(fontName(st.value("grid/font", QFont("Tahoma", 10)).value<QFont>()), this);
    connect(font, &QPushButton::clicked, this, [font, fontName] {
        bool ok;
        const QFont f = QFontDialog::getFont(&ok, QSettings().value("grid/font", QFont("Tahoma", 10)).value<QFont>());
        if (ok) {
            QSettings().setValue("grid/font", f);
            font->setText(fontName(f));
        }
    });
    editor->addRow(tr("Grid font (applies to new tabs)"), font);
    auto *dictDir = new QLineEdit(QDir::toNativeSeparators(spell::dir()), this);
    dictDir->setMinimumWidth(dictDir->fontMetrics().averageCharWidth() * 48);
    editor->addRow(tr("Dictionary folder (Hunspell .aff/.dic)"), dictDir);
    auto *dictLang = new QComboBox(this);
    dictLang->addItems(spell::languages());
    dictLang->setCurrentText(st.value("spell/lang").toString());
    connect(dictDir, &QLineEdit::editingFinished, this, [dictDir, dictLang] {
        const QString keep = dictLang->currentText();
        QSettings().setValue("spell/dir", QDir::fromNativeSeparators(dictDir->text()));
        dictLang->clear();
        dictLang->addItems(spell::languages());
        dictLang->setCurrentText(keep);
    });
    editor->addRow(tr("Spell check language"), dictLang);
    auto *language = new QComboBox(this);
    language->addItem(tr("System"), "");
    language->addItem("English", "en");
    language->addItem("Polski", "pl");
    language->setCurrentIndex(std::max(0, language->findData(st.value("main/language", ""))));
    editor->addRow(tr("Language (after restart)"), language);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this,
            [this, boxes, spins, inactive, leadIn, leadOut, autosave, dictDir, dictLang, language] {
        QSettings st;
        st.setValue("main/language", language->currentData());
        st.setValue("spell/dir", QDir::fromNativeSeparators(dictDir->text()));
        st.setValue("spell/lang", dictLang->currentText());
        spell::reload();
        for (auto &[k, b] : boxes) st.setValue(k, b->isChecked());
        st.setValue("audio/inactiveLines", inactive->currentIndex());
        st.setValue("audio/leadIn", leadIn->value());
        st.setValue("audio/leadOut", leadOut->value());
        st.setValue("main/autosaveSeconds", autosave->value());
        for (auto &[k, s] : spins) st.setValue(k, s->value());
        limits::reload();
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    list->setCurrentRow(0);
    list->setFixedWidth(list->sizeHintForColumn(0) + 2 * list->frameWidth() + 24);
    auto *g = new QGridLayout(this);
    g->addWidget(list, 0, 0);
    g->addWidget(pages, 0, 1, Qt::AlignTop);
    g->addWidget(bb, 1, 0, 1, 2);
    g->setColumnStretch(1, 1);
}

// ---- autosave -----------------------------------------------------------------

QString autosaveDir() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave"; }

AutosaveDialog::AutosaveDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Open autosave file"));
    resize(800, 350);
    auto *v = new QVBoxLayout(this);
    auto *h = new QHBoxLayout;
    auto *files = new QGroupBox(tr("Files"), this);
    auto *versions = new QGroupBox(tr("Versions"), this);
    m_files = new QListWidget(files);
    m_versions = new QListWidget(versions);
    (new QVBoxLayout(files))->addWidget(m_files);
    (new QVBoxLayout(versions))->addWidget(m_versions);
    h->addWidget(files);
    h->addWidget(versions);
    v->addLayout(h, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Open"));
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_versions, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    const QString suffix = ".AUTOSAVE.ass";
    QMap<QString, QList<std::pair<QDateTime, QString>>> byName;
    for (const QFileInfo &fi : QDir(autosaveDir()).entryInfoList({"*" + suffix}, QDir::Files)) {
        const QString stem = fi.fileName().chopped(suffix.size());
        const QDateTime date = QDateTime::fromString(stem.section('.', -1), "yyyy-MM-dd-HH-mm-ss");
        const QString name = date.isValid() ? stem.section('.', 0, -2) : stem;
        byName[name].append({date.isValid() ? date : fi.lastModified(), fi.absoluteFilePath()});
    }
    QList<QList<std::pair<QDateTime, QString>>> groups = byName.values();
    QStringList names = byName.keys();
    for (auto &g : groups) std::ranges::sort(g, std::greater{});
    std::vector<int> order(groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](int a, int b) { return groups[a][0].first > groups[b][0].first; });
    for (int i : order) {
        auto *it = new QListWidgetItem(names[i], m_files);
        QStringList paths, labels;
        for (const auto &[d, p] : groups[i]) {
            paths << p;
            labels << QLocale().toString(d, QLocale::LongFormat);
        }
        it->setData(Qt::UserRole, paths);
        it->setData(Qt::UserRole + 1, labels);
    }
    connect(m_files, &QListWidget::currentRowChanged, this, [this](int row) {
        m_versions->clear();
        if (row < 0) return;
        const QStringList paths = m_files->item(row)->data(Qt::UserRole).toStringList();
        const QStringList labels = m_files->item(row)->data(Qt::UserRole + 1).toStringList();
        for (int k = 0; k < paths.size(); ++k) (new QListWidgetItem(labels[k], m_versions))->setData(Qt::UserRole, paths[k]);
        m_versions->setCurrentRow(0);
    });
    bb->button(QDialogButtonBox::Ok)->setEnabled(m_files->count() > 0);
    m_files->setCurrentRow(0);
}

QString AutosaveDialog::chosen() const {
    auto *it = m_versions->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

// ---- spell checker ----------------------------------------------------------------

SpellCheckDialog::SpellCheckDialog(MainWindow *main, TabPanel *tab) : QDialog(main), m_main(main), m_tab(tab) {
    setWindowTitle(tr("Spell checker"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *g = new QGridLayout(this);
    m_word = new QLineEdit(this);
    m_word->setReadOnly(true);
    m_with = new QLineEdit(this);
    m_suggest = new QListWidget(this);
    auto *lang = new QComboBox(this);
    lang->addItems(spell::languages());
    lang->setCurrentText(QSettings().value("spell/lang", "pl_PL").toString());
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
    buttons->addStretch();
    button(tr("Close"), [this] { close(); });
    g->addLayout(buttons, 0, 2, 6, 1);

    connect(m_suggest, &QListWidget::currentTextChanged, m_with, &QLineEdit::setText);
    connect(m_suggest, &QListWidget::itemDoubleClicked, this, [this] { replace(false); });
    connect(lang, &QComboBox::currentTextChanged, this, [this](const QString &l) {
        QSettings().setValue("spell/lang", l);
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
        f.include_comments = true;
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

// ---- paste over ----------------------------------------------------------------

int chooseFields(QWidget *parent, bool copy) {
    QDialog d(parent);
    d.setWindowTitle(copy ? QDialog::tr("Select fields to copy") : QDialog::tr("Select fields to paste over"));
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(copy ? QDialog::tr("Please select the fields that you want to copy:")
                                 : QDialog::tr("Please select the fields that you want to paste over:"),
                            &d));
    auto *list = new QListWidget(&d);
    const QString key = copy ? "edit/copyColumnsFields" : "edit/pasteOverFields";
    const int saved = QSettings().value(key, 1024).toInt();
    for (const char *f : {QT_TRANSLATE_NOOP("QDialog", "Comment"), QT_TRANSLATE_NOOP("QDialog", "Layer"),
                          QT_TRANSLATE_NOOP("QDialog", "Start time"), QT_TRANSLATE_NOOP("QDialog", "End time"),
                          QT_TRANSLATE_NOOP("QDialog", "Style"), QT_TRANSLATE_NOOP("QDialog", "Actor"),
                          QT_TRANSLATE_NOOP("QDialog", "Margin left"), QT_TRANSLATE_NOOP("QDialog", "Margin right"),
                          QT_TRANSLATE_NOOP("QDialog", "Margin vertical"), QT_TRANSLATE_NOOP("QDialog", "Effect"),
                          QT_TRANSLATE_NOOP("QDialog", "Text")}) {
        auto *it = new QListWidgetItem(QDialog::tr(f), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(saved & (1 << (list->count() - 1)) ? Qt::Checked : Qt::Unchecked);
    }
    if (copy) {
        auto *it = new QListWidgetItem(QDialog::tr("Text without tags"), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(saved & 2048 ? Qt::Checked : Qt::Unchecked);
    }
    v->addWidget(list);
    auto *quick = new QHBoxLayout;
    auto set = [list](int bits) {
        for (int k = 0; k < list->count(); ++k) list->item(k)->setCheckState(bits & (1 << k) ? Qt::Checked : Qt::Unchecked);
    };
    for (auto [text, bits] : {std::pair{QT_TRANSLATE_NOOP("QDialog", "All"), 2047},
                              {QT_TRANSLATE_NOOP("QDialog", "None"), 0},
                              {QT_TRANSLATE_NOOP("QDialog", "Times"), 4 | 8},
                              {QT_TRANSLATE_NOOP("QDialog", "Text"), 1024}}) {
        auto *b = new QPushButton(QDialog::tr(text), &d);
        b->setAutoDefault(false);
        QObject::connect(b, &QPushButton::clicked, &d, [set, bits] { set(bits); });
        quick->addWidget(b);
    }
    v->addLayout(quick);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return -1;
    int bits = 0;
    for (int k = 0; k < list->count(); ++k)
        if (list->item(k)->checkState() == Qt::Checked) bits |= 1 << k;
    QSettings().setValue(key, bits);
    return bits;
}

// ---- attachments ----------------------------------------------------------------

AttachmentsDialog::AttachmentsDialog(QWidget *parent, TabPanel *tab) : QDialog(parent), m_tab(tab) {
    setWindowTitle(tr("Attachment list"));
    resize(520, 340);
    auto *v = new QVBoxLayout(this);
    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Attachment name"), tr("Size"), tr("Group")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(m_list);
    auto *h = new QHBoxLayout;
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    auto attach = [this](bool font) {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, font ? tr("Attach font") : tr("Attach graphics"), QFileInfo(m_tab->path()).absolutePath(),
            font ? tr("Fonts (*.ttf *.otf *.ttc *.pfb *.pfm)") : tr("Graphics (*.bmp *.gif *.jpg *.jpeg *.ico *.wmf *.png)"));
        for (const QString &f : files) {
            try {
                m_tab->s().attach_file(font, fs::Utf8(f));
            } catch (const std::exception &e) {
                QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
            }
        }
        m_tab->changed();
        refresh();
    };
    button(tr("Attach font"), [attach] { attach(true); });
    button(tr("Attach graphics"), [attach] { attach(false); });
    auto *extract = button(tr("Extract"), [this] {
        const auto items = m_list->selectedItems();
        if (items.isEmpty()) return;
        auto save = [this](QTreeWidgetItem *it, const QString &path) {
            try {
                m_tab->s().extract_attachment(it->data(0, Qt::UserRole).toBool(), fs::Utf8(it->text(0)), fs::Utf8(path));
            } catch (const std::exception &e) {
                QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
            }
        };
        const QString base = QFileInfo(m_tab->path()).absolutePath();
        if (items.size() == 1) {
            const QString f = QFileDialog::getSaveFileName(
                this, tr("Extract"), base + "/" + fs::q(fsb::attachment_file_name(fs::Utf8(items[0]->text(0)))));
            if (!f.isEmpty()) save(items[0], f);
        } else {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Extract to folder"), base);
            if (dir.isEmpty()) return;
            for (auto *it : items) save(it, dir + "/" + fs::q(fsb::attachment_file_name(fs::Utf8(it->text(0)))));
        }
    });
    auto *remove = button(tr("Delete"), [this] {
        for (auto *it : m_list->selectedItems())
            m_tab->s().remove_attachment(it->data(0, Qt::UserRole).toBool(), fs::Utf8(it->text(0)));
        m_tab->changed();
        refresh();
    });
    h->addStretch();
    button(tr("Close"), [this] { accept(); });
    v->addLayout(h);
    auto sync = [this, extract, remove] {
        const bool any = !m_list->selectedItems().isEmpty();
        extract->setEnabled(any);
        remove->setEnabled(any);
    };
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, sync);
    refresh();
    sync();
}

void AttachmentsDialog::refresh() {
    m_list->clear();
    for (const auto &a : m_tab->s().attachments()) {
        auto *it = new QTreeWidgetItem(m_list, {fs::q(a.name), QLocale().formattedDataSize(qint64(a.size)),
                                                a.font ? tr("Fonts") : tr("Graphics")});
        it->setData(0, Qt::UserRole, a.font);
    }
}

// ---- styling assistant ----------------------------------------------------------

// ---- kanji timer (Aegisub) ------------------------------------------------------

KanjiTimerDialog::KanjiTimerDialog(QWidget *parent, TabPanel *tab)
    : QDialog(parent), m_tab(tab), m_kt(fsb::new_kanji_timer()) {
    setWindowTitle(tr("Kanji timing"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    auto *v = new QVBoxLayout(this);
    auto *textBox = new QGroupBox(tr("Text"), this);
    auto *tv = new QVBoxLayout(textBox);
    m_srcView = new QLabel(textBox);
    m_dstView = new QLabel(textBox);
    for (auto *l : {m_srcView, m_dstView}) {
        l->setTextFormat(Qt::RichText);
        l->setMinimumHeight(36);
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 1.6);
        l->setFont(f);
        tv->addWidget(l);
    }
    v->addWidget(textBox);

    auto *row = new QHBoxLayout;
    auto *styles = new QGroupBox(tr("Styles"), this);
    auto *sf = new QFormLayout(styles);
    m_src = new QComboBox(styles);
    m_dst = new QComboBox(styles);
    for (const auto &st : tab->s().styles()) m_src->addItem(fs::q(st.name)), m_dst->addItem(fs::q(st.name));
    QSettings set;
    m_src->setCurrentText(set.value("kanji/source").toString());
    m_dst->setCurrentText(set.value("kanji/dest").toString());
    m_interpolate = new QCheckBox(tr("Attempt to interpolate kanji"), styles);
    m_interpolate->setChecked(set.value("kanji/interpolate", true).toBool());
    sf->addRow(tr("Source:"), m_src);
    sf->addRow(tr("Dest:"), m_dst);
    sf->addRow(m_interpolate);
    row->addWidget(styles, 1);

    auto *cmds = new QGroupBox(tr("Commands"), this);
    auto *cv = new QVBoxLayout(cmds);
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, cmds);
        b->setAutoDefault(false);
        b->setFocusPolicy(Qt::NoFocus); // keys go to the dialog (Enter links, arrows resize)
        connect(b, &QPushButton::clicked, this, fn);
        cv->addWidget(b);
    };
    button(tr("Start"), [this] {
        if (m_src->currentText() == m_dst->currentText()) {
            QMessageBox::warning(this, windowTitle(), tr("The source and destination styles must be different."));
            return;
        }
        QSettings st;
        st.setValue("kanji/source", m_src->currentText());
        st.setValue("kanji/dest", m_dst->currentText());
        m_changes.clear();
        m_srcRow = find(-1, m_src->currentText(), 1);
        m_dstRow = find(-1, m_dst->currentText(), 1);
        reset();
    });
    button(tr("Link"), [this] { link(); });
    button(tr("Unlink"), [this] {
        if (!m_kt->undo()) QApplication::beep();
        refresh();
    });
    button(tr("Skip source line"), [this] {
        m_srcRow = find(m_srcRow, m_src->currentText(), 1);
        reset();
    });
    button(tr("Skip dest line"), [this] {
        m_dstRow = find(m_dstRow, m_dst->currentText(), 1);
        reset();
    });
    button(tr("Go back a line"), [this] {
        if (!m_changes.empty()) m_changes.pop_back();
        m_srcRow = find(m_srcRow, m_src->currentText(), -1);
        m_dstRow = find(m_dstRow, m_dst->currentText(), -1);
        reset();
    });
    button(tr("Accept line"), [this] { acceptLine(); });
    button(tr("Close"), [this] { close(); });
    row->addWidget(cmds);
    v->addLayout(row);
    auto *help = new QLabel(tr("Right / Left: longer / shorter destination selection\n"
                               "Up / Down: longer / shorter source selection\n"
                               "Enter: link, or accept the line when all is linked\n"
                               "Backspace: unlink the last group"),
                            this);
    help->setEnabled(false);
    v->addWidget(help);
    connect(m_interpolate, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("kanji/interpolate", on); });
    refresh();
    resize(720, sizeHint().height());
}

// The next (step 1) or previous (step -1) non-empty line in the style.
int KanjiTimerDialog::find(int from, const QString &style, int step) const {
    if (!m_tab || (from < 0 && step < 0)) return -1;
    const int n = int(m_tab->s().row_count());
    for (int i = from + step; i >= 0 && i < n; i += step) {
        const auto r = m_tab->s().row(size_t(i));
        if (fs::q(r.style) == style && !r.text.empty()) return i;
    }
    return -1;
}

void KanjiTimerDialog::reset() {
    if (m_srcRow < 0 || m_dstRow < 0) {
        m_srcRow = m_dstRow = -1;
        QApplication::beep();
        m_kt = fsb::new_kanji_timer();
    } else {
        m_kt->set_lines(m_tab->s(), size_t(m_srcRow), size_t(m_dstRow));
        if (m_interpolate->isChecked()) m_kt->auto_match();
    }
    setFocus(); // the arrows and Enter drive the matching
    refresh();
}

void KanjiTimerDialog::refresh() {
    const auto st = m_kt->state();
    auto span = [](const QString &t, const char *style) {
        return QString("<span style=\"%1\">%2</span>").arg(style, t.toHtmlEscaped().replace(' ', "&nbsp;"));
    };
    const char *done = "background:#3a3f4b;color:#9aa3b5", *sel = "background:#f08a5d;color:#101010",
               *sep = "color:#5a6070";
    QStringList src, dst;
    for (size_t k = 0; k < st.group_src.size(); ++k) {
        src << span(fs::q(st.group_src[k]), done);
        dst << span(fs::q(st.group_dst[k]), done);
    }
    QString cur;
    for (const auto &s : st.sel_src) cur += fs::q(s);
    if (!cur.isEmpty()) src << span(cur, sel);
    for (const auto &s : st.rest_src) src << span(fs::q(s), "");
    if (!st.sel_dst.empty()) dst << span(fs::q(st.sel_dst), sel);
    if (!st.rest_dst.empty()) dst << span(fs::q(st.rest_dst), "");
    const QString bar = span("|", sep);
    m_srcView->setText(src.join(bar));
    m_dstView->setText(dst.join(bar));
}

void KanjiTimerDialog::link() {
    if (m_kt->accept()) {
        if (m_interpolate->isChecked()) m_kt->auto_match();
    } else {
        QApplication::beep();
    }
    refresh();
}

void KanjiTimerDialog::acceptLine() {
    if (m_dstRow < 0) return;
    if (m_kt->state().remaining_src > 0) {
        QMessageBox::warning(this, windowTitle(), tr("Group all of the source text."));
        return;
    }
    m_changes.emplace_back(size_t(m_dstRow), fs::q(m_kt->output()));
    m_srcRow = find(m_srcRow, m_src->currentText(), 1);
    m_dstRow = find(m_dstRow, m_dst->currentText(), 1);
    reset();
}

void KanjiTimerDialog::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Backspace:
        if (!m_kt->undo()) QApplication::beep();
        break;
    case Qt::Key_Right: m_kt->adjust(1, true); break;
    case Qt::Key_Left: m_kt->adjust(1, false); break;
    case Qt::Key_Up: m_kt->adjust(0, true); break;
    case Qt::Key_Down: m_kt->adjust(0, false); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const auto st = m_kt->state();
        if (st.remaining_src == 0 && st.remaining_dst == 0) acceptLine();
        else link();
        return;
    }
    default: QDialog::keyPressEvent(e); return;
    }
    refresh();
}

// Like Aegisub, the accepted lines are written when the dialog closes, as one undo step.
void KanjiTimerDialog::done(int r) {
    if (m_tab && !m_changes.empty()) {
        std::vector<size_t> rows;
        rust::Vec<rust::String> texts;
        for (const auto &[row, text] : m_changes) rows.push_back(row), texts.push_back(fs::rs(text));
        m_tab->s().set_texts(fs::slice(rows), {texts.data(), texts.size()}, "Kanji timing");
        m_tab->changed();
        m_changes.clear();
    }
    QDialog::done(r);
}

StylingAssistant::StylingAssistant(QWidget *parent, TabPanel *tab) : QDialog(parent), m_tab(tab) {
    setWindowTitle(tr("Styling assistant"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *g = new QGridLayout(this);
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setFixedHeight(64);
    m_styles = new QListWidget(this);
    QStringList names;
    for (const auto &s : tab->s().styles()) names << fs::q(s.name);
    m_styles->addItems(names);
    m_name = new QLineEdit(this);
    // Aegisub completes the typed prefix to the first matching style
    auto *c = new QCompleter(names, m_name);
    c->setCaseSensitivity(Qt::CaseInsensitive);
    c->setCompletionMode(QCompleter::InlineCompletion);
    m_name->setCompleter(c);
    m_seek = new QCheckBox(tr("Seek video to line start time"), this);
    m_seek->setChecked(QSettings().value("styling/seek", true).toBool());
    auto *playAudio = new QPushButton(tr("Play audio"), this), *playVideo = new QPushButton(tr("Play video"), this);
    for (auto *b : {playAudio, playVideo}) b->setAutoDefault(false);
    g->addWidget(new QLabel(tr("Current line"), this), 0, 0, 1, 2);
    g->addWidget(m_text, 1, 0, 1, 2);
    g->addWidget(new QLabel(tr("Styles available"), this), 2, 0);
    g->addWidget(m_styles, 3, 0, 4, 1);
    g->addWidget(new QLabel(tr("Set style (Enter: set and go to the next line)"), this), 2, 1);
    g->addWidget(m_name, 3, 1);
    g->addWidget(m_seek, 4, 1);
    auto *h = new QHBoxLayout;
    h->addWidget(playAudio);
    h->addWidget(playVideo);
    g->addLayout(h, 5, 1);
    g->setRowStretch(6, 1);

    connect(m_name, &QLineEdit::returnPressed, this, [this] { commit(true); });
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &t) {
        const bool known = !m_styles->findItems(t, Qt::MatchFixedString).isEmpty();
        m_name->setStyleSheet(t.isEmpty() || known ? QString() : "background: #803030");
    });
    connect(m_styles, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        m_name->setText(it->text());
        commit(false);
    });
    connect(m_styles, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        m_name->setText(it->text());
        commit(true);
    });
    connect(m_seek, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("styling/seek", on); });
    connect(playAudio, &QPushButton::clicked, this, [this] {
        if (!m_tab) return;
        const fsb::Row r = m_tab->s().row(size_t(m_tab->active()));
        m_tab->audio()->play(r.start, r.end);
        m_name->setFocus();
    });
    connect(playVideo, &QPushButton::clicked, this, [this] {
        if (m_tab) m_tab->playLine(m_tab->active());
        m_name->setFocus();
    });
    connect(tab, &TabPanel::activeChanged, this, &StylingAssistant::lineChanged);
    lineChanged();
}

void StylingAssistant::lineChanged() {
    if (!m_tab || m_tab->active() < 0) return;
    const fsb::Row r = m_tab->s().row(size_t(m_tab->active()));
    m_text->setPlainText(fs::q(r.text));
    m_name->setText(fs::q(r.style));
    m_name->selectAll();
    m_name->setFocus();
    for (auto *it : m_styles->findItems(fs::q(r.style), Qt::MatchFixedString)) m_styles->setCurrentItem(it);
    if (m_seek->isChecked()) m_tab->seekVideo(r.start);
}

void StylingAssistant::commit(bool next) {
    if (!m_tab || m_tab->active() < 0) return;
    const auto found = m_styles->findItems(m_name->text(), Qt::MatchFixedString);
    if (found.isEmpty()) return; // Aegisub: only existing styles
    const std::vector<size_t> row{size_t(m_tab->active())};
    m_tab->s().set_rows_field(fs::slice(row), "style", fs::Utf8(found[0]->text()));
    m_tab->changed();
    if (next && size_t(m_tab->active() + 1) < m_tab->s().row_count()) m_tab->setActive(m_tab->active() + 1);
    else lineChanged();
}

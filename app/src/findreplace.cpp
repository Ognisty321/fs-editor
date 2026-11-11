#include "findreplace.h"

#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QtConcurrentRun>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

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
    for (auto [c, key] : options()) c->setChecked(st.value(key).toBool());
    connect(m_skipTags, &QCheckBox::toggled, m_skipText, [this](bool on) { if (on) m_skipText->setChecked(false); });
    connect(m_skipText, &QCheckBox::toggled, m_skipTags, [this](bool on) { if (on) m_skipTags->setChecked(false); });
    v->addLayout(checks);
    auto *groups = new QHBoxLayout;
    auto *fg = new QGroupBox(tr("In field"), this);
    auto *fl = new QVBoxLayout(fg);
    const char *fields[] = {QT_TR_NOOP("Text"), QT_TR_NOOP("Styles"), QT_TR_NOOP("Actor"), QT_TR_NOOP("Effect")};
    for (int k = 0; k < 4; ++k) fl->addWidget(m_field[k] = new QRadioButton(tr(fields[k]), fg));
    m_field[std::clamp(st.value("find/field").toInt(), 0, 3)]->setChecked(true);
    auto *lg = new QGroupBox(tr("Lines"), this);
    auto *ll = new QVBoxLayout(lg);
    const char *lines[] = {QT_TR_NOOP("All"), QT_TR_NOOP("Selected"), QT_TR_NOOP("From selected")};
    for (int k = 0; k < 3; ++k) ll->addWidget(m_lines[k] = new QRadioButton(tr(lines[k]), lg));
    m_lines[std::clamp(st.value("find/lines").toInt(), 0, 2)]->setChecked(true);
    groups->addWidget(fg);
    groups->addWidget(lg);
    v->addLayout(groups);
    auto *bb = new QHBoxLayout;
    auto *find = new QPushButton(tr("Find"), this);
    auto *findAll = new QPushButton(tr("Find all"), this);
    auto *findOpen = new QPushButton(tr("Find in all open subtitles"), this);
    auto *findFiles = m_findFiles = new QPushButton(tr("Find in files"), this);
    auto *replFiles = m_replFiles = new QPushButton(tr("Replace in files"), this);
    auto *replNext = new QPushButton(tr("Replace next"), this);
    auto *replAll = new QPushButton(tr("Replace all"), this);
    auto *replOpen = new QPushButton(tr("Replace in all open subtitles"), this);
    auto *close = new QPushButton(tr("Close"), this);
    for (auto *b : {find, findAll, findOpen, findFiles, replFiles, replNext, replAll, replOpen, close}) bb->addWidget(b);
    v->addLayout(bb);
    connect(find, &QPushButton::clicked, this, &FindDialog::findNext);
    connect(findAll, &QPushButton::clicked, this, [this] { this->findAll({m_main->current()}); });
    connect(findOpen, &QPushButton::clicked, this, [this] { this->findAll(m_main->tabs()); });
    connect(findFiles, &QPushButton::clicked, this, &FindDialog::findInFiles);
    connect(replFiles, &QPushButton::clicked, this, &FindDialog::replaceInFiles);
    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    connect(replNext, &QPushButton::clicked, this, [this] {
        auto *tab = m_main->current();
        if (!tab || !ready()) return;
        remember(true);
        const auto s = spec(tab);
        // Aegisub: the match shown last is replaced while it is still selected, then the next one shown
        const auto sel = tab->edit()->textSelection(m_hit.original);
        if (m_hitTab == tab && m_hit.row == tab->active() &&
            (s.field != 0 || sel == std::pair{int(m_hit.start), int(m_hit.end)})) {
            try {
                const auto end = tab->s().replace_hit(s, m_hit);
                if (end >= 0) {
                    tab->changed();
                    m_hit.end = size_t(end);
                    if (s.field == 0) tab->edit()->selectText(int(end), 0, m_hit.original);
                }
            } catch (const rust::Error &e) {
                return m_main->error(QString::fromUtf8(e.what()));
            }
        }
        findNext();
    });
    connect(replAll, &QPushButton::clicked, this, [this] {
        auto *tab = m_main->current();
        if (!tab || !ready()) return;
        remember(true);
        try {
            const size_t n = tab->s().replace_all(spec(tab));
            if (n) tab->changed();
            m_main->status(tr("Replacements: %1").arg(n));
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
        }
    });
    connect(replOpen, &QPushButton::clicked, this, [this] {
        if (!ready()) return;
        remember(true);
        size_t n = 0;
        for (auto *tab : m_main->tabs()) {
            try {
                const size_t k = tab->s().replace_all(spec(tab));
                if (k) tab->changed();
                n += k;
            } catch (const rust::Error &e) {
                m_main->error(QString::fromUtf8(e.what()));
                return;
            }
        }
        m_main->status(tr("Replacements: %1").arg(n));
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [=, this](int i) {
        for (QWidget *w : std::initializer_list<QWidget *>{replNext, replAll, replOpen}) w->setVisible(i == 1);
        for (QWidget *w : std::initializer_list<QWidget *>{replLabel, m_replace}) w->setVisible(i >= 1);
        for (QWidget *w : std::initializer_list<QWidget *>{find, findAll, findOpen, lg}) w->setVisible(i < 2);
        for (QWidget *w : std::initializer_list<QWidget *>{filesBox, findFiles, replFiles}) w->setVisible(i == 2);
        (i < 2 ? find : findFiles)->setDefault(true);
        adjustSize();
    });
    // Enter replaces in the Replace with box, and finds in the Search text box
    connect(qApp, &QApplication::focusChanged, this, [=, this](QWidget *, QWidget *now) {
        if (now == m_replace->lineEdit()) replNext->setDefault(true);
        else if (now == m_find->lineEdit()) (m_tabs->currentIndex() < 2 ? find : findFiles)->setDefault(true);
    });
    showTab(0);
}

void FindDialog::showTab(int tab) {
    // the text selected in the edit box is looked for, unless it is the match shown last (Kainote)
    if (auto *t = m_main->current()) {
        for (bool orig : {false, true}) {
            const auto [a, b] = t->edit()->textSelection(orig);
            if (a == b) continue;
            const bool shown = m_hitTab == t && m_hit.row == t->active() && m_hit.original == orig &&
                               a == int(m_hit.start) && b == int(m_hit.end);
            if (!shown) m_find->setCurrentText(t->edit()->selectedText(orig));
            break;
        }
    }
    m_tabs->setCurrentIndex(tab);
    emit m_tabs->currentChanged(tab);
    show();
    raise();
    m_find->setFocus();
    m_find->lineEdit()->selectAll();
}

std::array<std::pair<QCheckBox *, const char *>, 7> FindDialog::options() const {
    return {{{m_case, "find/case"},
             {m_regex, "find/regex"},
             {m_comments, "find/comments"},
             {m_skipTags, "find/skipTags"},
             {m_skipText, "find/skipText"},
             {m_atStart, "find/atStart"},
             {m_atEnd, "find/atEnd"}}};
}

bool FindDialog::ready() const {
    return !m_find->currentText().isEmpty() || m_atStart->isChecked() || m_atEnd->isChecked();
}

void FindDialog::remember(bool replace) {
    QSettings st;
    for (auto [box, key] : {std::pair{m_find, "find/recent"}, std::pair{m_replace, "find/recentReplace"}}) {
        const QString t = box->currentText();
        if ((box == m_replace && !replace) || t.isEmpty() || box->itemText(0) == t) continue;
        QStringList l = st.value(key).toStringList();
        l.removeAll(t);
        l.prepend(t);
        l = l.mid(0, QSettings().value("find/recentCount", 20).toInt());
        st.setValue(key, l);
        box->clear(); // the list shows the new order at once
        box->addItems(l);
    }
    for (auto [c, key] : options()) st.setValue(key, c->isChecked());
    for (int k = 0; k < 4; ++k)
        if (m_field[k]->isChecked()) st.setValue("find/field", k);
    for (int k = 0; k < 3; ++k)
        if (m_lines[k]->isChecked()) st.setValue("find/lines", k);
}

fsb::FindSpec FindDialog::spec(TabPanel *tab) const {
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
    if (!tab) return s;
    // lines hidden by the filter or a folded tree are passed over (Kainote); "Selected" and "From
    // selected" are the current tab's
    const auto vis = tab->grid()->visibleRows();
    const auto sel = tab->selection();
    int lines = tab != m_main->current() ? 0 : m_lines[1]->isChecked() ? 1 : m_lines[2]->isChecked() ? 2 : 0;
    if (lines == 1 && sel.size() < 2) lines = 0; // one line selected: all of them (Aegisub)
    if (lines == 1)
        for (size_t r : sel) s.rows.push_back(r);
    else if (lines == 2 || vis.size() != tab->s().row_count())
        for (size_t r : vis)
            if (lines == 0 || sel.empty() || r >= sel.front()) s.rows.push_back(r);
    return s;
}

// shortcut: a list longer than this would take long to build and longer to read; a model
// instead of items if people need more
constexpr size_t kMaxResults = 20000;

void FindDialog::findAll(const QList<TabPanel *> &tabs) {
    if (!ready()) return;
    remember(false);
    clearResults();
    m_resultSpec = spec(nullptr);
    size_t matches = 0;
    int files = 0;
    for (auto *tab : tabs) {
        const auto s = spec(tab);
        try {
            const auto found = tab->s().find_matches(s, kMaxResults - matches);
            if (found.empty()) continue;
            addResults(tab, tab->title(), tab->path(), found);
            matches += found.size();
            ++files;
        } catch (const rust::Error &e) {
            return m_main->error(QString::fromUtf8(e.what()));
        }
        if (matches == kMaxResults) break;
    }
    showResults(tr("Matches found: %1, in files: %2").arg(matches).arg(files));
}

bool FindDialog::filesReady() {
    const QString dir = m_dir->text().trimmed();
    if (!ready() || !QFileInfo(dir).isDir()) return false;
    QSettings st;
    st.setValue("find/dir", dir);
    st.setValue("find/filter", m_filter->text());
    st.setValue("find/subdirs", m_subdirs->isChecked());
    try {
        (void)fsb::new_session()->find_all(spec(nullptr)); // a bad regex fails here once, not for every file
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

void FindDialog::findInFiles() {
    if (!filesReady()) return;
    remember(false);
    const QString dir = m_dir->text().trimmed();
    const auto s = spec(nullptr);
    // the files are read on a worker, each into its own session
    struct Found {
        std::vector<std::pair<QString, rust::Vec<fsb::FindMatch>>> files; // path, its matches
        int searched = 0;
    };
    const QStringList filters = m_filter->text().split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
    const auto flags = m_subdirs->isChecked() ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    m_findFiles->setEnabled(false);
    m_main->status(tr("Searching in %1...").arg(dir));
    auto *watch = new QFutureWatcher<Found>(this);
    connect(watch, &QFutureWatcher<Found>::finished, this, [this, watch, dir, s] {
        const Found f = watch->result();
        watch->deleteLater();
        m_findFiles->setEnabled(true);
        clearResults();
        m_resultSpec = s;
        size_t matches = 0;
        for (const auto &[path, found] : f.files) {
            addResults(nullptr, QDir(dir).relativeFilePath(path), path, found);
            matches += found.size();
        }
        showResults(tr("Matches found: %1, in files: %2 (searched: %3)").arg(matches).arg(f.files.size()).arg(f.searched));
    });
    watch->setFuture(QtConcurrent::run([dir, filters, flags, s] {
        Found f;
        size_t total = 0;
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
            auto found = file->find_matches(s, kMaxResults - total);
            if (found.empty()) continue;
            total += found.size();
            f.files.emplace_back(path, std::move(found));
            if (total == kMaxResults) break;
        }
        return f;
    }));
}

void FindDialog::replaceInFiles() {
    if (!filesReady()) return;
    const QString dir = m_dir->text().trimmed();
    const QString backup = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ReplaceBackup/" +
                           QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    const bool smoke = qEnvironmentVariableIsSet("FSED_SMOKE"); // asks nothing in a smoke test
    if (!smoke && QMessageBox::question(this, tr("Replace in files"),
                              tr("Replace in every matching file in %1?\n\nEach file changed is first copied to %2")
                                  .arg(QDir::toNativeSeparators(dir), QDir::toNativeSeparators(backup))) != QMessageBox::Yes)
        return;
    remember(true);
    const auto s = spec(nullptr);
    const QStringList filters = m_filter->text().split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
    const auto flags = m_subdirs->isChecked() ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    const QString noCopy = tr("could not be copied to the backup folder, left as it was");
    struct Done {
        int files = 0;
        size_t replaced = 0;
        QStringList failed;
    };
    m_findFiles->setEnabled(false);
    m_replFiles->setEnabled(false);
    m_main->status(tr("Replacing in %1...").arg(dir));
    auto *watch = new QFutureWatcher<Done>(this);
    connect(watch, &QFutureWatcher<Done>::finished, this, [this, watch, backup, smoke] {
        const Done d = watch->result();
        watch->deleteLater();
        m_findFiles->setEnabled(true);
        m_replFiles->setEnabled(true);
        QString msg = tr("Replaced %1 times in %2 files.").arg(d.replaced).arg(d.files);
        if (d.files) msg += "\n" + tr("The files as they were: %1").arg(QDir::toNativeSeparators(backup));
        if (!d.failed.isEmpty()) msg += "\n\n" + tr("Not saved:") + "\n" + d.failed.join('\n');
        m_main->status(tr("Replaced %1 times in %2 files.").arg(d.replaced).arg(d.files));
        if (!smoke) QMessageBox::information(this, tr("Replace in files"), msg);
        // a file among them open in a tab: the tab would save its old lines over the new ones
        if (d.files) m_main->rereadChanged();
    });
    watch->setFuture(QtConcurrent::run([dir, filters, flags, s, backup, noCopy] {
        Done d;
        QDirIterator it(dir, filters, QDir::Files, flags);
        while (it.hasNext()) {
            const QString path = it.next();
            auto file = fsb::new_session();
            try {
                file->open(fs::Utf8(path));
            } catch (const rust::Error &) {
                continue; // not subtitles after all
            }
            try {
                const size_t n = file->replace_all(s);
                if (!n) continue;
                // never saved without its copy
                const QString copy = backup + "/" + QDir(dir).relativeFilePath(path);
                if (!QDir().mkpath(QFileInfo(copy).path()) || !QFile::copy(path, copy)) {
                    d.failed << QDir::toNativeSeparators(path) + ": " + noCopy;
                    continue;
                }
                file->save(fs::Utf8(path)); // in its own format and encoding
                ++d.files;
                d.replaced += n;
            } catch (const rust::Error &e) {
                d.failed << QString::fromUtf8(e.what());
            }
        }
        return d;
    }));
}

namespace {
// the match drawn over its line's text (Kainote's results)
class MatchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        QStyledItemDelegate::paint(p, o, i);
        const QPoint at = i.data(Qt::UserRole).toPoint();
        if (at.y() <= at.x()) return;
        const QString text = i.data().toString();
        const QFontMetrics fm(o.font);
        const int left = o.rect.x() + o.widget->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, o.widget) + 1;
        const int x0 = left + fm.horizontalAdvance(text.left(at.x())), x1 = left + fm.horizontalAdvance(text.left(at.y()));
        p->save();
        p->setClipRect(o.rect);
        p->fillRect(QRect(x0, o.rect.y() + 1, std::max(2, x1 - x0), o.rect.height() - 2), QColor(255, 190, 0, 90));
        p->restore();
    }
};
} // namespace

QAbstractItemDelegate *matchDelegate(QObject *parent) { return new MatchDelegate(parent); }

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
        m_tree->setItemDelegateForColumn(1, new MatchDelegate(m_tree));
        connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) { goToResult(it); });
        // Kainote's: the matches ticked are replaced, each where it was found
        auto *row = new QHBoxLayout;
        const auto tickAll = [this](Qt::CheckState state) {
            for (int f = 0; f < m_tree->topLevelItemCount(); ++f) m_tree->topLevelItem(f)->setCheckState(0, state);
        };
        auto *all = new QPushButton(tr("Check all"), m_results);
        connect(all, &QPushButton::clicked, this, [tickAll] { tickAll(Qt::Checked); });
        auto *none = new QPushButton(tr("Uncheck all"), m_results);
        connect(none, &QPushButton::clicked, this, [tickAll] { tickAll(Qt::Unchecked); });
        m_replaceChecked = new QPushButton(tr("Replace"), m_results);
        m_replaceChecked->setToolTip(tr("Replace the matches ticked"));
        connect(m_replaceChecked, &QPushButton::clicked, this, &FindDialog::replaceChecked);
        m_resultReplace = new QLineEdit(m_results);
        m_resultReplace->setPlaceholderText(tr("Replace with"));
        for (QWidget *w : std::initializer_list<QWidget *>{all, none, m_replaceChecked}) row->addWidget(w);
        row->addWidget(m_resultReplace, 1);
        v->addLayout(row);
        m_results->resize(700, 400);
    }
    m_tree->clear();
    m_resultTabs.clear();
    m_replaceChecked->setEnabled(true);
    m_resultReplace->setText(m_replace->currentText());
}

void FindDialog::replaceChecked() {
    m_resultSpec.replacement = fs::rs(m_resultReplace->text());
    size_t replaced = 0, ticked = 0;
    const QString backup = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ReplaceBackup/" +
                           QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    for (int f = 0; f < m_tree->topLevelItemCount(); ++f) {
        auto *file = m_tree->topLevelItem(f);
        std::vector<fsb::FindMatch> matches;
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
        }
        if (matches.empty()) continue;
        ticked += matches.size();
        const rust::Slice<const fsb::FindMatch> ticks{matches.data(), matches.size()};
        const QString path = file->data(0, Qt::UserRole + 1).toString();
        QPointer<TabPanel> &tab = m_resultTabs[file->data(0, Qt::UserRole).toInt()];
        if (!tab && !path.isEmpty()) // a file from the folder open in a tab: replaced there, unsaved
            for (auto *t : m_main->tabs())
                if (!t->path().isEmpty() && QFileInfo(t->path()) == QFileInfo(path)) tab = t;
        try {
            if (tab) {
                const size_t n = tab->s().replace_matches(m_resultSpec, ticks);
                if (n) tab->changed();
                replaced += n;
            } else if (!path.isEmpty()) {
                auto session = fsb::new_session();
                session->open(fs::Utf8(path));
                const size_t n = session->replace_matches(m_resultSpec, ticks);
                if (!n) continue;
                // never saved without its copy (Kainote's ReplaceBackup)
                // under its path in the folder searched; a name that would leave the backup
                // folder is cut to the file's own
                QString name = file->text(0);
                if (QDir::isAbsolutePath(name) || QDir::cleanPath(name).startsWith("..")) name = QFileInfo(path).fileName();
                const QString copy = backup + "/" + name;
                if (!QDir().mkpath(QFileInfo(copy).path()) || !QFile::copy(path, copy)) {
                    m_main->error(tr("%1 could not be copied to the backup folder, left as it was")
                                      .arg(QDir::toNativeSeparators(path)));
                    continue;
                }
                session->save(fs::Utf8(path)); // in its own format and encoding
                replaced += n;
            }
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
        }
    }
    if (!ticked) return m_main->status(tr("No matches ticked"));
    // the places found are stale now: a new search before the next replacing
    m_replaceChecked->setEnabled(false);
    for (int f = 0; f < m_tree->topLevelItemCount(); ++f) m_tree->topLevelItem(f)->setCheckState(0, Qt::Unchecked);
    const QString done = tr("Replaced %1 of %2 matches ticked").arg(replaced).arg(ticked);
    m_main->status(replaced < ticked ? done + tr("; the lines changed since the search were left") : done);
}

void FindDialog::addResults(TabPanel *tab, const QString &title, const QString &path,
                            const rust::Vec<fsb::FindMatch> &found) {
    auto *file = new QTreeWidgetItem(m_tree, {title});
    file->setFirstColumnSpanned(true);
    file->setFlags(file->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    file->setCheckState(0, Qt::Unchecked);
    file->setData(0, Qt::UserRole, int(m_resultTabs.size()));
    file->setData(0, Qt::UserRole + 1, path);
    file->setToolTip(0, path);
    m_resultTabs.append(tab);
    for (const auto &m : found) {
        auto *it = new QTreeWidgetItem(file, {QString::number(m.row + 1), fs::q(m.text)});
        it->setData(0, Qt::UserRole, qulonglong(m.row));
        it->setData(0, Qt::UserRole + 1, m.original);
        it->setData(1, Qt::UserRole, QPoint(int(m.start), int(m.end)));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, Qt::Unchecked);
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
    const auto row = item->data(0, Qt::UserRole).toULongLong();
    if (row >= tab->s().row_count()) return; // the file changed since
    tab->setActive(int(row));
    // the match selected in the edit box, and Find next goes on from it (Kainote)
    const QPoint at = item->data(1, Qt::UserRole).toPoint();
    m_hit = {int64_t(row), item->data(0, Qt::UserRole + 1).toBool(), size_t(at.x()), size_t(at.y())};
    m_hitTab = tab;
    if (spec(nullptr).field == 0) tab->edit()->selectText(at.x(), at.y() - at.x(), m_hit.original);
}

// Aegisub's: match by match through the lines, each shown selected in the line's text
void FindDialog::findNext() {
    auto *tab = m_main->current();
    if (!tab || !ready()) return;
    remember(false);
    const auto s = spec(tab);
    const int act = std::max(0, tab->active());
    // on from the match shown last (or the cursor moved after it); a line not searched yet, from its start
    const bool on = m_hitTab == tab && m_hit.row == act;
    const bool orig = !on || m_hit.original;
    const size_t pos = !on ? 0 : s.field == 0 ? size_t(tab->edit()->textSelection(orig).second) : m_hit.end;
    const size_t n = tab->s().row_count();
    if (n == 0) return m_main->status(tr("Nothing found")); // no line to go on from
    try {
        auto &subs = tab->s();
        // the style, actor and effect are searched line by line (Aegisub); an empty match (^, $, \b)
        // is passed by a character, and where that finds it again, by the half or line it is in
        auto hit = on && s.field != 0                         ? subs.find_next(s, (size_t(act) + 1) % n, true, 0)
                   : on && m_hit.start == m_hit.end && pos == m_hit.end ? subs.find_next(s, size_t(act), orig, pos + 1)
                                                                        : subs.find_next(s, size_t(act), orig, pos);
        if (on && hit.row == m_hit.row && hit.original == m_hit.original && hit.start == m_hit.start &&
            hit.end == m_hit.end && hit.start == hit.end)
            hit = m_hit.original ? subs.find_next(s, size_t(act), false, 0) : subs.find_next(s, (size_t(act) + 1) % n, true, 0);
        if (hit.row < 0) return m_main->status(tr("Nothing found"));
        m_hit = hit;
        m_hitTab = tab;
        // going through the selected lines leaves the selection as it is (Aegisub)
        if (hit.row != tab->active()) tab->setActive(int(hit.row), !(m_lines[1]->isChecked() && tab->selection().size() > 1));
        if (s.field == 0) tab->edit()->selectText(int(hit.start), int(hit.end - hit.start), hit.original);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
    }
}

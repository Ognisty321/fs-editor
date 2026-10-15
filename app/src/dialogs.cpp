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
#include <QAction>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDataStream>
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
#include <QStyledItemDelegate>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QtConcurrentRun>
#include <QTemporaryDir>
#include <QTimer>
#include <QMenu>
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
        l = l.mid(0, 20);
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
    connect(watch, &QFutureWatcher<Found>::finished, this, [this, watch, dir] {
        const Found f = watch->result();
        watch->deleteLater();
        m_findFiles->setEnabled(true);
        clearResults();
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
        m_results->resize(700, 400);
    }
    m_tree->clear();
    m_resultTabs.clear();
}

void FindDialog::addResults(TabPanel *tab, const QString &title, const QString &path,
                            const rust::Vec<fsb::FindMatch> &found) {
    auto *file = new QTreeWidgetItem(m_tree, {title});
    file->setFirstColumnSpanned(true);
    file->setData(0, Qt::UserRole, int(m_resultTabs.size()));
    file->setData(0, Qt::UserRole + 1, path);
    file->setToolTip(0, path);
    m_resultTabs.append(tab);
    for (const auto &m : found) {
        auto *it = new QTreeWidgetItem(file, {QString::number(m.row + 1), fs::q(m.text)});
        it->setData(0, Qt::UserRole, qulonglong(m.row));
        it->setData(0, Qt::UserRole + 1, m.original);
        it->setData(1, Qt::UserRole, QPoint(int(m.start), int(m.end)));
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

// ---- select lines -------------------------------------------------------------

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
// Style catalogs are files of Style: lines, as Aegisub and Kainote keep them: theirs can be copied in
QString catalogFile(const QString &name) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/catalog/" + name + ".sty";
}
template <class List> QStringList namesOf(const List &styles) {
    QStringList l;
    for (const auto &s : styles) l << fs::q(s.name);
    return l;
}
// Aegisub's unique_name: "X - copy", then "X - copy (2)"...
QString uniqueName(const QString &first, const QStringList &taken) {
    QString n = first;
    for (int i = 2; taken.contains(n); ++i) n = QString("%1 (%2)").arg(first).arg(i);
    return n;
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
    auto ispin = [this](int v) {
        auto *s = new QSpinBox(this);
        s->setRange(-9999, 99999); // Aegisub's ranges, here and below
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
    m_size = dspin(style.size, 0, 10000);
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
        // the transparency picked too, and the preview follows the picker; Cancel puts it back (Aegisub)
        connect(b, &QToolButton::clicked, this, [this, k, paint] {
            const QString before = m_colors[k];
            const int alphaBefore = m_alpha[k]->value();
            QColor start = assColor(m_colors[k]);
            start.setAlpha(255 - alphaBefore);
            QColorDialog d(start, this);
            d.setOption(QColorDialog::ShowAlphaChannel);
            connect(&d, &QColorDialog::currentColorChanged, this, [&](const QColor &c) {
                m_colors[k] = assColorString(c);
                {
                    const QSignalBlocker block(m_alpha[k]);
                    m_alpha[k]->setValue(255 - c.alpha());
                }
                paint();
                preview();
            });
            if (d.exec() == QDialog::Accepted) return;
            m_colors[k] = before;
            m_alpha[k]->setValue(alphaBefore);
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
    // the preview's background, a colour or the checkerboard (Aegisub's colour button)
    m_previewBg = QSettings().value("styles/previewBg").value<QColor>();
    auto *bg = new QToolButton(this);
    bg->setIcon(icon("palette"));
    bg->setToolTip(tr("Preview background"));
    bg->setPopupMode(QToolButton::InstantPopup);
    auto *bgMenu = new QMenu(bg);
    const auto setBg = [this](QColor c) {
        m_previewBg = c;
        QSettings().setValue("styles/previewBg", c);
        preview();
    };
    bgMenu->addAction(tr("Checkerboard"), this, [setBg] { setBg({}); });
    bgMenu->addAction(tr("Colour..."), this, [this, setBg] {
        if (const QColor c = QColorDialog::getColor(m_previewBg.isValid() ? m_previewBg : Qt::black, this); c.isValid()) setBg(c);
    });
    bg->setMenu(bgMenu);
    auto *textRow = new QHBoxLayout;
    textRow->addWidget(m_previewText, 1);
    textRow->addWidget(bg);
    prevL->addLayout(textRow);
    prevL->addWidget(m_preview, 1);
    group(left, tr("Preview"), prevL);
    connect(m_previewText, &QLineEdit::textChanged, this, &StyleEditor::preview);

    auto *borderL = new QGridLayout;
    m_outline = dspin(style.outline, 0, 1000);
    m_shadow = dspin(style.shadow, 0, 1000);
    m_border = new QComboBox(this); // border style 1, 3 or 4, as Aegisub offers them
    m_border->addItems({tr("Outline"), tr("Opaque box"), tr("Shadow box (libass)")});
    m_border->setCurrentIndex(style.border_style == 3 ? 1 : style.border_style == 4 ? 2 : 0);
    labelled(borderL, 0, 0, tr("Outline:"), m_outline);
    labelled(borderL, 0, 1, tr("Shadow:"), m_shadow);
    borderL->addWidget(m_border, 2, 0, 1, 2);
    group(right, tr("Outline"), borderL);

    auto *geoL = new QGridLayout;
    m_scaleX = dspin(style.scale_x, 0, 10000);
    m_scaleY = dspin(style.scale_y, 0, 10000);
    m_angle = dspin(style.angle, -360, 360);
    m_spacing = dspin(style.spacing, -1000, 1000);
    labelled(geoL, 0, 0, tr("Scale X:"), m_scaleX);
    labelled(geoL, 0, 1, tr("Scale Y:"), m_scaleY);
    labelled(geoL, 1, 0, tr("Angle:"), m_angle);
    labelled(geoL, 1, 1, tr("Spacing:"), m_spacing);
    group(right, tr("Miscellaneous"), geoL);

    auto *marginL = new QGridLayout;
    m_ml = ispin(style.margin_l);
    m_mr = ispin(style.margin_r);
    m_mv = ispin(style.margin_v);
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
    connect(m_border, &QComboBox::currentIndexChanged, this, &StyleEditor::preview);
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
    s.font = fs::rs(m_font->currentText().replace(',', ';')); // a comma would split the Style line
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
    s.border_style = std::array{1, 3, 4}[size_t(std::max(0, m_border->currentIndex()))];
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
    if (m_previewBg.isValid()) img.fill(m_previewBg);
    else {
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
            .arg(w).arg(h).arg(fs::q(fsb::style_lines({&st, 1})), m_previewText->text());
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
    const QDir dir = QFileInfo(catalogFile("x")).dir();
    dir.mkpath(".");
    QSettings st;
    // catalogs kept in the settings before become their files, once
    for (const QString &n : st.value("styles/catalogs").toStringList()) {
        QFile f(catalogFile(QString(n).replace(QRegularExpression(R"([\\/:*?"<>|])"), "_")));
        if (!f.exists() && f.open(QIODevice::WriteOnly))
            f.write("\xEF\xBB\xBF" + st.value("styles/catalog/" + n).toStringList().join('\n').toUtf8() + '\n');
        if (f.exists()) st.remove("styles/catalog/" + n);
    }
    st.remove("styles/catalogs");
    QStringList cats = styleCatalogs();
    if (cats.isEmpty()) cats << "Default";
    m_catalog->addItems(cats);
    auto *newCat = new QPushButton(tr("New"), this);
    auto *delCat = m_delCat = new QPushButton(tr("Delete"), this);
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
    auto *sNew = new QPushButton(tr("New"), this), *sCopy = new QPushButton(tr("Copy"), this),
         *sEdit = new QPushButton(tr("Edit"), this), *sDel = new QPushButton(tr("Delete"), this),
         *sSort = new QPushButton(tr("Sort"), this), *sImport = new QPushButton(tr("Import..."), this);
    sImport->setToolTip(tr("Import styles from a subtitle file into the catalog"));
    for (auto *b : {sNew, sCopy, sEdit, sDel, sSort, sImport}) sb->addWidget(b);
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
    m_forOne[0] = {sCopy, sEdit};
    m_forAny[0] = {sDel, toFile, toAll};
    m_forOne[1] = {fCopy, fEdit};
    m_forAny[1] = {fDel, toStore};
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
            m_moves[storage ? 0 : 1][k] = b;
        }
        column->insertLayout(column->count() - 1, row); // above the stretch
    };
    moveButtons(sb, true);
    moveButtons(fb, false);
    auto *close = new QPushButton(tr("Close"), this);
    g->addWidget(close, 6, 1);
    connect(close, &QPushButton::clicked, this, &QDialog::close);

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
        auto d = defaultStyle();
        d.name = fs::rs(uniqueName("Default", namesOf(session().styles())));
        StyleEditor e(d, this);
        e.onApply = [&](const fsb::StyleData &s) { return applyToFile(idx, s); };
        e.exec();
    });
    // Copy opens the editor on the copy, which OK adds (Aegisub)
    connect(fCopy, &QPushButton::clicked, this, [=, this] {
        const int i = m_file->currentRow();
        if (i < 0) return;
        const auto styles = session().styles();
        auto s = styles[size_t(i)];
        s.name = fs::rs(uniqueName(fs::q(s.name) + tr(" - copy"), namesOf(styles)));
        int idx = -1;
        StyleEditor e(s, this);
        e.onApply = [&](const fsb::StyleData &st) { return applyToFile(idx, st); };
        e.exec();
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
    connect(m_file, &QListWidget::itemActivated, this, editFile); // a double click or Enter
    connect(fDel, &QPushButton::clicked, this, [=, this] {
        std::vector<size_t> rows;
        for (const auto &r : m_file->selectionModel()->selectedRows()) rows.push_back(size_t(r.row()));
        if (rows.empty() || !m_main->confirm(tr("Delete the selected styles?"))) return;
        session().delete_styles({rows.data(), rows.size()});
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
        std::vector<size_t> all(session().styles().size());
        std::iota(all.begin(), all.end(), size_t(0));
        session().delete_styles({all.data(), all.size()});
        edited();
    });
    const auto selectedStored = [this] {
        std::vector<fsb::StyleData> v;
        for (int r = 0; r < m_storage->count(); ++r)
            if (m_storage->item(r)->isSelected()) v.push_back(m_stored[size_t(r)]);
        return v;
    };
    connect(toFile, &QPushButton::clicked, this, [=, this] {
        const auto chosen = askOverwrite(selectedStored(), namesOf(session().styles()));
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    });
    connect(toAll, &QPushButton::clicked, this, [=, this] {
        QStringList taken;
        for (auto *tab : m_main->tabs()) taken << namesOf(tab->s().styles());
        const auto chosen = askOverwrite(selectedStored(), taken);
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
    // Aegisub's "Import from script": pick styles of another file, asking before overwriting;
    // Kainote's "Load" does the same for the catalog
    const auto importStyles = [=, this](bool storage) {
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
        std::vector<fsb::StyleData> picked;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->isSelected()) picked.push_back(styles[size_t(r)]);
        if (storage) return addToStorage(picked);
        const auto chosen = askOverwrite(picked, namesOf(session().styles()));
        if (chosen.empty()) return;
        session().add_styles({chosen.data(), chosen.size()});
        edited();
    };
    connect(fImport, &QPushButton::clicked, this, [importStyles] { importStyles(false); });
    connect(sImport, &QPushButton::clicked, this, [importStyles] { importStyles(true); });
    connect(toStore, &QPushButton::clicked, this, [=, this] {
        const auto styles = session().styles();
        std::vector<fsb::StyleData> picked;
        for (auto *it : m_file->selectedItems()) picked.push_back(styles[size_t(m_file->row(it))]);
        addToStorage(picked);
    });
    connect(sNew, &QPushButton::clicked, this, [=, this] {
        int idx = -1;
        auto d = defaultStyle();
        d.name = fs::rs(uniqueName("Default", namesOf(m_stored)));
        StyleEditor e(d, this);
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
    connect(m_storage, &QListWidget::itemActivated, this, editStored);
    connect(sCopy, &QPushButton::clicked, this, [this] {
        const int i = m_storage->currentRow();
        if (i < 0) return;
        auto s = m_stored[size_t(i)];
        s.name = fs::rs(uniqueName(fs::q(s.name) + tr(" - copy"), namesOf(m_stored)));
        int idx = -1;
        StyleEditor e(s, this);
        e.onApply = [&](const fsb::StyleData &st) { return applyToStorage(idx, st); };
        e.exec();
    });
    connect(sDel, &QPushButton::clicked, this, [this] {
        auto rows = m_storage->selectionModel()->selectedRows();
        if (rows.empty() || !m_main->confirm(tr("Delete the selected styles?"))) return;
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
        QString n = QInputDialog::getText(this, tr("Catalog"), tr("Name"), QLineEdit::Normal, {}, &ok).trimmed();
        n.replace(QRegularExpression(R"([\\/:*?"<>|])"), "_"); // it names a file (Aegisub)
        if (!ok || n.isEmpty()) return;
        // file names ignore case on Windows: the one there is
        if (const int k = m_catalog->findText(n, Qt::MatchFixedString); k >= 0) return m_catalog->setCurrentIndex(k);
        m_catalog->addItem(n);
        m_catalog->setCurrentText(n);
        saveCatalog(); // listed next time even while empty
    });
    connect(delCat, &QPushButton::clicked, this, [this] {
        if (m_catalog->count() <= 1 || !m_main->confirm(tr("Delete the catalog \"%1\"?").arg(m_catalog->currentText())))
            return;
        QFile::remove(catalogFile(m_catalog->currentText()));
        m_catalog->removeItem(m_catalog->currentIndex());
    });
    connect(m_catalog, &QComboBox::currentTextChanged, this, [this] {
        loadCatalog();
        reload();
        // with the file's project settings, as Aegisub keeps it: not an edit to undo
        m_main->current()->s().set_project_value("Last Style Storage", fs::Utf8(m_catalog->currentText()));
    });
    // Delete, Ctrl+C and Ctrl+V on the lists (Kainote); Enter edits, so no button is the default
    for (const bool storage : {true, false}) {
        QListWidget *list = storage ? m_storage : m_file;
        const auto key = [this, list](QKeySequence k, auto fn) {
            auto *a = new QAction(list);
            a->setShortcut(k);
            a->setShortcutContext(Qt::WidgetShortcut);
            connect(a, &QAction::triggered, this, fn);
            list->addAction(a);
        };
        key(QKeySequence::Delete, [b = storage ? sDel : fDel] { b->click(); });
        key(QKeySequence::Copy, [this, storage] { copyStyles(storage); });
        key(QKeySequence::Paste, [this, storage] { pasteStyles(storage); });
        connect(list, &QListWidget::itemSelectionChanged, this, &StyleManager::updateButtons);
    }
    for (auto *b : findChildren<QPushButton *>()) b->setAutoDefault(false);
    const QString last = fs::q(m_main->current()->s().project_value("Last Style Storage"));
    if (!last.isEmpty() && cats.contains(last)) m_catalog->setCurrentText(last);
    loadCatalog();
    tabChanged();
    // the active line's style picked (Aegisub)
    if (auto *tab = m_main->current(); tab->active() >= 0)
        if (const auto items = m_file->findItems(fs::q(tab->s().row(size_t(tab->active())).style), Qt::MatchExactly);
            !items.isEmpty())
            m_file->setCurrentItem(items.first());
    resize(560, 640);
}

void StyleManager::tabChanged() {
    disconnect(m_docConn);
    if (auto *tab = m_main->current())
        m_docConn = connect(tab, &TabPanel::docChanged, this, [this] {
            if (isVisible()) reload(); // an undo, a style edited in the line's box
        });
    reload();
}

void StyleManager::updateButtons() {
    m_delCat->setEnabled(m_catalog->count() > 1);
    for (int k = 0; k < 2; ++k) {
        QListWidget *list = k ? m_file : m_storage;
        int first = -1, last = -1, n = 0;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->isSelected()) {
                if (first < 0) first = r;
                last = r;
                ++n;
            }
        for (auto *b : m_forOne[k]) b->setEnabled(n == 1);
        for (auto *b : m_forAny[k]) b->setEnabled(n > 0);
        // a block moves only when it is one
        const bool block = n > 0 && last - first + 1 == n;
        for (int m = 0; m < 4; ++m) m_moves[k][m]->setEnabled(block && (m < 2 ? first > 0 : last < list->count() - 1));
    }
}

void StyleManager::addToStorage(const std::vector<fsb::StyleData> &styles) {
    for (const auto &st : askOverwrite(styles, namesOf(m_stored))) {
        auto same = std::find_if(m_stored.begin(), m_stored.end(), [&](auto &x) { return fs::q(x.name) == fs::q(st.name); });
        if (same != m_stored.end()) *same = st;
        else m_stored.push_back(st);
    }
    saveCatalog();
    reload();
}

void StyleManager::copyStyles(bool storage) {
    std::vector<fsb::StyleData> v;
    const auto styles = m_main->current()->s().styles();
    QListWidget *list = storage ? m_storage : m_file;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->isSelected()) v.push_back(storage ? m_stored[size_t(r)] : styles[size_t(r)]);
    if (!v.empty()) QApplication::clipboard()->setText(fs::q(fsb::style_lines({v.data(), v.size()})));
}

void StyleManager::pasteStyles(bool storage) {
    const auto parsed = fsb::parse_style_lines(fs::Utf8(QApplication::clipboard()->text()));
    const std::vector<fsb::StyleData> v(parsed.begin(), parsed.end());
    if (v.empty()) return;
    if (storage) return addToStorage(v);
    auto &s = m_main->current()->s();
    const auto chosen = askOverwrite(v, namesOf(s.styles()));
    if (chosen.empty()) return;
    s.add_styles({chosen.data(), chosen.size()});
    m_main->current()->changed();
    reload();
}

void StyleManager::loadCatalog() { m_stored = catalogStyles(m_catalog->currentText()); }

QStringList styleCatalogs() {
    QStringList cats;
    for (const QFileInfo &fi : QFileInfo(catalogFile("x")).dir().entryInfoList({"*.sty"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        cats << fi.completeBaseName();
    return cats;
}

std::vector<fsb::StyleData> catalogStyles(const QString &name) {
    QFile f(catalogFile(name));
    if (!f.open(QIODevice::ReadOnly)) return {}; // a catalog with nothing saved yet
    const auto parsed = fsb::parse_style_lines(fs::Utf8(QString::fromUtf8(f.readAll())));
    return {parsed.begin(), parsed.end()};
}

void StyleManager::saveCatalog() {
    QSaveFile f(catalogFile(m_catalog->currentText()));
    if (!f.open(QIODevice::WriteOnly)) return m_main->error(f.errorString());
    f.write("\xEF\xBB\xBF"); // UTF-8 with a BOM, as Aegisub writes them
    f.write(fs::q(fsb::style_lines({m_stored.data(), m_stored.size()})).toUtf8());
    if (!f.commit()) m_main->error(f.errorString());
}

void StyleManager::moveStyles(bool storage, int type) {
    QListWidget *list = storage ? m_storage : m_file;
    int first = list->count(), last = -1;
    for (int r = 0; r < list->count(); ++r)
        if (list->item(r)->isSelected()) first = std::min(first, r), last = r;
    if (last < 0) return;
    for (int r = first; r <= last; ++r)
        if (!list->item(r)->isSelected()) return; // Aegisub moves only a block
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

std::vector<fsb::StyleData> StyleManager::askOverwrite(const std::vector<fsb::StyleData> &chosen,
                                                       const QStringList &taken) {
    std::vector<fsb::StyleData> out;
    bool all = false;
    for (const auto &s : chosen) {
        if (!all && taken.contains(fs::q(s.name))) {
            const auto b = QMessageBox::question(this, tr("Style name collision"),
                                                 tr("A style named \"%1\" already exists. Replace it?").arg(fs::q(s.name)),
                                                 QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::No |
                                                     QMessageBox::Cancel);
            if (b == QMessageBox::Cancel) return {};
            if (b == QMessageBox::No) continue;
            all = b == QMessageBox::YesToAll;
        }
        out.push_back(s);
    }
    return out;
}

void StyleManager::reload() {
    // shortcut: the fonts installed when the manager first opened, and a font attached to the
    // file shows as missing; read them again if people install fonts while it is open
    static const QSet<QString> fonts = [] {
        QSet<QString> f;
        for (const QString &n : QFontDatabase::families()) f.insert(n.toLower());
        return f;
    }();
    const auto refill = [this](QListWidget *list, const auto &styles) {
        QSet<QString> selected;
        for (auto *it : list->selectedItems()) selected.insert(it->text());
        const QString current = list->currentItem() ? list->currentItem()->text() : QString();
        const QSignalBlocker block(list);
        list->clear();
        for (const auto &s : styles) {
            auto *it = new QListWidgetItem(fs::q(s.name), list);
            QString font = fs::q(s.font);
            if (font.startsWith('@')) font.remove(0, 1); // vertical text
            if (!fonts.contains(font.toLower())) { // Kainote marks them
                it->setForeground(QColor(230, 70, 70));
                it->setToolTip(tr("The font \"%1\" is not installed").arg(font));
            }
            it->setSelected(selected.contains(it->text()));
            if (it->text() == current) list->setCurrentItem(it, QItemSelectionModel::NoUpdate);
        }
    };
    refill(m_storage, m_stored);
    if (auto *tab = m_main->current()) refill(m_file, tab->s().styles());
    updateButtons();
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
    // Aegisub asks whether the lines (and \r tags) that used the old name follow a rename
    bool lines = true;
    if (index >= 0 && fs::q(styles[size_t(index)].name) != fs::q(s.name) &&
        session.style_in_use(fs::Utf8(fs::q(styles[size_t(index)].name)))) {
        const auto b = QMessageBox::question(
            this, tr("Update script?"), tr("Do you want to change all instances of this style in the script to this new name?"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (b == QMessageBox::Cancel) return false;
        lines = b == QMessageBox::Yes;
    }
    session.set_style(index, s, lines);
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
        {QT_TR_NOOP("Timer"), "Original Timing"}, {QT_TR_NOOP("Synch point"), "Synch Point"},
        {QT_TR_NOOP("Editing"), "Script Updated By"}, {QT_TR_NOOP("Update details"), "Update Details"}};
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
    // Aegisub's From video buttons: the resolutions and the matrix of the video open
    const auto fromVideo = [this](QBoxLayout *row, auto set) {
        auto *b = new QPushButton(tr("From video"), this);
        b->setEnabled(m_tab->video()->hasVideo());
        connect(b, &QPushButton::clicked, this, set);
        row->addWidget(b);
    };
    fromVideo(res, [this] {
        m_w->setValue(m_tab->video()->info().width);
        m_h->setValue(m_tab->video()->info().height);
    });
    f->addRow(tr("Resolution"), res);
    m_lw = spin(fs::q(s.info_value("LayoutResX")));
    m_lh = spin(fs::q(s.info_value("LayoutResY")));
    auto *lres = new QHBoxLayout;
    lres->addWidget(m_lw);
    lres->addWidget(new QLabel("x", this));
    lres->addWidget(m_lh);
    fromVideo(lres, [this] {
        m_lw->setValue(m_tab->video()->info().width);
        m_lh->setValue(m_tab->video()->info().height);
    });
    f->addRow(tr("Layout resolution"), lres);
    m_matrix = new QComboBox(this);
    m_matrix->addItems({"None", "TV.601", "PC.601", "TV.709", "PC.709", "TV.FCC", "PC.FCC", "TV.240M", "PC.240M"});
    const QString matrix = fs::q(s.info_value("YCbCr Matrix"));
    if (m_matrix->findText(matrix) < 0) m_matrix->insertItem(0, matrix); // kept as it is, even left out: None changes the colours
    m_matrix->setCurrentText(matrix);
    auto *mrow = new QHBoxLayout;
    mrow->addWidget(m_matrix, 1);
    fromVideo(mrow, [this] {
        const QString tagged = fs::q(m_tab->video()->info().matrix); // untagged: None, as Aegisub's best practice
        const QString v = tagged.isEmpty() ? QStringLiteral("None") : tagged;
        if (m_matrix->findText(v) < 0) m_matrix->addItem(v);
        m_matrix->setCurrentText(v);
    });
    f->addRow(tr("YCbCr matrix"), mrow);
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
    m_scaled->setToolTip(tr("Scale border and shadow together with script/render resolution. If this is unchecked, "
                            "relative border and shadow size will depend on renderer."));
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
        tab->reselect();
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

namespace {
// The files in a .zip (Aegisub's and Kainote's font packing), deflated by Qt's own zlib; an error or "".
// shortcut: no ZIP64, so an archive stays under 4 GB; add it if anyone packs that much.
QString writeZip(const QString &path, const QStringList &files) {
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return out.errorString();
    QDataStream z(&out);
    z.setByteOrder(QDataStream::LittleEndian);
    QByteArray central;
    QDataStream c(&central, QIODevice::WriteOnly);
    c.setByteOrder(QDataStream::LittleEndian);
    const QDateTime now = QDateTime::currentDateTime();
    const auto time = quint16(now.time().hour() << 11 | now.time().minute() << 5 | now.time().second() / 2);
    const auto date = quint16((now.date().year() - 1980) << 9 | now.date().month() << 5 | now.date().day());
    for (const QString &file : files) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly)) return file + ": " + in.errorString();
        const QByteArray data = in.readAll();
        quint32 crc = 0xFFFFFFFFu;
        for (const char ch : data) crc = table[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
        crc ^= 0xFFFFFFFFu;
        // qCompress gives a 4-byte length, zlib's 2-byte header, the deflate data and a 4-byte checksum
        const QByteArray packed = qCompress(data, 9);
        const QByteArray raw = packed.mid(6, packed.size() - 10);
        const bool deflate = raw.size() < data.size();
        const QByteArray &body = deflate ? raw : data;
        const QByteArray name = QFileInfo(file).fileName().toUtf8();
        const auto offset = quint32(out.pos());
        auto header = [&](QDataStream &s, bool local) {
            s << quint32(local ? 0x04034b50 : 0x02014b50);
            if (!local) s << quint16(20); // made by
            s << quint16(20) << quint16(0x0800) << quint16(deflate ? 8 : 0) << time << date << crc
              << quint32(body.size()) << quint32(data.size()) << quint16(name.size()) << quint16(0);
            if (!local) s << quint16(0) << quint16(0) << quint16(0) << quint32(0) << offset;
            s.writeRawData(name.constData(), int(name.size()));
        };
        header(z, true);
        z.writeRawData(body.constData(), int(body.size()));
        header(c, false);
    }
    const auto at = quint32(out.pos());
    z.writeRawData(central.constData(), int(central.size()));
    z << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(files.size()) << quint16(files.size())
      << quint32(central.size()) << at << quint16(0);
    out.close();
    return out.error() == QFile::NoError ? QString() : out.errorString();
}
} // namespace

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
    auto *allTabs = new QCheckBox(tr("All open tabs"), this); // Kainote
    allTabs->setChecked(st.value("fonts/allTabs").toBool());
    f->addRow(QString(), allTabs);
    auto *dest = new QLineEdit(st.value("fonts/destDir").toString(), this);
    f->addRow(tr("Copy fonts to"), folderRow(dest));
    auto *report = new QPlainTextEdit(this);
    report->setReadOnly(true);
    report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    f->addRow(report);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *check = bb->addButton(tr("Check"), QDialogButtonBox::ActionRole);
    auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto *attach = bb->addButton(tr("Attach to subtitles"), QDialogButtonBox::ActionRole);
    attach->setToolTip(tr("Embed the found fonts in the script's [Fonts] section (Aegisub)"));
    auto *pack = bb->addButton(tr("Pack into ZIP..."), QDialogButtonBox::ActionRole);
    // the script's fonts are read here, the font files on a worker (the first scan of the system's takes seconds)
    enum Mode { Check, Copy, Attach, Zip };
    auto run = [=, this](Mode mode, const QString &zip = {}) {
        if (mode == Copy && dest->text().isEmpty()) return report->setPlainText(tr("Choose a destination folder first."));
        QSettings s;
        s.setValue("fonts/extraDir", extra->text());
        s.setValue("fonts/destDir", dest->text());
        s.setValue("fonts/allTabs", allTabs->isChecked());
        auto uses = std::make_shared<rust::Box<fsb::FontUses>>(tab->s().font_uses());
        if (allTabs->isChecked())
            for (auto *t : tab->main()->tabs())
                if (t != tab) (*uses)->add(*t->s().font_uses());
        // attaching and packing collect the files into a folder of their own first, like a copy
        auto tmp = mode >= Attach ? std::make_shared<QTemporaryDir>() : nullptr;
        if (tmp && !tmp->isValid()) return report->setPlainText(tmp->errorString());
        const QString dir = extra->text(), from = attached->isChecked() ? media : QString(),
                      to = mode == Copy ? dest->text() : tmp ? tmp->path() : QString();
        for (auto *b : {check, copy, attach, pack}) b->setEnabled(false);
        report->setPlainText(tr("Searching for fonts..."));
        using Result = std::pair<bool, QString>; // ok, report or error
        auto *watch = new QFutureWatcher<Result>(this);
        connect(watch, &QFutureWatcher<Result>::finished, this, [=, this] {
            auto [ok, r] = watch->result();
            const int missing = r.count("MISSING") + r.count("missing glyphs");
            QString done = mode == Copy ? tr("Copied to %1").arg(to) : QString();
            QStringList files;
            if (ok && tmp)
                for (const QFileInfo &fi : QDir(tmp->path()).entryInfoList(QDir::Files)) files << fi.filePath();
            if (ok && mode == Zip) {
                const QString err = writeZip(zip, files);
                if (!err.isEmpty()) ok = false, r = err;
                done = tr("Packed into %1").arg(QDir::toNativeSeparators(zip));
            }
            if (ok && mode == Attach) {
                const auto paths = fs::rvec(files);
                try {
                    const size_t n = tab->s().attach_fonts({paths.data(), paths.size()});
                    if (n) tab->changed();
                    done = tr("Attached %1 font file(s) to the subtitles").arg(n);
                } catch (const rust::Error &e) {
                    ok = false;
                    r = QString::fromUtf8(e.what());
                }
            }
            report->setPlainText(!ok ? r
                                     : r + "\n" + (missing ? tr("%n problem(s) found.", nullptr, missing) : tr("All fonts found.")) +
                                           (done.isEmpty() ? QString() : "\n" + done));
            for (auto *b : {check, copy, attach, pack}) b->setEnabled(true);
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
    connect(check, &QPushButton::clicked, this, [run] { run(Check); });
    connect(copy, &QPushButton::clicked, this, [run] { run(Copy); });
    connect(attach, &QPushButton::clicked, this, [run] { run(Attach); });
    connect(pack, &QPushButton::clicked, this, [=, this] {
        const QString start = dest->text().isEmpty() ? QFileInfo(tab->path()).absolutePath() : dest->text();
        const QString zip = QFileDialog::getSaveFileName(this, tr("Pack fonts into a ZIP archive"),
                                                         QDir(start).filePath("fonts.zip"), tr("ZIP archives (*.zip)"));
        if (!zip.isEmpty()) run(Zip, zip);
    });
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
                *audio = page(tr("Audio")), *automation = page(tr("Automation"));
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
        {"audio/autoFocus", QT_TR_NOOP("Focus the audio when the mouse is over it"), true, audio},
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
    auto *duration = new QSpinBox(this); // of an inserted line (Aegisub's Timing/Default Duration)
    duration->setRange(0, 60000);
    duration->setSingleStep(500);
    duration->setSuffix(" ms");
    duration->setValue(st.value("subs/defaultDuration", 3000).toInt());
    subs->addRow(tr("Default line duration"), duration);
    spins.emplace_back("subs/defaultDuration", duration);
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
    spell::fillLanguages(dictLang, st.value("spell/lang").toString());
    connect(dictDir, &QLineEdit::editingFinished, this, [dictDir, dictLang] {
        const QString keep = dictLang->currentData().toString();
        QSettings().setValue("spell/dir", QDir::fromNativeSeparators(dictDir->text()));
        spell::fillLanguages(dictLang, keep);
    });
    editor->addRow(tr("Spell check language"), dictLang);
    auto *language = new QComboBox(this);
    language->addItem(tr("System"), "");
    language->addItem("English", "en");
    language->addItem("Polski", "pl");
    language->setCurrentIndex(std::max(0, language->findData(st.value("main/language", ""))));
    editor->addRow(tr("Language (after restart)"), language);
    auto *newCatalog = new QComboBox(this);
    newCatalog->addItem(tr("None"), "");
    for (const QString &c : styleCatalogs()) newCatalog->addItem(c, c);
    newCatalog->setCurrentIndex(std::max(0, newCatalog->findData(st.value("styles/newFileCatalog", "Default"))));
    newCatalog->setToolTip(tr("A new file starts with the styles of this catalog of the style manager"));
    subs->addRow(tr("Style catalog of new files"), newCatalog);
    // Aegisub's Automation/Trace Level: aegisub.debug.out messages above it are not shown
    auto *trace = new QComboBox(this);
    trace->addItems({tr("0: Fatal"), tr("1: Error"), tr("2: Warning"), tr("3: Hint"), tr("4: Debug"), tr("5: Trace")});
    trace->setCurrentIndex(std::clamp(st.value("automation/traceLevel", 3).toInt(), 0, 5));
    automation->addRow(tr("Trace level"), trace);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this,
            [this, boxes, spins, inactive, leadIn, leadOut, autosave, dictDir, dictLang, language, newCatalog, trace] {
        QSettings st;
        st.setValue("automation/traceLevel", trace->currentIndex());
        st.setValue("styles/newFileCatalog", newCatalog->currentData());
        st.setValue("main/language", language->currentData());
        st.setValue("spell/dir", QDir::fromNativeSeparators(dictDir->text()));
        st.setValue("spell/lang", dictLang->currentData().toString());
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
    for (const QFileInfo &fi : QDir(autosaveDir()).entryInfoList({"*" + suffix, "*.ORIGINAL.*"}, QDir::Files)) {
        if (!fi.fileName().endsWith(suffix)) { // the file as it was opened (Aegisub)
            byName[fi.fileName().section(".ORIGINAL.", 0, 0)].append({fi.lastModified(), fi.absoluteFilePath()});
            continue;
        }
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
            labels << QLocale().toString(d, QLocale::LongFormat) +
                          (p.endsWith(suffix) ? QString() : "  " + tr("(the file as opened)"));
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
    setModal(true); // it writes to rows it noted, so the grid stays as it is (Aegisub)
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

#include "mainwindow.h"

#include "audiobox.h"
#include "automationhost.h"
#include "dialogs.h"
#include "editbox.h"
#include "grid.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QActionGroup>
#include <set>
#include <map>
#include <QSplitter>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const QStringList kSubsExt = {"ass", "ssa", "srt", "sub", "mpl", "tmp", "txt"};
const QStringList kVideoExt = {"mkv", "mp4", "avi", "webm", "mov", "m2ts", "ts", "wmv", "flv", "m4v", "mpg", "mpeg", "ogv"};
const QStringList kAudioExt = {"wav", "mp3", "flac", "aac", "ac3", "eac3", "dts", "ogg", "opus", "m4a", "mka", "wma"};
const QString kSubsFilter = QObject::tr("Subtitles (*.ass *.ssa *.srt *.sub *.mpl *.txt *.tmp);;All files (*)");

// Default toolbar content (Kainote TOOLBAR_IDS order, our ids).
const QStringList kToolbarDefault = {"openSubs", "recentSubs", "openVideo", "recentVideo", "save", "saveAs",
                                     "saveAll", "removeSubs", "editor", "findReplace", "styleManager",
                                     "properties", "shiftTimes", "goStart", "goEnd", "startFromVideo",
                                     "endFromVideo", "toASS", "toSRT", "zoomVideo", "resample", "hideTags",
                                     "settings"};

// shortcut: letter glyphs instead of an icon set; draw a proper icon theme before release.
QIcon glyph(const QString &text) {
    QPixmap px(32, 32);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QApplication::palette().color(QPalette::WindowText));
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPixelSize(text.size() > 2 ? 11 : 14);
    p.setFont(f);
    p.drawRoundedRect(QRectF(1.5, 1.5, 29, 29), 5, 5);
    p.drawText(px.rect(), Qt::AlignCenter, text);
    return QIcon(px);
}

bool isKeyframesFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(64).toLower();
    return head.contains("keyframe format") || head.startsWith("# xvid") || head.startsWith("# ffindex");
}
} // namespace

MainWindow::MainWindow() {
    setWindowTitle("FS Editor");
    setAcceptDrops(true);
    m_tabs = new QTabWidget(this);
    m_tabs->setTabPosition(QTabWidget::South);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    auto *plus = new QToolButton(this);
    plus->setText("+");
    plus->setAutoRaise(true);
    plus->setToolTip(tr("New tab"));
    m_tabs->setCornerWidget(plus, Qt::BottomLeftCorner);
    connect(plus, &QToolButton::clicked, this, &MainWindow::newTab);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::tabBarDoubleClicked, this, [this](int i) {
        if (i < 0) newTab();
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (m_second && current() == m_second) setSplit(m_prev); // picking the right-hand tab swaps the sides
        m_prev = current();
        if (m_compareOn) compareTabs(true);
        updateActions();
        updateStatus();
    });
    m_tabs->tabBar()->installEventFilter(this);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu m;
        auto *group = new QActionGroup(&m);
        for (int i = 0; i < m_tabs->count(); ++i) {
            auto *a = m.addAction(m_tabs->tabText(i), [this, i] { m_tabs->setCurrentIndex(i); });
            a->setCheckable(true);
            a->setChecked(i == m_tabs->currentIndex());
            group->addAction(a);
        }
        m.addSeparator();
        m.addAction(action("save"));
        m.addAction(action("saveAll"));
        m.addAction(tr("Close all tabs"), this, [this] {
            for (int i = m_tabs->count() - 1; i >= 0; --i)
                if (!closeTab(i)) return;
        });
        m.addSeparator();
        auto openDir = [](const QString &file) {
            if (!file.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
        };
        auto *t = current();
        m.addAction(tr("Open subtitles folder"), this, [=] { openDir(t->path()); })->setEnabled(!t->path().isEmpty());
        m.addAction(tr("Open video folder"), this, [=] { openDir(t->video()->path()); })->setEnabled(!t->video()->path().isEmpty());
        m.addAction(tr("Open audio folder"), this, [=] { openDir(t->audio()->path()); })->setEnabled(!t->audio()->path().isEmpty());
        m.addSeparator();
        const int at = m_tabs->tabBar()->tabAt(pos);
        TabPanel *other = at >= 0 && panel(at) != t ? panel(at) : panel((m_tabs->currentIndex() + 1) % m_tabs->count());
        if (m_second)
            m.addAction(tr("Show one tab"), this, [this] { setSplit(nullptr); });
        else
            m.addAction(tr("Show two tabs"), this, [this, other] { setSplit(other); })->setEnabled(other != t);
        auto *cmp = m.addMenu(tr("Subtitle comparison"));
        cmp->setEnabled(m_tabs->count() > 1);
        cmp->addAction(tr("Compare by times"), this, [this, other] {
            if (!m_second) setSplit(other);
            compareTabs(true);
        });
        cmp->addAction(tr("Turn off comparison"), this, [this] { compareTabs(false); })->setEnabled(m_compareOn);
        m.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });
    m_split = new QSplitter(Qt::Horizontal, this);
    m_right = new QWidget(m_split);
    m_right->setLayout(new QVBoxLayout);
    m_right->layout()->setContentsMargins(0, 0, 0, 0);
    m_split->addWidget(m_tabs);
    m_split->addWidget(m_right);
    m_split->setChildrenCollapsible(false);
    m_right->hide();
    setCentralWidget(m_split);

    buildStatusBar();
    buildMenus();
    buildToolbar();
    qApp->installEventFilter(this);

    // Automation: bundled scripts next to the executable, user scripts in the app data folder.
    const QString user = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_automation.emplace(fsb::new_automation(fs::Utf8(QCoreApplication::applicationDirPath()), fs::Utf8(user)));
    m_host = new GuiAutomationHost(this);
    (*m_automation)->set_host(std::unique_ptr<fsb::AutomationHost>(m_host));
    (*m_automation)->set_cache_dir(fs::Utf8(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex"));
    for (const auto &e : (*m_automation)->load_autoload()) status(fs::q(e));
    buildAutomationMenu();

    newTab();
    restoreGeometry(QSettings().value("main/geometry").toByteArray());
    if (!QSettings().contains("main/geometry")) resize(1280, 860);
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    // tabs hold rust boxes; delete them before the automation host goes away
    setSplit(nullptr);
    while (m_tabs->count()) delete m_tabs->widget(0);
}

TabPanel *MainWindow::current() const { return panel(m_tabs->currentIndex()); }

// Each tab page is a slot holding its TabPanel, so a panel can be shown in the split view's right half.
TabPanel *MainWindow::panel(int index) const {
    auto *slot = m_tabs->widget(index);
    return slot ? qobject_cast<TabPanel *>(slot->property("panel").value<QObject *>()) : nullptr;
}

int MainWindow::indexOfPanel(TabPanel *t) const {
    for (int i = 0; i < m_tabs->count(); ++i)
        if (panel(i) == t) return i;
    return -1;
}

void MainWindow::setSplit(TabPanel *second) {
    if (m_second) m_tabs->widget(indexOfPanel(m_second))->layout()->addWidget(m_second);
    m_second = second != current() ? second : nullptr;
    if (m_second) {
        m_right->layout()->addWidget(m_second);
        m_second->show();
        m_split->setSizes({1, 1});
    }
    m_right->setVisible(m_second);
    if (auto *a = action("splitView")) a->setChecked(m_second);
    compareTabs(m_compareOn);
}

// Kainote "compare by times": lines with the same start and end in both tabs but different text are marked;
// lines without a time match are not compared.
void MainWindow::compareTabs(bool on) {
    m_compareOn = on && m_second;
    if (auto *a = action("compareTabs")) a->setChecked(m_compareOn);
    for (auto *t : tabs()) t->grid()->setCompare({});
    if (!m_compareOn) return;
    auto rows = [](TabPanel *t) {
        std::vector<fsb::Row> v;
        for (size_t i = 0; i < t->s().row_count(); ++i) v.push_back(t->s().row(i));
        return v;
    };
    auto diff = [](const std::vector<fsb::Row> &x, const std::vector<fsb::Row> &y) {
        std::multimap<std::pair<qint64, qint64>, QString> other;
        for (const auto &r : y)
            if (!r.comment) other.emplace(std::pair{qint64(r.start), qint64(r.end)}, fs::q(r.text));
        std::set<size_t> out;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i].comment) continue;
            const auto [lo, hi] = other.equal_range({qint64(x[i].start), qint64(x[i].end)});
            if (lo != hi && std::none_of(lo, hi, [&](const auto &kv) { return kv.second == fs::q(x[i].text); }))
                out.insert(i);
        }
        return out;
    };
    const auto a = rows(current()), b = rows(m_second);
    current()->grid()->setCompare(diff(a, b));
    m_second->grid()->setCompare(diff(b, a));
}

QList<TabPanel *> MainWindow::tabs() const {
    QList<TabPanel *> out;
    for (int i = 0; i < m_tabs->count(); ++i) out << panel(i);
    return out;
}

TabPanel *MainWindow::newTab() {
    auto *slot = new QWidget;
    slot->setLayout(new QVBoxLayout);
    slot->layout()->setContentsMargins(0, 0, 0, 0);
    auto *tab = new TabPanel(this);
    slot->layout()->addWidget(tab);
    slot->setProperty("panel", QVariant::fromValue<QObject *>(tab));
    const int i = m_tabs->addTab(slot, tab->title());
    connect(tab, &TabPanel::docChanged, this, [this] {
        if (m_compareOn) compareTabs(true);
    });
    auto refreshTitle = [this, tab] {
        const int idx = indexOfPanel(tab);
        if (idx < 0) return;
        m_tabs->setTabText(idx, tab->title());
        m_tabs->setTabToolTip(idx, tab->path() + (tab->video()->path().isEmpty() ? "" : "\n" + tab->video()->path()));
        if (tab == current()) setWindowTitle(tab->title() + " - FS Editor");
    };
    connect(tab, &TabPanel::titleChanged, this, refreshTitle);
    connect(tab, &TabPanel::statusChanged, this, [this, tab] {
        if (tab == current()) updateStatus();
    });
    tab->grid()->setHideTags(action("hideTags")->isChecked());
    m_tabs->setCurrentIndex(i);
    refreshTitle();
    return tab;
}

// An untitled tab holding only the empty starting line can take a file (it keeps its video/audio).
static bool untouched(TabPanel *tab) {
    return tab && tab->path().isEmpty() &&
           (tab->s().row_count() == 0 || (tab->s().row_count() == 1 && tab->s().row(0).text.empty()));
}

void MainWindow::openFile(const QString &path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (kVideoExt.contains(ext)) {
        // like Kainote: an empty tab offers the subtitles stored in the video
        if (const auto tracks = fsb::subtitle_tracks(fs::Utf8(path)); !tracks.empty() && untouched(current())) {
            QStringList items;
            for (const auto &t : tracks)
                items << QString("#%1  %2%3%4")
                             .arg(t.index)
                             .arg(fs::q(t.codec))
                             .arg(t.language.empty() ? QString() : " [" + fs::q(t.language) + "]")
                             .arg(t.title.empty() ? QString() : "  " + fs::q(t.title));
            bool ok = false;
            const QString pick = QInputDialog::getItem(this, tr("Subtitles in the video"),
                                                       tr("The video contains subtitles. Load them?"), items, 0, false,
                                                       &ok);
            if (ok) current()->openEmbedded(path, tracks[size_t(std::max(0, int(items.indexOf(pick))))].index);
        }
        current()->openVideo(path);
        updateActions();
    } else if (kAudioExt.contains(ext)) {
        current()->openAudio(path);
    } else if (ext == "txt" && isKeyframesFile(path)) {
        current()->openKeyframes(path);
    } else {
        // reuse an empty, untouched tab, otherwise open a new one
        TabPanel *tab = current();
        if (!untouched(tab)) tab = newTab();
        if (tab->openSubs(path)) addRecent("subs", path);
        updateActions();
    }
}

// ---- actions ------------------------------------------------------------------

QAction *MainWindow::add(const QString &id, const QString &text, const QKeySequence &key, QMenu *menu,
                         std::function<void()> fn) {
    auto *a = new QAction(text, this);
    a->setObjectName(id);
    a->setShortcut(QKeySequence(QSettings().value("hotkeys/" + id, key.toString()).toString()));
    a->setShortcutContext(Qt::ApplicationShortcut);
    a->setToolTip(a->shortcut().isEmpty() ? text : QString("%1 (%2)").arg(text, a->shortcut().toString(QKeySequence::NativeText)));
    if (fn) connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
    if (menu) menu->addAction(a);
    addAction(a); // shortcuts work while the menu is closed
    m_actions.insert(id, a);
    return a;
}

void MainWindow::withRow(const std::function<void(TabPanel *, fsb::Row &)> &fn) {
    auto *tab = current();
    if (tab->active() < 0) return;
    auto row = tab->s().row(size_t(tab->active()));
    fn(tab, row);
    tab->s().set_row(size_t(tab->active()), row, "");
    tab->changed();
}

void MainWindow::buildMenus() {
    auto *mb = menuBar();
    auto menu = [this, mb](const QString &title) {
        auto *m = mb->addMenu(title);
        m->installEventFilter(this);
        return m;
    };
    auto sub = [this](QMenu *parent, const QString &title) {
        auto *m = parent->addMenu(title);
        m->installEventFilter(this);
        return m;
    };
    auto tab = [this] { return current(); };

    // File
    auto *file = menu(tr("&File"));
    add("openSubs", tr("Open subtitles"), QKeySequence("Ctrl+O"), file, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open subtitles"),
                                                                QSettings().value("main/lastDir").toString(), kSubsFilter);
        for (const auto &f : files) openFile(f);
        if (!files.isEmpty()) QSettings().setValue("main/lastDir", QFileInfo(files.first()).absolutePath());
    })->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    add("save", tr("Save"), QKeySequence("Ctrl+S"), file, [=, this] {
        if (tab()->path().isEmpty() || !tab()->save()) saveAs(tab());
        else addRecent("subs", tab()->path());
    })->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    add("saveAll", tr("Save all"), {}, file, [this] {
        for (auto *t : tabs())
            if (t->modified() && (t->path().isEmpty() || !t->save())) saveAs(t);
    })->setIcon(glyph("SA"));
    add("saveAs", tr("Save as..."), QKeySequence("Ctrl+Shift+S"), file, [=, this] { saveAs(tab()); })->setIcon(glyph("S…"));
    add("saveTranslation", tr("Save translation"), {}, file, [=, this] {
        const QString p = QFileDialog::getSaveFileName(this, tr("Save translation"), tab()->path(), kSubsFilter);
        if (!p.isEmpty()) tab()->saveTranslation(p);
    });
    file->addMenu(recentMenu("subs"));
    add("removeSubs", tr("Remove subtitles from the editor"), {}, file, [=, this] {
        if (maybeSave(tab())) tab()->clearSubs();
    })->setIcon(glyph("RS"));
    auto *videoName = add("saveWithVideoName", tr("Save subtitles using the video name"), {}, file, {});
    videoName->setCheckable(true);
    videoName->setChecked(QSettings().value("main/saveWithVideoName").toBool());
    connect(videoName, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/saveWithVideoName", on); });
    add("removeTemp", tr("Remove temporary files"), {}, file, [this] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex";
        if (confirm(tr("Remove the video and audio index cache in\n%1?").arg(dir))) QDir(dir).removeRecursively();
    });
    // Log window: automation output and errors.
    auto *logDlg = new QDialog(this);
    logDlg->setWindowTitle(tr("Log"));
    auto *logText = new QPlainTextEdit(logDlg);
    logText->setObjectName("log");
    logText->setReadOnly(true);
    (new QVBoxLayout(logDlg))->addWidget(logText);
    logDlg->resize(560, 360);
    add("showLog", tr("Show / Hide log window"), {}, file, [logDlg] { logDlg->setVisible(!logDlg->isVisible()); });
    auto *session = sub(file, tr("Last session"));
    add("loadSession", tr("Load last session"), {}, session, [this] {
        const QStringList files = QSettings().value("session/files").toStringList();
        for (const auto &f : files)
            if (QFileInfo::exists(f)) openFile(f);
        m_tabs->setCurrentIndex(std::clamp(QSettings().value("session/current").toInt(), 0, m_tabs->count() - 1));
    });
    auto *ask = add("sessionAtStart", tr("Load last session after program start"), {}, session, {});
    ask->setCheckable(true);
    ask->setChecked(QSettings().value("main/askSession", true).toBool());
    connect(ask, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/askSession", on); });
    // shortcut: .kls session files are not read or written; add with session import from Kainote.
    file->addSeparator();
    add("fontCollector", tr("Fonts collector"), {}, file, [=, this] { FontCollectorDialog(tab()).exec(); })->setIcon(glyph("FC"));
    add("settings", tr("Settings"), {}, file, [this] { SettingsDialog(this).exec(); })->setIcon(glyph("⚙"));
    add("exit", tr("Exit"), QKeySequence("Alt+F4"), file, [this] { close(); });

    // Edit
    auto *edit = menu(tr("&Edit"));
    add("undo", tr("Undo"), QKeySequence("Ctrl+Z"), edit, [=] { tab()->undo(); });
    add("undoToSave", tr("Undo to last save"), {}, edit, [=] {
        for (int guard = 0; tab()->modified() && tab()->s().can_undo() && guard < 100000; ++guard) tab()->s().undo();
        tab()->changed();
    });
    add("redo", tr("Redo"), QKeySequence("Ctrl+Y"), edit, [=] { tab()->redo(); });
    add("history", tr("History"), QKeySequence("Ctrl+Shift+H"), edit, [=] { HistoryDialog(tab()).exec(); });
    edit->addSeparator();
    auto findDlg = [this] {
        if (!m_find) m_find = new FindDialog(this);
        return m_find;
    };
    add("findReplace", tr("Find and replace"), QKeySequence("Ctrl+H"), edit, [=] { findDlg()->showTab(1); })
        ->setIcon(glyph("F/R"));
    add("find", tr("Find"), QKeySequence("Ctrl+F"), edit, [=] { findDlg()->showTab(0); });
    add("findNext", tr("Find next"), QKeySequence("F3"), edit, [=, this] {
        if (m_find) m_find->findNext();
        else findDlg()->showTab(0);
    });
    edit->addSeparator();
    const QString sortNames[] = {tr("Start time"), tr("End time"), tr("Styles"), tr("Actor"), tr("Effect"), tr("Layer")};
    for (bool selected : {false, true}) {
        auto *sm = sub(edit, selected ? tr("Sort selected lines") : tr("Sort all lines"));
        for (int k = 0; k < 6; ++k)
            add(QString("sort%1%2").arg(selected ? "Sel" : "All").arg(k), sortNames[k], {}, sm, [=] {
                tab()->s().sort_rows(uint8_t(k), selected, fs::slice(tab()->selection()));
                tab()->changed();
            });
    }
    add("selectLines", tr("Select lines"), {}, edit, [this] { SelectLinesDialog(this).exec(); });
    edit->addSeparator();
    // Line commands from the Kainote global hotkey table.
    auto *lines = sub(edit, tr("Lines"));
    add("duplicate", tr("Duplicate lines"), QKeySequence("Ctrl+D"), lines, [=] { tab()->grid()->duplicate(); });
    add("deleteText", tr("Delete text"), QKeySequence("Alt+Del"), lines, [=] { tab()->grid()->deleteText(); });
    add("deleteLines", tr("Delete lines"), QKeySequence("Shift+Del"), lines, [=] { tab()->grid()->deleteRows(); });
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

    // Video
    auto *video = menu(tr("&Video"));
    add("openVideo", tr("Open video"), QKeySequence("Ctrl+Shift+O"), video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Open video"), QFileInfo(tab()->path()).absolutePath(),
            tr("Video (%1);;All files (*)").arg("*." + kVideoExt.join(" *.")));
        if (!f.isEmpty()) tab()->openVideo(f);
    })->setIcon(glyph("OV"));
    video->addMenu(recentMenu("video"));
    add("openKeyframes", tr("Open keyframes"), {}, video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open keyframes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Keyframes (*.txt *.log);;All files (*)"));
        if (!f.isEmpty()) tab()->openKeyframes(f);
    });
    video->addMenu(recentMenu("keyframes"));
    add("openDummy", tr("Open dummy video"), {}, video, [=, this] {
        QDialog d(this);
        d.setWindowTitle(tr("Dummy video"));
        auto *f = new QFormLayout(&d);
        auto *w = new QSpinBox(&d), *h = new QSpinBox(&d);
        w->setRange(16, 8192);
        h->setRange(16, 8192);
        w->setValue(1920);
        h->setValue(1080);
        auto *fps = new QDoubleSpinBox(&d);
        fps->setRange(1, 240);
        fps->setDecimals(3);
        fps->setValue(23.976);
        auto *len = new QLineEdit("0:30:00.00", &d);
        f->addRow(tr("Width"), w);
        f->addRow(tr("Height"), h);
        f->addRow(tr("FPS"), fps);
        f->addRow(tr("Duration"), len);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        const qint64 ms = fs::parseTime(len->text());
        tab()->openDummyVideo(w->value(), h->value(), fps->value(), ms > 0 ? ms : 1800000);
    });
    video->addSeparator();
    add("startFromVideo", tr("Insert start time from video"), QKeySequence("Ctrl+Left"), video, [this] {
        withRow([](TabPanel *t, fsb::Row &r) { r.start = t->videoTime(); });
    })->setIcon(glyph("[←"));
    add("endFromVideo", tr("Insert end time from video"), QKeySequence("Ctrl+Right"), video, [this] {
        withRow([](TabPanel *t, fsb::Row &r) {
            r.end = t->video()->hasVideo() ? t->video()->nextFrameTime() : t->videoTime();
        });
    })->setIcon(glyph("→]"));
    add("prevFrame", tr("Previous frame"), QKeySequence("Left"), video, [=] { tab()->video()->stepFrames(-1); });
    add("nextFrame", tr("Next frame"), QKeySequence("Right"), video, [=] { tab()->video()->stepFrames(1); });
    add("goStart", tr("Go to start time of line"), {}, video, [=] {
        if (tab()->active() >= 0) tab()->video()->seekStart(tab()->s().row(size_t(tab()->active())).start);
    })->setIcon(glyph("|◀"));
    add("goEnd", tr("Go to end time of line"), {}, video, [=] {
        if (tab()->active() >= 0) tab()->seekVideo(tab()->s().row(size_t(tab()->active())).end - 1);
    })->setIcon(glyph("▶|"));
    add("playPause", tr("Play / Pause"), QKeySequence("Alt+Space"), video, [=] { tab()->video()->togglePlay(); });
    auto stepKeyframe = [=](int dir) {
        const auto kf = tab()->keyframes();
        const qint64 f = tab()->video()->frame();
        if (dir > 0) {
            auto it = std::upper_bound(kf.begin(), kf.end(), f);
            if (it != kf.end()) tab()->video()->seekFrame(*it);
        } else {
            auto it = std::lower_bound(kf.begin(), kf.end(), f);
            if (it != kf.begin()) tab()->video()->seekFrame(*std::prev(it));
        }
    };
    add("prevKeyframe", tr("Go to previous keyframe"), {}, video, [=] { stepKeyframe(-1); });
    add("nextKeyframe", tr("Go to next keyframe"), {}, video, [=] { stepKeyframe(1); });
    add("audioToVideo", tr("Set audio position to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->centerOn(tab()->videoTime()); });
    add("markToVideo", tr("Set audio marker to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->setMark(tab()->videoTime()); });
    video->addSeparator();
    add("zoomVideo", tr("Zoom video"), {}, video, [=] { tab()->video()->zoomMode(); })->setIcon(glyph("🔍"));
    add("resetZoom", tr("Turn off video zoom"), QKeySequence("Ctrl+Shift+Z"), video, [=] { tab()->video()->resetZoom(); });

    // Audio
    auto *audio = menu(tr("&Audio"));
    add("openAudio", tr("Open audio"), {}, audio, [=, this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Open audio"), QFileInfo(tab()->path()).absolutePath(),
            tr("Audio and video (%1);;All files (*)").arg("*." + (kAudioExt + kVideoExt).join(" *.")));
        if (!f.isEmpty()) tab()->openAudio(f);
    });
    audio->addMenu(recentMenu("audio"));
    add("audioFromVideo", tr("Open audio from video"), {}, audio, [=] {
        if (!tab()->video()->path().isEmpty()) tab()->openAudio(tab()->video()->path());
    });
    add("blankAudio", tr("Open blank 2h30m audio"), {}, audio, [=] { tab()->openBlankAudio(150LL * 60 * 1000); });
    add("closeAudio", tr("Close audio"), {}, audio, [=] { tab()->closeAudio(); });

    // View
    auto *view = menu(tr("Vie&w"));
    auto *modes = new QActionGroup(this);
    const std::pair<const char *, QString> viewModes[] = {{"viewAll", tr("All")},
                                                          {"viewVideoSubs", tr("Video and subs")},
                                                          {"viewAudioSubs", tr("Audio and subs")},
                                                          {"viewOnlyVideo", tr("Only video")},
                                                          {"viewOnlySubs", tr("Only subtitles")}};
    for (int k = 0; k < 5; ++k) {
        auto *a = add(viewModes[k].first, viewModes[k].second, {}, view, [=] { tab()->setViewMode(k + 1); });
        a->setCheckable(true);
        modes->addAction(a);
    }
    view->addSeparator();
    auto *split = add("splitView", tr("Show two tabs"), {}, view, [this] {
        setSplit(m_second ? nullptr : panel((m_tabs->currentIndex() + 1) % m_tabs->count()));
    });
    split->setCheckable(true);
    auto *compare = add("compareTabs", tr("Compare subtitles by times"), {}, view, [this] {
        if (!m_second) setSplit(panel((m_tabs->currentIndex() + 1) % m_tabs->count()));
        compareTabs(!m_compareOn);
    });
    compare->setCheckable(true);

    // Subtitles
    auto *subs = menu(tr("&Subtitles"));
    auto *editor = add("editor", tr("Enable / Disable editor"), QKeySequence("Ctrl+E"), subs, {});
    editor->setIcon(glyph("ED"));
    editor->setCheckable(true);
    editor->setChecked(true);
    connect(editor, &QAction::triggered, this, [=](bool on) { tab()->setPlayerMode(!on); });
    add("properties", tr("ASS file properties"), {}, subs, [=] { PropertiesDialog(tab()).exec(); })->setIcon(glyph("P"));
    add("styleManager", tr("Style manager"), QKeySequence("Ctrl+M"), subs, [this] { StyleManager(this).exec(); })
        ->setIcon(glyph("SM"));
    auto *conv = sub(subs, tr("Conversion"));
    const std::tuple<const char *, const char *, const char *> formats[] = {
        {"toASS", "ASS", "F9"}, {"toSRT", "SRT", "F8"}, {"toMDVD", "MicroDVD", "F10"},
        {"toMPL2", "MPL2", "F11"}, {"toTMP", "TMP", "Ctrl+F12"}};
    for (auto [id, fmt, key] : formats)
        add(id, tr("Convert to %1").arg(fmt == QString("MicroDVD") ? "MDVD" : fmt), QKeySequence(key), conv,
            [this, fmt] { convert(fmt); })
            ->setIcon(glyph(QString(fmt).left(3)));
    auto *shift = add("shiftTimes", tr("Shift times"), QKeySequence("Ctrl+I"), subs, {});
    shift->setIcon(glyph("±"));
    shift->setCheckable(true);
    shift->setChecked(QSettings().value("layout/shiftTimes", true).toBool());
    connect(shift, &QAction::triggered, this, [this](bool on) {
        for (auto *t : tabs()) t->showShiftPanel(on);
    });
    auto *spellOn = add("spellCheck", tr("Spell checker"), {}, subs, {});
    spellOn->setCheckable(true);
    spellOn->setChecked(QSettings().value("spell/enabled", true).toBool());
    connect(spellOn, &QAction::triggered, this, [](bool on) {
        QSettings().setValue("spell/enabled", on);
        spell::reload();
    });
    add("resample", tr("Resample subtitles"), {}, subs, [=] { ResampleDialog(tab()).exec(); })->setIcon(glyph("RES"));
    auto *hide = add("hideTags", tr("Hide tags"), {}, subs, {});
    hide->setIcon(glyph("{}"));
    hide->setCheckable(true);
    hide->setChecked(QSettings().value("grid/hideTags").toBool());
    connect(hide, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("grid/hideTags", on);
        for (auto *t : tabs()) t->grid()->setHideTags(on);
    });

    // Automation
    m_automationMenu = menu(tr("Au&tomation"));
    connect(m_automationMenu, &QMenu::aboutToShow, this, [this] {
        auto *tab = current();
        const auto rows = tab->selection();
        for (auto *a : m_actions)
            if (a->property("macroId").isValid())
                a->setEnabled((*m_automation)->macro_enabled(size_t(a->property("macroId").toULongLong()), tab->s(),
                                                              fs::slice(rows), tab->active()));
    });

    // Help
    auto *help = menu(tr("&Help"));
    add("help", tr("Help"), QKeySequence("F1"), help, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/README.md"));
    });
    add("about", tr("About"), {}, help, [this] {
        QMessageBox::about(this, tr("About FS Editor"),
                           tr("<b>FS Editor</b><br>ASS/SSA subtitle editor.<br>Licensed under BSD-3-Clause.<br>"
                              "Layout modelled on Kainote, automation compatible with Aegisub Automation 4."));
    });
    add("aboutQt", tr("About Qt"), {}, help, [] { QApplication::aboutQt(); });
}

QMenu *MainWindow::recentMenu(const QString &kind) {
    if (auto *m = m_recent.value(kind)) return m;
    static const QMap<QString, QString> titles = {{"subs", tr("Recently opened subtitles")},
                                                  {"video", tr("Recently opened videos")},
                                                  {"audio", tr("Recently opened audio")},
                                                  {"keyframes", tr("Recently opened keyframes")}};
    auto *m = new QMenu(titles.value(kind, kind), this);
    m_recent.insert(kind, m);
    connect(m, &QMenu::aboutToShow, this, [this, m, kind] {
        m->clear();
        for (const QString &f : QSettings().value("recent/" + kind).toStringList()) {
            m->addAction(QFileInfo(f).fileName(), this, [this, f, kind] {
                if (kind == "subs") openFile(f);
                else if (kind == "video") current()->openVideo(f);
                else if (kind == "audio") current()->openAudio(f);
                else current()->openKeyframes(f);
            })->setToolTip(f);
        }
        if (m->isEmpty()) m->addAction(tr("(empty)"))->setEnabled(false);
    });
    // a toolbar button shows the submenu
    auto *a = new QAction(m->title(), this);
    a->setMenu(m);
    a->setIcon(glyph(kind == "subs" ? "RS…" : kind == "video" ? "RV…" : "R…"));
    m_actions.insert("recent" + QString(kind == "subs" ? "Subs" : kind == "video" ? "Video" : kind == "audio" ? "Audio" : "Keyframes"), a);
    return m;
}

void MainWindow::addRecent(const QString &kind, const QString &path) {
    QSettings st;
    QStringList l = st.value("recent/" + kind).toStringList();
    l.removeAll(path);
    l.prepend(path);
    st.setValue("recent/" + kind, l.mid(0, 20));
}

// ---- toolbar ------------------------------------------------------------------

void MainWindow::buildToolbar() {
    if (m_toolbar) {
        removeToolBar(m_toolbar);
        m_toolbar->deleteLater();
    }
    m_toolbar = new QToolBar(tr("Toolbar"), this);
    m_toolbar->setObjectName("toolbar");
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(22, 22));
    static const Qt::ToolBarArea areas[] = {Qt::LeftToolBarArea, Qt::TopToolBarArea, Qt::RightToolBarArea,
                                            Qt::BottomToolBarArea};
    auto rebuild = [this] {
        m_toolbar->clear();
        for (const QString &id : QSettings().value("toolbar/ids", kToolbarDefault).toStringList()) {
            auto *a = action(id);
            if (!a) continue;
            m_toolbar->addAction(a);
            if (auto *b = qobject_cast<QToolButton *>(m_toolbar->widgetForAction(a)); b && a->menu())
                b->setPopupMode(QToolButton::InstantPopup);
        }
        // trailing customisation arrow
        auto *more = new QToolButton(m_toolbar);
        more->setText("»");
        more->setToolTip(tr("Choose toolbar icons"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more);
        more->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            const QStringList ids = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
            auto *align = menu->addMenu(tr("Toolbar alignment"));
            const QString names[] = {tr("Left"), tr("Top"), tr("Right"), tr("Bottom")};
            for (int k = 0; k < 4; ++k) {
                auto *a = align->addAction(names[k], this, [this, k] {
                    QSettings().setValue("toolbar/alignment", k);
                    addToolBar(areas[k], m_toolbar);
                });
                a->setCheckable(true);
                a->setChecked(QSettings().value("toolbar/alignment", 0).toInt() == k);
            }
            menu->addSeparator();
            for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it) {
                if (it.key().startsWith("macro:")) continue;
                auto *a = menu->addAction(it.value()->icon(), QString(it.value()->text()).remove('&'));
                a->setCheckable(true);
                a->setChecked(ids.contains(it.key()));
                connect(a, &QAction::toggled, this, [this, id = it.key()](bool on) {
                    QStringList l = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
                    if (on) l << id;
                    else l.removeAll(id);
                    QSettings().setValue("toolbar/ids", l);
                    QTimer::singleShot(0, this, [this] { buildToolbar(); });
                });
            }
        });
        m_toolbar->addWidget(more);
    };
    // shortcut: toolbar buttons are not reorderable by drag; edit toolbar/ids order when needed.
    rebuild();
    addToolBar(areas[std::clamp(QSettings().value("toolbar/alignment", 0).toInt(), 0, 3)], m_toolbar);
}

// ---- status bar ---------------------------------------------------------------

void MainWindow::buildStatusBar() {
    for (int i = 0; i < 9; ++i) {
        m_statusFields[i] = new QLabel(this);
        m_statusFields[i]->setContentsMargins(6, 0, 6, 0);
        if (i == 0) statusBar()->addWidget(m_statusFields[i], 1);
        else statusBar()->addPermanentWidget(m_statusFields[i]);
    }
    m_statusFields[1]->setToolTip(tr("Video scale"));
    m_statusFields[2]->setToolTip(tr("Video zoom"));
    m_statusFields[3]->setToolTip(tr("Video duration"));
    m_statusFields[4]->setToolTip(tr("FPS"));
    m_statusFields[5]->setToolTip(tr("Video resolution"));
    m_statusFields[6]->setToolTip(tr("Aspect ratio"));
    m_statusFields[7]->setToolTip(tr("Subtitle resolution"));
    m_statusFields[8]->setToolTip(tr("Video file"));
}

void MainWindow::updateStatus() {
    auto *tab = current();
    if (!tab) return;
    const auto fi = tab->s().file_info();
    auto *v = tab->video();
    const bool has = v->hasVideo();
    const auto info = v->info();
    for (int i = 1; i < 9; ++i) m_statusFields[i]->clear();
    if (has && info.width > 0) {
        const QRectF r = v->view()->displayRect();
        const QRectF z = v->view()->zoom();
        m_statusFields[1]->setText(QString("%1%").arg(qRound(r.width() * z.width() / info.width * 100)));
        if (z.width() < 0.999) m_statusFields[2]->setText(QString("%1%").arg(qRound(100 / z.width())));
        m_statusFields[3]->setText(fs::fmtTime(info.duration_ms));
        m_statusFields[4]->setText(QString::number(info.fps, 'f', 3) + (info.vfr ? " VFR" : ""));
        m_statusFields[5]->setText(QString("%1x%2").arg(info.width).arg(info.height));
        const bool mismatch = info.width != fi.play_res_x || info.height != fi.play_res_y;
        m_statusFields[5]->setStyleSheet(mismatch ? "color: #d04040;" : "");
        const double sar = info.sar_den > 0 && info.sar_num > 0 ? double(info.sar_num) / info.sar_den : 1.0;
        m_statusFields[6]->setText(QString::number(info.width * sar / info.height, 'f', 3));
        m_statusFields[8]->setText(QFileInfo(v->path()).fileName());
    }
    m_statusFields[7]->setText(QString("%1x%2").arg(fi.play_res_x).arg(fi.play_res_y));
}

void MainWindow::status(const QString &msg) {
    m_statusFields[0]->setText(msg);
    QTimer::singleShot(8000, m_statusFields[0], [l = m_statusFields[0], msg] {
        if (l->text() == msg) l->clear();
    });
}

void MainWindow::error(const QString &msg) { QMessageBox::warning(this, tr("Error"), msg); }

bool MainWindow::confirm(const QString &msg) {
    return QMessageBox::question(this, tr("Confirm"), msg) == QMessageBox::Yes;
}

void MainWindow::editStyle(const QString &name) {
    StyleManager sm(this);
    sm.editStyle(name);
}

void MainWindow::pasteTranslation() {
    auto *tab = current();
    if (!tab->s().file_info().tl_mode) return status(tr("Translation mode is off"));
    bool ok;
    const QString text = QInputDialog::getMultiLineText(this, tr("Paste translation"),
                                                        tr("Text pasted line by line from the active line:"),
                                                        QApplication::clipboard()->text(), &ok);
    if (!ok || text.isEmpty()) return;
    const size_t n = tab->s().paste_translation(size_t(std::max(0, tab->active())), fs::Utf8(text));
    tab->changed();
    status(tr("Pasted %1 lines").arg(n));
}

// ---- files --------------------------------------------------------------------

bool MainWindow::saveAs(TabPanel *tab) {
    QString start = tab->path();
    if (QSettings().value("main/saveWithVideoName").toBool() && !tab->video()->path().isEmpty()) {
        const QFileInfo vf(tab->video()->path());
        start = vf.dir().filePath(vf.completeBaseName() + "." +
                                  (start.isEmpty() ? QString("ass") : QFileInfo(start).suffix()));
    }
    if (start.isEmpty()) start = QSettings().value("main/lastDir").toString() + "/" + tr("Untitled") + ".ass";
    const QString p = QFileDialog::getSaveFileName(this, tr("Save subtitles as"), start, kSubsFilter);
    if (p.isEmpty() || !tab->save(p)) return false;
    addRecent("subs", p);
    return true;
}

bool MainWindow::maybeSave(TabPanel *tab) {
    if (!tab->modified()) return true;
    m_tabs->setCurrentIndex(indexOfPanel(tab));
    const auto r = QMessageBox::question(this, tr("Save changes"),
                                         tr("Save changes to %1?").arg(tab->title()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Discard) return true;
    return tab->path().isEmpty() ? saveAs(tab) : tab->save();
}

bool MainWindow::closeTab(int index) {
    auto *tab = panel(index);
    if (!tab || !maybeSave(tab)) return false;
    index = indexOfPanel(tab); // maybeSave may have switched tabs
    if (tab == m_second || tab == current()) setSplit(nullptr);
    QWidget *slot = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete slot;
    if (m_tabs->count() == 0) newTab();
    return true;
}

void MainWindow::convert(const QString &format) {
    auto *tab = current();
    try {
        tab->s().convert(fs::Utf8(format));
    } catch (const rust::Error &e) {
        return error(QString::fromUtf8(e.what()));
    }
    tab->changed();
    updateActions();
}

void MainWindow::updateActions() {
    auto *tab = current();
    if (!tab) return;
    const auto fi = tab->s().file_info();
    const QString fmt = fs::q(fi.format).toUpper();
    const std::pair<const char *, const char *> conv[] = {
        {"toASS", "ASS"}, {"toSRT", "SRT"}, {"toMDVD", "MICRODVD"}, {"toMPL2", "MPL2"}, {"toTMP", "TMP"}};
    for (auto [id, f] : conv) action(id)->setEnabled(fmt != f);
    action("saveTranslation")->setEnabled(fi.tl_mode);
    const char *views[] = {"viewAll", "viewVideoSubs", "viewAudioSubs", "viewOnlyVideo", "viewOnlySubs"};
    action(views[std::clamp(tab->viewMode() - 1, 0, 4)])->setChecked(true);
    action("editor")->setChecked(!tab->playerMode());
}

void MainWindow::closeEvent(QCloseEvent *e) {
    QStringList files;
    for (auto *t : tabs()) {
        if (!maybeSave(t)) return e->ignore();
        if (!t->path().isEmpty()) files << t->path();
    }
    QSettings st;
    st.setValue("session/files", files);
    st.setValue("session/current", m_tabs->currentIndex());
    st.setValue("main/geometry", saveGeometry());
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    e->acceptProposedAction();
    // after the drop returns: openFile may ask a question, which must not block the drag source
    QTimer::singleShot(0, this, [this, files] {
        for (const QString &f : files) openFile(f);
    });
}

// ---- hotkeys ------------------------------------------------------------------

void MainWindow::assignHotkey(QAction *a) {
    QDialog d(this);
    d.setWindowTitle(tr("Hotkey for \"%1\"").arg(QString(a->text()).remove('&')));
    auto *v = new QVBoxLayout(&d);
    auto *edit = new QKeySequenceEdit(a->shortcut(), &d);
    edit->setMaximumSequenceLength(1);
    v->addWidget(edit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, &d);
    bb->button(QDialogButtonBox::Reset)->setText(tr("Clear"));
    connect(bb->button(QDialogButtonBox::Reset), &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    edit->setFocus();
    if (d.exec() != QDialog::Accepted) return;
    const QKeySequence key = edit->keySequence();
    QSettings st;
    if (!key.isEmpty()) {
        for (auto *other : m_actions) {
            if (other == a || other->shortcut() != key) continue;
            if (!confirm(tr("%1 is used by \"%2\". Take it over?").arg(key.toString(), QString(other->text()).remove('&'))))
                return;
            other->setShortcut({});
            st.setValue("hotkeys/" + other->objectName(), QString());
        }
    }
    a->setShortcut(key);
    st.setValue("hotkeys/" + a->objectName(), key.toString());
}

// Shift+click on a menu item assigns its hotkey; also routes keys to the
// panels that use plain keys (video, audio, grid, edit box) before the
// global shortcuts take them, like Kainote's per-context hotkeys.
bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    // files dropped anywhere in the window are opened; text fields would paste the path otherwise
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove || e->type() == QEvent::Drop) {
        auto *de = static_cast<QDropEvent *>(e);
        auto *w = qobject_cast<QWidget *>(o);
        if (w && w != this && w->window() == this && de->mimeData()->hasUrls() &&
            de->mimeData()->urls().value(0).isLocalFile()) {
            if (e->type() == QEvent::Drop) dropEvent(de);
            else de->acceptProposedAction();
            return true;
        }
    }
    // a click in the split view's right half makes that tab active (the sides swap)
    if (e->type() == QEvent::MouseButtonPress && m_second)
        if (auto *w = qobject_cast<QWidget *>(o); w && m_second->isAncestorOf(w))
            QTimer::singleShot(0, this, [this] {
                if (m_second) m_tabs->setCurrentIndex(indexOfPanel(m_second));
            });
    if (e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (auto *menu = qobject_cast<QMenu *>(o); menu && (me->modifiers() & Qt::ShiftModifier)) {
            if (auto *a = menu->actionAt(me->position().toPoint()); a && !a->menu() && !a->objectName().isEmpty()) {
                menu->close();
                QTimer::singleShot(0, this, [this, a] { assignHotkey(a); });
                return true;
            }
        }
        if (o == m_tabs->tabBar() && me->button() == Qt::MiddleButton) {
            const int i = m_tabs->tabBar()->tabAt(me->position().toPoint());
            if (i >= 0) QTimer::singleShot(0, this, [this, i] { closeTab(i); });
            return true;
        }
    } else if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const auto mods = ke->modifiers() & ~Qt::KeypadModifier;
        const int k = ke->key();
        bool wanted = false;
        if (dynamic_cast<VideoView *>(o) || dynamic_cast<AudioView *>(o)) {
            wanted = !(mods & Qt::ControlModifier) && !(mods & Qt::AltModifier) && k < Qt::Key_F1;
        } else if (dynamic_cast<Grid *>(o)) {
            wanted = (!(mods & Qt::ControlModifier) &&
                      (k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_PageUp || k == Qt::Key_PageDown ||
                       k == Qt::Key_Home || k == Qt::Key_End || k == Qt::Key_Return || k == Qt::Key_Enter)) ||
                     ke->matches(QKeySequence::SelectAll) || ke->matches(QKeySequence::Copy) ||
                     ke->matches(QKeySequence::Cut) || ke->matches(QKeySequence::Paste);
        } else if (dynamic_cast<SubsTextEdit *>(o)) {
            wanted = ((mods & Qt::ControlModifier) &&
                      (k == Qt::Key_D || k == Qt::Key_R || k == Qt::Key_B || k == Qt::Key_I || k == Qt::Key_Comma ||
                       k == Qt::Key_Period)) ||
                     ((mods & Qt::AltModifier) && k == Qt::Key_Down);
        }
        if (wanted) {
            e->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

// ---- automation ---------------------------------------------------------------

void MainWindow::buildAutomationMenu() {
    auto *m = m_automationMenu;
    m->clear();
    for (auto it = m_actions.begin(); it != m_actions.end();) {
        if (it.key().startsWith("macro:")) {
            delete it.value();
            it = m_actions.erase(it);
        } else {
            ++it;
        }
    }
    auto log = [this](const QString &text) {
        if (text.isEmpty()) return;
        auto *edit = findChild<QPlainTextEdit *>("log");
        edit->appendPlainText(text);
        edit->parentWidget()->show();
    };
    m->addAction(tr("Load script"), this, [=, this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Load script"), {},
                                                       tr("Automation scripts (*.lua *.moon);;All files (*)"));
        if (f.isEmpty()) return;
        const size_t before = (*m_automation)->macros().size();
        try {
            (*m_automation)->load_script(fs::Utf8(f));
            if ((*m_automation)->macros().size() > before) m_lastMacro = qint64(before);
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        buildAutomationMenu();
    });
    m->addAction(tr("Refresh autoload scripts"), this, [=, this] {
        for (const auto &e : (*m_automation)->reload()) log(fs::q(e));
        buildAutomationMenu();
    });
    m->addAction(tr("Run the last loaded script"), this, [this] {
        if (m_lastMacro >= 0 && size_t(m_lastMacro) < (*m_automation)->macros().size()) runMacro(size_t(m_lastMacro));
    });
    m->addSeparator();
    QMap<QString, QMenu *> submenus;
    for (const auto &mi : (*m_automation)->macros()) {
        // Aegisub convention: "Group/Name" makes a submenu
        QString name = fs::q(mi.name);
        QMenu *parent = m;
        QStringList parts = name.split('/');
        name = parts.takeLast();
        QString key;
        for (const QString &p : parts) {
            key += p + "/";
            if (!submenus.contains(key)) {
                submenus[key] = parent->addMenu(p);
                submenus[key]->installEventFilter(this);
            }
            parent = submenus[key];
        }
        const size_t id = mi.id;
        auto *a = add("macro:" + fs::q(mi.script) + "/" + fs::q(mi.name), name, {}, parent, [this, id] { runMacro(id); });
        a->setToolTip(fs::q(mi.description));
        a->setProperty("macroId", qulonglong(id));
    }
}

void MainWindow::runMacro(size_t id) {
    auto *tab = current();
    const auto rows = tab->selection();
    fsb::MacroResult r;
    QString err;
    try {
        r = (*m_automation)->run_macro(id, tab->s(), fs::slice(rows), tab->active());
    } catch (const rust::Error &e) {
        err = QString::fromUtf8(e.what());
    }
    m_host->finish();
    QString log = fs::q((*m_automation)->take_log());
    if (!err.isEmpty()) log += (log.isEmpty() ? "" : "\n") + err;
    if (!log.isEmpty()) {
        auto *edit = findChild<QPlainTextEdit *>("log");
        edit->appendPlainText(log);
        edit->parentWidget()->show();
    }
    if (r.changed) tab->changed();
    // a macro may only return a new selection (e.g. Select overlaps)
    const auto sel = fs::vec(r.selection);
    if (!sel.empty()) tab->setSelection(sel, r.active >= 0 ? int(r.active) : int(sel.front()));
}

#include "mainwindow_p.h"

#include "audiobox.h"
#include "automationhost.h"
#include "dialogutil.h"
#include "editbox.h"
#include "grid.h"
#include "historydialog.h"
#include "icons.h"
#include "shiftpanel.h"
#include "stylemanager.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

namespace {
// Action id -> Tabler icon and tint; an invalid colour follows the palette text.
const QHash<QString, std::pair<QString, QColor>> kIcons = {
    {"openSubs", {"folder-open", kAmber}}, {"save", {"device-floppy", kBlue}}, {"saveAll", {"files", kBlue}},
    {"saveAs", {"file-export", kBlue}}, {"exportAs", {"file-arrow-right", kBlue}}, {"saveTranslation", {"language", kBlue}}, {"removeSubs", {"file-x", kRed}},
    {"saveWithVideoName", {"file-symlink", kBlue}}, {"removeTemp", {"trash", kRed}}, {"openAutosave", {"restore", kAmber}}, {"showLog", {"terminal-2", {}}},
    {"loadSession", {"restore", {}}}, {"fontCollector", {"typography", kPurple}}, {"settings", {"settings", {}}},
    {"automationManager", {"list-check", kAmber}}, {"loadScript", {"script", kAmber}},
    {"reloadScripts", {"refresh", kGreen}}, {"runLastScript", {"player-play-filled", kGreen}},
    {"closeVideo", {"x", kRed}}, {"playVideoLine", {"player-track-next", kGreen}},
    {"exit", {"logout", kRed}}, {"undo", {"arrow-back-up", {}}}, {"undoToSave", {"arrow-back-up-double", {}}},
    {"redo", {"arrow-forward-up", {}}}, {"history", {"history", {}}}, {"findReplace", {"replace", kOrange}},
    {"find", {"search", kOrange}}, {"findNext", {"search", kOrange}}, {"correctErrors", {"replace", kGreen}},
    {"selectLines", {"list-check", kGreen}},
    {"duplicate", {"copy", {}}}, {"deleteText", {"eraser", kRed}}, {"deleteTextKeepTags", {"eraser", kOrange}}, {"deleteLines", {"row-remove", kRed}}, {"pasteOver", {"clipboard-text", kBlue}},
    {"prevLine", {"arrow-up", {}}}, {"nextLine", {"arrow-down", {}}}, {"joinPrev", {"arrows-join", {}}},
    {"joinNext", {"arrows-join-2", {}}}, {"recombine", {"arrows-join", kGreen}},
    {"splitKaraoke", {"arrows-split", kGreen}}, {"selectFromVideo", {"focus-2", kGreen}}, {"newTab", {"browser-plus", {}}},
    {"closeTab", {"browser-x", kRed}}, {"nextTab", {"chevron-right", {}}}, {"prevTab", {"chevron-left", {}}},
    {"openVideo", {"movie", kBlue}}, {"openKeyframes", {"key", kAmber}}, {"saveKeyframes", {"device-floppy", kAmber}}, {"openDummy", {"photo", kBlue}},
    {"startFromVideo", {"brackets-contain-start", kGreen}}, {"endFromVideo", {"brackets-contain-end", kGreen}},
    {"prevFrame", {"chevron-left", kBlue}}, {"nextFrame", {"chevron-right", kBlue}},
    {"prevFrames", {"chevrons-left", kBlue}}, {"nextFrames", {"chevrons-right", kBlue}},
    {"goStart", {"arrow-bar-to-left", kGreen}}, {"goEnd", {"arrow-bar-to-right", kGreen}},
    {"playPause", {"player-play", kGreen}}, {"prevKeyframe", {"chevrons-left", kAmber}},
    {"nextKeyframe", {"chevrons-right", kAmber}}, {"audioToVideo", {"wave-sine", kTeal}},
    {"markToVideo", {"map-pin", kTeal}}, {"prevBoundary", {"arrow-bar-to-left", kBlue}},
    {"nextBoundary", {"arrow-bar-to-right", kBlue}}, {"jumpTo", {"arrow-right-to-arc", kBlue}}, {"videoDetails", {"info-circle", kBlue}},
    {"snapScene", {"keyframes", kAmber}}, {"shiftToFrame", {"clock-play", kGreen}}, {"zoomVideo", {"zoom-in", kBlue}}, {"resetZoom", {"zoom-reset", kBlue}},
    {"openAudio", {"music", kTeal}}, {"audioFromVideo", {"file-music", kTeal}}, {"blankAudio", {"wave-square", kTeal}},
    {"closeAudio", {"music-off", kRed}}, {"splitView", {"layout-columns", {}}}, {"compareTabs", {"git-compare", {}}},
    {"editor", {"edit", kBlue}}, {"properties", {"file-info", kPurple}}, {"styleManager", {"palette", kPurple}},
    {"shiftTimes", {"clock-edit", kBlue}}, {"runShift", {"clock-edit", kGreen}}, {"changeFps", {"clock-play", kPurple}}, {"spellChecker", {"text-spellcheck", kGreen}}, {"attachments", {"paperclip", kAmber}}, {"stylingAssistant", {"palette", kPurple}}, {"kanjiTimer", {"language-hiragana", kRed}},
    {"resample", {"aspect-ratio", kPurple}}, {"hideTags", {"code-off", kOrange}}, {"help", {"help", kBlue}},
    {"about", {"info-circle", kBlue}}};
} // namespace

static QWidget *makeHolder(QWidget *parent); // a PanelHolder

MainWindow::MainWindow() {
    limits::reload(); // the counter options, before any row's characters per second
    applyMediaSettings();
    m_compareLater = new QTimer(this);
    m_compareLater->setSingleShot(true);
    m_compareLater->setInterval(150); // a comparison of 100k lines takes 34 ms: not every keystroke
    connect(m_compareLater, &QTimer::timeout, this, [this] {
        if (m_compareOn) compareTabs(true);
    });
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
        const QPointer<TabPanel> was = m_prev;
        if (m_second && current() == m_second) setSplit(m_prev); // picking the right-hand tab swaps the sides
        m_prev = current();
        if (was && current() && was != current()) {
            const QSettings st;
            // Kainote's: the shift times values go along to the tab picked
            if (st.value("shift/followTab", false).toBool()) {
                was->shift()->save();
                current()->shift()->load();
            }
            // Kainote's: the line at the time of the last tab's first selected line becomes active
            if (st.value("grid/selectFromLastTab", false).toBool() && !was->selection().empty())
                current()->grid()->selectNear(was->s().row(was->selection().front()).start);
        }
        if (m_compareOn) compareTabs(true);
        followActive();
        updateActions();
        updateStatus();
        if (m_styles) m_styles->tabChanged();
        checkDisk(current());
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
        m.addAction(tr("Open keyframes folder"), this, [=] { openDir(t->keyframesPath()); })
            ->setEnabled(!t->keyframesPath().isEmpty());
        m.addSeparator();
        const int at = m_tabs->tabBar()->tabAt(pos);
        TabPanel *other = at >= 0 && panel(at) != t ? panel(at) : panel((m_tabs->currentIndex() + 1) % m_tabs->count());
        if (m_second)
            m.addAction(tr("Show one tab"), this, [this] { setSplit(nullptr); });
        else
            m.addAction(tr("Show two tabs"), this, [this, other] { setSplit(other); })->setEnabled(other != t);
        auto *cmp = m.addMenu(tr("Subtitle comparison"));
        cmp->setEnabled(m_tabs->count() > 1);
        // the modes combine; while off, a mode starts a comparison by just that one
        const int by = QSettings().value("compare/by", 1).toInt();
        const char *modes[] = {QT_TR_NOOP("Compare by times"), QT_TR_NOOP("Compare by visible lines"),
                               QT_TR_NOOP("Compare by selections"), QT_TR_NOOP("Compare by styles")};
        for (int k = 0; k < 4; ++k) {
            auto *a = cmp->addAction(tr(modes[k]), this, [this, other, by, bit = 1 << k] {
                const int now = m_compareOn ? by ^ bit : bit;
                if (now) QSettings().setValue("compare/by", now);
                if (now && !m_second) setSplit(other);
                compareTabs(now != 0);
            });
            a->setCheckable(true);
            a->setChecked(m_compareOn && by & 1 << k);
        }
        // Kainote's Compare by selected styles: only the lines of the styles ticked, of both tabs
        auto *stMenu = cmp->addMenu(tr("Compare by selected styles"));
        QSet<QString> mine;
        for (const auto &s : t->s().styles()) mine.insert(fs::q(s.name));
        const QStringList chosen = QSettings().value("compare/styles").toStringList();
        for (const auto &s : other->s().styles()) {
            const QString name = fs::q(s.name);
            if (!mine.contains(name)) continue;
            auto *a = stMenu->addAction(fs::menuText(name), this, [this, other, by, name] {
                QStringList now = QSettings().value("compare/styles").toStringList();
                if (!now.removeAll(name)) now << name;
                QSettings().setValue("compare/styles", now);
                const int mode = now.isEmpty() ? by & ~16 : (m_compareOn ? by : 0) | 16;
                if (mode) QSettings().setValue("compare/by", mode);
                if (mode && !m_second) setSplit(other);
                compareTabs(mode != 0);
            });
            a->setCheckable(true);
            a->setChecked(m_compareOn && by & 16 && chosen.contains(name));
        }
        stMenu->setEnabled(!stMenu->isEmpty() && other != t);
        cmp->addSeparator();
        cmp->addAction(tr("Turn off comparison"), this, [this] { compareTabs(false); })->setEnabled(m_compareOn);
        m.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });
    m_split = new QSplitter(Qt::Horizontal, this);
    m_right = makeHolder(m_split);
    m_split->addWidget(m_tabs);
    m_split->addWidget(m_right);
    m_split->setChildrenCollapsible(false);
    m_right->hide();
    setCentralWidget(m_split);

    buildStatusBar();
    buildMenus();
    buildToolbar();

    // Autosave (Aegisub): modified tabs are copied to autosaveDir() every N seconds; at startup the
    // newest copies within 100 MB stay (Aegisub's CleanCache; their number is Kainote's max autosave
    // files, 1000 as in Aegisub), and so do those of the files as opened.
    const qsizetype keep = std::max(2, QSettings().value("main/autosaveMax", 1000).toInt());
    for (const char *kind : {"*.AUTOSAVE.ass", "*.ORIGINAL.*"}) {
        qint64 bytes = 0;
        const auto old = QDir(autosaveDir()).entryInfoList({kind}, QDir::Files, QDir::Time);
        for (qsizetype i = 0; i < old.size(); ++i)
            if ((bytes += old[i].size()) > (100 << 20) || i >= keep) QFile::remove(old[i].absoluteFilePath());
    }
    // an autosave's record of its file goes with it
    for (const QFileInfo &f : QDir(autosaveDir()).entryInfoList({"*.AUTOSAVE.ass.from"}, QDir::Files))
        if (!QFileInfo::exists(f.absoluteFilePath().chopped(5))) QFile::remove(f.absoluteFilePath());
    // a Windows logoff or shutdown may end the app without closeEvent: the work goes to autosaves
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { autosaveTabs(); });
    // Kainote's: a file another program changed is offered again when the app or its tab comes back
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive) checkDisk(current());
    });
    auto *autosave = new QTimer(this);
    autosave->setObjectName("autosave");
    connect(autosave, &QTimer::timeout, this, [this, autosave] {
        const int secs = QSettings().value("main/autosaveSeconds", 60).toInt();
        autosave->setInterval(std::max(secs, 10) * 1000);
        if (secs > 0) autosaveTabs(false);
    });
    autosave->start(std::max(QSettings().value("main/autosaveSeconds", 60).toInt(), 10) * 1000);
    qApp->installEventFilter(this);

    // Automation: bundled scripts next to the executable, user scripts in the app data folder.
    const QString user = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_automation.emplace(fsb::new_automation(fs::Utf8(QCoreApplication::applicationDirPath()), fs::Utf8(user)));
    m_host = new GuiAutomationHost(this);
    m_host->onStatus = [this](const QString &text) { status(text); };
    m_host->onStop = [this] { (*m_automation)->stop_script(); };
    m_host->onLog = [this](const QString &text) { log(text); };
    (*m_automation)->set_host(std::unique_ptr<fsb::AutomationHost>(m_host));
    (*m_automation)->set_cache_dir(fs::Utf8(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex"));
    (*m_automation)->set_trace_level(QSettings().value("automation/traceLevel", 3).toInt());
    (*m_automation)->set_search_paths(fs::Utf8(QSettings().value("automation/autoloadPath").toString()),
                                      fs::Utf8(QSettings().value("automation/includePath").toString()));
    // Kainote's loading of the autoload scripts: 0 at start in the background, 1 at start (as
    // Aegisub does), 2 and 3 as the menu first opens, in the background or not
    if (const int method = QSettings().value("automation/loadMethod", 1).toInt(); method == 1) {
        ensureAutoload();
    } else {
        if (method == 0) startAutoload();
        buildAutomationMenu();
    }

    newTab();
    watchScripts();
    restoreGeometry(QSettings().value("main/geometry").toByteArray());
    if (!QSettings().contains("main/geometry")) resize(1280, 860);
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    // the scripts still loading may call the host, which goes with this window
    if (m_autoloadJob) (*m_automation)->finish_autoload(std::move(*m_autoloadJob));
    // tabs hold rust boxes; delete them before the automation host goes away
    setSplit(nullptr);
    while (m_tabs->count()) delete m_tabs->widget(0);
}

namespace {
// Holds one panel, filling it: a tab's page, or the split view's right half. Not a layout: a layout
// passes every layout request from below on to its parent, and the tab widget paints itself whole
// for each one, so a label in the panel that changed its text (the line's length, its characters
// per second) had the whole window painted again for every key typed. Here a request goes up only
// when the panel's own size hints changed.
class PanelHolder final : public QWidget {
public:
    using QWidget::QWidget;
    void setPanel(QWidget *panel) {
        m_panel = panel;
        panel->setParent(this);
        panel->setGeometry(rect());
        panel->show();
        hintsChanged();
        updateGeometry();
    }
    QSize sizeHint() const override { return held() ? m_panel->sizeHint() : QWidget::sizeHint(); }
    QSize minimumSizeHint() const override { return held() ? m_panel->minimumSizeHint() : QWidget::minimumSizeHint(); }

protected:
    void resizeEvent(QResizeEvent *) override {
        if (held()) m_panel->setGeometry(rect());
    }
    void showEvent(QShowEvent *e) override { // a hidden holder is told of no request
        if (hintsChanged()) updateGeometry();
        QWidget::showEvent(e);
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::LayoutRequest) {
            if (hintsChanged()) updateGeometry();
            return true;
        }
        return QWidget::event(e);
    }

private:
    bool held() const { return m_panel && m_panel->parentWidget() == this; } // not while the other holder shows it
    bool hintsChanged() {
        const std::pair now{sizeHint(), minimumSizeHint()};
        return std::exchange(m_hints, now) != now;
    }
    QPointer<QWidget> m_panel;
    std::pair<QSize, QSize> m_hints;
};
void hold(QWidget *holder, QWidget *panel) { static_cast<PanelHolder *>(holder)->setPanel(panel); }
} // namespace

static QWidget *makeHolder(QWidget *parent) { return new PanelHolder(parent); }

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
    if (m_second) hold(m_tabs->widget(indexOfPanel(m_second)), m_second);
    m_second = second != current() ? second : nullptr;
    if (m_second) {
        hold(m_right, m_second);
        m_split->setSizes({1, 1});
    }
    m_right->setVisible(m_second);
    if (auto *a = action("splitView")) a->setChecked(m_second);
    compareTabs(m_compareOn);
    followActive();
}

// Kainote's subtitle preview, in the split view: the other tab marks and shows the lines timed with the
// active line, or the nearest one
void MainWindow::followActive() {
    for (auto *t : tabs()) t->grid()->setLinked({});
    auto *t = current();
    if (!m_second || !t || t->active() < 0) return;
    const auto r = t->s().row(size_t(t->active()));
    const auto spans = m_second->s().row_spans();
    std::set<size_t> rows;
    size_t nearest = 0;
    qint64 best = std::numeric_limits<qint64>::max();
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].start < r.end && spans[i].end > r.start) rows.insert(i);
        else if (const qint64 d = std::min(std::abs(spans[i].start - r.start), std::abs(spans[i].end - r.end)); d < best) {
            best = d;
            nearest = i;
        }
    }
    if (rows.empty() && !spans.empty()) rows.insert(nearest);
    if (rows.empty()) return;
    m_second->grid()->ensureVisible(int(*rows.begin()));
    m_second->grid()->setLinked(std::move(rows));
}

// Kainote's subtitle comparison: lines of the two tabs pair up in order, by the modes in "compare/by"
// (1 same times, 2 visible lines only, 4 selected lines only, 8 same style); paired lines with different
// texts are marked, with the differing characters.
void MainWindow::compareTabs(bool on) {
    const int by = QSettings().value("compare/by", 1).toInt();
    m_compareOn = on && m_second && by;
    if (auto *a = action("compareTabs")) a->setChecked(m_compareOn);
    for (auto *t : tabs()) t->grid()->setCompare({});
    if (!m_compareOn) return;
    // 16: only the lines of the styles chosen (Kainote's Compare by selected styles), matched by style
    const QStringList styles = by & 16 ? QSettings().value("compare/styles").toStringList() : QStringList();
    auto rows = [by, &styles](TabPanel *t) {
        std::vector<size_t> r = by & 2 ? t->grid()->visibleRows() : std::vector<size_t>(t->s().row_count());
        if (!(by & 2)) std::iota(r.begin(), r.end(), size_t(0));
        if (by & 4) std::erase_if(r, [t](size_t i) { return !t->isSelected(i); });
        if (by & 16) std::erase_if(r, [t, &styles](size_t i) { return !styles.contains(fs::q(t->s().row(i).style)); });
        return r;
    };
    const auto a = rows(current()), b = rows(m_second);
    std::map<size_t, std::vector<size_t>> x, y;
    for (const auto &p : current()->s().compare(m_second->s(), {a.data(), a.size()}, {b.data(), b.size()}, by & 1,
                                                by & 24)) {
        x[p.row] = fs::vec(p.spans);
        y[p.other] = fs::vec(p.other_spans);
    }
    current()->grid()->setCompare(std::move(x));
    m_second->grid()->setCompare(std::move(y));
}

void MainWindow::showPanel(TabPanel *t) {
    if (const int i = indexOfPanel(t); i >= 0) m_tabs->setCurrentIndex(i);
}

QList<TabPanel *> MainWindow::tabs() const {
    QList<TabPanel *> out;
    for (int i = 0; i < m_tabs->count(); ++i) out << panel(i);
    return out;
}

// The title on a tab, cut to Kainote's tab title length; the tool tip has the whole path
static QString tabText(TabPanel *tab) {
    const QString t = tab->title();
    const int n = std::max(20, QSettings().value("main/tabTitleChars", 40).toInt());
    return fs::menuText(t.size() > n ? t.left(n - 1) + QChar(0x2026) : t);
}

TabPanel *MainWindow::newTab() {
    auto *slot = new PanelHolder;
    auto *tab = new TabPanel(this);
    tab->startNew();
    slot->setPanel(tab);
    slot->setProperty("panel", QVariant::fromValue<QObject *>(tab));
    const int i = m_tabs->addTab(slot, tabText(tab));
    connect(tab, &TabPanel::docChanged, this, [this] {
        if (m_compareOn) m_compareLater->start();
    });
    connect(tab, &TabPanel::activeChanged, this, [this, tab] {
        if (tab == current()) followActive();
    });
    auto refreshTitle = [this, tab] {
        const int idx = indexOfPanel(tab);
        if (idx < 0) return;
        // only what changed: this runs on every change of the lines, and a tab text set again
        // lays the tab widget out anew, which paints the whole window for each key typed
        if (const QString text = tabText(tab); m_tabs->tabText(idx) != text) m_tabs->setTabText(idx, text);
        if (const QString tip = tab->path() + (tab->video()->path().isEmpty() ? "" : "\n" + tab->video()->path());
            m_tabs->tabToolTip(idx) != tip)
            m_tabs->setTabToolTip(idx, tip);
        if (tab == current()) setWindowTitle(tab->title() + " - FS Editor");
    };
    connect(tab, &TabPanel::titleChanged, this, refreshTitle);
    connect(tab, &TabPanel::statusChanged, this, [this, tab] {
        if (tab == current()) updateStatus();
    });
    connect(tab->video(), &VideoBox::opened, this, [this, tab] { // video opens in the background
        if (tab == current()) updateActions();
    });
    connect(tab->audio(), &AudioBox::opened, this, [this, tab] {
        if (tab == current()) updateActions();
    });
    connect(tab, &TabPanel::titleChanged, this, [this, tab] { // every change: Undo's step
        if (tab == current()) updateActions();
    });
    m_tabs->setCurrentIndex(i);
    refreshTitle();
    return tab;
}

QAction *MainWindow::add(const QString &id, const QString &text, const QKeySequence &key, QMenu *menu,
                         std::function<void()> fn) {
    auto *a = new QAction(text, this);
    a->setObjectName(id);
    if (auto it = kIcons.constFind(id); it != kIcons.cend()) a->setIcon(icon(it->first, it->second));
    a->setProperty("defaultKey", key);
    // any number of keys per command, as Aegisub binds them; "" when all were taken off
    QList<QKeySequence> keys;
    for (const QString &k : QSettings().value("hotkeys/" + id, key.toString()).toStringList())
        if (!k.isEmpty()) keys << QKeySequence(k);
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::ApplicationShortcut);
    showHotkey(a);
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

void MainWindow::error(const QString &msg) { QMessageBox::warning(this, tr("Error"), msg); }

void MainWindow::log(const QString &text) {
    if (text.trimmed().isEmpty()) return;
    auto *edit = findChild<QPlainTextEdit *>("log");
    edit->appendPlainText(text);
    edit->parentWidget()->show();
}

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

bool MainWindow::closeTab(int index) {
    auto *tab = panel(index);
    if (!tab || !maybeSave(tab)) return false;
    index = indexOfPanel(tab); // maybeSave may have switched tabs
    if (tab == m_second || tab == current()) setSplit(nullptr);
    QWidget *slot = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete slot;
    if (m_tabs->count() == 0) newTab();
    // Aegisub's window closing with its local scripts: those no open file lists are unloaded
    rust::Vec<rust::String> listed;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *p = panel(i))
            for (auto &s : (*m_automation)->listed_scripts(p->s())) listed.push_back(std::move(s));
    if ((*m_automation)->keep_scripts({listed.data(), listed.size()})) {
        m_lastMacro = -1; // the macros were renumbered
        buildAutomationMenu();
    }
    return true;
}

void MainWindow::updateActions() {
    auto *tab = current();
    if (!tab) return;
    const auto fi = tab->s().file_info();
    const QString fmt = fs::q(fi.format).toUpper();
    const std::pair<const char *, const char *> conv[] = {
        {"toASS", "ASS"}, {"toSRT", "SRT"}, {"toMDVD", "MICRODVD"}, {"toMPL2", "MPL2"}, {"toTMP", "TMP"}};
    for (auto [id, f] : conv) action(id)->setEnabled(fmt != f && !fi.tl_mode);
    action("saveTranslation")->setEnabled(fi.tl_mode);
    for (const char *id : {"translationMatching", "pasteTranslation", "filterDoubtful", "filterUntranslated"})
        action(id)->setEnabled(fi.tl_mode);
    for (const char *id : {"recombine", "splitKaraoke"}) action(id)->setEnabled(!fi.tl_mode);
    action("subsFromVideo")->setEnabled(!tab->video()->path().isEmpty());
    action("closeVideo")->setEnabled(tab->video()->hasVideo());
    action("videoDetails")->setEnabled(tab->video()->hasVideo());
    action("saveAudioClip")->setEnabled(tab->audio()->hasAudio());
    // closing a file still being indexed stops its index
    action("closeAudio")->setEnabled(tab->audio()->hasAudio() || tab->audio()->opening());
    for (const char *id : {"saveKeyframes", "closeKeyframes"}) action(id)->setEnabled(!tab->keyframes().empty());
    action("saveTimecodes")->setEnabled(tab->video()->timecodes().size() >= 2);
    action("closeTimecodes")->setEnabled(!tab->timecodesPath().isEmpty());
    // Aegisub's "Undo <step>" / "Nothing to undo"
    const QString undo = undoStepName(fs::q(tab->s().undo_description()));
    const QString redo = undoStepName(fs::q(tab->s().redo_description()));
    action("undo")->setText(undo.isEmpty() ? tr("Nothing to undo") : tr("Undo %1").arg(undo));
    action("undo")->setEnabled(!undo.isEmpty());
    action("redo")->setText(redo.isEmpty() ? tr("Nothing to redo") : tr("Redo %1").arg(redo));
    action("redo")->setEnabled(!redo.isEmpty());
    action("undoToSave")->setEnabled(tab->modified() && !undo.isEmpty());
    for (const char *id : {"undo", "redo"}) showHotkey(action(id));
    const char *views[] = {"viewAll", "viewVideoSubs", "viewAudioSubs", "viewOnlyVideo", "viewOnlySubs"};
    action(views[std::clamp(tab->viewMode() - 1, 0, 4)])->setChecked(true);
    action("editor")->setChecked(!tab->playerMode());
}

void MainWindow::closeEvent(QCloseEvent *e) {
    for (auto *t : tabs())
        if (!maybeSave(t)) return e->ignore();
    int front = 0;
    const QStringList files = sessionFiles(&front);
    QSettings st;
    st.setValue("session/files", files);
    st.setValue("session/current", front);
    st.setValue("main/geometry", saveGeometry());
    e->accept();
}

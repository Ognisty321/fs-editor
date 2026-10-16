#include "mainwindow.h"

#include "audiobox.h"
#include "automationhost.h"
#include "colorpicker.h"
#include "dialogs.h"
#include "editbox.h"
#include "icons.h"
#include "grid.h"
#include "shiftpanel.h"
#include "tabpanel.h"
#include "videobox.h"
#include "visualtools.h"

#include <QActionGroup>
#include <set>
#include <map>
#include <QSplitter>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QSet>
#include <limits>
#include <numeric>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QProxyStyle>
#include <QStyleOptionToolButton>
#include <QTabBar>
#include <QTabWidget>
#include <QtConcurrentRun>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const QStringList kSubsExt = {"ass", "ssa", "srt", "sub", "mpl", "tmp", "txt", "ttxt"};
const QStringList kVideoExt = {"mkv", "mp4", "avi", "webm", "mov", "m2ts", "ts", "wmv", "flv", "m4v", "mpg", "mpeg", "ogv"};
const QStringList kAudioExt = {"wav", "mp3", "flac", "aac", "ac3", "eac3", "dts", "ogg", "opus", "m4a", "mka", "wma"};
const QString kSubsFilter = QObject::tr("Subtitles (*.ass *.ssa *.srt *.sub *.mpl *.txt *.tmp *.ttxt);;All files (*)");
// Export also writes Aegisub's write-only formats, which the end of the name picks
struct ExportType {
    const char *filter, *suffix; // suffix: what a name chosen with the filter must end with
};
const ExportType kExportTypes[] = {
    {QT_TRANSLATE_NOOP("MainWindow", "Subtitles (*.ass *.ssa *.srt *.sub *.mpl *.txt *.tmp *.ttxt)"), ""},
    {QT_TRANSLATE_NOOP("MainWindow", "MPEG-4 Streaming Text (*.ttxt)"), ".ttxt"},
    {QT_TRANSLATE_NOOP("MainWindow", "Adobe Encore (*.encore.txt)"), ".encore.txt"},
    {QT_TRANSLATE_NOOP("MainWindow", "TranStation (*.transtation.txt)"), ".transtation.txt"},
    {QT_TRANSLATE_NOOP("MainWindow", "EBU subtitling data exchange format, EBU tech 3264 (*.stl)"), ".stl"},
    {QT_TRANSLATE_NOOP("MainWindow", "All files (*)"), ""},
};

// Default toolbar content (Kainote TOOLBAR_IDS order, our ids).
const QStringList kToolbarDefault = {"openSubs", "recentSubs", "openVideo", "recentVideo", "save", "saveAs",
                                     "saveAll", "removeSubs", "editor", "findReplace", "styleManager",
                                     "properties", "shiftTimes", "goStart", "goEnd", "startFromVideo",
                                     "endFromVideo", "toASS", "toSRT", "zoomVideo", "resample", "hideTags",
                                     "settings"};

const QColor kAmber(0xf0b429), kBlue(0x4aa3ff), kGreen(0x4cc38a), kRed(0xf2665c), kPurple(0xb48cff),
    kTeal(0x36c5d0), kOrange(0xff8c42);

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

// Format badge (ASS, SRT, ...) for the conversion actions, as Kainote shows them.
QIcon glyph(const QString &text) {
    QPixmap px(32, 32);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kBlue, 2));
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

// A toolbar button with a menu: the native style takes the arrow's room from the icon, which
// then sits left of the others; here the icon is drawn centred and the arrow in the corner.
class MenuButtonStyle final : public QProxyStyle {
public:
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w) const override {
        const auto *tb = qstyleoption_cast<const QStyleOptionToolButton *>(opt);
        if (cc != CC_ToolButton || !tb || (tb->features & QStyleOptionToolButton::MenuButtonPopup) ||
            !(tb->features & QStyleOptionToolButton::HasMenu))
            return QProxyStyle::drawComplexControl(cc, opt, p, w);
        QStyleOptionToolButton o = *tb;
        o.features &= ~QStyleOptionToolButton::HasMenu;
        QProxyStyle::drawComplexControl(cc, &o, p, w);
        // QCommonStyle's corner arrow
        const int mbi = pixelMetric(PM_MenuButtonIndicator, tb, w);
        const QRect r = tb->rect;
        o.rect = visualRect(tb->direction, r, QRect(r.right() + 5 - mbi, r.bottom() + 5 - mbi, mbi - 6, mbi - 6));
        drawPrimitive(PE_IndicatorArrowDown, &o, p, w);
    }
};
} // namespace

MainWindow::MainWindow() {
    limits::reload(); // the counter options, before any row's characters per second
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
        if (m_second && current() == m_second) setSplit(m_prev); // picking the right-hand tab swaps the sides
        m_prev = current();
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
        cmp->addSeparator();
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

    // Autosave (Aegisub): modified tabs are copied to autosaveDir() every N seconds; at startup the
    // newest 1000 copies within 100 MB stay (Aegisub's CleanCache), and so do those of the files as opened.
    for (const char *kind : {"*.AUTOSAVE.ass", "*.ORIGINAL.*"}) {
        qint64 bytes = 0;
        const auto old = QDir(autosaveDir()).entryInfoList({kind}, QDir::Files, QDir::Time);
        for (qsizetype i = 0; i < old.size(); ++i)
            if ((bytes += old[i].size()) > (100 << 20) || i >= 1000) QFile::remove(old[i].absoluteFilePath());
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
    auto rows = [by](TabPanel *t) {
        std::vector<size_t> r = by & 2 ? t->grid()->visibleRows() : std::vector<size_t>(t->s().row_count());
        if (!(by & 2)) std::iota(r.begin(), r.end(), size_t(0));
        if (by & 4) std::erase_if(r, [t](size_t i) { return !t->isSelected(i); });
        return r;
    };
    const auto a = rows(current()), b = rows(m_second);
    std::map<size_t, std::vector<size_t>> x, y;
    for (const auto &p : current()->s().compare(m_second->s(), {a.data(), a.size()}, {b.data(), b.size()}, by & 1,
                                                by & 8)) {
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

TabPanel *MainWindow::newTab() {
    auto *slot = new QWidget;
    slot->setLayout(new QVBoxLayout);
    slot->layout()->setContentsMargins(0, 0, 0, 0);
    auto *tab = new TabPanel(this);
    // shortcut: only new files take a catalog; Aegisub has one for each imported format too, add
    // them if SRT/TXT conversions should start styled
    if (const auto cat = catalogStyles(QSettings().value("styles/newFileCatalog", "Default").toString()); !cat.empty())
        tab->s().start_with_styles({cat.data(), cat.size()});
    slot->layout()->addWidget(tab);
    slot->setProperty("panel", QVariant::fromValue<QObject *>(tab));
    const int i = m_tabs->addTab(slot, tab->title());
    connect(tab, &TabPanel::docChanged, this, [this] {
        if (m_compareOn) m_compareLater->start();
    });
    connect(tab, &TabPanel::activeChanged, this, [this, tab] {
        if (tab == current()) followActive();
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
    connect(tab->video(), &VideoBox::opened, this, [this, tab] { // video opens in the background
        if (tab == current()) updateActions();
    });
    m_tabs->setCurrentIndex(i);
    refreshTitle();
    return tab;
}

// An untitled tab holding only the empty starting line can take a file (it keeps its video/audio).
static bool untouched(TabPanel *tab) {
    return tab && tab->path().isEmpty() && !tab->modified() &&
           (tab->s().row_count() == 0 || (tab->s().row_count() == 1 && tab->s().row(0).text.empty()));
}

int MainWindow::pickEmbeddedTrack(const rust::Vec<fsb::SubTrack> &tracks, const QString &question) {
    QStringList items;
    for (const auto &t : tracks)
        items << QString("#%1  %2%3%4")
                     .arg(t.index)
                     .arg(fs::q(t.codec))
                     .arg(t.language.empty() ? QString() : " [" + fs::q(t.language) + "]")
                     .arg(t.title.empty() ? QString() : "  " + fs::q(t.title));
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Subtitles in the video"), question, items, 0, false, &ok);
    return ok ? tracks[size_t(std::max(0, int(items.indexOf(pick))))].index : -1;
}

void MainWindow::openFile(const QString &path, const QString &encoding) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (kVideoExt.contains(ext)) {
        // like Kainote: an empty tab offers the subtitles stored in the video
        if (const auto tracks = fsb::subtitle_tracks(fs::Utf8(path)); !tracks.empty() && untouched(current()))
            if (const int i = pickEmbeddedTrack(tracks, tr("The video contains subtitles. Load them?")); i >= 0)
                current()->openEmbedded(path, i);
        current()->openVideo(path);
        updateActions();
    } else if (kAudioExt.contains(ext)) {
        current()->openAudio(path);
    } else if (ext == "txt" && isKeyframesFile(path)) {
        current()->openKeyframes(path);
    } else {
        // plain text, as Aegisub imports it, asks how its actors and comments are marked
        if (ext == "txt" && fsb::is_plain_text(fs::Utf8(path)) && !TabPanel::askTextImport(this)) return;
        // a file open already: its tab, read again if unchanged there (two tabs would save over each other)
        for (auto *t : tabs())
            if (!t->path().isEmpty() && QFileInfo(t->path()) == QFileInfo(path)) {
                showPanel(t);
                if (t->modified()) status(tr("%1 is open already").arg(QFileInfo(path).fileName()));
                else t->openSubs(path, encoding);
                return updateActions();
            }
        // reuse an empty, untouched tab, otherwise open a new one
        TabPanel *tab = current();
        if (!untouched(tab)) tab = newTab();
        if (tab->openSubs(path, encoding)) addRecent("subs", path);
        updateActions();
    }
}

// ---- actions ------------------------------------------------------------------

namespace {
void showHotkey(QAction *a) {
    const QString key = a->shortcut().toString(QKeySequence::NativeText);
    a->setToolTip(key.isEmpty() ? a->text() : QString("%1 (%2)").arg(a->text(), key));
}
} // namespace

QAction *MainWindow::add(const QString &id, const QString &text, const QKeySequence &key, QMenu *menu,
                         std::function<void()> fn) {
    auto *a = new QAction(text, this);
    a->setObjectName(id);
    if (auto it = kIcons.constFind(id); it != kIcons.cend()) a->setIcon(icon(it->first, it->second));
    a->setProperty("defaultKey", key);
    a->setShortcut(QKeySequence(QSettings().value("hotkeys/" + id, key.toString()).toString()));
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
    });
    // Aegisub's "Open Subtitles with Charset": for files whose encoding detection guesses wrong
    add("openSubsCharset", tr("Open subtitles with encoding..."), {}, file, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open subtitles"),
                                                       QSettings().value("main/lastDir").toString(), kSubsFilter);
        if (f.isEmpty()) return;
        bool ok = false;
        const QString enc = QInputDialog::getItem(this, tr("Choose encoding"), tr("Text encoding:"),
                                                  {"UTF-8", "UTF-16LE", "UTF-16BE", "windows-1250", "windows-1251",
                                                   "windows-1252", "ISO-8859-2", "Shift_JIS", "GBK", "Big5", "EUC-KR"},
                                                  0, true, &ok);
        if (ok) openFile(f, enc);
    });
    add("save", tr("Save"), QKeySequence("Ctrl+S"), file, [=, this] { saveTab(tab()); });
    add("saveAll", tr("Save all"), {}, file, [this] {
        for (auto *t : tabs())
            if (t->modified()) saveTab(t);
    });
    add("saveAs", tr("Save as..."), QKeySequence("Ctrl+Shift+S"), file, [=, this] { saveAs(tab()); });
    // Kainote's "Load subtitles from MKV": a track of the open video replaces the subtitles
    add("subsFromVideo", tr("Load subtitles from video..."), {}, file, [=, this] {
        const QString video = tab()->video()->path();
        const auto tracks = fsb::subtitle_tracks(fs::Utf8(video));
        if (tracks.empty()) return status(tr("The video contains no subtitles"));
        if (!maybeSave(tab())) return;
        if (const int i = pickEmbeddedTrack(tracks, tr("Subtitles to load:")); i >= 0) tab()->openEmbedded(video, i);
    });
    add("saveTranslation", tr("Save translation"), {}, file, [=, this] {
        // a name of its own: Enter in the dialog must not replace the file with both texts
        const QFileInfo fi(tab()->path());
        const QString start = fi.fileName().isEmpty() ? QString() : fi.dir().filePath(fi.completeBaseName() + ".translation.ass");
        const QString p = QFileDialog::getSaveFileName(this, tr("Save translation"), start, kSubsFilter);
        if (!p.isEmpty()) tab()->saveTranslation(p);
    });
    // Aegisub's Export: a filtered copy in another format/encoding; the open file stays as it is
    add("exportAs", tr("Export as..."), {}, file, [=, this] {
        auto *t = tab();
        const bool video = t->video()->hasVideo();
        QSettings st;
        // Aegisub's DialogExport: every filter (the built-in ones among the scripts' by priority), checked
        // as the file last exported, the settings panel of each checked one beside the list. The scripts'
        // config functions run as it opens, as there.
        const auto filters = (*m_automation)->export_filters(t->s());
        rust::Vec<fsb::DialogData> panels;
        try {
            auto job = (*m_automation)->start_filter_configs(t->s());
            runScript(*job);
            panels = (*m_automation)->finish_filter_configs(std::move(job));
        } catch (const rust::Error &) {
            return; // cancelled while the config functions ran
        }
        QDialog d(this);
        d.setWindowTitle(tr("Export"));
        auto *filterBox = new QGroupBox(tr("Filters"), &d);
        auto *left = new QVBoxLayout(filterBox);
        auto *list = new QListWidget(filterBox);
        list->setMinimumSize(220, 120);
        const QStringList checked = fs::q(t->s().project_value("Export Filters")).split('|');
        auto description = [&](size_t k) {
            switch (filters[k].kind) {
            case 1:
                return tr("Transform subtitle times, including those in override tags, from an input framerate to an "
                          "output framerate.\n\nThis is useful for converting regular time subtitles to VFRaC time "
                          "subtitles for hardsubbing.\nIt can also be used to convert subtitles to a different speed "
                          "video, such as NTSC to PAL speedup.");
            case 2: return tr("Fixes styles by replacing any style that isn't available on file with Default.");
            default: return fs::q(filters[k].description);
            }
        };
        for (size_t k = 0; k < filters.size(); ++k) {
            const auto &fi = filters[k];
            const QString name = fs::q(fi.name);
            auto *it = new QListWidgetItem(fi.kind == 1   ? tr("Transform Framerate")
                                           : fi.kind == 2 ? tr("Fix Styles")
                                                          : name,
                                           list);
            it->setData(Qt::UserRole, qulonglong(k));
            it->setCheckState(checked.contains(name) ? Qt::Checked : Qt::Unchecked);
        }
        left->addWidget(list, 1);
        auto *moves = new QHBoxLayout;
        auto *up = new QPushButton(tr("Move &Up"), filterBox), *down = new QPushButton(tr("Move &Down"), filterBox);
        auto *all = new QPushButton(tr("Select &All"), filterBox), *none = new QPushButton(tr("Select &None"), filterBox);
        for (auto *b : {up, down, all, none}) moves->addWidget(b);
        left->addLayout(moves);
        auto *about = new QPlainTextEdit(filterBox);
        about->setReadOnly(true);
        about->setFixedHeight(64);
        left->addWidget(about);
        auto rate = [&](const QString &def) {
            auto *c = new QComboBox(&d);
            c->setEditable(true);
            if (video) c->addItem(tr("From video"), "video");
            c->addItems({"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"});
            c->setCurrentText(def);
            return c;
        };
        auto *from = rate(video ? tr("From video") : "23.976"), *to = rate(st.value("export/fpsTo", "25").toString());
        auto *enc = new QComboBox(filterBox);
        enc->addItems({"UTF-8", "UTF-16LE", "windows-1250", "windows-1252", "ISO-8859-2", "windows-1251"});
        {
            // the file's own, as Aegisub keeps it; Aegisub's names ("Unicode (UTF-8)") by what is in brackets
            QString e = fs::q(t->s().project_value("Export Encoding"));
            if (const qsizetype a = e.lastIndexOf('('), b = e.lastIndexOf(')'); a >= 0 && b > a) e = e.mid(a + 1, b - a - 1);
            const int k = enc->findText(e, Qt::MatchFixedString);
            enc->setCurrentIndex(std::max(0, k));
        }
        auto *encRow = new QHBoxLayout;
        encRow->addWidget(new QLabel(tr("Text encoding:"), filterBox));
        encRow->addWidget(enc, 1);
        left->addLayout(encRow);
        // the settings panels, shown for the checked filters
        auto *right = new QVBoxLayout;
        std::map<size_t, QGroupBox *> boxes;
        std::map<size_t, std::vector<QWidget *>> widgets;
        for (size_t k = 0; k < filters.size(); ++k) {
            if (filters[k].kind == 1) {
                auto *box = new QGroupBox(tr("Transform Framerate"), &d);
                auto *form = new QFormLayout(box);
                form->addRow(tr("Subtitles timed to:"), from);
                form->addRow(tr("Convert to:"), to);
                boxes[k] = box;
            } else if (k < panels.size() && !panels[k].controls.empty()) {
                auto *box = new QGroupBox(fs::q(filters[k].name), &d);
                widgets[k] = scriptControls(panels[k].controls, new QGridLayout(box), box);
                boxes[k] = box;
            }
            if (boxes.count(k)) right->addWidget(boxes[k]);
        }
        right->addStretch();
        auto refresh = [&] {
            for (int r = 0; r < list->count(); ++r)
                if (auto b = boxes.find(list->item(r)->data(Qt::UserRole).toULongLong()); b != boxes.end())
                    b->second->setVisible(list->item(r)->checkState() == Qt::Checked);
            d.adjustSize();
        };
        connect(list, &QListWidget::itemChanged, &d, refresh);
        connect(list, &QListWidget::currentRowChanged, &d, [&](int r) {
            about->setPlainText(r < 0 ? QString() : description(list->item(r)->data(Qt::UserRole).toULongLong()));
        });
        auto move = [&](int by) {
            const int r = list->currentRow();
            if (r < 0 || r + by < 0 || r + by >= list->count()) return;
            list->insertItem(r + by, list->takeItem(r));
            list->setCurrentRow(r + by);
        };
        connect(up, &QPushButton::clicked, &d, [&] { move(-1); });
        connect(down, &QPushButton::clicked, &d, [&] { move(1); });
        auto setAll = [&](bool on) {
            for (int r = 0; r < list->count(); ++r) list->item(r)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        };
        connect(all, &QPushButton::clicked, &d, [&] { setAll(true); });
        connect(none, &QPushButton::clicked, &d, [&] { setAll(false); });
        auto *columns = new QHBoxLayout;
        columns->addWidget(filterBox);
        columns->addLayout(right, 1);
        auto *f = new QVBoxLayout(&d);
        f->addLayout(columns, 1);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addWidget(bb);
        list->setCurrentRow(0);
        refresh();
        const bool accepted = d.exec() == QDialog::Accepted;
        // in list order, as they run; the file remembers which, closed or not (Aegisub)
        std::vector<size_t> ids;
        QStringList on;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->checkState() == Qt::Checked) {
                const size_t k = list->item(r)->data(Qt::UserRole).toULongLong();
                ids.push_back(k);
                on << fs::q(filters[k].name);
            }
        t->s().set_project_value("Export Filters", fs::Utf8(on.join('|')));
        if (!accepted) return;
        for (auto &[k, w] : widgets) panels[k].controls = scriptValues(panels[k].controls, w);
        t->s().set_project_value("Export Encoding", fs::Utf8(enc->currentText()));
        st.setValue("export/fpsTo", to->currentText());
        QStringList types;
        for (const auto &e : kExportTypes) types << tr(e.filter);
        QString type = st.value("export/type", types[0]).toString();
        QString p = QFileDialog::getSaveFileName(this, tr("Export subtitles"), QFileInfo(t->path()).absolutePath(),
                                                 types.join(";;"), &type);
        if (p.isEmpty()) return;
        st.setValue("export/type", type);
        // a name typed without the type's double extension still gets it
        if (const qsizetype k = types.indexOf(type); k >= 0) {
            const QString suffix = kExportTypes[k].suffix;
            if (!suffix.isEmpty() && !p.endsWith(suffix, Qt::CaseInsensitive)) {
                if (suffix.count('.') > 1 && p.endsWith(".txt", Qt::CaseInsensitive)) p.chop(4);
                p += suffix;
            }
        }
        auto value = [](QComboBox *c) {
            return c->currentText() == c->itemText(0) && c->itemData(0).isValid() ? QString("video") : c->currentText();
        };
        fsb::ExportSpec spec;
        spec.fps_from = fs::rs(value(from));
        spec.fps_to = fs::rs(value(to)); // read by the chain's Transform Framerate only
        spec.encoding = fs::rs(enc->currentText());
        const QString lower = p.toLower();
        if (lower.endsWith(".encore.txt") || lower.endsWith(".transtation.txt")) {
            // Aegisub's AskForFPS: SMPTE times need a constant rate, so the video's only when it has one
            struct Rate {
                QString name;
                const char *fps;
                bool drop;
            };
            std::vector<Rate> rates;
            const auto vi = t->video()->info();
            if (video && !vi.vfr) rates.push_back({tr("From video (%1)").arg(vi.fps, 0, 'g', 6), "video", false});
            rates.insert(rates.end(), {{tr("15.000 FPS"), "15", false},
                                       {tr("23.976 FPS (Decimated NTSC)"), "24000/1001", false},
                                       {tr("24.000 FPS (FILM)"), "24", false},
                                       {tr("25.000 FPS (PAL)"), "25", false},
                                       {tr("29.970 FPS (NTSC)"), "30000/1001", false},
                                       {tr("29.970 FPS (NTSC with SMPTE dropframe)"), "30000/1001", true},
                                       {tr("30.000 FPS"), "30", false},
                                       {tr("50.000 FPS (PAL x2)"), "50", false},
                                       {tr("59.940 FPS (NTSC x2)"), "60000/1001", false},
                                       {tr("60.000 FPS"), "60", false},
                                       {tr("119.880 FPS (NTSC x4)"), "120000/1001", false},
                                       {tr("120.000 FPS"), "120", false}});
            QStringList names;
            for (const auto &r : rates) names << r.name;
            bool ok = false;
            const QString pick = QInputDialog::getItem(this, tr("FPS"), tr("Please choose the appropriate FPS for the subtitles:"),
                                                       names, 0, false, &ok);
            if (!ok) return;
            const auto &r = rates[size_t(std::max<qsizetype>(0, names.indexOf(pick)))];
            spec.smpte_fps = rust::String(r.fps);
            spec.smpte_drop = r.drop;
        }
        if (lower.endsWith(".stl") && !ebuSettings(spec)) return;
        const auto tc = t->video()->timecodes();
        try {
            bool done = true;
            if (ids.empty()) {
                spec.fps_to = rust::String();
                t->s().export_copy(fs::Utf8(p), spec, {tc.data(), tc.size()});
            } else {
                auto job = (*m_automation)->start_export(fs::slice(ids), {panels.data(), panels.size()}, spec,
                                                         {tc.data(), tc.size()}, t->s());
                runScript(*job);
                done = (*m_automation)->finish_export(std::move(job), t->s(), fs::Utf8(p), spec, {tc.data(), tc.size()});
            }
            if (done) status(tr("Exported %1").arg(QFileInfo(p).fileName()));
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Export"), QString::fromUtf8(e.what()));
        }
    });
    file->addMenu(recentMenu("subs"));
    add("removeSubs", tr("Remove subtitles from the editor"), {}, file, [=, this] {
        if (maybeSave(tab())) tab()->clearSubs();
    });
    auto *videoName = add("saveWithVideoName", tr("Save subtitles using the video name"), {}, file, {});
    videoName->setCheckable(true);
    videoName->setChecked(QSettings().value("main/saveWithVideoName").toBool());
    connect(videoName, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/saveWithVideoName", on); });
    add("openAutosave", tr("Open autosave"), {}, file, [this] {
        AutosaveDialog d(this);
        if (d.exec() != QDialog::Accepted || d.chosen().isEmpty()) return;
        // as an unsaved script: Save asks where, not into the autosave folder that gets cleaned, and
        // starts at the file it was autosaved from
        TabPanel *tab = untouched(current()) ? current() : newTab();
        if (tab->openSubs(d.chosen())) {
            tab->s().detach();
            QFile from(d.chosen() + ".from");
            tab->setProperty("savePath", from.open(QIODevice::ReadOnly) ? QString::fromUtf8(from.readAll()) : QString());
            tab->changed();
            updateActions();
        }
    });
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
        const int front = QSettings().value("session/current").toInt();
        TabPanel *pick = nullptr;
        for (int k = 0; k < files.size(); ++k) {
            if (!QFileInfo::exists(files[k])) continue;
            // each file its own tab: a video alone would otherwise take the next script in
            if (!untouched(current()) || current()->video()->hasVideo()) newTab();
            openFile(files[k]);
            if (k == front || !pick) pick = current();
        }
        if (pick) showPanel(pick);
    });
    auto *ask = add("sessionAtStart", tr("Load last session after program start"), {}, session, {});
    ask->setCheckable(true);
    ask->setChecked(QSettings().value("main/askSession", true).toBool());
    connect(ask, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/askSession", on); });
    file->addSeparator();
    add("settings", tr("Settings"), {}, file, [this] {
        if (SettingsDialog(this).exec() != QDialog::Accepted) return;
        (*m_automation)->set_trace_level(QSettings().value("automation/traceLevel", 3).toInt());
        for (auto *t : tabs()) { // the limits colour the grid and the edit box
            t->grid()->viewport()->update();
            t->edit()->reload();
            t->audio()->docChanged(); // inactive lines
        }
    });
    add("hotkeys", tr("Hotkeys..."), {}, file, [this] { editHotkeys(); });
    add("exit", tr("Exit"), QKeySequence("Alt+F4"), file, [this] { close(); });

    // Edit
    auto *edit = menu(tr("&Edit"));
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
        auto *sm = sub(edit, selected ? tr("Sort selected lines") : tr("Sort all lines"));
        for (int k = 0; k < 6; ++k)
            add(QString("sort%1%2").arg(selected ? "Sel" : "All").arg(k), sortNames[k], {}, sm, [=] {
                tab()->s().sort_rows(uint8_t(k), selected, fs::slice(tab()->selection()));
                tab()->reselect(); // the same lines, where the sort put them
            });
    }
    add("selectLines", tr("Select lines"), {}, edit, [this] { SelectLinesDialog(this).exec(); });
    edit->addSeparator();
    // Line commands from the Kainote global hotkey table.
    auto *lines = sub(edit, tr("Lines"));
    add("duplicate", tr("Duplicate lines"), QKeySequence("Ctrl+D"), lines, [=] { tab()->grid()->duplicate(); });
    // the grid's context menu commands (Aegisub's subtitle/insert, grid/swap, edit/line/join, …)
    auto *insert = sub(lines, tr("Insert lines"));
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
    auto *splitMenu = sub(lines, tr("Split lines"));
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

    // a panel's own keys, which work while it has the keyboard (Kainote's audio and video hotkeys):
    // listed in its menu and in the hotkey editor, where they take other keys
    const auto panelKeys = [this](QMenu *m, std::span<const PanelKey> keys, const char *context,
                                  const std::function<void(const char *)> &run) {
        for (const auto &k : keys) {
            auto *a = add(k.id, QCoreApplication::translate(context, k.name), QKeySequence(k.key), m,
                          [=] { run(k.id); });
            a->setShortcutContext(Qt::WidgetShortcut); // not the window's: G and the arrows are typed elsewhere
            removeAction(a);
            a->setProperty("context", context); // the hotkey editor's conflicts are within one panel
        }
    };
    panelKeys(sub(edit, tr("Edit box keys")), EditBox::keys(), "EditBox",
              [=](const char *id) { tab()->edit()->runCommand(id); });

    // Video
    auto *video = menu(tr("&Video"));
    add("openVideo", tr("Open video"), QKeySequence("Ctrl+Shift+O"), video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Open video"), QFileInfo(tab()->path()).absolutePath(),
            tr("Video (%1);;All files (*)").arg("*." + kVideoExt.join(" *.")));
        if (!f.isEmpty()) tab()->openVideo(f);
    });
    video->addMenu(recentMenu("video"));
    // Aegisub's video/close: the file stays where it is (Remove video recycles it)
    add("closeVideo", tr("Close video"), {}, video, [=] { tab()->closeVideo(); });
    add("playVideoLine", tr("Play the current line"), {}, video, [=] { tab()->playLine(tab()->active()); });
    add("openKeyframes", tr("Open keyframes"), {}, video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open keyframes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Keyframes (*.txt *.log);;All files (*)"));
        if (!f.isEmpty()) tab()->openKeyframes(f);
    });
    video->addMenu(recentMenu("keyframes"));
    add("saveKeyframes", tr("Save keyframes..."), {}, video, [=, this] {
        if (tab()->keyframes().empty()) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save keyframes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Keyframes (*.txt);;All files (*)"));
        if (f.isEmpty()) return;
        const auto kf = tab()->keyframes();
        try {
            fsb::save_keyframes(fs::Utf8(f), {kf.data(), kf.size()});
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Save keyframes"), QString::fromUtf8(e.what()));
        }
    });
    add("closeKeyframes", tr("Close keyframes"), {}, video, [=, this] {
        tab()->closeKeyframes(); // back to the video's own, like Aegisub
    });
    // Aegisub's timecodes: override the video's frame times (VFR typesetting on a CFR encode)
    add("openTimecodes", tr("Open timecodes..."), {}, video, [=, this] {
        if (!tab()->video()->hasVideo()) return status(tr("Open a video first"));
        const QString f = QFileDialog::getOpenFileName(this, tr("Open timecodes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Timecodes (*.txt);;All files (*)"));
        if (!f.isEmpty()) tab()->openTimecodes(f);
    });
    video->addMenu(recentMenu("timecodes"));
    add("saveTimecodes", tr("Save timecodes..."), {}, video, [=, this] {
        const auto tc = tab()->video()->timecodes();
        if (tc.size() < 2) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save timecodes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Timecodes (*.txt);;All files (*)"));
        if (f.isEmpty()) return;
        try {
            fsb::save_timecodes(fs::Utf8(f), {tc.data(), tc.size()});
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Save timecodes"), QString::fromUtf8(e.what()));
        }
    });
    add("closeTimecodes", tr("Close timecodes"), {}, video, [=] { tab()->openTimecodes({}); });
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
        // the colour and Aegisub's checkerboard, the last ones used
        QSettings st;
        QColor colour = st.value("video/dummyColor", QColor(0x30, 0x30, 0x30)).value<QColor>();
        auto *pick = new QPushButton(&d);
        const auto show = [pick, &colour] { pick->setStyleSheet(QString("background:%1").arg(colour.name())); };
        show();
        connect(pick, &QPushButton::clicked, &d, [&, show] {
            if (const QColor c = ColorPicker::get(&d, colour, false); c.isValid()) colour = c, show();
        });
        auto *pattern = new QCheckBox(tr("Checkerboard pattern"), &d);
        pattern->setChecked(st.value("video/dummyPattern").toBool());
        f->addRow(tr("Width"), w);
        f->addRow(tr("Height"), h);
        f->addRow(tr("FPS"), fps);
        f->addRow(tr("Duration"), len);
        f->addRow(tr("Colour"), pick);
        f->addRow(pattern);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        const qint64 ms = fs::parseTime(len->text());
        st.setValue("video/dummyColor", colour);
        st.setValue("video/dummyPattern", pattern->isChecked());
        tab()->openDummyVideo(VideoBox::dummySpec(fps->value(), std::max<qint64>(1, qint64((ms > 0 ? ms : 1800000) * fps->value() / 1000)),
                                                  w->value(), h->value(), colour, pattern->isChecked()));
    });
    video->addSeparator();
    // Aegisub's: every selected line, and the other end follows when it would be passed
    auto toVideo = [this](bool start) {
        auto *t = current();
        if (t->active() < 0) return;
        const auto sel = t->selection();
        t->s().snap_to_video(fs::slice(sel), t->videoStartTime(), t->videoEndTime(), start);
        t->changed();
    };
    add("startFromVideo", tr("Insert start time from video"), QKeySequence("Ctrl+Left"), video,
        [toVideo] { toVideo(true); });
    add("endFromVideo", tr("Insert end time from video"), QKeySequence("Ctrl+Right"), video,
        [toVideo] { toVideo(false); });
    // Kainote's snap: the active line's start (end) to the nearest keyframe or start/end of the line
    // before or after it, within 5 s and never past the line's other end
    auto snap = [this](bool start) {
        withRow([start](TabPanel *t, fsb::Row &r) {
            const qint64 at = start ? r.start : r.end, other = start ? r.end : r.start;
            std::vector<qint64> near;
            if (t->video()->hasVideo())
                for (auto f : t->keyframes()) near.push_back(t->video()->timeAtStart(f));
            for (int n : {t->active() - 1, t->active() + 1})
                if (n >= 0 && size_t(n) < t->s().row_count()) {
                    const auto o = t->s().row(size_t(n));
                    near.insert(near.end(), {o.start, o.end});
                }
            qint64 best = at;
            for (qint64 c : near)
                if (c != at && std::abs(c - at) < 5000 && (start ? c < other : c > other) &&
                    (best == at || std::abs(c - at) < std::abs(best - at)))
                    best = c;
            (start ? r.start : r.end) = best;
        });
    };
    add("snapStart", tr("Snap start to keyframe"), QKeySequence("Shift+Left"), video, [=] { snap(true); });
    add("snapEnd", tr("Snap end to keyframe"), QKeySequence("Shift+Right"), video, [=] { snap(false); });
    add("prevFrame", tr("Previous frame"), QKeySequence("Left"), video, [=] { tab()->video()->stepFrames(-1); });
    add("nextFrame", tr("Next frame"), QKeySequence("Right"), video, [=] { tab()->video()->stepFrames(1); });
    // Aegisub's video/frame/*/large, by its Fast Jump Step setting
    const auto jump = [] { return std::max(1, QSettings().value("video/fastJump", 10).toInt()); };
    add("prevFrames", tr("Back several frames"), QKeySequence("Alt+Left"), video, [=] { tab()->video()->stepFrames(-jump()); });
    add("nextFrames", tr("Forward several frames"), QKeySequence("Alt+Right"), video, [=] { tab()->video()->stepFrames(jump()); });
    // Aegisub's keys
    add("goStart", tr("Go to start time of line"), QKeySequence("Ctrl+1"), video, [=] {
        if (tab()->active() >= 0) tab()->video()->seekStart(tab()->s().row(size_t(tab()->active())).start);
    });
    add("goEnd", tr("Go to end time of line"), QKeySequence("Ctrl+2"), video, [=] {
        if (tab()->active() >= 0) tab()->seekVideo(tab()->s().row(size_t(tab()->active())).end - 1);
    });
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
    // the frame shown as a picture: saved beside the video, or copied
    const std::tuple<const char *, const char *, const char *, VideoBox::Grab> grabs[] = {
        {"saveFrameSubs", QT_TR_NOOP("Save frame with subtitles as PNG"), QT_TR_NOOP("Copy frame with subtitles to clipboard"), VideoBox::WithSubs},
        {"saveFrame", QT_TR_NOOP("Save frame as PNG"), QT_TR_NOOP("Copy frame to clipboard"), VideoBox::Raw},
        {"saveFrameSubsOnly", QT_TR_NOOP("Save subtitles only as PNG"), QT_TR_NOOP("Copy subtitles only to clipboard"), VideoBox::SubsOnly}};
    for (auto [id, save, copy, mode] : grabs) {
        add(id, tr(save), {}, video, [=] { tab()->video()->saveFrame(mode); });
        add(QString(id).replace("save", "copy"), tr(copy), {}, video,
            [=] { QApplication::clipboard()->setImage(tab()->video()->grabFrame(mode)); });
    }
    add("prevChapter", tr("Go to previous chapter"), {}, video, [=] { tab()->video()->stepChapter(-1); });
    add("nextChapter", tr("Go to next chapter"), {}, video, [=] { tab()->video()->stepChapter(1); });
    // Aegisub: video/frame/prev|next/boundary - the active line's start or end, then the neighbour line
    add("prevBoundary", tr("Previous boundary"), {}, video, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        auto *v = t->video();
        const fsb::Row r = t->s().row(size_t(t->active()));
        for (qint64 f : {v->frameAt(r.end - 1), v->frameAtStart(r.start)})
            if (f < v->frame()) return v->seekFrame(f);
        if (t->active() > 0) {
            t->setActive(t->active() - 1);
            v->seekFrame(v->frameAt(t->s().row(size_t(t->active())).end - 1));
        }
    });
    add("nextBoundary", tr("Next boundary"), {}, video, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        auto *v = t->video();
        const fsb::Row r = t->s().row(size_t(t->active()));
        for (qint64 f : {v->frameAtStart(r.start), v->frameAt(r.end - 1)})
            if (f > v->frame()) return v->seekFrame(f);
        if (size_t(t->active()) + 1 < t->s().row_count()) {
            t->setActive(t->active() + 1);
            v->seekStart(t->s().row(size_t(t->active())).start);
        }
    });
    add("jumpTo", tr("Jump to..."), QKeySequence("Ctrl+G"), video, [=, this] {
        auto *v = tab()->video();
        if (!v->hasVideo()) return;
        QDialog d(this);
        d.setWindowTitle(tr("Jump to"));
        auto *f = new QFormLayout(&d);
        auto *time = new TimeEdit(&d);
        auto *frame = new QSpinBox(&d);
        frame->setRange(0, int(std::max<qint64>(0, v->info().frames - 1)));
        time->setMs(v->time(), v->frame());
        frame->setValue(int(v->frame()));
        connect(time, &TimeEdit::edited, frame, [=] {
            if (time->ms() >= 0) frame->setValue(int(v->frameAt(time->ms())));
        });
        connect(frame, &QSpinBox::valueChanged, time, [=](int n) { time->setMs(v->timeAt(n), n); });
        f->addRow(tr("Frame:"), frame);
        f->addRow(tr("Time:"), time);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() == QDialog::Accepted) v->seekFrame(frame->value());
    });
    // Aegisub's Video details
    auto *overscan = add("showOverscan", tr("Show overscan mask"), {}, video, {});
    overscan->setCheckable(true);
    overscan->setChecked(QSettings().value("video/overscan").toBool());
    connect(overscan, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("video/overscan", on);
        for (auto *t : tabs()) t->video()->view()->updateOverlay();
    });
    add("videoDetails", tr("Video details..."), {}, video, [=, this] {
        auto *v = tab()->video();
        if (!v->hasVideo()) return;
        const fsb::VideoInfo i = v->info();
        const int g = std::gcd(i.width, i.height);
        QMessageBox::information(
            this, tr("Video details"),
            tr("File name: %1\nFPS: %2%3\nResolution: %4x%5 (%6:%7)\nPixel aspect: %8:%9\nLength: %10 frames (%11)\nDecoder: BestSource")
                .arg(QFileInfo(v->path()).fileName())
                .arg(i.fps, 0, 'f', 3)
                .arg(i.vfr ? tr(" (variable)") : QString())
                .arg(i.width).arg(i.height).arg(i.width / std::max(1, g)).arg(i.height / std::max(1, g))
                .arg(i.sar_num).arg(i.sar_den)
                .arg(i.frames).arg(fs::fmtTime(i.duration_ms)));
    });
    add("audioToVideo", tr("Set audio position to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->centerOn(tab()->videoTime()); });
    add("markToVideo", tr("Set audio marker to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->setMark(tab()->videoTime()); });
    video->addSeparator();
    add("zoomVideo", tr("Zoom video"), {}, video, [=] { tab()->video()->zoomMode(); });
    // the tool row's tools, for keys of one's own (Aegisub has them on A..J, taken here by WASD)
    auto *visualTools = sub(video, tr("Visual tools"));
    for (int t = 0; t < VisualTools::ToolCount; ++t)
        add(VisualTools::tools()[t].id, QCoreApplication::translate("VisualTools", VisualTools::tools()[t].name), {},
            visualTools, [=] { tab()->video()->tools()->setTool(t); });
    // Aegisub's video/tool/vclip/*: each mode of the vector clip and drawing tools
    auto *vecModes = sub(visualTools, tr("Vector clip modes"));
    for (int k = 0; k < VisualTools::VecModeCount; ++k)
        add(QString("vecMode%1").arg(k), QCoreApplication::translate("VisualTools", VisualTools::kVecModes[k]), {},
            vecModes, [=] { tab()->video()->tools()->setVecMode(k); });
    add("resetZoom", tr("Turn off video zoom"), QKeySequence("Ctrl+Shift+Z"), video, [=] { tab()->video()->resetZoom(); });
    add("resetPan", tr("Center the zoomed video"), {}, video, [=] { tab()->video()->view()->resetPan(); });
    // Aegisub's video/zoom/* (its keys for in and out) and video/focus_seek
    add("zoomIn", tr("Zoom in"), QKeySequence("Ctrl+Num++"), video, [=] {
        auto *v = tab()->video()->view();
        v->zoomBy(1 / 1.2, v->displayRect().center());
    });
    add("zoomOut", tr("Zoom out"), QKeySequence("Ctrl+Num+-"), video, [=] {
        auto *v = tab()->video()->view();
        v->zoomBy(1.2, v->displayRect().center());
    });
    for (const int p : {50, 100, 200})
        add(QString("zoom%1").arg(p), tr("Zoom %1%").arg(p), {}, video, [=] { tab()->video()->view()->zoomScale(p / 100.0); });
    add("focusSeek", tr("Focus the video slider"), QKeySequence("Ctrl+Space"), video,
        [=] { tab()->video()->focusSeek(); });
    panelKeys(sub(video, tr("Video keys")), VideoBox::keys(), "VideoBox",
              [=](const char *id) { tab()->video()->runCommand(id); });
    auto *detach = add("detachVideo", tr("Detach video"), {}, video, [=] {
        tab()->video()->setDetached(!tab()->video()->detached());
    });
    detach->setCheckable(true);
    connect(video, &QMenu::aboutToShow, this, [=] { detach->setChecked(tab()->video()->detached()); });

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
    // Aegisub's audio/open/noise
    add("noiseAudio", tr("Open 2h30m noise audio"), {}, audio, [=] { tab()->openBlankAudio(150LL * 60 * 1000, true); });
    add("closeAudio", tr("Close audio"), {}, audio, [=] { tab()->closeAudio(); });
    // Aegisub's audio/view/spectrum|waveform
    add("audioSpectrum", tr("Spectrum display"), {}, audio, [=] { tab()->audio()->setSpectrum(true); });
    add("audioWaveform", tr("Waveform display"), {}, audio, [=] { tab()->audio()->setSpectrum(false); });
    panelKeys(sub(audio, tr("Timing keys")), AudioBox::keys(), "AudioBox",
              [=](const char *id) { tab()->audio()->runCommand(id); });
    // Aegisub's app/toggle/global_hotkeys
    auto *medusa = add("medusaMode", tr("Timing keys anywhere (Medusa mode)"), {}, audio, {});
    medusa->setCheckable(true);
    medusa->setChecked(m_medusa = QSettings().value("audio/medusa").toBool());
    connect(medusa, &QAction::toggled, this, [this](bool on) { QSettings().setValue("audio/medusa", m_medusa = on); });
    // Aegisub's audio/save/clip: from the earliest start to the latest end of the selected lines
    add("saveAudioClip", tr("Save audio clip..."), {}, audio, [=, this] {
        auto *a = tab()->audio()->source();
        const auto sel = tab()->selection();
        if (!a || sel.empty()) return;
        qint64 start = std::numeric_limits<qint64>::max(), end = 0;
        for (size_t r : sel) {
            const fsb::Row row = tab()->s().row(r);
            start = std::min<qint64>(start, row.start);
            end = std::max<qint64>(end, row.end);
        }
        const QString f = QFileDialog::getSaveFileName(this, tr("Save audio clip"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("WAV audio (*.wav)"));
        if (f.isEmpty()) return;
        try {
            a->save_clip(fs::Utf8(f), start, end);
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Save audio clip"), QString::fromUtf8(e.what()));
        }
    });

    // View
    auto *view = menu(tr("Vie&w"));
    // Aegisub's app/toggle/toolbar
    auto *showToolbar = add("toggleToolbar", tr("Show toolbar"), {}, view, {});
    showToolbar->setCheckable(true);
    showToolbar->setChecked(QSettings().value("toolbar/visible", true).toBool());
    connect(showToolbar, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("toolbar/visible", on);
        if (m_toolbar) m_toolbar->setVisible(on);
    });
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
    auto *compare = add("compareTabs", tr("Compare subtitles"), {}, view, [this] {
        if (!m_second) setSplit(panel((m_tabs->currentIndex() + 1) % m_tabs->count()));
        compareTabs(!m_compareOn);
    });
    compare->setCheckable(true);
    view->addSeparator();
    // the grid's columns and filter (Kainote's grid hotkeys); a toggle flips the current grid's own value
    auto toggles = std::make_shared<std::vector<std::pair<QAction *, std::function<bool()>>>>();
    const auto gridToggle = [=, this](const char *id, const QString &text, QMenu *m, std::function<bool()> get,
                                      std::function<void(bool)> set) {
        auto *a = add(id, text, {}, m, {});
        a->setCheckable(true);
        connect(a, &QAction::triggered, this, [=] {
            set(!get());
            a->setChecked(get());
        });
        toggles->push_back({a, get});
    };
    auto *cols = sub(view, tr("Hide columns"));
    m_actions.insert("gridColumns", cols->menuAction()); // the grid's context menu shows it too
    const std::pair<const char *, int> columns[] = {
        {"hideLayer", Grid::Layer},     {"hideStart", Grid::Start},     {"hideEnd", Grid::End},
        {"hideActor", Grid::Actor},     {"hideStyle", Grid::Style},     {"hideMarginL", Grid::MarginL},
        {"hideMarginR", Grid::MarginR}, {"hideMarginV", Grid::MarginV}, {"hideEffect", Grid::Effect},
        {"hideCps", Grid::Cps},         {"hideWraps", Grid::Wraps}};
    for (auto [id, c] : columns)
        gridToggle(id, Grid::colName(c), cols, [=] { return tab()->grid()->columnHidden(c); },
                   [=](bool on) { tab()->grid()->setColumnHidden(c, on); });
    auto *flt = sub(view, tr("Filtering"));
    m_actions.insert("gridFiltering", flt->menuAction());
    const auto spec = [=]() -> fsb::FilterSpec & { return tab()->grid()->filterSpec; };
    gridToggle("filterInvert", tr("Reverse filtering"), flt, [=] { return spec().invert; },
               [=](bool v) { spec().invert = v; });
    auto *byStyle = sub(flt, tr("Hide lines with styles"));
    connect(byStyle, &QMenu::aboutToShow, this, [=, this] {
        byStyle->clear();
        for (const auto &st : tab()->s().styles()) {
            const QString name = fs::q(st.name);
            auto *a = byStyle->addAction(name);
            a->setCheckable(true);
            a->setChecked(std::any_of(spec().hide_styles.begin(), spec().hide_styles.end(),
                                      [&](auto &x) { return fs::q(x) == name; }));
            connect(a, &QAction::toggled, this, [=](bool on) {
                // rust::Vec has no erase: rebuild without the style
                rust::Vec<rust::String> v;
                for (const auto &x : spec().hide_styles)
                    if (fs::q(x) != name) v.push_back(x);
                if (on) v.push_back(fs::rs(name));
                spec().hide_styles = std::move(v);
            });
        }
    });
    gridToggle("filterComments", tr("Hide comments"), flt, [=] { return spec().hide_comments; },
               [=](bool v) { spec().hide_comments = v; });
    gridToggle("filterDoubtful", tr("Show unconfirmed"), flt, [=] { return spec().only_doubtful; },
               [=](bool v) { spec().only_doubtful = v; });
    gridToggle("filterUntranslated", tr("Show untranslated"), flt, [=] { return spec().only_untranslated; },
               [=](bool v) { spec().only_untranslated = v; });
    const auto setting = [](const char *key) {
        return std::pair{std::function<bool()>([key] { return QSettings().value(key).toBool(); }),
                         std::function<void(bool)>([key](bool v) { QSettings().setValue(key, v); })};
    };
    const auto [afterLoad, setAfterLoad] = setting("grid/filterAfterLoad");
    gridToggle("filterAfterLoad", tr("Filter after loading subtitles"), flt, afterLoad, setAfterLoad);
    const auto [keep, setKeep] = setting("grid/filterKeep");
    gridToggle("filterKeep", tr("Do not reset the earlier filtering"), flt, keep, setKeep);
    flt->addSeparator();
    add("filter", tr("Filter"), {}, flt, [=] { tab()->grid()->filterNow(); });
    add("filterOff", tr("Turn off filtering"), {}, flt, [=] { tab()->grid()->filterOff(); });
    add("hideSelected", tr("Hide selected lines"), {}, view, [=] { tab()->grid()->hideSelected(); });
    gridToggle("filterIgnoreInActions", tr("Ignore filtering in some actions"), view,
               [=] { return tab()->grid()->ignoreFilterInActions; },
               [=](bool v) { tab()->grid()->ignoreFilterInActions = v; });
    for (auto *m : {view, cols, flt})
        connect(m, &QMenu::aboutToShow, this, [toggles] {
            for (const auto &[a, get] : *toggles) a->setChecked(get());
        });
    // the grid's context menu only, as Kainote
    add("makeTree", tr("Make tree"), {}, nullptr, [=] { tab()->grid()->makeTree(); });
    add("pasteTranslation", tr("Paste translation text"), {}, nullptr, [this] { pasteTranslation(); });

    // Subtitles
    auto *subs = menu(tr("&Subtitles"));
    auto *editor = add("editor", tr("Enable / Disable editor"), QKeySequence("Ctrl+E"), subs, {});
    editor->setCheckable(true);
    editor->setChecked(true);
    connect(editor, &QAction::triggered, this, [=](bool on) { tab()->setPlayerMode(!on); });
    add("properties", tr("ASS file properties"), {}, subs, [=] { PropertiesDialog(tab()).exec(); });
    add("styleManager", tr("Style manager"), QKeySequence("Ctrl+M"), subs, [this] {
        if (!m_styles) {
            m_styles = new StyleManager(this);
            m_styles->setAttribute(Qt::WA_DeleteOnClose);
        }
        m_styles->show();
        m_styles->raise();
        m_styles->activateWindow();
    });
    auto *conv = sub(subs, tr("Conversion"));
    const std::tuple<const char *, const char *, const char *> formats[] = {
        {"toASS", "ASS", "F9"}, {"toSRT", "SRT", "F8"}, {"toMDVD", "MicroDVD", "F10"},
        {"toMPL2", "MPL2", "F11"}, {"toTMP", "TMP", "Ctrl+F12"}};
    for (auto [id, fmt, key] : formats)
        add(id, tr("Convert to %1").arg(fmt == QString("MicroDVD") ? "MDVD" : fmt), QKeySequence(key), conv,
            [this, fmt] { convert(fmt); })
            ->setIcon(glyph(QString(fmt).left(3)));
    auto *shift = add("shiftTimes", tr("Shift times"), QKeySequence("Ctrl+I"), subs, {});
    shift->setCheckable(true);
    shift->setChecked(QSettings().value("layout/shiftTimes", true).toBool());
    connect(shift, &QAction::triggered, this, [this](bool on) {
        for (auto *t : tabs()) t->showShiftPanel(on);
    });
    // Kainote's: runs the panel's page shown, hidden or not
    add("runShift", tr("Shift times / run post processor"), {}, subs, [=] { tab()->shift()->run(); });
    // Kainote's: two lines selected, the first keeps its start and the second's moves to the
    // video; every line stretches with them
    add("fpsFromVideo", tr("Set FPS from video"), {}, subs, [=, this] {
        auto *t = tab();
        const auto sel = t->selection();
        if (sel.size() != 2 || !t->video()->hasVideo()) return status(tr("Select two lines and open a video"));
        t->s().stretch_times(t->s().row(sel[0]).start, t->s().row(sel[1]).start, t->videoStartTime());
        t->changed();
    });
    // Kainote "Set new FPS" / Aegisub "Transform Framerate": keep every line on its frames
    add("changeFps", tr("Change frame rate..."), {}, subs, [=, this] {
        auto *t = tab();
        const bool video = t->video()->hasVideo();
        QDialog d(this);
        d.setWindowTitle(tr("Change frame rate"));
        auto *f = new QFormLayout(&d);
        const QStringList rates = {"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"};
        auto rate = [&](const QString &def) {
            auto *c = new QComboBox(&d);
            c->setEditable(true);
            if (video) c->addItem(tr("From video"), "video");
            c->addItems(rates);
            c->setCurrentText(def);
            return c;
        };
        auto *from = rate(video ? tr("From video") : "23.976");
        auto *to = rate("25");
        f->addRow(tr("Subtitles timed to:"), from);
        f->addRow(tr("Convert to:"), to);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        auto value = [](QComboBox *c) {
            return c->currentText() == c->itemText(0) && c->itemData(0).isValid() ? QString("video") : c->currentText();
        };
        const auto tc = t->video()->timecodes();
        try {
            t->s().change_fps(fs::Utf8(value(from)), fs::Utf8(value(to)), {tc.data(), tc.size()});
            t->changed();
        } catch (const rust::Error &e) {
            error(QString::fromUtf8(e.what()));
        }
    });
    add("fontCollector", tr("Fonts collector"), {}, subs, [=, this] { FontCollectorDialog(tab()).exec(); });
    add("resample", tr("Resample subtitles"), {}, subs, [=] { ResampleDialog(tab()).exec(); });
    // Aegisub time/snap/scene: selected lines span the keyframes around the video frame
    add("snapScene", tr("Snap to scene"), {}, subs, [=] {
        auto *t = tab();
        const auto kf = t->keyframes();
        auto *v = t->video();
        if (kf.empty() || !v->hasVideo()) return;
        const qint64 cur = v->frame();
        qint64 prev = 0, next = v->info().frames;
        if (cur < kf.front()) next = kf.front();
        else if (cur < kf.back()) {
            auto it = std::upper_bound(kf.begin(), kf.end(), cur);
            prev = *std::prev(it);
            next = *it;
        } else prev = kf.back();
        t->s().set_rows_field(fs::slice(t->selection()), "times",
                              fs::Utf8(QString("%1,%2").arg(v->timeAtStart(prev)).arg(v->timeAtEnd(next - 1))));
        t->changed();
    });
    // Aegisub time/frame/current: shift the selection so the active line starts at the video frame
    add("shiftToFrame", tr("Shift to current frame"), {}, subs, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        fsb::ShiftSpec spec;
        spec.amount = t->videoStartTime() - t->s().row(size_t(t->active())).start;
        spec.lines = 1;
        for (size_t r : t->selection()) spec.selected.push_back(r);
        spec.active = size_t(t->active());
        const auto tc = t->video()->timecodes();
        t->s().shift(spec, {tc.data(), tc.size()});
        t->changed();
    });
    add("attachments", tr("Attachments..."), {}, subs, [=, this] { AttachmentsDialog(this, tab()).exec(); });
    add("kanjiTimer", tr("Kanji timer..."), {}, subs, [=, this] {
        if (tab()->s().row_count()) (new KanjiTimerDialog(this, tab()))->show();
    });
    add("stylingAssistant", tr("Styling assistant..."), {}, subs, [=, this] {
        if (tab()->s().row_count()) (new StylingAssistant(this, tab()))->show();
    });
    // Aegisub's tool/translation_assistant, one at a time; its keys run in it while it is open
    add("translationAssistant", tr("Translation assistant..."), {}, subs, [=, this] {
        if (auto *d = findChild<TranslationAssistant *>()) return d->raise(), d->activateWindow();
        auto *d = new TranslationAssistant(this, tab());
        if (d->start()) d->show();
    });
    panelKeys(sub(subs, tr("Translation assistant keys")), TranslationAssistant::keys(), "TranslationAssistant",
              [=, this](const char *id) {
                  if (auto *d = findChild<TranslationAssistant *>()) d->runCommand(id);
              });
    // Kainote's window for lining a pasted translation up with the timed original; non-modal,
    // it acts on the grid selection
    add("translationMatching", tr("Translation matching..."), {}, subs, [this] {
        if (auto *open = findChild<QDialog *>("translationMatching")) return open->activateWindow();
        auto *d = new QDialog(this);
        d->setObjectName("translationMatching");
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle(tr("Translation matching"));
        auto *g = new QGridLayout(d);
        g->addWidget(new QLabel(tr("Original text"), d), 0, 0);
        g->addWidget(new QLabel(tr("Translation text"), d), 0, 1);
        const std::pair<const char *, const char *> buttons[] = {
            {QT_TR_NOOP("Insert line"),
             QT_TR_NOOP("Inserts an empty line before the selected one.\nThe original moves down.\n"
                        "Set the times of the inserted line.")},
            {QT_TR_NOOP("Insert line"),
             QT_TR_NOOP("Inserts an empty translation at the selected line.\nThe translation moves down.")},
            {QT_TR_NOOP("Join lines"), QT_TR_NOOP("Joins the next line with the selected one.\nThe original moves up.")},
            {QT_TR_NOOP("Join lines"),
             QT_TR_NOOP("Joins the next translation with the selected one.\nThe translation moves up.")},
            {QT_TR_NOOP("Delete line"), QT_TR_NOOP("Deletes the selected line.\nThe original moves up.")},
            {QT_TR_NOOP("Delete line"),
             QT_TR_NOOP("Deletes the translation of the selected line.\nThe translation moves up.")}};
        const uint8_t ops[] = {4, 1, 5, 2, 3, 0}; // fsb::tl_shift order
        for (int k = 0; k < 6; ++k) {
            auto *b = new QPushButton(tr(buttons[k].first), d);
            b->setToolTip(tr(buttons[k].second));
            g->addWidget(b, 1 + k / 2, k % 2);
            connect(b, &QPushButton::clicked, this, [this, op = ops[k]] {
                auto *t = current();
                const auto sel = t->selection();
                if (sel.empty() || !t->s().file_info().tl_mode) return;
                t->s().tl_shift(sel.front(), sel.size(), op);
                t->changed();
            });
        }
        auto *note = new QLabel(tr("Original: the timed subtitles the pasted dialogue is matched against, "
                                   "removed afterwards.\nTranslation: the pasted text, which takes the original's "
                                   "timing."),
                                d);
        note->setWordWrap(true);
        g->addWidget(note, 4, 0, 1, 2);
        d->show();
    });
    add("spellChecker", tr("Spell checker..."), {}, subs, [=, this] {
        auto *d = new SpellCheckDialog(this, tab());
        if (d->findNext()) d->show();
    });
    auto *spellOn = add("spellCheck", tr("Underline misspelled words"), {}, subs, {});
    spellOn->setCheckable(true);
    spellOn->setChecked(QSettings().value("spell/enabled", true).toBool());
    connect(spellOn, &QAction::triggered, this, [spellOn] {
        // from the setting: the edit box's language menu turns it off too
        const bool on = !QSettings().value("spell/enabled", true).toBool();
        spellOn->setChecked(on);
        QSettings().setValue("spell/enabled", on);
        spell::reload();
    });
    connect(subs, &QMenu::aboutToShow, spellOn,
            [spellOn] { spellOn->setChecked(QSettings().value("spell/enabled", true).toBool()); });
    // Aegisub's tag hiding modes in the grid; the toolbar's "hideTags" cycles them
    auto *tagModes = new QActionGroup(this);
    const std::pair<const char *, QString> tagNames[] = {
        {"tagsShow", tr("Show tags")}, {"tagsSimplify", tr("Simplify tags")}, {"tagsHide", tr("Hide tags")}};
    for (int k = 0; k < 3; ++k) {
        auto *a = add(tagNames[k].first, tagNames[k].second, {}, subs, [this, k] {
            QSettings().setValue("grid/tagMode", k);
            for (auto *t : tabs()) t->grid()->setTagMode(k);
        });
        a->setCheckable(true);
        a->setChecked(k == QSettings().value("grid/tagMode").toInt());
        tagModes->addAction(a);
    }
    add("hideTags", tr("Cycle tag hiding mode"), {}, subs, [this, tagModes] {
        const auto modes = tagModes->actions();
        QAction *next = modes[(modes.indexOf(tagModes->checkedAction()) + 1) % modes.size()];
        next->trigger();
        status(next->text());
    });

    // Automation
    m_automationMenu = menu(tr("Au&tomation"));
    add("automationManager", tr("Automation manager..."), {}, nullptr, [this] { automationManager(); });
    add("loadScript", tr("Load script"), {}, nullptr, [this] {
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
        (*m_automation)->add_local_script(current()->s(), fs::Utf8(f)); // the file asks for it next time (Aegisub)
        buildAutomationMenu();
    });
    add("reloadScripts", tr("Reload all scripts"), {}, nullptr, [this] {
        for (const auto &e : (*m_automation)->reload()) log(fs::q(e));
        buildAutomationMenu();
    });
    add("runLastScript", tr("Run the last loaded script"), {}, nullptr, [this] {
        if (m_lastMacro >= 0 && size_t(m_lastMacro) < (*m_automation)->macros().size()) runMacro(size_t(m_lastMacro));
    });
    // Aegisub's validate (enabled, and a help text for the status bar in place of the
    // description) and is_active (a toggle macro's check mark), as the menu opens
    connect(m_automationMenu, &QMenu::aboutToShow, this, [this] {
        auto *tab = current();
        const auto rows = tab->selection();
        const auto states = (*m_automation)->macro_states(tab->s(), fs::slice(rows), tab->active());
        for (auto *a : m_actions) {
            const QVariant id = a->property("macroId");
            if (!id.isValid() || id.toULongLong() >= states.size()) continue;
            const auto &st = states[size_t(id.toULongLong())];
            a->setVisible(st.visible); // another file's script (Aegisub's other window)
            a->setEnabled(st.enabled);
            a->setCheckable(st.toggle);
            a->setChecked(st.checked);
            a->setStatusTip(st.help.empty() ? a->toolTip() : fs::q(st.help));
        }
        // a submenu shows while something in it does; the inner ones first
        const auto subs = m_automationMenu->findChildren<QMenu *>();
        for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
            const auto acts = (*it)->actions();
            (*it)->menuAction()->setVisible(std::ranges::any_of(acts, &QAction::isVisible));
        }
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
                                                  {"keyframes", tr("Recently opened keyframes")},
                                                  {"timecodes", tr("Recently opened timecodes")}};
    auto *m = new QMenu(titles.value(kind, kind), this);
    m_recent.insert(kind, m);
    connect(m, &QMenu::aboutToShow, this, [this, m, kind] {
        m->clear();
        for (const QString &f : QSettings().value("recent/" + kind).toStringList()) {
            m->addAction(QFileInfo(f).fileName(), this, [this, f, kind] {
                if (kind == "subs") openFile(f);
                else if (kind == "video") current()->openVideo(f);
                else if (kind == "audio") current()->openAudio(f);
                else if (kind == "timecodes") current()->openTimecodes(f);
                else current()->openKeyframes(f);
            })->setToolTip(f);
        }
        if (m->isEmpty()) m->addAction(tr("(empty)"))->setEnabled(false);
    });
    // a toolbar button shows the submenu
    auto *a = new QAction(m->title(), this);
    a->setMenu(m);
    a->setIcon(kind == "subs" ? icon("file-time", kAmber) : kind == "video" ? icon("clock-play", kBlue) : icon("history", kTeal));
    m_actions.insert("recent" + kind.left(1).toUpper() + kind.mid(1), a);
    return m;
}

void MainWindow::checkDisk(TabPanel *t) {
    if (!t || t->path().isEmpty() || m_checkingDisk) return;
    const QFileInfo fi(t->path());
    if (!fi.exists() || fi.lastModified() == t->diskTime()) return;
    t->noteDisk(); // asked once for each change
    m_checkingDisk = true;
    const auto r = QMessageBox::question(
        this, tr("File changed"),
        (t->modified() ? tr("%1 was changed by another program. Read it again, losing the changes made here?")
                       : tr("%1 was changed by another program. Read it again?"))
            .arg(fi.fileName()));
    m_checkingDisk = false;
    if (r == QMessageBox::Yes) t->openSubs(t->path());
    updateActions();
}

bool MainWindow::autosaveTabs(bool wait) {
    QDir().mkpath(autosaveDir());
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss");
    QSet<QString> used; // two untitled tabs in the same second must not share a file
    bool any = false;
    // the current tab last: after a panic in it, the others are written before it can fail again
    auto all = tabs();
    std::ranges::stable_partition(all, [this](TabPanel *t) { return t != current(); });
    for (auto *t : all) {
        const quint64 gen = t->s().generation();
        if (!t->modified() || t->property("autosaved").toULongLong() == gen) continue;
        const QString base = t->path().isEmpty() ? tr("Untitled") : QFileInfo(t->path()).completeBaseName();
        QString name = base;
        for (int k = 2; used.contains(name); ++k) name = QString("%1 (%2)").arg(base).arg(k);
        used.insert(name);
        const QString path = autosaveDir() + "/" + name + "." + stamp + ".AUTOSAVE.ass";
        // the file it is a copy of, for Save once it is recovered
        if (QFile from(path + ".from"); !t->path().isEmpty() && from.open(QIODevice::WriteOnly))
            from.write(t->path().toUtf8());
        if (!wait) {
            // the timer's: serialised and written on a worker so a big file does not stall typing;
            // marked now so the next tick skips it, unmarked if the write fails
            const QVariant was = t->property("autosaved");
            t->setProperty("autosaved", gen);
            auto backup = std::make_shared<rust::Box<fsb::Backup>>(t->s().backup());
            auto *watch = new QFutureWatcher<QString>(this);
            connect(watch, &QFutureWatcher<QString>::finished, this, [this, watch, tab = QPointer(t), gen, was] {
                watch->deleteLater();
                const QString err = watch->result();
                if (err.isEmpty()) return;
                if (tab && tab->property("autosaved").toULongLong() == gen) tab->setProperty("autosaved", was);
                status(err);
            });
            watch->setFuture(QtConcurrent::run([backup, path]() -> QString {
                try {
                    (*backup)->write(fs::Utf8(path));
                    return {};
                } catch (const rust::Error &e) {
                    return QString::fromUtf8(e.what());
                }
            }));
            any = true;
            continue;
        }
        try {
            t->s().autosave(fs::Utf8(path));
            t->setProperty("autosaved", gen);
            any = true;
        } catch (const rust::Error &e) {
            status(QString::fromUtf8(e.what()));
        }
    }
    return any;
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
    auto *menuStyle = new MenuButtonStyle;
    menuStyle->setParent(m_toolbar);
    auto rebuild = [this, menuStyle] {
        m_toolbar->clear();
        for (const QString &id : QSettings().value("toolbar/ids", kToolbarDefault).toStringList()) {
            auto *a = action(id);
            if (!a) continue;
            m_toolbar->addAction(a);
            if (auto *b = qobject_cast<QToolButton *>(m_toolbar->widgetForAction(a)); b && a->menu()) {
                b->setPopupMode(QToolButton::InstantPopup);
                b->setStyle(menuStyle);
            }
        }
        // trailing customisation arrow
        auto *more = new QToolButton(m_toolbar);
        more->setIcon(icon("chevrons-right", {}));
        more->setToolTip(tr("Choose toolbar icons"));
        more->setPopupMode(QToolButton::InstantPopup);
        more->setStyle(menuStyle);
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
    rebuild();
    addToolBar(areas[std::clamp(QSettings().value("toolbar/alignment", 0).toInt(), 0, 3)], m_toolbar);
    m_toolbar->setVisible(QSettings().value("toolbar/visible", true).toBool());
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

// ---- files --------------------------------------------------------------------

bool MainWindow::saveAs(TabPanel *tab) {
    QString start = tab->path().isEmpty() ? tab->property("savePath").toString() : tab->path();
    if (!start.isEmpty() && !tab->s().lossless(fs::Utf8(start))) {
        // Aegisub's: to an ASS name when the file's format cannot hold it all
        status(tr("Its format cannot hold everything in these subtitles: save them as ASS"));
        const QFileInfo fi(start);
        start = fi.dir().filePath(fi.completeBaseName() + ".ass");
    }
    if (QSettings().value("main/saveWithVideoName").toBool() && !tab->video()->path().isEmpty()) {
        const QFileInfo vf(tab->video()->path());
        start = vf.dir().filePath(vf.completeBaseName() + "." +
                                  (start.isEmpty() ? QString("ass") : QFileInfo(start).suffix()));
    }
    if (start.isEmpty()) start = QSettings().value("main/lastDir").toString() + "/" + tr("Untitled") + ".ass";
    const QString p = QFileDialog::getSaveFileName(this, tr("Save subtitles as"), start, kSubsFilter);
    if (p.isEmpty()) return false;
    if (!tab->s().lossless(fs::Utf8(p)) &&
        QMessageBox::warning(this, tr("Save subtitles as"),
                             tr("%1 cannot hold everything in these subtitles: styles, tags, comments or "
                                "attachments will be missing from the file. Save anyway?")
                                 .arg(QFileInfo(p).suffix().toUpper()),
                             QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
        return false;
    if (!tab->save(p)) return false;
    addRecent("subs", p);
    return true;
}

// a file in a format that cannot hold the subtitles goes to Save as (Aegisub), and so does a
// converted one, not to overwrite a file of the new extension (Kainote)
bool MainWindow::saveTab(TabPanel *tab) {
    if (tab->path().isEmpty() || tab->s().converted() || !tab->s().lossless(fs::Utf8(tab->path())) ||
        !tab->save())
        return saveAs(tab);
    addRecent("subs", tab->path());
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
    return saveTab(tab);
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

void MainWindow::convert(const QString &format) {
    auto *tab = current();
    // Kainote's: a conversion that loses something asks first
    if (!tab->s().lossless(fs::Utf8("x." + format.toLower())) &&
        QMessageBox::warning(this, tr("Conversion"),
                             tr("%1 cannot hold everything in these subtitles: styles, tags, comments or "
                                "attachments will be lost. Convert anyway?")
                                 .arg(format),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
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
    for (auto [id, f] : conv) action(id)->setEnabled(fmt != f && !fi.tl_mode);
    action("saveTranslation")->setEnabled(fi.tl_mode);
    for (const char *id : {"translationMatching", "pasteTranslation", "filterDoubtful", "filterUntranslated"})
        action(id)->setEnabled(fi.tl_mode);
    for (const char *id : {"recombine", "splitKaraoke"}) action(id)->setEnabled(!fi.tl_mode);
    action("subsFromVideo")->setEnabled(!tab->video()->path().isEmpty());
    action("closeVideo")->setEnabled(tab->video()->hasVideo());
    const char *views[] = {"viewAll", "viewVideoSubs", "viewAudioSubs", "viewOnlyVideo", "viewOnlySubs"};
    action(views[std::clamp(tab->viewMode() - 1, 0, 4)])->setChecked(true);
    action("editor")->setChecked(!tab->playerMode());
}

void MainWindow::closeEvent(QCloseEvent *e) {
    QStringList files;
    int front = 0;
    for (auto *t : tabs()) {
        if (!maybeSave(t)) return e->ignore();
        // a tab with no script file comes back with its video, as in Kainote's session
        const QString f = t->path().isEmpty() ? t->video()->path() : t->path();
        if (t == current()) front = int(files.size()); // counted among the files kept
        if (!f.isEmpty()) files << f;
    }
    QSettings st;
    st.setValue("session/files", files);
    st.setValue("session/current", front);
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
    if (d.exec() == QDialog::Accepted) setHotkey(a, edit->keySequence());
}

// Takes the key over from another command after asking; a default key is not stored, so a new
// default applies.
bool MainWindow::setHotkey(QAction *a, const QKeySequence &key) {
    const auto store = [](QAction *x) {
        QSettings st;
        if (x->shortcut() == x->property("defaultKey").value<QKeySequence>()) st.remove("hotkeys/" + x->objectName());
        else st.setValue("hotkeys/" + x->objectName(), x->shortcut().toString());
        showHotkey(x);
    };
    if (!key.isEmpty())
        for (auto *other : std::as_const(m_actions)) {
            // the audio's keys and the window's are apart
            if (other == a || other->shortcut() != key || other->property("context") != a->property("context")) continue;
            if (!confirm(tr("%1 is used by \"%2\". Take it over?").arg(key.toString(), QString(other->text()).remove('&'))))
                return false;
            other->setShortcut({});
            store(other);
        }
    a->setShortcut(key);
    store(a);
    return true;
}

// Aegisub's and Kainote's hotkey list: every command, also those only reachable by a key (tabs);
// a hotkey that differs from the default is bold.
void MainWindow::editHotkeys() {
    QDialog d(this);
    d.setWindowTitle(tr("Hotkeys"));
    d.resize(680, 600);
    auto *v = new QVBoxLayout(&d);
    auto *filter = new QLineEdit(&d);
    filter->setPlaceholderText(tr("Search"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto *list = new QTreeWidget(&d);
    list->setHeaderLabels({tr("Command"), tr("Menu"), tr("Hotkey")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    v->addWidget(list, 1);
    const int side = style()->pixelMetric(QStyle::PM_SmallIconSize);
    QPixmap blank(side, side); // keeps names without an icon in line
    blank.fill(Qt::transparent);
    for (QAction *a : std::as_const(m_actions)) {
        if (a->menu()) continue; // a submenu (recent files) has nothing to run
        QString menu;
        for (QObject *o : a->associatedObjects())
            if (auto *m = qobject_cast<QMenu *>(o)) {
                menu = QString(m->title()).remove('&');
                break;
            }
        auto *it = new QTreeWidgetItem(list, {QString(a->text()).remove('&'), menu});
        it->setIcon(0, a->icon().isNull() ? QIcon(blank) : a->icon());
        it->setData(0, Qt::UserRole, QVariant::fromValue(a));
    }
    list->sortItems(1, Qt::AscendingOrder);
    const auto refresh = [list] {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *it = list->topLevelItem(i);
            auto *a = it->data(0, Qt::UserRole).value<QAction *>();
            it->setText(2, a->shortcut().toString(QKeySequence::NativeText));
            QFont f = it->font(0);
            f.setBold(a->shortcut() != a->property("defaultKey").value<QKeySequence>());
            it->setFont(2, f);
        }
    };
    refresh();
    for (int c = 0; c < 3; ++c) list->resizeColumnToContents(c);
    connect(filter, &QLineEdit::textChanged, &d, [list](const QString &s) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *it = list->topLevelItem(i);
            it->setHidden(!s.isEmpty() && !it->text(0).contains(s, Qt::CaseInsensitive) &&
                          !it->text(2).contains(s, Qt::CaseInsensitive));
        }
    });
    const auto current = [list] {
        auto *it = list->currentItem();
        return it ? it->data(0, Qt::UserRole).value<QAction *>() : nullptr;
    };
    const auto change = [=, this] {
        if (auto *a = current()) assignHotkey(a);
        refresh();
    };
    connect(list, &QTreeWidget::itemActivated, &d, change);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    connect(bb->addButton(tr("Change..."), QDialogButtonBox::ActionRole), &QPushButton::clicked, &d, change);
    connect(bb->addButton(tr("Default"), QDialogButtonBox::ActionRole), &QPushButton::clicked, &d, [=, this] {
        if (auto *a = current()) setHotkey(a, a->property("defaultKey").value<QKeySequence>());
        refresh();
    });
    connect(bb->addButton(tr("All defaults"), QDialogButtonBox::ResetRole), &QPushButton::clicked, &d, [=, this] {
        if (!confirm(tr("Restore all hotkeys to their defaults?"))) return;
        QSettings().remove("hotkeys");
        for (auto *a : std::as_const(m_actions)) {
            a->setShortcut(a->property("defaultKey").value<QKeySequence>());
            showHotkey(a);
        }
        refresh();
    });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.exec();
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
    // a toolbar button dragged onto another takes its place (Kainote); menu buttons open on press
    if ((e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonRelease) && m_toolbar &&
        o->parent() == m_toolbar)
        if (auto *src = qobject_cast<QToolButton *>(o); src && src->defaultAction()) {
            auto *me = static_cast<QMouseEvent *>(e);
            auto *dst = qobject_cast<QToolButton *>(QApplication::widgetAt(me->globalPosition().toPoint()));
            const bool onOther = dst && dst != src && dst->parent() == m_toolbar && dst->defaultAction();
            // the button whose place the drop takes is shown pressed
            static QPointer<QToolButton> target;
            auto mark = [](QToolButton *b) {
                if (target == b) return;
                if (target) target->setDown(false);
                if ((target = b)) b->setDown(true);
            };
            if (e->type() == QEvent::MouseMove && (me->buttons() & Qt::LeftButton)) {
                src->setCursor(onOther ? Qt::DragMoveCursor : Qt::ArrowCursor);
                mark(onOther ? dst : nullptr);
            } else if (e->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
                src->unsetCursor();
                mark(nullptr);
                QStringList l = QSettings().value("toolbar/ids", kToolbarDefault).toStringList();
                const int from = l.indexOf(m_actions.key(src->defaultAction()));
                const int to = onOther ? l.indexOf(m_actions.key(dst->defaultAction())) : -1;
                if (from >= 0 && to >= 0) {
                    l.move(from, to);
                    QSettings().setValue("toolbar/ids", l);
                    QTimer::singleShot(0, this, [this] { buildToolbar(); });
                }
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
    }
    // Medusa mode: the audio's keys that type nothing (the keypad's, with Ctrl or Alt, F keys) time
    // the line from anywhere outside the video and audio, which keep their own keys
    if ((e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) && m_medusa && current() &&
        !dynamic_cast<VideoView *>(o) && !dynamic_cast<AudioView *>(o)) {
        auto *ke = static_cast<QKeyEvent *>(e);
        auto *w = qobject_cast<QWidget *>(o);
        const bool typesNothing = (ke->modifiers() & (Qt::KeypadModifier | Qt::ControlModifier | Qt::AltModifier)) ||
                                  (ke->key() >= Qt::Key_F1 && ke->key() <= Qt::Key_F35);
        if (w && w->window() == this && typesNothing && current()->audio()->hasAudio())
            if (const char *id = panelCommand(ke, AudioBox::keys())) {
                if (e->type() == QEvent::KeyPress) current()->audio()->runCommand(id);
                e->accept();
                return true;
            }
    }
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const auto mods = ke->modifiers() & ~Qt::KeypadModifier;
        const int k = ke->key();
        bool wanted = false;
        if (auto *vv = dynamic_cast<VideoView *>(o); vv || dynamic_cast<AudioView *>(o)) {
            // the panel's own commands take their keys even with Ctrl or Alt
            wanted = (!(mods & Qt::ControlModifier) && !(mods & Qt::AltModifier) && k < Qt::Key_F1) ||
                     panelCommand(ke, vv ? VideoBox::keys() : AudioBox::keys());
            // Ctrl+A picks every point of a vector clip or drawing
            if (const int t = current()->video()->tools()->tool(); vv && (t == VisualTools::VClip || t == VisualTools::Draw))
                wanted = wanted || ke->matches(QKeySequence::SelectAll);
        } else if (dynamic_cast<Grid *>(o)) {
            // Alt+arrows are left to the window's commands (Aegisub's grid/move/up|down)
            wanted = (!(mods & (Qt::ControlModifier | Qt::AltModifier)) &&
                      (k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_PageUp || k == Qt::Key_PageDown ||
                       k == Qt::Key_Home || k == Qt::Key_End || k == Qt::Key_Return || k == Qt::Key_Enter)) ||
                     ke->matches(QKeySequence::SelectAll) || ke->matches(QKeySequence::Copy) ||
                     ke->matches(QKeySequence::Cut) || ke->matches(QKeySequence::Paste);
        } else if (dynamic_cast<SubsTextEdit *>(o)) {
            wanted = panelCommand(ke, EditBox::keys());
        }
        if (wanted) {
            e->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

const char *MainWindow::panelCommand(const QKeyEvent *e, std::span<const PanelKey> keys) const {
    // composed text (a dead key, Alt+numpad) has no key: its empty sequence would match every unbound command
    if (e->key() == 0 || e->key() == Qt::Key_unknown) return nullptr;
    const auto key = Qt::Key(e->key() == Qt::Key_Enter ? Qt::Key_Return : e->key());
    for (const auto mods : {e->modifiers(), e->modifiers() & ~Qt::KeypadModifier}) {
        const QKeySequence pressed(QKeyCombination(mods, key));
        for (const auto &k : keys)
            if (auto *a = action(k.id); a && a->shortcut() == pressed) return k.id;
    }
    return nullptr;
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
    for (const char *id : {"loadScript", "reloadScripts", "automationManager", "runLastScript"})
        m->addAction(m_actions.value(id));
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

// Aegisub's Automation Manager: G(lobal, autoload) / L(oaded by hand) scripts, failed ones in red.
// Kainote's minor error corrector: checked regex rules, run in one undo step. Rules
// live in corrector/rules; a case-insensitive rule keeps the case of what it replaces.
void MainWindow::correctErrors() {
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

    QDialog d(this);
    d.setWindowTitle(tr("Correct minor errors"));
    d.resize(1000, 420);
    auto *v = new QVBoxLayout(&d);
    auto *list = new QTreeWidget(&d);
    list->setHeaderLabels(
        {tr("Description"), tr("Find (regular expression)"), tr("Replace with"), tr("Match case"), tr("Only text")});
    list->setRootIsDecorated(false);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(list);
    auto addRule = [list](const QString &desc, const QString &find, const QString &repl, bool on, bool matchCase,
                          bool onlyText) {
        auto *it = new QTreeWidgetItem(list, {desc, find, repl});
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        it->setCheckState(3, matchCase ? Qt::Checked : Qt::Unchecked);
        it->setCheckState(4, onlyText ? Qt::Checked : Qt::Unchecked);
        return it;
    };
    QSettings st;
    const int n = st.beginReadArray("corrector/rules");
    for (int i = 0; i < n; ++i) {
        st.setArrayIndex(i);
        addRule(st.value("description").toString(), st.value("find").toString(), st.value("replace").toString(),
                st.value("enabled").toBool(), st.value("matchCase").toBool(), st.value("onlyText", true).toBool());
    }
    st.endArray();
    auto restore = [&] {
        for (const Rule &r : defaults)
            if (list->findItems(r.find, Qt::MatchExactly, 1).isEmpty())
                addRule(r.description, r.find, r.replace, false, false, true);
    };
    if (!n) restore();
    for (int c = 1; c < 5; ++c) list->resizeColumnToContents(c);
    // text columns edit on a double click, the check columns toggle
    connect(list, &QTreeWidget::itemDoubleClicked, &d, [list](QTreeWidgetItem *it, int col) {
        if (col < 3) list->editItem(it, col);
    });

    auto *h = new QHBoxLayout;
    v->addLayout(h);
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, &d);
        connect(b, &QPushButton::clicked, &d, fn);
        h->addWidget(b);
        return b;
    };
    button(tr("Add"), [&] {
        auto *it = addRule(tr("New rule"), {}, {}, true, false, true);
        list->setCurrentItem(it);
        list->editItem(it, 1);
    });
    button(tr("Remove"), [&] { delete list->currentItem(); });
    button(tr("Restore defaults"), restore);
    h->addStretch();
    auto *scope = new QComboBox(&d);
    scope->addItems({tr("All lines"), tr("Selected lines")});
    h->addWidget(scope);
    auto *run = button(tr("Replace"), [&, this] {
        auto *t = current();
        if (!t) return;
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
            if (scope->currentIndex() == 1)
                for (size_t r : t->selection()) s.rows.push_back(r);
        }
        if (specs.empty()) return status(tr("No rule is checked"));
        // nothing selected must not mean every line
        if (scope->currentIndex() == 1 && t->selection().empty()) return status(tr("No lines selected"));
        try {
            const size_t count = t->s().replace_rules({specs.data(), specs.size()});
            t->changed();
            status(tr("Replacements: %1").arg(count));
        } catch (const rust::Error &e) {
            error(QString::fromUtf8(e.what()));
        }
    });
    run->setDefault(true);
    button(tr("Close"), [&] { d.accept(); });
    d.exec();

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
    }
    st.endArray();
}

void MainWindow::automationManager() {
    QDialog d(this);
    d.setWindowTitle(tr("Automation manager"));
    d.resize(720, 380);
    auto *v = new QVBoxLayout(&d);
    auto *list = new QTreeWidget(&d);
    list->setHeaderLabels({"", tr("Name"), tr("File name"), tr("Description")});
    list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    v->addWidget(list);
    auto *h = new QHBoxLayout;
    v->addLayout(h);
    rust::Vec<fsb::ScriptItem> scripts;
    QPushButton *remove = nullptr, *reload = nullptr, *info = nullptr;
    const auto row = [list] { return list->currentIndex().row(); };
    const auto enable = [&] {
        const int r = row();
        const bool any = r >= 0 && size_t(r) < scripts.size();
        remove->setEnabled(any && !scripts[size_t(r)].autoload);
        reload->setEnabled(any);
        info->setEnabled(any);
    };
    auto refresh = [&, this](int select) {
        buildAutomationMenu();
        scripts = (*m_automation)->scripts(current()->s());
        list->clear();
        for (const auto &s : scripts) {
            const bool ok = s.error.empty();
            auto *it = new QTreeWidgetItem(list, {s.autoload ? "G" : "L", fs::q(s.name),
                                                  QFileInfo(fs::q(s.path)).fileName(),
                                                  fs::q(ok ? s.description : s.error)});
            it->setHidden(!s.shown); // another file's (Aegisub: another window's manager)
            if (!ok)
                for (int c = 0; c < 4; ++c) it->setForeground(c, QColor(255, 128, 128));
        }
        for (int c = 0; c < 3; ++c) list->resizeColumnToContents(c);
        list->setCurrentItem(list->topLevelItem(std::max(0, std::min(select, list->topLevelItemCount() - 1))));
        enable();
    };
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, &d);
        connect(b, &QPushButton::clicked, &d, fn);
        h->addWidget(b);
        return b;
    };
    button(tr("Add"), [&, this] {
        const QString f = QFileDialog::getOpenFileName(&d, tr("Load script"), {},
                                                       tr("Automation scripts (*.lua *.moon);;All files (*)"));
        if (f.isEmpty()) return;
        try {
            (*m_automation)->load_script(fs::Utf8(f));
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        // Aegisub's local scripts: the file lists those added here
        (*m_automation)->add_local_script(current()->s(), fs::Utf8(f));
        refresh(int(scripts.size()));
    });
    remove = button(tr("Remove"), [&, this] {
        (*m_automation)->remove_local_script(current()->s(), scripts[size_t(row())].path);
        (*m_automation)->remove_script(size_t(row()));
        refresh(row());
    });
    reload = button(tr("Reload"), [&, this] {
        try {
            (*m_automation)->reload_script(size_t(row()));
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        refresh(row());
    });
    info = button(tr("Show info"), [&] {
        const auto &s = scripts[size_t(row())];
        QMessageBox::information(
            &d, tr("Automation script info"),
            tr("Name: %1\nDescription: %2\nAuthor: %3\nVersion: %4\nFull path: %5\nState: %6\nMacros: %7\n"
               "Export filters: %8")
                .arg(fs::q(s.name), fs::q(s.description), fs::q(s.author), fs::q(s.version), fs::q(s.path),
                     s.error.empty() ? tr("Correctly loaded") : tr("Failed to load: %1").arg(fs::q(s.error)),
                     QString::number(s.macros), QString::number(s.filters)));
    });
    button(tr("Reload all"), [&, this] {
        for (const auto &e : (*m_automation)->reload()) log(fs::q(e));
        refresh(row());
    });
    h->addStretch();
    button(tr("Close"), [&] { d.accept(); });
    connect(list, &QTreeWidget::currentItemChanged, &d, enable);
    refresh(0);
    d.exec();
}

void MainWindow::loadLocalScripts(TabPanel *t) {
    // Aegisub's LocalScriptManager: the scripts the file lists load only once the user trusts them
    const auto scripts = (*m_automation)->local_scripts(t->s());
    QStringList listed;
    for (const auto &s : scripts) {
        if (s.path.empty()) {
            const QString tok = fs::q(s.token);
            log(tr("Automation Script referenced with unknown location specifier character.\nLocation specifier "
                   "found: %1\nFilename specified: %2")
                    .arg(tok.left(1), tok.mid(1)));
            continue;
        }
        listed << tr("    %1 (%2)").arg(fs::q(s.token), QDir::toNativeSeparators(fs::q(s.path)));
    }
    if (listed.isEmpty()) return;
    QMessageBox box(QMessageBox::Warning, tr("Load associated automation scripts?"),
                    tr("The subtitle file you have opened contains local automation scripts. Do you want to load "
                       "the associated automation scripts?\n\nList of scripts:\n\n%1\n\nYou should only load "
                       "associated automation scripts if you trust their authors! They are not required to view or "
                       "edit the subtitle file.")
                        .arg(listed.join('\n')),
                    QMessageBox::Ok | QMessageBox::Cancel, this);
    box.button(QMessageBox::Ok)->setText(tr("Trust the authors && load scripts"));
    box.button(QMessageBox::Cancel)->setText(tr("No"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;
    for (const auto &s : scripts) {
        if (s.path.empty()) continue;
        if (!s.exists) {
            log(tr("Automation Script referenced could not be found.\nFilename specified: %1\nSearched relative to: "
                   "%2\nResolved filename: %3")
                    .arg(fs::q(s.token).mid(1), QDir::toNativeSeparators(fs::q(s.base)),
                         QDir::toNativeSeparators(fs::q(s.path))));
            continue;
        }
        try {
            (*m_automation)->load_script(s.path);
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
    }
    buildAutomationMenu();
}

void MainWindow::runScript(fsb::ScriptJob &job) {
    // Aegisub's BackgroundScriptRunner: the script runs on a worker and m_host brings its dialogs
    // and progress back here. Input waits for the first 300 ms, then the progress window holds it.
    m_host->begin();
    QFutureWatcher<void> watch;
    QEventLoop loop;
    connect(&watch, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    watch.setFuture(QtConcurrent::run([&job] { fsb::run_script_job(job); }));
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    if (!watch.isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!watch.isFinished()) {
        m_host->showProgress();
        loop.exec();
    }
    // quitting the application ends the loops early: stop the script, the worker holds the job
    if (!watch.isFinished()) (*m_automation)->stop_script();
    while (!watch.isFinished()) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    m_host->finish();
}

void MainWindow::runMacro(size_t id) {
    auto *tab = current();
    const auto rows = tab->selection();
    fsb::MacroContext ctx;
    if (tab->audio()->hasAudio()) {
        ctx.has_audio = true;
        std::tie(ctx.audio_start, ctx.audio_end) = tab->audio()->selection();
    }
    tab->edit()->scriptCursor(ctx.cursor, ctx.sel_start, ctx.sel_end);
    fsb::MacroResult r;
    QString err;
    try {
        auto job = (*m_automation)->start_macro(id, tab->s(), fs::slice(rows), tab->active(), ctx);
        runScript(*job);
        r = (*m_automation)->finish_macro(std::move(job), tab->s());
    } catch (const rust::Error &e) {
        err = QString::fromUtf8(e.what());
    }
    log(err); // what the script wrote was in its progress window
    if (r.changed) tab->changed();
    // a macro may only return a new selection (e.g. Select overlaps)
    const auto sel = fs::vec(r.selection);
    if (!sel.empty()) tab->setSelection(sel, r.active >= 0 ? int(r.active) : int(sel.front()));
    if (r.edit_changed) tab->edit()->setScriptCursor(r.cursor, r.sel_start, r.sel_end);
}

bool MainWindow::ebuSettings(fsb::ExportSpec &spec) {
    QSettings st;
    st.beginGroup("ebuStl");
    QDialog d(this);
    d.setWindowTitle(tr("Export to EBU STL format"));
    auto *f = new QFormLayout(&d);
    auto combo = [&](const QStringList &items, const char *key, int def) {
        auto *c = new QComboBox(&d);
        c->addItems(items);
        c->setCurrentIndex(std::clamp(st.value(key, def).toInt(), 0, int(items.size()) - 1));
        return c;
    };
    auto *tv = combo({tr("23.976 fps (non-standard, STL24.01)"), tr("24 fps (non-standard, STL24.01)"),
                      tr("25 fps (STL25.01)"), tr("29.97 fps (non-dropframe, STL30.01)"),
                      tr("29.97 fps (dropframe, STL30.01)"), tr("30 fps (STL30.01)")},
                     "tvStandard", 0);
    auto *enc = combo({tr("ISO 6937-2 (Latin/Western Europe)"), tr("ISO 8859-5 (Cyrillic)"), tr("ISO 8859-6 (Arabic)"),
                       tr("ISO 8859-7 (Greek)"), tr("ISO 8859-8 (Hebrew)"), tr("UTF-8 Unicode (non-standard)")},
                      "textEncoding", 0);
    auto *wrap = combo({tr("Automatically wrap long lines (ASS)"), tr("Automatically wrap long lines (Balanced)"),
                        tr("Abort if any lines are too long"), tr("Skip lines that are too long")},
                       "lineWrapping", 1);
    auto *len = new QSpinBox(&d);
    len->setRange(10, 99);
    len->setValue(st.value("maxLineLength", 42).toInt());
    auto *align = new QCheckBox(tr("Translate alignments"), &d);
    align->setChecked(st.value("translateAlignments", true).toBool());
    auto *offset = new QLineEdit(st.value("timecodeOffset", "00:00:00:00").toString(), &d);
    offset->setInputMask("99:99:99:99");
    auto *inclusive = new QCheckBox(tr("Out-times are inclusive"), &d);
    inclusive->setChecked(st.value("inclusiveEndTimes", true).toBool());
    auto *display = combo({tr("Open captions"), tr("Level-1 teletext"), tr("Level-2 teletext")}, "displayStandard", 0);
    f->addRow(tr("TV standard:"), tv);
    f->addRow(tr("Text encoding:"), enc);
    f->addRow(tr("Line wrapping:"), wrap);
    f->addRow(tr("Max. line length:"), len);
    f->addRow(align);
    f->addRow(tr("Time code offset:"), offset);
    f->addRow(inclusive);
    f->addRow(tr("Display standard:"), display);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    f->addRow(bb);
    if (d.exec() != QDialog::Accepted) return false;
    st.setValue("tvStandard", tv->currentIndex());
    st.setValue("textEncoding", enc->currentIndex());
    st.setValue("lineWrapping", wrap->currentIndex());
    st.setValue("maxLineLength", len->value());
    st.setValue("translateAlignments", align->isChecked());
    st.setValue("timecodeOffset", offset->text());
    st.setValue("inclusiveEndTimes", inclusive->isChecked());
    st.setValue("displayStandard", display->currentIndex());
    spec.ebu_tv_standard = std::uint8_t(tv->currentIndex());
    spec.ebu_text_encoding = std::uint8_t(enc->currentIndex());
    spec.ebu_line_wrapping = std::uint8_t(wrap->currentIndex());
    spec.ebu_max_line_length = len->value();
    spec.ebu_translate_alignments = align->isChecked();
    spec.ebu_timecode_offset = fs::rs(offset->text());
    spec.ebu_inclusive_end_times = inclusive->isChecked();
    spec.ebu_display_standard = std::uint8_t(display->currentIndex());
    return true;
}

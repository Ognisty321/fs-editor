// MainWindow's menu bar: the menus built one after another in the bar's order (the larger ones
// in the files beside this one), then laid out as Kainote has them.
#include "mainwindow_p.h"

#include "audiobox.h"
#include "grid.h"
#include "tabpanel.h"
#include "updates.h"
#include "videobox.h"

#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QToolBar>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

void MainWindow::buildMenus() {
    Menus menus;
    buildFileMenu(menus);
    buildEditMenu(menus);
    buildVideoMenu(menus);
    buildAudioMenu(menus);
    buildViewMenu(menus);
    buildSubtitlesMenu(menus);
    buildAutomationCommands();
    buildHelpMenu();
    layOutMenus(menus);
}

QMenu *MainWindow::barMenu(const QString &title) {
    auto *m = menuBar()->addMenu(title);
    m->installEventFilter(this);
    // what can run now, and Undo's step, as Aegisub validates its menus when they open
    connect(m, &QMenu::aboutToShow, this, &MainWindow::updateActions);
    return m;
}

QMenu *MainWindow::subMenu(QMenu *parent, const QString &title) {
    auto *m = parent->addMenu(title);
    m->installEventFilter(this);
    return m;
}

// a panel's own keys, which work while it has the keyboard (Kainote's audio and video hotkeys):
// listed in the hotkey editor under their menu's name, where they take other keys; the menu
// itself is not on the bar, as in Kainote
void MainWindow::addPanelKeys(QMenu *m, std::span<const PanelKey> keys, const char *context,
                              const std::function<void(const char *)> &run) {
    for (const auto &k : keys) {
        auto *a = add(k.id, QCoreApplication::translate(context, k.name), QKeySequence(k.key), m,
                      [=] { run(k.id); });
        a->setShortcutContext(Qt::WidgetShortcut); // not the window's: G and the arrows are typed elsewhere
        removeAction(a);
        a->setProperty("context", context); // the hotkey editor's conflicts are within one panel
    }
}

void MainWindow::buildAudioMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *audio = menus.audio = barMenu(tr("&Audio"));
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
    add("closeAudio", tr("Close audio"), {}, audio, [=, this] {
        tab()->closeAudio();
        updateActions();
    });
    // Aegisub's audio/view/spectrum|waveform
    add("audioSpectrum", tr("Spectrum display"), {}, audio, [=] { tab()->audio()->setSpectrum(true); });
    add("audioWaveform", tr("Waveform display"), {}, audio, [=] { tab()->audio()->setSpectrum(false); });
    addPanelKeys(subMenu(audio, tr("Timing keys")), AudioBox::keys(), "AudioBox",
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
}

void MainWindow::buildViewMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *view = menus.view = barMenu(tr("Vie&w"));
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
    // Kainote's key for its preview of another tab's subtitles in the grid, which this is
    auto *split = add("splitView", tr("Show two tabs"), QKeySequence("Ctrl+Q"), view, [this] {
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
    auto *cols = subMenu(view, tr("Hide columns"));
    m_actions.insert("gridColumns", cols->menuAction()); // the grid's context menu shows it too
    const std::pair<const char *, int> columns[] = {
        {"hideLayer", Grid::Layer},     {"hideStart", Grid::Start},     {"hideEnd", Grid::End},
        {"hideActor", Grid::Actor},     {"hideStyle", Grid::Style},     {"hideMarginL", Grid::MarginL},
        {"hideMarginR", Grid::MarginR}, {"hideMarginV", Grid::MarginV}, {"hideEffect", Grid::Effect},
        {"hideCps", Grid::Cps},         {"hideWraps", Grid::Wraps}};
    for (auto [id, c] : columns)
        gridToggle(id, Grid::colName(c), cols, [=] { return tab()->grid()->columnHidden(c); },
                   [=](bool on) { tab()->grid()->setColumnHidden(c, on); });
    auto *flt = menus.filter = subMenu(view, tr("Filtering"));
    m_actions.insert("gridFiltering", flt->menuAction());
    const auto spec = [=]() -> fsb::FilterSpec & { return tab()->grid()->filterSpec; };
    gridToggle("filterInvert", tr("Reverse filtering"), flt, [=] { return spec().invert; },
               [=](bool v) { spec().invert = v; });
    auto *byStyle = subMenu(flt, tr("Hide lines with styles"));
    connect(byStyle, &QMenu::aboutToShow, this, [=, this] {
        byStyle->clear();
        for (const auto &st : tab()->s().styles()) {
            const QString name = fs::q(st.name);
            auto *a = byStyle->addAction(fs::menuText(name));
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
}

void MainWindow::buildHelpMenu() {
    auto *help = barMenu(tr("&Help"));
    add("help", tr("Help"), QKeySequence("F1"), help, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + "/README.md"));
    });
    // Aegisub's and Kainote's check for a newer version, here against the project's releases
    add("checkUpdates", tr("Check for updates..."), {}, help, [this] { updates::check(this, true); });
    add("about", tr("About"), {}, help, [this] {
        QMessageBox::about(this, tr("About FS Editor"),
                           tr("<b>FS Editor %1</b><br>ASS/SSA subtitle editor.<br>Licensed under BSD-3-Clause.<br>"
                              "Layout modelled on Kainote, automation compatible with Aegisub Automation 4.")
                               .arg(QCoreApplication::applicationVersion()));
    });
    add("aboutQt", tr("About Qt"), {}, help, [] { QApplication::aboutQt(); });
}

void MainWindow::layOutMenus(const Menus &menus) {
    // The menus as Kainote lays them out: its entries first and in its order, and what Aegisub and
    // this editor add to them after, under a few names instead of in one list of fifty. An entry is
    // a command's id, "-" a separator, ">" with a title opens a submenu of the entries up to "<",
    // "@" with its title is a submenu the menu had, and "!" one that stays off the bar: a panel's
    // keys are commands of the hotkey window, which lists them under that menu's name. What a
    // layout does not name is kept at the menu's end, so a command added later is not lost.
    const auto lay = [this](QMenu *m, std::initializer_list<const char *> entries) {
        const QList<QAction *> old = m->actions();
        QHash<QString, QAction *> had; // by id; a submenu by its title
        for (QAction *a : old) {
            m->removeAction(a);
            if (a->isSeparator()) a->deleteLater();
            else had.insert(a->menu() ? a->menu()->title() : a->objectName(), a);
        }
        QSet<QAction *> placed;
        QList<QMenu *> open{m};
        for (auto e = entries.begin(); e != entries.end(); ++e) {
            const QString s = QString::fromLatin1(*e);
            if (s == "-") {
                open.last()->addSeparator();
            } else if (s == "<") {
                if (open.size() > 1) open.removeLast();
            } else if (s == ">" && e + 1 != entries.end()) {
                auto *sub = open.last()->addMenu(tr(*++e));
                sub->installEventFilter(this);
                open << sub;
            } else if ((s == "@" || s == "!") && e + 1 != entries.end()) {
                if (QAction *a = had.value(tr(*++e))) {
                    if (s == "@") open.last()->addAction(a);
                    placed << a;
                }
            } else if (QAction *a = had.contains(s) ? had.value(s) : m_actions.value(s)) {
                open.last()->addAction(a);
                placed << a;
            }
        }
        bool rest = false;
        for (QAction *a : old) {
            if (a->isSeparator() || placed.contains(a)) continue;
            // another menu's layout took it
            const auto objects = a->associatedObjects();
            if (std::ranges::any_of(objects, [](QObject *o) { return qobject_cast<QMenu *>(o) != nullptr; })) continue;
            if (!rest) m->addSeparator();
            rest = true;
            m->addAction(a);
        }
    };
    lay(menus.lines, {"duplicate", "@", "Insert lines", "-", "copyLines", "cutLines", "pasteLines", "pasteOver", "copyColumns", "-",
                      "swapLines", "moveLinesUp", "moveLinesDown", "-",
                      ">", QT_TR_NOOP("Join"), "joinLines", "joinKeepFirst", "joinKeepLast", "joinKaraoke", "-", "joinPrev", "joinNext",
                      "-", "recombine", "<",
                      "@", "Split lines", "splitKaraoke", "-", "continuousPrev", "continuousNext", "-",
                      "deleteText", "deleteTextKeepTags", "deleteLines", "-",
                      "prevLine", "nextLine", "selectFromVideo", "selectOnVideo"});
    lay(menus.filter, {"filterInvert", "@", "Hide lines with styles", "filterComments", "filterDoubtful", "filterUntranslated", "-",
                       "filterAfterLoad", "filterKeep", "filterIgnoreInActions", "-", "filter", "filterOff", "hideSelected"});
    lay(menus.file, {"openSubs", "save", "saveAll", "saveAs", "saveTranslation", "@", "Recently opened subtitles", "removeSubs",
                     "saveWithVideoName", "openAutosave", "removeTemp", "showLog", "@", "Last session", "-",
                     ">", QT_TR_NOOP("Import and export"), "openSubsCharset", "subsFromVideo", "exportAs", "<", "-",
                     "settings", "hotkeys", "exit"});
    lay(menus.edit, {"undo", "undoToSave", "redo", "history", "-", "findReplace", "find", "findNext", "-",
                     "@", "Sort all lines", "@", "Sort selected lines", "correctErrors", "selectLines", "-",
                     "@", "Lines", "!", "Edit box keys"});
    lay(menus.video, {"openVideo", "@", "Recently opened videos", "closeVideo", "openKeyframes", "@", "Recently opened keyframes",
                      "openDummy", "-",
                      "startFromVideo", "endFromVideo", "prevFrame", "nextFrame", "goStart", "goEnd", "playPause", "prevKeyframe",
                      "nextKeyframe", "audioToVideo", "markToVideo", "-",
                      "zoomVideo", "resetZoom", "-",
                      ">", QT_TR_NOOP("Navigation"), "playVideoLine", "prevFrames", "nextFrames", "-", "snapStart", "snapEnd", "-",
                      "prevChapter", "nextChapter", "prevBoundary", "nextBoundary", "-", "jumpTo", "focusSeek", "<",
                      ">", QT_TR_NOOP("Keyframes and timecodes"), "saveKeyframes", "closeKeyframes", "-", "openTimecodes",
                      "@", "Recently opened timecodes", "saveTimecodes", "closeTimecodes", "<",
                      ">", QT_TR_NOOP("Frame"), "saveFrameSubs", "copyFrameSubs", "-", "saveFrame", "copyFrame", "-",
                      "saveFrameSubsOnly", "copyFrameSubsOnly", "<",
                      ">", QT_TR_NOOP("Zoom"), "zoomIn", "zoomOut", "-", "zoom50", "zoom100", "zoom200", "-", "resetPan", "<",
                      "@", "Visual tools",
                      ">", QT_TR_NOOP("Video window"), "detachVideo", "showOverscan", "videoDetails", "<",
                      "!", "Video keys"});
    lay(menus.audio, {"openAudio", "@", "Recently opened audio", "audioFromVideo", "blankAudio", "noiseAudio", "closeAudio", "-",
                      "audioSpectrum", "audioWaveform", "-", "saveAudioClip", "medusaMode", "!", "Timing keys"});
    lay(menus.view, {"viewAll", "viewVideoSubs", "viewAudioSubs", "viewOnlyVideo", "viewOnlySubs", "-",
                     "toggleToolbar", "splitView", "compareTabs", "-", "@", "Hide columns", "@", "Filtering"});
    lay(menus.subs, {"editor", "properties", "styleManager", "@", "Conversion", "shiftTimes", "fontCollector", "resample",
                     "spellChecker", ">", QT_TR_NOOP("Tags"), "tagsShow", "tagsSimplify", "tagsHide", "-", "hideTags", "<", "-",
                     ">", QT_TR_NOOP("Timing"), "runShift", "fpsFromVideo", "changeFps", "-", "snapScene", "shiftToFrame", "kanjiTimer",
                     "<",
                     ">", QT_TR_NOOP("Assistants"), "stylingAssistant", "translationAssistant", "translationMatching", "<",
                     "attachments", "cleanStyles", "spellCheck", "!", "Styling assistant keys", "!", "Translation assistant keys"});
}

// Main window in the Kainote layout: menu bar, vertical toolbar on the left,
// tabs (one TabPanel per subtitle file) with the tab bar at the bottom and a
// 9-field status bar. Every command is a QAction with a stable id that the
// menus, the toolbar and the hotkey settings share.
#pragma once

#include "bridge.h"

#include <QMainWindow>
#include <QPointer>
#include <QMap>
#include <span>

class TabPanel;
class QSplitter;
class QTabWidget;
class QToolBar;
class QLabel;
class FindDialog;
class StyleManager;
class GuiAutomationHost;
class QKeyEvent;

// A command of a panel's own keys (Kainote's audio and video hotkeys): the hotkey editor lists them,
// and they run while that panel has the keyboard
struct PanelKey {
    const char *id, *name, *key; // name: translated in the panel's own context
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    TabPanel *current() const;
    QList<TabPanel *> tabs() const;
    void showPanel(TabPanel *t); // brings its tab to the front
    TabPanel *newTab();
    // Opens subtitles, video, audio or keyframes by extension.
    void openFile(const QString &path, const QString &encoding = {});
    // Copies modified tabs to autosaveDir(); true if any file was written. With !wait the files
    // are written on a worker and true means one was started.
    bool autosaveTabs(bool wait = true);

    QAction *action(const QString &id) const { return m_actions.value(id); }
    // The panel command the key is bound to, nullptr if none: a numpad key's own first ("Num+4"),
    // else the same key of the main keyboard; the numpad's Enter is Enter
    const char *panelCommand(const QKeyEvent *e, std::span<const PanelKey> keys) const;
    QMenu *recentMenu(const QString &kind);
    void addRecent(const QString &kind, const QString &path);

    bool confirm(const QString &msg);
    void status(const QString &msg);
    void error(const QString &msg);
    void log(const QString &text); // the log window, shown when text is not blank
    void editStyle(const QString &name);
    // Offers the scripts the tab's file lists ("Automation Scripts") that are not loaded yet.
    void loadLocalScripts(TabPanel *t);
    void pasteTranslation();
    void updateStatus();

protected:
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    QAction *add(const QString &id, const QString &text, const QKeySequence &key, QMenu *menu,
                 std::function<void()> fn);
    void buildMenus();
    void buildToolbar();
    void buildStatusBar();
    void buildAutomationMenu();
    void assignHotkey(QAction *a);
    bool setHotkey(QAction *a, const QList<QKeySequence> &keys);
    void editHotkeys();
    bool closeTab(int index);
    TabPanel *panel(int index) const;
    int indexOfPanel(TabPanel *t) const;
    void setSplit(TabPanel *second);
    void compareTabs(bool on);
    void followActive();
    bool maybeSave(TabPanel *tab);
    int pickEmbeddedTrack(const rust::Vec<fsb::SubTrack> &tracks, const QString &question); // -1: cancelled
    bool saveAs(TabPanel *tab);
    bool saveTab(TabPanel *tab);
    void checkDisk(TabPanel *t); // offers to read a file another program changed again
    void convert(const QString &format);
    bool ebuSettings(fsb::ExportSpec &spec); // Aegisub's "Export to EBU STL format"; false: cancelled
    void runMacro(size_t id);
    void runScript(fsb::ScriptJob &job); // on a worker, with the progress window
    void automationManager();
    void correctErrors();
    void updateActions();
    void withRow(const std::function<void(TabPanel *, fsb::Row &)> &fn);

    QTabWidget *m_tabs;
    // Kainote "show two tabs": the second tab's panel lives in m_right instead of its tab slot
    QSplitter *m_split = nullptr;
    QWidget *m_right = nullptr;
    QPointer<TabPanel> m_second, m_prev;
    bool m_compareOn = false;
    bool m_medusa = false; // the audio's timing keys work anywhere in the window (Aegisub's Medusa mode)
    class QTimer *m_compareLater = nullptr; // edits rerun the comparison once typing pauses
    QToolBar *m_toolbar = nullptr;
    QMenu *m_automationMenu = nullptr;
    QMap<QString, QAction *> m_actions;
    QMap<QString, QMenu *> m_recent;
    QLabel *m_statusFields[9];
    FindDialog *m_find = nullptr;
    QPointer<StyleManager> m_styles; // open beside the editor (Kainote), gone when closed
    std::optional<rust::Box<fsb::Automation>> m_automation;
    GuiAutomationHost *m_host = nullptr; // owned by m_automation
    qint64 m_lastMacro = -1; // first macro of the last script loaded by hand
    bool m_checkingDisk = false;
};

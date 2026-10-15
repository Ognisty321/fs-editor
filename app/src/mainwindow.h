// Main window in the Kainote layout: menu bar, vertical toolbar on the left,
// tabs (one TabPanel per subtitle file) with the tab bar at the bottom and a
// 9-field status bar. Every command is a QAction with a stable id that the
// menus, the toolbar and the hotkey settings share.
#pragma once

#include "bridge.h"

#include <QMainWindow>
#include <QPointer>
#include <QMap>

class TabPanel;
class QSplitter;
class QTabWidget;
class QToolBar;
class QLabel;
class FindDialog;
class GuiAutomationHost;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    TabPanel *current() const;
    QList<TabPanel *> tabs() const;
    TabPanel *newTab();
    // Opens subtitles, video, audio or keyframes by extension.
    void openFile(const QString &path, const QString &encoding = {});
    // Copies modified tabs to autosaveDir(); true if any file was written.
    bool autosaveTabs();

    QAction *action(const QString &id) const { return m_actions.value(id); }
    QMenu *recentMenu(const QString &kind);
    void addRecent(const QString &kind, const QString &path);

    bool confirm(const QString &msg);
    void status(const QString &msg);
    void error(const QString &msg);
    void log(const QString &text); // the log window, shown when text is not blank
    void editStyle(const QString &name);
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
    bool closeTab(int index);
    TabPanel *panel(int index) const;
    int indexOfPanel(TabPanel *t) const;
    void setSplit(TabPanel *second);
    void compareTabs(bool on);
    bool maybeSave(TabPanel *tab);
    bool saveAs(TabPanel *tab);
    void convert(const QString &format);
    void runMacro(size_t id);
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
    QToolBar *m_toolbar = nullptr;
    QMenu *m_automationMenu = nullptr;
    QMap<QString, QAction *> m_actions;
    QMap<QString, QMenu *> m_recent;
    QLabel *m_statusFields[9];
    FindDialog *m_find = nullptr;
    std::optional<rust::Box<fsb::Automation>> m_automation;
    GuiAutomationHost *m_host = nullptr; // owned by m_automation
    qint64 m_lastMacro = -1; // first macro of the last script loaded by hand
};

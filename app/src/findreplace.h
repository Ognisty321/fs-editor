// Find and Replace (Aegisub's fields, Kainote's Find all, find and replace in files and
// the results window).
#pragma once

#include "bridge.h"

#include <QDialog>
#include <QPointer>
#include <array>
#include <utility>

class TabPanel;
class MainWindow;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QLabel;
class QTabWidget;
class QTreeWidget;
class QPushButton;

class FindDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindDialog(MainWindow *main);
    void showTab(int tab); // 0 find, 1 replace, 2 find in files
    void findNext();
private:
    fsb::FindSpec spec(TabPanel *tab) const;
    bool ready() const; // something to look for: Kainote's start / end of text find empty fields
    void remember(bool replace); // the recent searches and the options, as Aegisub keeps them
    std::array<std::pair<QCheckBox *, const char *>, 7> options() const;
    // Kainote's Find all: the matching lines of the tabs, or of the files in a folder, in a results window
    void findAll(const QList<TabPanel *> &tabs);
    void findInFiles();
    void replaceInFiles(); // Kainote's Replace in subtitles, each file backed up first
    bool filesReady();     // the folder search's settings kept; false without a folder or a valid search
    void clearResults();
    void addResults(TabPanel *tab, const QString &title, const QString &path, const rust::Vec<fsb::FindMatch> &found);
    void showResults(const QString &summary);
    void goToResult(class QTreeWidgetItem *item);
    void replaceChecked(); // Kainote's: the matches ticked in the results, each where it was found
    MainWindow *m_main;
    QTabWidget *m_tabs;
    QComboBox *m_find, *m_replace;
    QCheckBox *m_case, *m_regex, *m_comments, *m_skipTags, *m_skipText, *m_atStart, *m_atEnd, *m_subdirs;
    QRadioButton *m_field[4], *m_lines[3];
    QLineEdit *m_dir, *m_filter;
    class QPushButton *m_findFiles, *m_replFiles;
    QDialog *m_results = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_replaceChecked = nullptr;
    QLineEdit *m_resultReplace = nullptr;
    fsb::FindSpec m_resultSpec; // the search the results come from
    QList<QPointer<TabPanel>> m_resultTabs; // per file in the results; null: not open (yet)
    fsb::FindHit m_hit{-1, false, 0, 0}; // the match shown last, where Find and Replace next go on
    QPointer<TabPanel> m_hitTab;
};

// draws the match, its item's Qt::UserRole QPoint(start, end), over the text (Kainote's results)
class QAbstractItemDelegate *matchDelegate(QObject *parent);

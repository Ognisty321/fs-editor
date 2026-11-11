// Aegisub's spell checker window.
#pragma once

#include <QDialog>
#include <QPointer>
#include <QSet>

class TabPanel;
class MainWindow;
class QLineEdit;
class QCheckBox;
class QListWidget;
class QPushButton;

// Aegisub's spell checker: walks the misspelled words from the active line on.
class SpellCheckDialog : public QDialog {
    Q_OBJECT
public:
    SpellCheckDialog(MainWindow *main, TabPanel *tab);
    bool findNext(); // false when the whole script was checked

private:
    void showWord(int row, int start, const QString &word);
    void replace(bool all);
    MainWindow *m_main;
    QPointer<TabPanel> m_tab;
    QLineEdit *m_word, *m_with;
    QListWidget *m_suggest;
    QPushButton *m_remove;
    QCheckBox *m_skipComments, *m_skipUpper;
    QSet<QString> m_ignored;
    int m_row = 0, m_pos = 0, m_start = 0, m_startRow = 0;
    bool m_wrapped = false;
};

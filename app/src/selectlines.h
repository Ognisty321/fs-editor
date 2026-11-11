// Aegisub's Select Lines.
#pragma once

#include <QDialog>

class TabPanel;
class MainWindow;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QButtonGroup;

class SelectLinesDialog : public QDialog {
    Q_OBJECT
public:
    explicit SelectLinesDialog(MainWindow *main);
private:
    void apply(const QList<TabPanel *> &tabs);
    size_t applyTo(TabPanel *tab); // the lines selected
    void chooseStyles();           // Kainote's "+": the styles picked, as a regex for the Styles field
    MainWindow *m_main;
    QComboBox *m_find;
    QRadioButton *m_with, *m_without;
    QCheckBox *m_case, *m_regex, *m_exact, *m_dialogue, *m_comments;
    QButtonGroup *m_mode; // 0 set, 1 add, 2 subtract, 3 intersect (Aegisub)
    QComboBox *m_field, *m_action;
};

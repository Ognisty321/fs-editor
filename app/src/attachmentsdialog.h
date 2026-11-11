// Aegisub's attachment manager.
#pragma once

#include <QDialog>

class TabPanel;
class QTreeWidget;

// Aegisub's attachment manager: fonts and graphics embedded in the script.
class AttachmentsDialog : public QDialog {
    Q_OBJECT
public:
    AttachmentsDialog(QWidget *parent, TabPanel *tab);

private:
    void refresh();
    TabPanel *m_tab;
    QTreeWidget *m_list;
};

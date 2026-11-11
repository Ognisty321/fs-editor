// The undo history of a tab and the names its steps are shown under.
#pragma once

#include <QDialog>

class TabPanel;

// An undo step's description as the user reads it: the core's steps translated, a macro's name as it is
QString undoStepName(const QString &description);

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistoryDialog(TabPanel *tab);
};

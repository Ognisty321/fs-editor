// The fonts collector: check, copy, pack into a zip or attach.
#pragma once

#include <QDialog>

class TabPanel;

class FontCollectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit FontCollectorDialog(TabPanel *tab);
};

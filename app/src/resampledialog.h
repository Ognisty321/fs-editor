// Aegisub's Resample Resolution.
#pragma once

#include <QDialog>

class TabPanel;

class ResampleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ResampleDialog(TabPanel *tab);
};

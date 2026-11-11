// Aegisub's "Open autosave file".
#pragma once

#include <QDialog>

class QListWidget;

// Aegisub's "Open autosave file": files on the left, their versions (newest first) on the right.
class AutosaveDialog : public QDialog {
    Q_OBJECT
public:
    explicit AutosaveDialog(QWidget *parent);
    QString chosen() const;

private:
    QListWidget *m_files, *m_versions;
};

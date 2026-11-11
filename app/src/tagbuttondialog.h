// The edit box's tag buttons: the window that sets one up.
#pragma once

#include <QDialog>

class QLineEdit;
class QComboBox;

class TagButtonDialog : public QDialog {
    Q_OBJECT
public:
    enum { Delete = 2 };
    TagButtonDialog(const QString &name, const QString &tag, int mode, QWidget *parent);
    QString name() const;
    QString tag() const;
    int mode() const;
private:
    QLineEdit *m_name, *m_tag;
    QComboBox *m_mode;
};

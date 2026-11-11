// The script's properties ([Script Info]).
#pragma once

#include <QDialog>
#include <vector>

class TabPanel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;

class PropertiesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PropertiesDialog(TabPanel *tab);
    void accept() override;
private:
    TabPanel *m_tab;
    std::vector<std::pair<QString, QLineEdit *>> m_fields;
    QSpinBox *m_w, *m_h, *m_lw, *m_lh;
    QComboBox *m_matrix, *m_wrap, *m_collisions;
    QCheckBox *m_scaled;
};

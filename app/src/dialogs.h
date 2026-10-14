// Dialogs: find/replace, select lines, style manager and editor, ASS
// properties, history, resample, paste translation, tag buttons, settings.
#pragma once

#include "bridge.h"

#include <QDialog>

class TabPanel;
class MainWindow;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QListWidget;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QTabWidget;

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

class FindDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindDialog(MainWindow *main);
    void showTab(int tab); // 0 find, 1 replace
    void findNext();
private:
    fsb::FindSpec spec() const;
    MainWindow *m_main;
    QTabWidget *m_tabs;
    QComboBox *m_find, *m_replace;
    QCheckBox *m_case, *m_regex, *m_comments, *m_skipTags;
    QRadioButton *m_field[4], *m_lines[3];
};

class SelectLinesDialog : public QDialog {
    Q_OBJECT
public:
    explicit SelectLinesDialog(MainWindow *main);
private:
    void apply();
    MainWindow *m_main;
    QLineEdit *m_find;
    QRadioButton *m_with, *m_without, *m_add, *m_deselect, *m_dialogue, *m_comments;
    QCheckBox *m_case, *m_regex;
    QComboBox *m_field, *m_action;
};

class StyleEditor : public QDialog {
    Q_OBJECT
public:
    StyleEditor(const fsb::StyleData &style, QWidget *parent);
    fsb::StyleData style() const;
private:
    void preview();
    fsb::StyleData m_style;
    QLineEdit *m_name;
    QComboBox *m_font;
    QDoubleSpinBox *m_size, *m_outline, *m_shadow, *m_scaleX, *m_scaleY, *m_angle, *m_spacing;
    QCheckBox *m_bold, *m_italic, *m_underline, *m_strike, *m_box;
    QString m_colors[4];
    class QToolButton *m_colorButtons[4];
    QSpinBox *m_alpha[4], *m_ml, *m_mr, *m_mv, *m_encoding;
    QRadioButton *m_align[9];
    QLabel *m_preview;
};

class StyleManager : public QDialog {
    Q_OBJECT
public:
    explicit StyleManager(MainWindow *main);
    void editStyle(const QString &name);
private:
    void reload();
    void loadCatalog();
    void saveCatalog();
    MainWindow *m_main;
    QComboBox *m_catalog;
    QListWidget *m_storage, *m_file;
    std::vector<fsb::StyleData> m_stored;
};

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

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistoryDialog(TabPanel *tab);
};

class ResampleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ResampleDialog(TabPanel *tab);
};

class FontCollectorDialog : public QDialog {
    Q_OBJECT
public:
    explicit FontCollectorDialog(TabPanel *tab);
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);
};

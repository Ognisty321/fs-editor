// The style editor and Kainote's font catalogs.
#pragma once

#include "bridge.h"

#include <QDialog>
#include <functional>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QRadioButton;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;

class StyleEditor : public QDialog {
    Q_OBJECT
public:
    StyleEditor(const fsb::StyleData &style, QWidget *parent);
    fsb::StyleData style() const;
    // Apply / OK hand the style here; false (e.g. a name clash) keeps the editor open
    std::function<bool(const fsb::StyleData &)> onApply;
    void accept() override;
    // Kainote's font catalogs window, `font` shown in the first catalog holding it
    static void manageFontCatalogs(QWidget *parent, const QString &font);
protected:
    void resizeEvent(QResizeEvent *e) override;
private:
    static QString askCatalogName(QWidget *parent, const QString &old); // empty: none or not allowed
    void preview();
    QLineEdit *m_previewText;
    QComboBox *m_encodingBox, *m_border;
    fsb::StyleData m_style;
    QLineEdit *m_name;
    QComboBox *m_font;
    QDoubleSpinBox *m_size, *m_outline, *m_shadow, *m_scaleX, *m_scaleY, *m_angle, *m_spacing;
    QCheckBox *m_bold, *m_italic, *m_underline, *m_strike;
    QString m_colors[4];
    class QToolButton *m_colorButtons[4];
    QSpinBox *m_alpha[4], *m_ml, *m_mr, *m_mv;
    QRadioButton *m_align[9];
    QLabel *m_preview = nullptr;
    QColor m_previewBg; // invalid: the checkerboard
};

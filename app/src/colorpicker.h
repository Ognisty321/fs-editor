// Aegisub's colour picker (DialogColorPicker), made anew for Qt: a spectrum and a slider in five
// modes, RGB, HSL and HSV fields, the colour as ASS and HTML text, the alpha, recent colours and a
// screen dropper. Every change is reported as it is made, so the caller can preview it.
#pragma once

#include <QColor>
#include <QDialog>
#include <array>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class ColorPad;
class ScreenDropper;

class ColorPicker : public QDialog {
    Q_OBJECT
public:
    // Modal. live gets every change (Aegisub previews it on the line); returns the colour taken, or
    // an invalid one on Cancel. Without alpha there are no alpha controls and the alpha comes back
    // as it went in.
    static QColor get(QWidget *parent, const QColor &initial, bool alpha,
                      const std::function<void(const QColor &)> &live = {});
    // Kainote's colour sampler: a small window that only takes colours from the screen, told to
    // live as they are taken; the alpha stays as it was
    static QColor sample(QWidget *parent, const QColor &initial, const std::function<void(const QColor &)> &live);

private:
    ColorPicker(const QColor &initial, bool alpha, QWidget *parent);
    // the field the user typed in keeps its text; HSL and HSV keep their values when they are the
    // source, so a hue survives a grey
    enum Source { None, Rgb, Hsl, Hsv, Ass, Html };
    QColor color() const;
    void set(const QColor &rgb, Source from);
    void pickSpectrum(int x, int y);
    void pickSlider(int y);
    void refresh(); // the spectrum, the sliders and the preview; tells live
    void remember() const; // this colour first in the recent ones

    QColor m_initial;
    int m_r = 0, m_g = 0, m_b = 0, m_a = 0; // m_a: the ASS alpha, 0 opaque
    ColorPad *m_spectrum, *m_slider, *m_alphaSlider = nullptr;
    ScreenDropper *m_dropper;
    QComboBox *m_mode;
    std::array<QSpinBox *, 3> m_rgb{}, m_hsl{}, m_hsv{};
    QSpinBox *m_alpha = nullptr;
    QLineEdit *m_ass, *m_html;
    QLabel *m_preview;
    std::function<void(const QColor &)> m_live;
};

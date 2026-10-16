#include "colorpicker.h"

#include "icons.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kRecentCols = 8, kRecent = 32, kCell = 16; // Aegisub's 8 x 4 recent colours
constexpr int kDrop = 7, kZoom = 8;                      // the dropper: 7 x 7 screen pixels, each drawn 8 x 8
constexpr int kBar = 10;                                 // a slider's width

// the recent colours, Aegisub's eight to begin with: black, red, yellow, green, cyan, blue, magenta, white
QStringList recentColors() {
    return QSettings()
        .value("colorPicker/recent",
               QStringList{"#000000", "#ff0000", "#ffff00", "#00ff00", "#00ffff", "#0000ff", "#ff00ff", "#ffffff"})
        .toStringList();
}

// Aegisub's colour spaces: every channel 0..255, the hue too
struct Hsx {
    int h, s, x;
};
float hueF(int h) { return float(std::fmod(h / 255.0, 1.0)); }
QColor fromHsl(Hsx c) { return QColor::fromHslF(hueF(c.h), c.s / 255.f, c.x / 255.f); }
QColor fromHsv(Hsx c) { return QColor::fromHsvF(hueF(c.h), c.s / 255.f, c.x / 255.f); }
int hue255(float f) { return f < 0 ? 0 : qRound(f * 255); } // a grey has no hue (-1)
Hsx toHsl(const QColor &c) { return {hue255(c.hslHueF()), qRound(c.hslSaturationF() * 255), qRound(c.lightnessF() * 255)}; }
Hsx toHsv(const QColor &c) { return {hue255(c.hsvHueF()), qRound(c.hsvSaturationF() * 255), qRound(c.valueF() * 255)}; }

void put(const std::array<QSpinBox *, 3> &spins, Hsx v) {
    const int vals[] = {v.h, v.s, v.x};
    for (int k = 0; k < 3; ++k) {
        const QSignalBlocker quiet(spins[k]);
        spins[k]->setValue(vals[k]);
    }
}
Hsx values(const std::array<QSpinBox *, 3> &spins) { return {spins[0]->value(), spins[1]->value(), spins[2]->value()}; }

// Aegisub's checkerboard under transparent colours
void checker(QPainter &p, const QRect &r) {
    p.fillRect(r, QColor(0xff, 0xff, 0xff));
    for (int y = r.top(); y <= r.bottom(); y += 8)
        for (int x = r.left() + ((y - r.top()) / 8 % 2) * 8; x <= r.right(); x += 16)
            p.fillRect(QRect(x, y, 8, 8).intersected(r), QColor(0xcc, 0xcc, 0xcc));
}
} // namespace

// A picture the mouse picks a point of: the 256 x 256 spectrum, or a slider 256 tall. Its mark is
// drawn inverted over the picture, as Aegisub draws it.
class ColorPad : public QWidget {
public:
    ColorPad(bool slider, QWidget *parent) : QWidget(parent), m_slider(slider) {
        setFixedSize(slider ? kBar + 2 : 258, 258);
        setCursor(Qt::CrossCursor);
    }
    void set(QImage img, int x, int y) {
        m_img = std::move(img);
        m_x = x;
        m_y = y;
        update();
    }
    std::function<void(int x, int y)> picked;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        const QRect in = rect().adjusted(1, 1, -1, -1);
        p.drawImage(in, m_img);
        p.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
        p.setPen(Qt::white);
        p.drawLine(in.left(), in.top() + m_y, in.right(), in.top() + m_y);
        if (!m_slider) p.drawLine(in.left() + m_x, in.top(), in.left() + m_x, in.bottom());
    }
    void mousePressEvent(QMouseEvent *e) override { pick(e); }
    void mouseMoveEvent(QMouseEvent *e) override { pick(e); }

private:
    void pick(QMouseEvent *e) {
        if (!(e->buttons() & Qt::LeftButton) || !picked) return;
        const QPoint p = e->position().toPoint() - QPoint(1, 1);
        picked(m_slider ? 0 : std::clamp(p.x(), 0, 255), std::clamp(p.y(), 0, 255));
    }
    bool m_slider;
    QImage m_img;
    int m_x = 0, m_y = 0;
};

// Aegisub's screen dropper: the screen around a point, 7 x 7 pixels magnified; a click on one of
// them takes its colour.
class ScreenDropper : public QWidget {
public:
    explicit ScreenDropper(QWidget *parent) : QWidget(parent), m_img(kDrop, kDrop, QImage::Format_RGB32) {
        setFixedSize(kDrop * kZoom + 2, kDrop * kZoom + 2);
        setCursor(Qt::CrossCursor);
        m_img.fill(Qt::black);
    }
    // copies the screen's pixels around the global point; returns the one under it
    QColor grab(QPoint global) {
        QScreen *s = QGuiApplication::screenAt(global);
        if (!s) return {};
        const QPoint at = global - s->geometry().topLeft();
        // device pixels: on a scaled screen a logical one is more than one, so a margin is grabbed
        const QImage shot = s->grabWindow(0, at.x() - kDrop, at.y() - kDrop, 2 * kDrop + 1, 2 * kDrop + 1).toImage();
        if (shot.isNull()) return {};
        const int c = int(kDrop * shot.devicePixelRatio());
        m_img = shot.copy(c - kDrop / 2, c - kDrop / 2, kDrop, kDrop).convertToFormat(QImage::Format_RGB32);
        update();
        return m_img.pixelColor(kDrop / 2, kDrop / 2);
    }
    std::function<void(const QColor &)> picked;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        p.drawImage(rect().adjusted(1, 1, -1, -1), m_img); // no smoothing: the pixels as squares
        // the middle pixel, the one a drop takes
        p.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
        p.setPen(Qt::white);
        p.drawRect(1 + kDrop / 2 * kZoom, 1 + kDrop / 2 * kZoom, kZoom - 1, kZoom - 1);
    }
    void mousePressEvent(QMouseEvent *e) override {
        const QPoint p = e->position().toPoint() - QPoint(1, 1);
        if (picked && QRect(0, 0, kDrop * kZoom, kDrop * kZoom).contains(p))
            picked(m_img.pixelColor(p.x() / kZoom, p.y() / kZoom)); // QPoint / int would round
    }

private:
    QImage m_img;
};

namespace {
// Aegisub's eyedropper: dragged over the screen, or clicked and then clicked where to take from;
// the dropper follows the mouse and the pixel under it is taken where the mouse lets go
class DropperButton : public QToolButton {
public:
    explicit DropperButton(QWidget *parent) : QToolButton(parent) {
        setIcon(::icon("color-picker"));
        setAutoRaise(true);
        setMouseTracking(true); // the moves while it holds the mouse after a click
    }
    std::function<void(QPoint global, bool done)> at;

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (m_grabbed) return finish(e->globalPosition().toPoint());
        m_pressed = true;
        m_from = e->globalPosition().toPoint();
        setCursor(Qt::CrossCursor);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (m_grabbed || m_pressed) at(e->globalPosition().toPoint(), false);
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!m_pressed) return; // the release of the click that ended a grab
        m_pressed = false;
        const QPoint p = e->globalPosition().toPoint();
        if ((p - m_from).manhattanLength() > 7) return finish(p);
        m_grabbed = true;
        grabMouse(Qt::CrossCursor);
    }

private:
    void finish(QPoint p) {
        if (m_grabbed) releaseMouse();
        m_grabbed = false;
        unsetCursor();
        at(p, true);
    }
    QPoint m_from;
    bool m_pressed = false, m_grabbed = false;
};

// Aegisub's grid of recent colours; a click takes one
class RecentColors : public QWidget {
public:
    explicit RecentColors(QWidget *parent) : QWidget(parent) {
        const QStringList kept = recentColors();
        for (int k = 0; k < kRecent; ++k) {
            const QColor c(kept.value(k));
            m_colors[size_t(k)] = c.isValid() ? c : QColor(Qt::black);
        }
        setFixedSize(kRecentCols * kCell + 2, kRecent / kRecentCols * kCell + 2);
        setCursor(Qt::CrossCursor);
    }
    std::function<void(const QColor &)> picked;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        for (int k = 0; k < kRecent; ++k)
            p.fillRect(1 + k % kRecentCols * kCell, 1 + k / kRecentCols * kCell, kCell, kCell, m_colors[size_t(k)]);
    }
    void mousePressEvent(QMouseEvent *e) override {
        const QPoint p = e->position().toPoint() - QPoint(1, 1);
        const int col = p.x() / kCell, row = p.y() / kCell;
        if (picked && p.x() >= 0 && p.y() >= 0 && col < kRecentCols && row < kRecent / kRecentCols)
            picked(m_colors[size_t(row * kRecentCols + col)]);
    }

private:
    std::array<QColor, kRecent> m_colors;
};
} // namespace

ColorPicker::ColorPicker(const QColor &initial, bool alpha, QWidget *parent) : QDialog(parent), m_initial(initial) {
    setWindowTitle(tr("Select Color"));
    // Aegisub's layout: the mode and the preview over the spectrum, its slider and the alpha's
    auto *specBox = new QGroupBox(tr("Color spectrum"), this);
    auto *sg = new QGridLayout(specBox);
    m_mode = new QComboBox(specBox);
    m_mode->addItems({tr("RGB/R"), tr("RGB/G"), tr("RGB/B"), tr("HSL/L"), tr("HSV/H")});
    m_mode->setCurrentIndex(std::clamp(QSettings().value("colorPicker/mode", 4).toInt(), 0, 4));
    m_preview = new QLabel(specBox);
    m_preview->setFixedSize(40, 40);
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(tr("Spectrum mode:"), specBox));
    modeRow->addWidget(m_mode);
    modeRow->addStretch();
    modeRow->addWidget(m_preview);
    sg->addLayout(modeRow, 0, 0, 1, 3);
    m_spectrum = new ColorPad(false, specBox);
    m_slider = new ColorPad(true, specBox);
    sg->addWidget(m_spectrum, 1, 0);
    sg->addWidget(m_slider, 1, 1);
    if (alpha) {
        m_alphaSlider = new ColorPad(true, specBox);
        m_alphaSlider->setToolTip(tr("Alpha (transparency)"));
        sg->addWidget(m_alphaSlider, 1, 2);
    }
    sg->setRowStretch(2, 1);

    const auto form = [](QWidget *box, std::array<QSpinBox *, 3> &out, std::array<const char *, 3> names) {
        auto *f = new QFormLayout;
        for (int k = 0; k < 3; ++k) {
            out[k] = new QSpinBox(box);
            out[k]->setRange(0, 255);
            f->addRow(tr(names[k]), out[k]);
        }
        return f;
    };
    auto *rgbBox = new QGroupBox(tr("RGB color"), this);
    auto *rgbRow = new QHBoxLayout(rgbBox);
    rgbRow->addLayout(form(rgbBox, m_rgb, {QT_TR_NOOP("Red:"), QT_TR_NOOP("Green:"), QT_TR_NOOP("Blue:")}));
    auto *text = new QFormLayout;
    m_ass = new QLineEdit(rgbBox);
    m_html = new QLineEdit(rgbBox);
    m_ass->setMinimumWidth(m_ass->fontMetrics().horizontalAdvance("&H000000&") + 16);
    text->addRow("ASS:", m_ass);
    text->addRow("HTML:", m_html);
    if (alpha) {
        m_alpha = new QSpinBox(rgbBox);
        m_alpha->setRange(0, 255);
        m_alpha->setToolTip(tr("0 is opaque, 255 transparent"));
        text->addRow(tr("Alpha:"), m_alpha);
    }
    rgbRow->addLayout(text);
    auto *hslBox = new QGroupBox(tr("HSL color"), this);
    hslBox->setLayout(form(hslBox, m_hsl, {QT_TR_NOOP("Hue:"), QT_TR_NOOP("Sat.:"), QT_TR_NOOP("Lum.:")}));
    auto *hsvBox = new QGroupBox(tr("HSV color"), this);
    hsvBox->setLayout(form(hsvBox, m_hsv, {QT_TR_NOOP("Hue:"), QT_TR_NOOP("Sat.:"), QT_TR_NOOP("Value:")}));

    // the eyedropper, the screen dropper and the recent colours
    m_dropper = new ScreenDropper(this);
    m_dropper->setToolTip(tr("Click a pixel to take its color"));
    m_dropper->picked = [this](const QColor &c) { set(c, None); };
    auto *eyedropper = new DropperButton(this);
    eyedropper->setToolTip(tr("Drag over the screen, or click and then click where, to take a color"));
    eyedropper->at = [this](QPoint p, bool done) {
        if (const QColor c = m_dropper->grab(p); done && c.isValid()) set(c, None);
    };
    auto *recent = new RecentColors(this);
    recent->picked = [this](const QColor &c) { set(c, None); }; // Aegisub: the alpha stays as it is
    auto *pickRow = new QHBoxLayout;
    pickRow->addStretch();
    pickRow->addWidget(eyedropper);
    pickRow->addWidget(m_dropper);
    pickRow->addStretch();
    pickRow->addWidget(recent);
    pickRow->addStretch();

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *spaces = new QHBoxLayout;
    spaces->addWidget(hslBox);
    spaces->addWidget(hsvBox);
    auto *right = new QVBoxLayout;
    right->addWidget(rgbBox);
    right->addLayout(spaces);
    right->addStretch(1);
    right->addLayout(pickRow);
    right->addStretch(2);
    right->addWidget(bb);
    auto *top = new QHBoxLayout(this);
    top->addWidget(specBox, 1);
    top->addLayout(right);

    for (auto *s : m_rgb)
        connect(s, &QSpinBox::valueChanged, this,
                [this] { set(QColor(m_rgb[0]->value(), m_rgb[1]->value(), m_rgb[2]->value()), Rgb); });
    for (auto *s : m_hsl) connect(s, &QSpinBox::valueChanged, this, [this] { set(fromHsl(values(m_hsl)), Hsl); });
    for (auto *s : m_hsv) connect(s, &QSpinBox::valueChanged, this, [this] { set(fromHsv(values(m_hsv)), Hsv); });
    if (m_alpha) {
        connect(m_alpha, &QSpinBox::valueChanged, this, [this](int v) {
            m_a = v;
            refresh();
        });
        m_alphaSlider->picked = [this](int, int y) { m_alpha->setValue(y); };
    }
    connect(m_ass, &QLineEdit::textEdited, this, [this](const QString &t) {
        // &HBBGGRR&, or &HAABBGGRR& with the alpha, or just the digits
        QString h = t.trimmed();
        h.remove("&H", Qt::CaseInsensitive).remove('&');
        bool ok = false;
        const uint v = h.toUInt(&ok, 16);
        if (!ok || h.size() > 8) return;
        if (h.size() > 6 && m_alpha) {
            const QSignalBlocker quiet(m_alpha);
            m_alpha->setValue(m_a = int(v >> 24));
        }
        set(QColor(int(v & 0xff), int(v >> 8 & 0xff), int(v >> 16 & 0xff)), Ass);
    });
    connect(m_html, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (const QColor c = QColor::fromString(t.trimmed()); c.isValid()) set(c, Html);
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int k) {
        QSettings().setValue("colorPicker/mode", k);
        refresh();
    });
    m_spectrum->picked = [this](int x, int y) { pickSpectrum(x, y); };
    m_slider->picked = [this](int, int y) { pickSlider(y); };

    if (m_alpha) {
        const QSignalBlocker quiet(m_alpha);
        m_alpha->setValue(m_a = 255 - initial.alpha());
    }
    set(initial, None);
}

QColor ColorPicker::get(QWidget *parent, const QColor &initial, bool alpha,
                        const std::function<void(const QColor &)> &live) {
    ColorPicker d(initial.isValid() ? initial : QColor(Qt::white), alpha, parent);
    d.m_live = live; // after the start: only the user's changes are told
    // where it was last (Aegisub's PersistLocation), if that is still on a screen
    QSettings st;
    if (const QPoint p = st.value("colorPicker/pos").toPoint(); st.contains("colorPicker/pos") && QGuiApplication::screenAt(p))
        d.move(p);
    const bool ok = d.exec() == QDialog::Accepted;
    st.setValue("colorPicker/pos", d.pos());
    if (!ok) return {};
    d.remember();
    return d.color();
}

QColor ColorPicker::sample(QWidget *parent, const QColor &initial, const std::function<void(const QColor &)> &live) {
    QDialog d(parent);
    d.setWindowTitle(tr("Color sampler"));
    auto *v = new QVBoxLayout(&d);
    auto *hex = new QLineEdit(&d);
    hex->setReadOnly(true);
    auto *dropper = new ScreenDropper(&d);
    dropper->setToolTip(tr("Click a pixel to take its color"));
    auto *eyedropper = new DropperButton(&d);
    eyedropper->setToolTip(tr("Drag over the screen, or click and then click where, to take a color"));
    QColor color = initial.isValid() ? initial : QColor(Qt::white);
    const auto take = [&](const QColor &c, bool tell) {
        color.setRgb(c.red(), c.green(), c.blue(), color.alpha()); // the alpha stays as it was
        hex->setText(QString::asprintf("&H%02X%02X%02X&", color.blue(), color.green(), color.red()));
        QPalette pal = hex->palette();
        pal.setColor(QPalette::Base, QColor(color.rgb()));
        pal.setColor(QPalette::Text, color.lightness() > 127 ? Qt::black : Qt::white);
        hex->setPalette(pal);
        if (tell && live) live(color);
    };
    take(color, false);
    dropper->picked = [&](const QColor &c) { take(c, true); };
    eyedropper->at = [&](QPoint p, bool done) {
        if (const QColor c = dropper->grab(p); done && c.isValid()) take(c, true);
    };
    auto *row = new QHBoxLayout;
    row->addWidget(eyedropper);
    row->addWidget(dropper, 1, Qt::AlignCenter);
    v->addWidget(hex);
    v->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    // Kainote's: it opens where the mouse is, near the colour to take
    d.adjustSize();
    d.move(QCursor::pos() - QPoint(d.width() / 2, d.height() / 2));
    if (d.exec() != QDialog::Accepted) return {};
    return color;
}

QColor ColorPicker::color() const {
    QColor c(m_r, m_g, m_b);
    c.setAlpha(m_alpha ? 255 - m_a : m_initial.alpha());
    return c;
}

void ColorPicker::set(const QColor &rgb, Source from) {
    m_r = rgb.red();
    m_g = rgb.green();
    m_b = rgb.blue();
    if (from != Rgb) put(m_rgb, {m_r, m_g, m_b});
    if (from != Hsl) put(m_hsl, toHsl(rgb));
    if (from != Hsv) put(m_hsv, toHsv(rgb));
    if (from != Ass) m_ass->setText(QString::asprintf("&H%02X%02X%02X&", m_b, m_g, m_r));
    if (from != Html) m_html->setText(QString::asprintf("#%02X%02X%02X", m_r, m_g, m_b));
    refresh();
}

// Aegisub's planes: R mode blue across and green down, G blue and red, B green and red; HSL
// saturation across and hue down; HSV saturation across and value down. The slider is the third.
void ColorPicker::pickSpectrum(int x, int y) {
    switch (m_mode->currentIndex()) {
    case 0: set(QColor(m_r, y, x), None); break;
    case 1: set(QColor(y, m_g, x), None); break;
    case 2: set(QColor(y, x, m_b), None); break;
    case 3: {
        const Hsx v{y, x, m_hsl[2]->value()};
        put(m_hsl, v);
        set(fromHsl(v), Hsl);
        break;
    }
    default: {
        const Hsx v{m_hsv[0]->value(), x, y};
        put(m_hsv, v);
        set(fromHsv(v), Hsv);
    }
    }
}

void ColorPicker::pickSlider(int y) {
    switch (m_mode->currentIndex()) {
    case 0: set(QColor(y, m_g, m_b), None); break;
    case 1: set(QColor(m_r, y, m_b), None); break;
    case 2: set(QColor(m_r, m_g, y), None); break;
    case 3: {
        const Hsx v{m_hsl[0]->value(), m_hsl[1]->value(), y};
        put(m_hsl, v);
        set(fromHsl(v), Hsl);
        break;
    }
    default: {
        const Hsx v{y, m_hsv[1]->value(), m_hsv[2]->value()};
        put(m_hsv, v);
        set(fromHsv(v), Hsv);
    }
    }
}

void ColorPicker::refresh() {
    QImage spec(256, 256, QImage::Format_RGB32), bar(kBar, 256, QImage::Format_RGB32);
    const auto line = [](QImage &img, int y) { return reinterpret_cast<QRgb *>(img.scanLine(y)); };
    const int mode = m_mode->currentIndex();
    int sx, sy, by;
    if (mode < 3) {
        for (int y = 0; y < 256; ++y) {
            QRgb *l = line(spec, y);
            for (int x = 0; x < 256; ++x) l[x] = mode == 0 ? qRgb(m_r, y, x) : mode == 1 ? qRgb(y, m_g, x) : qRgb(y, x, m_b);
            // the slider as the colour would be: the two other channels as they are (Aegisub's are 0)
            std::fill_n(line(bar, y), kBar, mode == 0 ? qRgb(y, m_g, m_b) : mode == 1 ? qRgb(m_r, y, m_b) : qRgb(m_r, m_g, y));
        }
        sx = mode == 2 ? m_g : m_b;
        sy = mode == 0 ? m_g : m_r;
        by = mode == 0 ? m_r : mode == 1 ? m_g : m_b;
    } else if (mode == 3) {
        // at one lightness the saturation is a straight line from the grey to the full hue
        const Hsx v = values(m_hsl);
        for (int y = 0; y < 256; ++y) {
            const QColor full = fromHsl({y, 255, v.x});
            QRgb *l = line(spec, y);
            for (int x = 0; x < 256; ++x) {
                const auto mix = [&](int c) { return v.x + (c - v.x) * x / 255; };
                l[x] = qRgb(mix(full.red()), mix(full.green()), mix(full.blue()));
            }
            std::fill_n(line(bar, y), kBar, fromHsl({v.h, v.s, y}).rgb());
        }
        sx = v.s;
        sy = v.h;
        by = v.x;
    } else {
        // at one hue a channel is value * (1 - saturation * (1 - the pure hue's channel))
        const Hsx v = values(m_hsv);
        const QColor pure = fromHsv({v.h, 255, 255});
        for (int y = 0; y < 256; ++y) {
            QRgb *l = line(spec, y);
            for (int x = 0; x < 256; ++x) {
                const auto mix = [&](int c) { return y * (255 * 255 - x * (255 - c)) / (255 * 255); };
                l[x] = qRgb(mix(pure.red()), mix(pure.green()), mix(pure.blue()));
            }
            std::fill_n(line(bar, y), kBar, fromHsv({y, 255, 255}).rgb());
        }
        sx = v.s;
        sy = v.x;
        by = v.h;
    }
    m_spectrum->set(std::move(spec), sx, sy);
    m_slider->set(std::move(bar), 0, by);
    if (m_alphaSlider) {
        // the colour over Aegisub's darker checkerboard, opaque at the top
        QImage alpha(kBar, 256, QImage::Format_RGB32);
        for (int y = 0; y < 256; ++y) {
            QRgb *l = line(alpha, y);
            for (int x = 0; x < kBar; ++x) {
                const int bg = (x / 5 + y / 5) % 2 ? 0x99 : 0x66;
                const auto mix = [&](int c) { return (c * (255 - y) + bg * y) / 255; };
                l[x] = qRgb(mix(m_r), mix(m_g), mix(m_b));
            }
        }
        m_alphaSlider->set(std::move(alpha), 0, m_a);
    }
    QPixmap px(m_preview->size());
    {
        QPainter p(&px);
        checker(p, px.rect());
        p.fillRect(px.rect(), color());
    }
    m_preview->setPixmap(px);
    if (m_live) m_live(color());
}

void ColorPicker::remember() const {
    QSettings st;
    QStringList recent = recentColors();
    const QString name = QColor(m_r, m_g, m_b).name();
    recent.removeAll(name);
    recent.prepend(name);
    st.setValue("colorPicker/recent", recent.mid(0, kRecent));
}

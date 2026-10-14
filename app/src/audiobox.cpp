#include "audiobox.h"

#include "editbox.h"
#include "grid.h"
#include "icons.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPointer>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <thread>

namespace {
constexpr int RulerH = 20, KaraH = 18;
const QColor kBg(0x10, 0x14, 0x18), kSel(0x26, 0x34, 0x4e), kSelMod(0x45, 0x30, 0x48), kWave(0x4c, 0xb0, 0x60),
    kWaveAvg(0x9c, 0xe8, 0xa8), kStart(0xe0, 0x30, 0x30), kEnd(0xf0, 0x90, 0x20), kInactive(0x80, 0x80, 0x90),
    kMark(0xe0, 0x40, 0xe0), kKeyframe(0x40, 0xd0, 0x40), kCursor(0xff, 0xff, 0xff), kSyl(0xf0, 0xe0, 0x40);
} // namespace

// ---- view ---------------------------------------------------------------------

AudioView::AudioView(AudioBox *box) : QRhiWidget(box), m_box(box) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(80);
}

void AudioView::initialize(QRhiCommandBuffer *) {
    if (m_wave.ready()) return;
    auto *u = rhi()->nextResourceUpdateBatch();
    m_data.reset(rhi()->newTexture(QRhiTexture::RGBA32F, QSize(1, 1)));
    m_data->create();
    QImage blank(4, 4, QImage::Format_RGBA8888_Premultiplied);
    blank.fill(Qt::transparent);
    uploadImage(rhi(), m_overlayTex, blank, u);
    auto *rp = renderTarget()->renderPassDescriptor();
    m_wave.init(rhi(), rp, sampleCount(), ":/shaders/audio.frag.qsb", false, false, m_data.get(), u);
    m_overlay.init(rhi(), rp, sampleCount(), ":/shaders/texture.frag.qsb", true, false, m_overlayTex.get(), u);
    m_pendingInit = u;
    m_dataDirty = m_overlayDirty = true;
}

void AudioView::releaseResources() {
    m_wave.release();
    m_overlay.release();
    m_data.reset();
    m_overlayTex.reset();
    m_pendingInit = nullptr;
}

void AudioView::render(QRhiCommandBuffer *cb) {
    auto *u = m_pendingInit ? m_pendingInit : rhi()->nextResourceUpdateBatch();
    m_pendingInit = nullptr;
    const QSize out = renderTarget()->pixelSize();
    const double dpr = devicePixelRatioF();
    auto &b = *m_box;
    if (m_dataDirty && b.m_audio) {
        const int w = std::max(1, out.width());
        const double rate = (*b.m_audio)->rate();
        const double start = b.m_left * rate / 1000.0, spp = b.m_msPerPx / dpr * rate / 1000.0;
        std::vector<float> data;
        QSize sz;
        QRhiTexture::Format fmt;
        if (b.m_spectrum) {
            const int h = std::max(1, out.height());
            data.resize(size_t(w) * size_t(h));
            (*b.m_audio)->spectrum(start, spp, size_t(w), size_t(h), b.m_enhance, {data.data(), data.size()});
            sz = {w, h};
            fmt = QRhiTexture::R32F;
        } else {
            std::vector<float> peaks(size_t(w) * 3);
            (*b.m_audio)->peaks(start, spp, {peaks.data(), peaks.size()});
            data.resize(size_t(w) * 4);
            for (int x = 0; x < w; ++x) {
                data[x * 4] = peaks[x * 3];
                data[x * 4 + 1] = peaks[x * 3 + 1];
                data[x * 4 + 2] = peaks[x * 3 + 2];
            }
            sz = {w, 1};
            fmt = QRhiTexture::RGBA32F;
        }
        if (m_data->pixelSize() != sz || m_data->format() != fmt) {
            m_data.reset(rhi()->newTexture(fmt, sz));
            m_data->create();
            m_wave.setTexture(m_data.get());
        }
        QRhiTextureSubresourceUploadDescription sub(data.data(), quint32(data.size() * sizeof(float)));
        u->uploadTexture(m_data.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, sub)}));
        m_dataDirty = false;
    }
    if (m_overlayDirty) {
        QImage ov(out, QImage::Format_RGBA8888_Premultiplied);
        ov.fill(Qt::transparent);
        ov.setDevicePixelRatio(dpr);
        {
            QPainter p(&ov);
            paintOverlay(p);
        }
        if (uploadImage(rhi(), m_overlayTex, ov, u)) m_overlay.setTexture(m_overlayTex.get());
        m_overlayDirty = false;
    }
    RhiQuad::Uniforms wu;
    wu.p0[0] = b.m_spectrum ? 1 : 0;
    wu.p0[1] = b.m_spectrum ? 1.0f + (b.m_vzoom - 1) * 0.5f : b.m_vzoom;
    const double w = std::max(1, width());
    wu.p0[2] = float(b.xOf(b.m_selStart) / w);
    wu.p0[3] = float(b.xOf(b.m_selEnd) / w);
    RhiQuad::setColor(wu.c0, kBg);
    RhiQuad::setColor(wu.c1, b.m_selModified ? kSelMod : kSel);
    RhiQuad::setColor(wu.c2, kWave);
    RhiQuad::setColor(wu.c3, kWaveAvg);
    m_wave.setUniforms(u, wu, QRectF(QPointF(0, 0), QSizeF(out)), out);
    m_overlay.setUniforms(u, RhiQuad::Uniforms{}, QRectF(QPointF(0, 0), QSizeF(out)), out);
    cb->beginPass(renderTarget(), kBg, {1.0f, 0}, u);
    if (b.m_audio) m_wave.draw(cb, out);
    m_overlay.draw(cb, out);
    cb->endPass();
}

void AudioView::paintOverlay(QPainter &p) {
    auto &b = *m_box;
    const int w = width(), h = height();
    p.setFont(QFont(font().family(), 8));
    // ruler
    p.fillRect(0, 0, w, RulerH, QColor(0, 0, 0, 170));
    p.setPen(QColor(200, 200, 200));
    const qint64 l = b.m_left, r = b.msAt(w);
    qint64 step = 100;
    for (qint64 s : {100, 500, 1000, 5000, 10000, 30000, 60000})
        if (s / b.m_msPerPx >= 60) {
            step = s;
            break;
        }
    for (qint64 t = l / step * step; t <= r; t += step) {
        const int x = b.xOf(t);
        p.drawLine(x, RulerH - 6, x, RulerH);
        p.drawText(x + 2, RulerH - 7, fs::fmtTime(t).mid(2, 5));
    }
    if (!b.m_audio) {
        p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, tr("No audio"));
        return;
    }
    // keyframes
    auto *video = b.m_tab->video();
    p.setPen(QPen(kKeyframe, 1, Qt::DashLine));
    for (qint64 kf : b.m_tab->keyframes()) {
        const qint64 t = video->hasVideo() ? video->timeAt(kf) : -1;
        if (t < l || t > r) continue;
        p.drawLine(b.xOf(t), RulerH, b.xOf(t), h);
    }
    // previous and next lines' boundaries
    auto &s = b.m_tab->s();
    const int act = b.m_tab->active();
    p.setPen(QPen(kInactive, 1, Qt::DotLine));
    for (int d : {-1, 1}) {
        const int row = act + d;
        if (row < 0 || size_t(row) >= s.row_count()) continue;
        const auto rr = s.row(size_t(row));
        for (qint64 t : {rr.start, rr.end})
            if (t >= l && t <= r) p.drawLine(b.xOf(t), RulerH, b.xOf(t), h);
    }
    // karaoke
    if (b.m_karaoke && !b.m_syl.empty()) {
        p.fillRect(0, RulerH, w, KaraH, QColor(0, 0, 0, 120));
        for (size_t k = 0; k < b.m_syl.size(); ++k) {
            const auto &sy = b.m_syl[k];
            const int x0 = b.xOf(sy.start), x1 = b.xOf(sy.end);
            if (k > 0) {
                p.setPen(QPen(kSyl, 1));
                p.drawLine(x0, RulerH, x0, h);
            }
            p.setPen(kSyl);
            p.drawText(QRect(x0 + 2, RulerH, std::max(1, x1 - x0 - 2), KaraH), Qt::AlignVCenter | Qt::AlignLeft,
                       fs::q(sy.text));
            if (int(k) == b.m_activeSyl) {
                p.setPen(QPen(kStart, 1));
                p.drawRect(x0, RulerH, x1 - x0, h - RulerH - 1);
            }
        }
        if (m_hoverX >= 0) {
            p.setPen(QPen(kStart, 1));
            p.drawLine(m_hoverX, RulerH, m_hoverX, RulerH + KaraH);
        }
    }
    // active line
    p.setPen(QPen(kStart, 2));
    p.drawLine(b.xOf(b.m_selStart), RulerH, b.xOf(b.m_selStart), h);
    p.setPen(QPen(kEnd, 2));
    p.drawLine(b.xOf(b.m_selEnd), RulerH, b.xOf(b.m_selEnd), h);
    p.setPen(QColor(230, 230, 230));
    p.drawText(QRect(b.xOf(b.m_selStart) + 3, h - 16, 400, 14), Qt::AlignLeft,
               QString("%1 - %2 (%3 ms)").arg(fs::fmtTime(b.m_selStart), fs::fmtTime(b.m_selEnd))
                   .arg(b.m_selEnd - b.m_selStart));
    if (b.m_mark >= 0) {
        p.setPen(QPen(kMark, 1));
        p.drawLine(b.xOf(b.m_mark), 0, b.xOf(b.m_mark), h);
    }
    // play cursor or video position
    const qint64 cur = b.playing() ? b.positionMs() : video->hasVideo() ? video->time() : -1;
    if (cur >= l && cur <= r) {
        p.setPen(QPen(kCursor, 1));
        p.drawLine(b.xOf(cur), 0, b.xOf(cur), h);
    }
}

void AudioView::mousePressEvent(QMouseEvent *e) {
    auto &b = *m_box;
    setFocus();
    if (!b.m_audio) return;
    const int x = int(e->position().x()), y = int(e->position().y());
    const qint64 t = b.msAt(x);
    m_button = e->button();
    m_pressX = x;
    m_pressLeft = b.m_left;
    b.m_dragId++;
    if (y < RulerH) {
        if (e->button() == Qt::RightButton) {
            b.m_mark = t;
            overlayChanged();
        } else {
            m_drag = 3;
        }
        return;
    }
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))) {
        if (b.m_karaoke && e->button() == Qt::MiddleButton) {
            for (size_t k = 1; k < b.m_syl.size(); ++k)
                if (std::abs(b.xOf(b.m_syl[k].start) - x) < 5) {
                    b.m_tab->s().kara_join(size_t(b.m_tab->active()), k - 1);
                    b.m_tab->changed(TabPanel::FromAudio);
                    b.karaokeChanged();
                    return;
                }
            for (size_t k = 0; k < b.m_syl.size(); ++k)
                if (t >= b.m_syl[k].start && t < b.m_syl[k].end) {
                    b.play(b.m_syl[k].start, b.m_syl[k].end);
                    return;
                }
        }
        b.m_tab->seekVideo(t);
        return;
    }
    if (b.m_karaoke && !b.m_syl.empty()) {
        // boundaries between syllables
        for (size_t k = 1; k < b.m_syl.size(); ++k)
            if (std::abs(b.xOf(b.m_syl[k].start) - x) < 5) {
                m_drag = 10 + int(k);
                return;
            }
        // click on the syllable text splits it at the character under the mouse
        if (y < RulerH + KaraH && e->button() == Qt::LeftButton) {
            for (size_t k = 0; k < b.m_syl.size(); ++k) {
                const auto &sy = b.m_syl[k];
                const int x0 = b.xOf(sy.start) + 2;
                const QString text = fs::q(sy.text);
                const QFontMetrics fm(QFont(font().family(), 8));
                if (x < x0 || x > x0 + fm.horizontalAdvance(text)) continue;
                int pos = 0;
                while (pos < text.size() && x0 + fm.horizontalAdvance(text.left(pos + 1)) < x) ++pos;
                if (pos > 0 && pos < text.size()) {
                    b.m_tab->s().kara_split(size_t(b.m_tab->active()), k, size_t(pos));
                    b.m_tab->changed(TabPanel::FromAudio);
                    b.karaokeChanged();
                }
                return;
            }
        }
        for (size_t k = 0; k < b.m_syl.size(); ++k)
            if (t >= b.m_syl[k].start && t < b.m_syl[k].end) b.m_activeSyl = int(k);
    }
    const bool snapToggle = e->modifiers() & Qt::ShiftModifier;
    if (std::abs(b.xOf(b.m_selStart) - x) < 6) m_drag = 1;
    else if (std::abs(b.xOf(b.m_selEnd) - x) < 6) m_drag = 2;
    else if (e->button() == Qt::LeftButton) {
        m_drag = 1;
        b.setSelection(b.snap(t, snapToggle), std::max(b.m_selEnd, t), true);
    } else if (e->button() == Qt::RightButton) {
        m_drag = 2;
        b.setSelection(b.m_selStart, std::max(b.m_selStart, b.snap(t, snapToggle)), true);
    }
    overlayChanged();
}

void AudioView::mouseMoveEvent(QMouseEvent *e) {
    auto &b = *m_box;
    const int x = int(e->position().x());
    const qint64 t = b.msAt(x);
    if (!m_drag) {
        const bool kara = b.m_karaoke && e->position().y() >= RulerH && e->position().y() < RulerH + KaraH;
        m_hoverX = kara ? x : -1;
        const bool edge = std::abs(b.xOf(b.m_selStart) - x) < 6 || std::abs(b.xOf(b.m_selEnd) - x) < 6;
        setCursor(edge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        if (b.m_karaoke) overlayChanged();
        return;
    }
    const bool snapToggle = e->modifiers() & Qt::ShiftModifier;
    if (m_drag == 3) {
        b.setLeft(m_pressLeft - qint64((x - m_pressX) * b.m_msPerPx));
    } else if (m_drag == 1) {
        b.setSelection(std::min(b.snap(t, snapToggle), b.m_selEnd), b.m_selEnd, true);
    } else if (m_drag == 2) {
        b.setSelection(b.m_selStart, std::max(b.snap(t, snapToggle), b.m_selStart), true);
    } else if (m_drag >= 10) {
        b.m_tab->s().kara_set_boundary(size_t(b.m_tab->active()), size_t(m_drag - 10), t, m_button == Qt::RightButton);
        b.m_tab->changed(TabPanel::FromAudio);
        b.karaokeChanged();
    }
    overlayChanged();
}

void AudioView::mouseReleaseEvent(QMouseEvent *) {
    auto &b = *m_box;
    if ((m_drag == 1 || m_drag == 2) && b.m_autoCommit && b.m_selModified) b.commit();
    m_drag = 0;
}

void AudioView::leaveEvent(QEvent *) {
    m_hoverX = -1;
    overlayChanged();
}

void AudioView::wheelEvent(QWheelEvent *e) {
    auto &b = *m_box;
    const int d = e->angleDelta().y();
    bool zoom = e->modifiers() & Qt::ShiftModifier;
    if (QSettings().value("audio/wheelZooms", false).toBool()) zoom = !zoom;
    if (e->modifiers() & Qt::ControlModifier) {
        b.m_vzoomSlider->setValue(b.m_vzoomSlider->value() + d / 30);
    } else if (zoom) {
        b.zoom(d > 0 ? 1 / 1.25 : 1.25, int(e->position().x()));
    } else {
        b.setLeft(b.m_left - qint64(d / 120.0 * width() / 8 * b.m_msPerPx));
    }
}

void AudioView::keyPressEvent(QKeyEvent *e) { m_box->keyPressEvent(e); }

// ---- box ----------------------------------------------------------------------

AudioBox::AudioBox(TabPanel *tab) : QWidget(tab), m_tab(tab) {
    QSettings st;
    st.beginGroup("audio");
    m_karaoke = st.value("karaoke", false).toBool();
    m_autoSplit = st.value("autoSplit", false).toBool();
    m_autoCommit = st.value("autoCommit", true).toBool();
    m_nextOnCommit = st.value("nextOnCommit", false).toBool();
    m_autoScroll = st.value("autoScroll", true).toBool();
    m_spectrum = st.value("spectrum", false).toBool();
    m_enhance = st.value("enhance", false).toBool();

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto *top = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    m_view = new AudioView(this);
    left->addWidget(m_view, 1);
    m_scroll = new QScrollBar(Qt::Horizontal, this);
    m_scroll->setToolTip(tr("Search bar"));
    left->addWidget(m_scroll);
    top->addLayout(left, 1);
    auto slider = [this](const QString &tip, int lo, int hi, int val) {
        auto *s = new QSlider(Qt::Vertical, this);
        s->setRange(lo, hi);
        s->setValue(val);
        s->setToolTip(tip);
        return s;
    };
    m_hzoom = slider(tr("Horizontal stretching"), 0, 100, st.value("hzoom", 50).toInt());
    m_vzoomSlider = slider(tr("Vertical stretching"), 10, 400, st.value("vzoom", 100).toInt());
    m_volume = slider(tr("Volume"), 0, 100, st.value("volume", 80).toInt());
    auto *sliders = new QVBoxLayout;
    auto *srow = new QHBoxLayout;
    for (auto *s : {m_hzoom, m_vzoomSlider, m_volume}) srow->addWidget(s);
    sliders->addLayout(srow, 1);
    m_link = new QToolButton(this);
    m_link->setIcon(icon("link"));
    m_link->setText(tr("Link"));
    m_link->setCheckable(true);
    m_link->setChecked(st.value("link", false).toBool());
    m_link->setToolTip(tr("Link the volume and stretch sliders"));
    sliders->addWidget(m_link);
    top->addLayout(sliders);
    v->addLayout(top, 1);
    v->addWidget(buttonRow());
    st.endGroup();

    m_msPerPx = std::pow(1.07, 50 - m_hzoom->value()) * 10;
    m_vzoom = m_vzoomSlider->value() / 100.0f;
    connect(m_scroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_left = qint64(v) * 10;
        m_view->dataChanged();
    });
    connect(m_hzoom, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue("audio/hzoom", v);
        const double c = msAt(m_view->width() / 2);
        m_msPerPx = std::pow(1.07, 50 - v) * 10;
        setLeft(qint64(c - m_view->width() / 2 * m_msPerPx));
    });
    connect(m_vzoomSlider, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue("audio/vzoom", v);
        m_vzoom = v / 100.0f;
        if (m_link->isChecked()) m_volume->setValue(std::clamp(v / 4, 0, 100));
        m_view->overlayChanged();
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue("audio/volume", v);
        setVolume(v / 100.0f);
    });
    connect(m_link, &QToolButton::toggled, this, [](bool on) { QSettings().setValue("audio/link", on); });
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &AudioBox::tick);
}

AudioBox::~AudioBox() { stop(); }

QWidget *AudioBox::buttonRow() {
    auto *row = new QWidget(this);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(1);
    auto btn = [&](const QString &text, const QString &tip, auto f, bool gap = false) {
        if (gap) h->addSpacing(8);
        auto *b = new QToolButton(row);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, f);
        h->addWidget(b);
        return b;
    };
    auto toggle = [&](const QString &text, const QString &tip, bool *flag, const char *key, auto after) {
        auto *b = new QToolButton(row);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setChecked(*flag);
        b->setAutoRaise(true);
        connect(b, &QToolButton::toggled, this, [this, flag, key, after](bool on) {
            *flag = on;
            QSettings().setValue(QString("audio/") + key, on);
            after();
        });
        h->addWidget(b);
        return b;
    };
    // icon-only buttons; the text stays as the accessible name
    const QColor teal(0x36c5d0), green(0x4cc38a);
    btn("◀▶", tr("Play the previous line"), [this] { stepLine(-1); })->setIcon(icon("player-track-prev-filled", teal));
    btn("▶◀", tr("Play the next line"), [this] { stepLine(1); })->setIcon(icon("player-track-next-filled", teal));
    btn("▷", tr("Play the current syllable / line"), [this] {
        if (m_karaoke && size_t(m_activeSyl) < m_syl.size()) play(m_syl[m_activeSyl].start, m_syl[m_activeSyl].end);
        else playSelection();
    })->setIcon(icon("player-play", green));
    btn("▶", tr("Play the current line"), [this] { playSelection(); })->setIcon(icon("player-play-filled", green));
    btn("■", tr("Stop playback"), [this] { stop(); })->setIcon(icon("player-stop-filled", teal));
    btn("|◀", tr("Play before the mark"), [this] { if (m_mark >= 0) play(std::max<qint64>(0, m_mark - 1000), m_mark); }, true)->setIcon(icon("arrow-bar-left", teal));
    btn("▶|", tr("Play after the mark"), [this] { if (m_mark >= 0) play(m_mark, m_mark + 1000); })->setIcon(icon("arrow-bar-right", teal));
    btn("◁s", tr("Play 500 ms before the start time"), [this] { play(std::max<qint64>(0, m_selStart - 500), m_selStart); }, true);
    btn("s▷", tr("Play 500 ms after the start time"), [this] { play(m_selStart, std::min(m_selEnd, m_selStart + 500)); });
    btn("◁e", tr("Play 500 ms before the end time"), [this] { play(std::max(m_selStart, m_selEnd - 500), m_selEnd); });
    btn("e▷", tr("Play 500 ms after the end time"), [this] { play(m_selEnd, m_selEnd + 500); });
    btn("▶▶", tr("Play to the end"), [this] { play(m_selStart, m_duration); })->setIcon(icon("player-skip-forward", teal));
    btn("+in", tr("Add lead-in to the active line"), [this] {
        setSelection(std::max<qint64>(0, m_selStart - QSettings().value("audio/leadIn", 200).toInt()), m_selEnd, false);
        commit();
    }, true)->setIcon(icon("arrow-autofit-left", green));
    btn("+out", tr("Add lead-out to the active line"), [this] {
        setSelection(m_selStart, m_selEnd + QSettings().value("audio/leadOut", 300).toInt(), false);
        commit();
    })->setIcon(icon("arrow-autofit-right", green));
    btn("✓", tr("Apply changes (commit)"), [this] { commit(); }, true)->setIcon(icon("check", green));
    btn("⌖", tr("Go to selection"), [this] { ensureVisible(m_selStart, m_selEnd); })->setIcon(icon("focus-centered", teal));
    h->addSpacing(8);
    toggle("K", tr("Enable / disable karaoke creation"), &m_karaoke, "karaoke", [this] { karaokeChanged(); })->setIcon(icon("microphone", teal));
    toggle("Ks", tr("Enable / disable automatic splitting of syllables"), &m_autoSplit, "autoSplit", [] {})->setIcon(icon("wave-saw-tool", teal));
    toggle("AC", tr("Automatically apply changes"), &m_autoCommit, "autoCommit", [] {});
    toggle("NL", tr("Go to the next line after applying changes"), &m_nextOnCommit, "nextOnCommit", [] {})->setIcon(icon("arrow-down-bar", teal));
    toggle("AS", tr("Automatic scrolling to the active line"), &m_autoScroll, "autoScroll", [] {});
    toggle("Sp", tr("Spectrum mode"), &m_spectrum, "spectrum", [this] { m_view->dataChanged(); })->setIcon(icon("chart-histogram", teal));
    toggle("En", tr("Enhance speech frequencies in the spectrum"), &m_enhance, "enhance", [this] { m_view->dataChanged(); })->setIcon(icon("music-bolt", teal));
    h->addStretch();
    return row;
}

void AudioBox::open(const QString &path) {
    close();
    m_tab->main()->status(tr("Loading audio %1…").arg(path));
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex";
    QPointer<AudioBox> self(this);
    std::thread([self, path, cache] {
        std::optional<rust::Box<fsb::Audio>> a;
        QString err;
        try {
            a.emplace(fsb::open_audio(fs::Utf8(path), fs::Utf8(cache)));
        } catch (const rust::Error &e) {
            err = QString::fromUtf8(e.what());
        }
        auto holder = std::make_shared<std::optional<rust::Box<fsb::Audio>>>(std::move(a));
        QMetaObject::invokeMethod(qApp, [self, holder, err, path] {
            if (!self) return;
            if (!err.isEmpty()) {
                self->m_tab->main()->error(err);
                return;
            }
            self->m_audio = std::move(*holder);
            self->m_path = path;
            self->loaded();
        });
    }).detach();
}

void AudioBox::openBlank(qint64 ms) {
    close();
    m_audio.emplace(fsb::blank_audio(ms));
    m_path.clear();
    loaded();
}

void AudioBox::loaded() {
    m_duration = (*m_audio)->duration_ms();
    setVolume(m_volume->value() / 100.0f);
    updateScroll();
    lineChanged();
    m_tab->main()->status(QString());
    emit opened();
}

void AudioBox::close() {
    stop();
    m_audio.reset();
    m_path.clear();
    m_duration = 0;
    m_view->dataChanged();
    emit opened();
}

void AudioBox::play(qint64 from, qint64 to) {
    if (!m_audio || to <= from) return;
    try {
        (*m_audio)->play(from, to);
    } catch (const rust::Error &e) {
        m_tab->main()->status(QString::fromUtf8(e.what()));
        return;
    }
    m_playEnd = to;
    m_timer->start();
}

void AudioBox::stop() {
    if (m_audio) (*m_audio)->stop();
    m_timer->stop();
    m_view->overlayChanged();
}

bool AudioBox::playing() const { return m_audio && (*m_audio)->playing(); }
qint64 AudioBox::positionMs() const { return m_audio ? (*m_audio)->position_ms() : -1; }
void AudioBox::setVolume(float v) {
    if (m_audio) (*m_audio)->set_volume(v);
}

void AudioBox::tick() {
    if (!playing()) m_timer->stop();
    const qint64 p = positionMs();
    if (m_autoScroll && p > msAt(m_view->width())) setLeft(p - qint64(m_view->width() * m_msPerPx / 8));
    m_view->overlayChanged();
}

void AudioBox::playSelection() { play(m_selStart, m_selEnd); }

qint64 AudioBox::snap(qint64 ms, bool toggle) const {
    QSettings st;
    const bool kf = st.value("audio/snapKeyframes", false).toBool() != toggle;
    const bool lines = st.value("audio/snapLines", false).toBool() != toggle;
    const qint64 range = qint64(8 * m_msPerPx);
    qint64 best = ms, bestD = range + 1;
    auto consider = [&](qint64 t) {
        if (std::abs(t - ms) < bestD) {
            bestD = std::abs(t - ms);
            best = t;
        }
    };
    if (kf && m_tab->video()->hasVideo())
        for (qint64 f : m_tab->keyframes()) consider(m_tab->video()->timeAt(f));
    if (lines) {
        auto &s = m_tab->s();
        for (int d : {-1, 1}) {
            const int row = m_tab->active() + d;
            if (row < 0 || size_t(row) >= s.row_count()) continue;
            const auto r = s.row(size_t(row));
            consider(r.start);
            consider(r.end);
        }
    }
    return bestD <= range ? best : ms;
}

void AudioBox::setLeft(qint64 ms) {
    const qint64 maxLeft = std::max<qint64>(0, m_duration - qint64(m_view->width() * m_msPerPx / 2));
    m_left = std::clamp<qint64>(ms, 0, maxLeft);
    m_scroll->blockSignals(true);
    updateScroll();
    m_scroll->setValue(int(m_left / 10));
    m_scroll->blockSignals(false);
    m_view->dataChanged();
}

void AudioBox::zoom(double f, int x) {
    const qint64 at = msAt(x);
    const int v = m_hzoom->value() + (f < 1 ? 3 : -3);
    m_hzoom->blockSignals(true);
    m_hzoom->setValue(v);
    m_hzoom->blockSignals(false);
    m_msPerPx = std::pow(1.07, 50 - m_hzoom->value()) * 10;
    setLeft(at - qint64(x * m_msPerPx));
}

void AudioBox::updateScroll() {
    m_scroll->setRange(0, int(std::max<qint64>(0, m_duration / 10)));
    m_scroll->setPageStep(int(m_view->width() * m_msPerPx / 10));
}

void AudioBox::centerOn(qint64 ms) {
    setLeft(ms - qint64(m_view->width() * m_msPerPx / 2));
    m_view->overlayChanged();
}

void AudioBox::setMark(qint64 ms) {
    m_mark = ms;
    ensureVisible(ms, ms);
    m_view->overlayChanged();
}

void AudioBox::ensureVisible(qint64 a, qint64 b) {
    const qint64 span = qint64(m_view->width() * m_msPerPx);
    if (a < m_left || b > m_left + span) setLeft(a - (span - std::min(span, b - a)) / 2);
}

void AudioBox::setSelection(qint64 start, qint64 end, bool fromDrag) {
    m_selStart = std::max<qint64>(0, start);
    m_selEnd = std::max(m_selStart, end);
    m_selModified = true;
    if (!fromDrag && m_autoCommit) commit();
    m_view->overlayChanged();
}

void AudioBox::commit() {
    const int row = m_tab->active();
    if (row < 0 || size_t(row) >= m_tab->s().row_count()) return;
    auto r = m_tab->s().row(size_t(row));
    if (r.start != m_selStart || r.end != m_selEnd) {
        r.start = m_selStart;
        r.end = m_selEnd;
        m_tab->s().set_row(size_t(row), r, fs::Utf8(QString("audio-%1").arg(m_dragId)));
        m_tab->changed(TabPanel::FromAudio);
    }
    m_selModified = false;
    m_view->overlayChanged();
    if (m_nextOnCommit) stepLine(1);
}

void AudioBox::lineChanged() {
    const int row = m_tab->active();
    if (row >= 0 && size_t(row) < m_tab->s().row_count()) {
        const auto r = m_tab->s().row(size_t(row));
        m_selStart = r.start;
        m_selEnd = r.end;
        m_selModified = false;
        if (m_autoScroll && m_audio) ensureVisible(m_selStart, m_selEnd);
    }
    m_activeSyl = 0;
    karaokeChanged();
}

void AudioBox::docChanged() {
    if (!m_selModified) {
        const int row = m_tab->active();
        if (row >= 0 && size_t(row) < m_tab->s().row_count()) {
            const auto r = m_tab->s().row(size_t(row));
            m_selStart = r.start;
            m_selEnd = r.end;
        }
    }
    if (m_karaoke && size_t(std::max(0, m_tab->active())) < m_tab->s().row_count()) {
        const auto syl = m_tab->s().syllables(size_t(std::max(0, m_tab->active())));
        m_syl.assign(syl.begin(), syl.end());
    }
    m_view->overlayChanged();
}

void AudioBox::karaokeChanged() {
    m_syl.clear();
    const int row = m_tab->active();
    if (m_karaoke && row >= 0 && size_t(row) < m_tab->s().row_count()) {
        const auto syl = m_tab->s().syllables(size_t(row));
        m_syl.assign(syl.begin(), syl.end());
        if (m_syl.size() <= 1 && m_autoSplit) {
            autoSplit();
            const auto again = m_tab->s().syllables(size_t(row));
            m_syl.assign(again.begin(), again.end());
        }
    }
    m_activeSyl = std::clamp(m_activeSyl, 0, std::max(0, int(m_syl.size()) - 1));
    m_view->overlayChanged();
}

// Split a line without \k tags into syllables: words, and vowel-ended
// chunks inside words (romaji).
void AudioBox::autoSplit() {
    const int row = m_tab->active();
    auto r = m_tab->s().row(size_t(row));
    const QString plain = fs::q(fsb::strip_tags(r.text));
    if (plain.trimmed().isEmpty()) return;
    static const QRegularExpression syl("[^aeiouAEIOU\\s]*[aeiouAEIOU]+n?(?![aeiouAEIOU])|\\S+?(?=\\s|$)|\\s+");
    QStringList parts;
    for (auto it = syl.globalMatch(plain); it.hasNext();) {
        const QString m = it.next().captured();
        if (m.trimmed().isEmpty() && !parts.isEmpty()) parts.last() += m;
        else parts << m;
    }
    if (parts.size() < 2) return;
    const qint64 cs = (r.end - r.start) / 10;
    QString out;
    qint64 used = 0;
    for (int k = 0; k < parts.size(); ++k) {
        const qint64 d = k + 1 == parts.size() ? cs - used : cs / parts.size();
        used += d;
        out += QString("{\\k%1}").arg(d) + parts[k];
    }
    r.text = fs::rs(out);
    m_tab->s().set_row(size_t(row), r, "");
    m_tab->changed(TabPanel::FromAudio);
}

void AudioBox::stepLine(int dir) {
    if (m_selModified && m_autoCommit) commit();
    const auto vis = m_tab->grid()->visibleRows();
    const int act = m_tab->active();
    auto it = std::find(vis.begin(), vis.end(), size_t(std::max(0, act)));
    if (it == vis.end()) return;
    if (dir < 0 && it == vis.begin()) return;
    auto next = dir < 0 ? it - 1 : it + 1;
    if (next == vis.end()) return;
    m_tab->setActive(int(*next));
    playSelection();
}

void AudioBox::stepSyllable(int dir) {
    if (m_syl.empty()) return stepLine(dir);
    const int n = m_activeSyl + dir;
    if (n < 0 || n >= int(m_syl.size())) return stepLine(dir);
    m_activeSyl = n;
    play(m_syl[n].start, m_syl[n].end);
    m_view->overlayChanged();
}

void AudioBox::keyPressEvent(QKeyEvent *e) {
    const qint64 span = qint64(m_view->width() * m_msPerPx);
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_G: commit(); break;
    case Qt::Key_Left: m_karaoke ? stepSyllable(-1) : stepLine(-1); break;
    case Qt::Key_Z: stepLine(-1); break;
    case Qt::Key_Right: m_karaoke ? stepSyllable(1) : stepLine(1); break;
    case Qt::Key_X: stepLine(1); break;
    case Qt::Key_Down:
    case Qt::Key_S:
        if (m_karaoke && size_t(m_activeSyl) < m_syl.size()) play(m_syl[m_activeSyl].start, m_syl[m_activeSyl].end);
        else playSelection();
        break;
    case Qt::Key_Up:
    case Qt::Key_R: playSelection(); break;
    case Qt::Key_H: stop(); break;
    case Qt::Key_B: ensureVisible(m_selStart, m_selEnd); break;
    case Qt::Key_A: setLeft(m_left - span / 4); break;
    case Qt::Key_F: setLeft(m_left + span / 4); break;
    case Qt::Key_0:
        if (m_mark >= 0) play(std::max<qint64>(0, m_mark - 1000), m_mark);
        break;
    case Qt::Key_Period:
        if (m_mark >= 0) play(m_mark, m_mark + 1000);
        break;
    case Qt::Key_E: play(m_selStart, std::min(m_selEnd, m_selStart + 500)); break;
    case Qt::Key_D: play(std::max(m_selStart, m_selEnd - 500), m_selEnd); break;
    case Qt::Key_Q: play(std::max<qint64>(0, m_selStart - 500), m_selStart); break;
    case Qt::Key_W: play(m_selEnd, m_selEnd + 500); break;
    case Qt::Key_T: play(m_selStart, m_duration); break;
    case Qt::Key_C:
        setSelection(std::max<qint64>(0, m_selStart - QSettings().value("audio/leadIn", 200).toInt()), m_selEnd, false);
        break;
    case Qt::Key_V: setSelection(m_selStart, m_selEnd + QSettings().value("audio/leadOut", 300).toInt(), false); break;
    default: QWidget::keyPressEvent(e); return;
    }
}

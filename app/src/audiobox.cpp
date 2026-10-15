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
#include <map>
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
        std::vector<float> texels;
        QSize sz;
        QRhiTexture::Format fmt;
        if (b.m_spectrum) {
            const int h = std::max(1, out.height());
            texels.resize(size_t(w) * size_t(h));
            (*b.m_audio)->spectrum(start, spp, size_t(w), size_t(h), b.m_enhance, {texels.data(), texels.size()});
            sz = {w, h};
            fmt = QRhiTexture::R32F;
        } else {
            std::vector<float> peaks(size_t(w) * 3);
            (*b.m_audio)->peaks(start, spp, {peaks.data(), peaks.size()});
            texels.resize(size_t(w) * 4);
            for (int x = 0; x < w; ++x) {
                texels[x * 4] = peaks[x * 3];
                texels[x * 4 + 1] = peaks[x * 3 + 1];
                texels[x * 4 + 2] = peaks[x * 3 + 2];
            }
            sz = {w, 1};
            fmt = QRhiTexture::RGBA32F;
        }
        if (m_data->pixelSize() != sz || m_data->format() != fmt) {
            m_data.reset(rhi()->newTexture(fmt, sz));
            m_data->create();
            m_wave.setTexture(m_data.get());
        }
        QRhiTextureSubresourceUploadDescription sub(texels.data(), quint32(texels.size() * sizeof(float)));
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
        const qint64 t = video->hasVideo() ? video->timeAtStart(kf) : -1; // where snapping puts a line
        if (t < l || t > r) continue;
        p.drawLine(b.xOf(t), RulerH, b.xOf(t), h);
    }
    // inactive lines: shaded, with dotted boundaries
    p.setPen(QPen(kInactive, 1, Qt::DotLine));
    for (const auto &[start, end] : b.inactiveLines()) {
        if (end < l || start > r) continue;
        p.fillRect(QRect(QPoint(b.xOf(start), RulerH), QPoint(b.xOf(end), h)), QColor(0x80, 0x80, 0x90, 48));
        for (qint64 t : {start, end})
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
    // the time under the mouse while nothing plays (Aegisub's and Kainote's track cursor)
    if (m_hoverX >= 0 && !b.playing()) {
        p.setPen(QPen(QColor(255, 255, 255, 150), 1));
        p.drawLine(m_hoverX, RulerH, m_hoverX, h);
        const QString at = fs::fmtTime(b.msAt(m_hoverX));
        const int tw = p.fontMetrics().horizontalAdvance(at) + 6, ty = RulerH + (b.m_karaoke ? KaraH : 0) + 2;
        const int tx = m_hoverX + tw + 4 > w ? m_hoverX - tw - 2 : m_hoverX + 2;
        p.fillRect(tx, ty, tw, 15, QColor(0, 0, 0, 160));
        p.setPen(QColor(230, 230, 230));
        p.drawText(QRect(tx, ty, tw, 15), Qt::AlignCenter, at);
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
    m_hoverX = -1;
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
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const int nearMarker = std::abs(b.xOf(b.m_selStart) - x) < 6 ? 1 : std::abs(b.xOf(b.m_selEnd) - x) < 6 ? 2 : 0;
    if (e->button() == Qt::LeftButton && ctrl && nearMarker) {
        // the other lines starting or ending on the marker move with it (Aegisub's Ctrl+drag), so a
        // boundary two lines share stays shared
        // shortcut: they move on the commit, not while dragging; draw them along if that confuses
        const qint64 at = nearMarker == 1 ? b.m_selStart : b.m_selEnd;
        b.m_linkStarts.clear();
        b.m_linkEnds.clear();
        b.m_linkMarker = nearMarker;
        const auto spans = b.m_tab->s().row_spans();
        for (size_t i = 0; i < spans.size(); ++i) {
            if (int(i) == b.m_tab->active()) continue;
            if (spans[i].start == at) b.m_linkStarts.push_back(i);
            if (spans[i].end == at) b.m_linkEnds.push_back(i);
        }
        m_drag = nearMarker;
        return;
    }
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && ctrl)) {
        if (b.m_karaoke && e->button() == Qt::MiddleButton) {
            for (size_t k = 1; k < b.m_syl.size(); ++k)
                if (std::abs(b.xOf(b.m_syl[k].start) - x) < 5) {
                    b.writeProposedSplit();
                    b.m_tab->s().kara_join(size_t(b.m_tab->active()), k);
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
        m_drag = 6; // and on while the button is held (Aegisub's scrubbing)
        return;
    }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) {
        // Alt+drag moves the whole line (Aegisub)
        m_drag = 5;
        m_pressT = t;
        m_moveFrom = {b.m_selStart, b.m_selEnd};
        b.m_multi |= 4;
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
                    b.writeProposedSplit();
                    b.m_tab->s().kara_split(size_t(b.m_tab->active()), k, size_t(pos));
                    b.m_tab->changed(TabPanel::FromAudio);
                    b.karaokeChanged();
                }
                return;
            }
        }
        for (size_t k = 0; k < b.m_syl.size(); ++k)
            if (t >= b.m_syl[k].start && t < b.m_syl[k].end) b.m_activeSyl = int(k);
        // a click only picks the syllable (Aegisub, Kainote), a right click plays it too; the
        // line's own markers still drag
        if (std::abs(b.xOf(b.m_selStart) - x) >= 6 && std::abs(b.xOf(b.m_selEnd) - x) >= 6) {
            if (e->button() == Qt::RightButton && size_t(b.m_activeSyl) < b.m_syl.size())
                b.play(b.m_syl[size_t(b.m_activeSyl)].start, b.m_syl[size_t(b.m_activeSyl)].end);
            overlayChanged();
            return;
        }
    }
    const bool snapToggle = e->modifiers() & Qt::ShiftModifier;
    if (std::abs(b.xOf(b.m_selStart) - x) < 6) m_drag = 1;
    else if (std::abs(b.xOf(b.m_selEnd) - x) < 6) m_drag = 2;
    else if (e->button() == Qt::LeftButton) {
        // away from the markers the start goes here, and a drag times the line from scratch
        // (Aegisub's drag timing, Kainote)
        m_drag = 4;
        m_pressT = b.snap(t, snapToggle);
        b.m_multi |= 1;
        b.setSelection(m_pressT, std::max(b.m_selEnd, m_pressT), true);
    } else if (e->button() == Qt::RightButton) {
        m_drag = 2;
        b.m_multi |= 2;
        b.setSelection(b.m_selStart, std::max(b.m_selStart, b.snap(t, snapToggle)), true);
    }
    overlayChanged();
}

void AudioView::mouseMoveEvent(QMouseEvent *e) {
    auto &b = *m_box;
    const int x = int(e->position().x());
    if (!m_drag) {
        m_hoverX = e->position().y() >= RulerH ? x : -1;
        const bool edge = std::abs(b.xOf(b.m_selStart) - x) < 6 || std::abs(b.xOf(b.m_selEnd) - x) < 6;
        setCursor(edge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        overlayChanged();
        return;
    }
    // a marker dragged past the edges scrolls the view along (Aegisub)
    // shortcut: only while the mouse moves; a timer if holding it still out there should scroll too
    if (m_drag != 3 && m_drag != 6 && (x < 0 || x >= width())) b.setLeft(b.m_left + qint64((x < 0 ? x : x - width() + 1) * b.m_msPerPx));
    const qint64 t = b.msAt(x);
    const bool snapToggle = e->modifiers() & Qt::ShiftModifier;
    if (m_drag == 3) {
        b.setLeft(m_pressLeft - qint64((x - m_pressX) * b.m_msPerPx));
    } else if (m_drag == 1) {
        b.setSelection(std::min(b.snap(t, snapToggle), b.m_selEnd), b.m_selEnd, true);
    } else if (m_drag == 2) {
        b.setSelection(b.m_selStart, std::max(b.snap(t, snapToggle), b.m_selStart), true);
    } else if (m_drag == 4) {
        if (std::abs(x - m_pressX) > 3) {
            b.m_multi |= 2;
            const qint64 u = b.snap(t, snapToggle);
            b.setSelection(std::min(m_pressT, u), std::max(m_pressT, u), true);
        }
    } else if (m_drag == 5) {
        const qint64 d = std::max(t - m_pressT, -m_moveFrom.first);
        b.setSelection(m_moveFrom.first + d, m_moveFrom.second + d, true);
    } else if (m_drag == 6) {
        b.m_tab->seekVideo(t);
    } else if (m_drag >= 10) {
        b.writeProposedSplit();
        b.m_tab->s().kara_set_boundary(size_t(b.m_tab->active()), size_t(m_drag - 10), t, m_button == Qt::RightButton,
                                       fs::Utf8(QString("kara-%1").arg(b.m_dragId))); // one drag, one undo step
        b.m_tab->changed(TabPanel::FromAudio);
        b.karaokeChanged();
    }
    overlayChanged();
}

void AudioView::mouseReleaseEvent(QMouseEvent *) {
    auto &b = *m_box;
    if ((m_drag == 1 || m_drag == 2 || m_drag == 4 || m_drag == 5) && b.m_autoCommit && b.m_selModified) b.commit();
    m_drag = 0;
}

void AudioView::leaveEvent(QEvent *) {
    m_hoverX = -1;
    overlayChanged();
}

// the audio keys work as soon as the mouse is over it (Kainote's default, Aegisub's option)
void AudioView::enterEvent(QEnterEvent *) {
    if (window()->isActiveWindow() && QSettings().value("audio/autoFocus", true).toBool()) setFocus();
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
    m_nextOnCommit = st.value("nextOnCommit", true).toBool(); // on in Aegisub and Kainote
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

AudioBox::~AudioBox() {
    *m_openCancel = true;
    stop();
}

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
    btn("■", tr("Stop playback"), [this] { m_tab->video()->pause(), stop(); })->setIcon(icon("player-stop-filled", teal));
    btn("|◀", tr("Play before the mark"), [this] { if (m_mark >= 0) play(std::max<qint64>(0, m_mark - 1000), m_mark); }, true)->setIcon(icon("arrow-bar-left", teal));
    btn("▶|", tr("Play after the mark"), [this] { if (m_mark >= 0) play(m_mark, m_mark + 1000); })->setIcon(icon("arrow-bar-right", teal));
    btn("◁s", tr("Play 500 ms before the start time"), [this] { play(std::max<qint64>(0, m_selStart - 500), m_selStart); }, true);
    btn("s▷", tr("Play 500 ms after the start time"), [this] { play(m_selStart, std::min(m_selEnd, m_selStart + 500)); });
    btn("◁e", tr("Play 500 ms before the end time"), [this] { play(std::max(m_selStart, m_selEnd - 500), m_selEnd); });
    btn("e▷", tr("Play 500 ms after the end time"), [this] { play(m_selEnd, m_selEnd + 500); });
    btn("▶▶", tr("Play to the end"), [this] { play(m_selStart, m_duration); })->setIcon(icon("player-skip-forward", teal));
    btn("+in", tr("Add lead-in to the active line"), [this] {
        setSelection(std::max<qint64>(0, m_selStart - QSettings().value("audio/leadIn", 200).toInt()), m_selEnd, false);
    }, true)->setIcon(icon("arrow-autofit-left", green));
    btn("+out", tr("Add lead-out to the active line"), [this] {
        setSelection(m_selStart, m_selEnd + QSettings().value("audio/leadOut", 300).toInt(), false);
    })->setIcon(icon("arrow-autofit-right", green));
    btn("✓", tr("Apply changes (commit)"), [this] { commitAndNext(); }, true)->setIcon(icon("check", green));
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
    close(); // also stops a load still decoding
    m_openCancel = std::make_shared<std::atomic_bool>(false);
    m_tab->main()->status(tr("Loading audio %1…").arg(path));
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex";
    QPointer<AudioBox> self(this);
    auto cancel = m_openCancel;
    std::thread([self, path, cache, cancel] {
        std::optional<rust::Box<fsb::Audio>> a;
        QString err;
        try {
            const fs::PercentProgress progress([self, path, cancel](int pct) {
                QMetaObject::invokeMethod(qApp, [self, path, pct, cancel] {
                    if (self && !*cancel) self->m_tab->main()->status(tr("Loading audio %1… %2%").arg(path).arg(pct));
                });
                return !*cancel;
            });
            a.emplace(fsb::open_audio(fs::Utf8(path), fs::Utf8(cache), progress));
        } catch (const rust::Error &e) {
            err = QString::fromUtf8(e.what());
        }
        auto holder = std::make_shared<std::optional<rust::Box<fsb::Audio>>>(std::move(a));
        QMetaObject::invokeMethod(qApp, [self, holder, err, path, cancel] {
            if (!self) return;
            if (*cancel) { // closed, or replaced by a newer open (which has its own status)
                if (cancel == self->m_openCancel) self->m_tab->main()->status(QString());
                return;
            }
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
    *m_openCancel = true;
    stop();
    m_audio.reset();
    m_path.clear();
    m_duration = 0;
    m_view->dataChanged();
    emit opened();
}

void AudioBox::play(qint64 from, qint64 to, bool withVideo) {
    if (!m_audio || to <= from) return;
    if (!withVideo) m_tab->video()->pause(); // or the video would follow this range as its clock
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
    if (kf && m_tab->video()->hasVideo()) {
        for (qint64 f : m_tab->keyframes()) consider(m_tab->video()->timeAtStart(f)); // Aegisub's markers
        // and the start and end of the frame shown (Aegisub)
        const qint64 f = m_tab->video()->frame();
        consider(m_tab->video()->timeAtStart(f));
        consider(m_tab->video()->timeAtEnd(f));
    }
    if (lines)
        for (const auto &[start, end] : inactiveLines()) {
            consider(start);
            consider(end);
        }
    return bestD <= range ? best : ms;
}

// Aegisub's inactive lines: none (0), the previous (1), the previous and next (2) or all the other
// lines (3), comments only when asked. Cached: the overlay repaints with every play cursor step.
const std::vector<std::pair<qint64, qint64>> &AudioBox::inactiveLines() const {
    QSettings st;
    const int mode = std::clamp(st.value("audio/inactiveLines", 3).toInt(), 0, 3);
    const bool comments = st.value("audio/inactiveComments", false).toBool();
    auto &s = m_tab->s();
    const int act = m_tab->active();
    const std::tuple key{s.generation(), act, mode | (comments ? 4 : 0)};
    if (key == m_inactiveKey) return m_inactive;
    m_inactiveKey = key;
    m_inactive.clear();
    const auto spans = s.row_spans();
    const int n = int(spans.size());
    const auto shown = [&](int i) { return i != act && (comments || !spans[size_t(i)].comment); };
    const auto add = [&](int i) { m_inactive.emplace_back(spans[size_t(i)].start, spans[size_t(i)].end); };
    if (mode == 3)
        for (int i = 0; i < n; ++i)
            if (shown(i)) add(i);
    if ((mode == 1 || mode == 2) && act >= 0 && act < n) {
        int prev = act - 1, next = act + 1;
        while (prev >= 0 && !shown(prev)) --prev;
        while (next < n && !shown(next)) ++next;
        if (prev >= 0) add(prev);
        if (mode == 2 && next < n) add(next);
    }
    return m_inactive;
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
    const qint64 oldEnd = m_selEnd;
    m_selStart = std::max<qint64>(0, start);
    m_selEnd = std::max(m_selStart, end);
    // the selection playing stops where its end was dragged to (Aegisub, Kainote)
    if (m_selEnd != oldEnd && m_playEnd == oldEnd && playing()) {
        m_playEnd = m_selEnd;
        (*m_audio)->set_end_ms(m_selEnd);
    }
    m_selModified = true;
    if (!fromDrag && m_autoCommit) commit();
    m_view->overlayChanged();
}

void AudioBox::commit() {
    const int row = m_tab->active();
    if (row < 0 || size_t(row) >= m_tab->s().row_count()) return;
    writeProposedSplit();
    auto r = m_tab->s().row(size_t(row));
    if (r.start != m_selStart || r.end != m_selEnd) {
        const auto amend = fs::Utf8(QString("audio-%1").arg(m_dragId));
        const qint64 shift = m_selStart - r.start; // of a line moved whole
        // karaoke: the outer syllables take the change, the others keep their place (Aegisub); a
        // line moved whole keeps its syllables as they are
        if (m_karaoke && m_selEnd - m_selStart != r.end - r.start) {
            m_tab->s().kara_set_times(size_t(row), m_selStart, m_selEnd, amend);
        } else {
            r.start = m_selStart;
            r.end = m_selEnd;
            m_tab->s().set_row(size_t(row), r, amend);
        }
        if (m_linkMarker) {
            // the lines a Ctrl+drag took along, in the same step; one dragged past its other end turns
            const qint64 at = m_linkMarker == 1 ? m_selStart : m_selEnd;
            std::map<size_t, std::pair<qint64, qint64>> times;
            const auto spans = m_tab->s().row_spans();
            for (size_t i : m_linkStarts)
                if (i < spans.size()) times[i] = {at, spans[i].end};
            for (size_t i : m_linkEnds)
                if (i < spans.size()) times.try_emplace(i, spans[i].start, at).first->second.second = at;
            std::vector<size_t> rows;
            std::vector<rust::String> values;
            for (auto [i, t] : times) {
                rows.push_back(i);
                values.push_back(fs::rs(QString("%1,%2").arg(std::min(t.first, t.second)).arg(std::max(t.first, t.second))));
            }
            m_tab->s().set_rows_values(fs::slice(rows), "times", {values.data(), values.size()}, amend);
        }
        // the other selected lines (Aegisub): a click away from the markers gave them its start, the
        // drag on from it or a right click its end, Alt+drag moved them along
        if (const auto sel = m_tab->selection(); m_multi && sel.size() > 1) {
            const auto spans = m_tab->s().row_spans();
            const qint64 by = m_multi & 4 ? shift : 0;
            std::vector<size_t> rows;
            std::vector<rust::String> values;
            for (size_t i : sel) {
                if (int(i) == row || i >= spans.size()) continue;
                qint64 s = spans[i].start + by, e = spans[i].end + by;
                if (m_multi & 1) s = m_selStart;
                if (m_multi & 2) e = m_selEnd;
                rows.push_back(i);
                values.push_back(fs::rs(QString("%1,%2").arg(std::max<qint64>(0, std::min(s, e))).arg(std::max(s, e))));
            }
            m_tab->s().set_rows_values(fs::slice(rows), "times", {values.data(), values.size()}, amend);
        }
        m_tab->changed(TabPanel::FromAudio);
    }
    m_multi = 0;
    m_linkMarker = 0;
    m_selModified = false;
    m_view->overlayChanged();
}

// Aegisub's audio/commit: apply, then with "next line on commit" go on as grid/line/next/create
// does - past the last line a new one follows - and a line not timed yet (end 0) is proposed
// from where this one ended, for the default 5 s.
void AudioBox::commitAndNext(bool resetNext, bool next) {
    commit();
    ++m_dragId; // what follows is a step of its own: Aegisub never merges commits
    if (!m_nextOnCommit && !resetNext && !next) return;
    const qint64 end = m_selEnd;
    const auto vis = m_tab->grid()->visibleRows();
    const auto it = std::upper_bound(vis.begin(), vis.end(), size_t(std::max(0, m_tab->active())));
    if (it != vis.end()) m_tab->setActive(int(*it));
    else m_tab->grid()->insertRow(true, 0); // starts where this line ends
    if (const int row = m_tab->active(); row >= 0 && (resetNext || m_tab->s().row(size_t(row)).end == 0))
        setSelection(end, end + QSettings().value("subs/defaultDuration", 3000).toLongLong(), true);
    playSelection();
}

// The markers follow the active line; one not timed yet (end 0) keeps them where they are, to be
// timed from there (Aegisub). False when they stayed.
bool AudioBox::followLine() {
    const int row = m_tab->active();
    if (row < 0 || size_t(row) >= m_tab->s().row_count()) return false;
    const auto r = m_tab->s().row(size_t(row));
    if (r.end <= 0 && m_audio) return false;
    m_selStart = r.start;
    m_selEnd = r.end;
    return true;
}

void AudioBox::lineChanged() {
    ++m_dragId; // a new line never merges into the last one's undo step
    m_selModified = false;
    m_linkMarker = m_multi = 0;
    if (followLine() && m_autoScroll && m_audio) ensureVisible(m_selStart, m_selEnd);
    m_activeSyl = 0;
    karaokeChanged();
}

void AudioBox::docChanged() {
    if (!m_selModified) followLine();
    karaokeChanged();
}

void AudioBox::karaokeChanged() {
    m_syl.clear();
    m_autoText.clear();
    const int row = m_tab->active();
    if (m_karaoke && row >= 0 && size_t(row) < m_tab->s().row_count()) {
        auto syl = m_tab->s().syllables(size_t(row));
        if (syl.size() <= 1 && m_autoSplit) {
            // Kainote: the split is a proposal until it is edited or committed
            const auto r = m_tab->s().row(size_t(row));
            m_autoText = autoSplitText(fs::q(r.text), r.end - r.start);
            if (!m_autoText.isEmpty()) syl = fsb::syllables_of(fs::Utf8(m_autoText), r.start, r.end);
        }
        m_syl.assign(syl.begin(), syl.end());
    }
    m_activeSyl = std::clamp(m_activeSyl, 0, std::max(0, int(m_syl.size()) - 1));
    m_view->overlayChanged();
}

void AudioBox::writeProposedSplit() {
    const int row = m_tab->active();
    if (m_autoText.isEmpty() || row < 0 || size_t(row) >= m_tab->s().row_count()) return;
    auto r = m_tab->s().row(size_t(row));
    r.text = fs::rs(m_autoText);
    m_autoText.clear();
    m_tab->s().set_row(size_t(row), r, "");
    m_tab->changed(TabPanel::FromAudio);
}

// Split a line without \k tags into syllables: words, and vowel-ended chunks inside words
// (romaji). Tags stay where they are and count for nothing (Kainote); one at a cut starts the next
// syllable. "" when there is nothing to split, or the line has \k tags already.
QString AudioBox::autoSplitText(const QString &text, qint64 duration) const {
    static const QRegularExpression kara("\\{[^}]*\\\\[kK]");
    if (text.contains(kara)) return {};
    QString plain; // the visible characters, and where each one is in the text
    std::vector<int> at;
    for (int i = 0, tag = 0; i < text.size(); ++i) {
        if (text[i] == '{') tag = 1;
        else if (tag && text[i] == '}') tag = 0;
        else if (!tag) plain += text[i], at.push_back(i);
    }
    if (plain.trimmed().isEmpty()) return {};
    static const QRegularExpression syl("[^aeiouAEIOU\\s]*[aeiouAEIOU]+n?(?![aeiouAEIOU])|\\S+?(?=\\s|$)|\\s+");
    std::vector<int> cuts{0}; // where each syllable starts in plain; spaces end the one before
    for (auto it = syl.globalMatch(plain); it.hasNext();)
        if (const auto m = it.next(); m.capturedStart() > 0 && !m.captured().trimmed().isEmpty())
            cuts.push_back(int(m.capturedStart()));
    const int n = int(cuts.size());
    if (n < 2) return {};
    const qint64 cs = duration / 10;
    QString out;
    qint64 used = 0;
    for (int k = 0; k < n; ++k) {
        const qint64 d = k + 1 == n ? cs - used : cs / n;
        used += d;
        const int from = k == 0 ? 0 : at[cuts[k] - 1] + 1;
        const int to = k + 1 == n ? int(text.size()) : at[cuts[k + 1] - 1] + 1;
        out += QString("{\\k%1}").arg(d) + text.mid(from, to - from);
    }
    return out;
}

bool AudioBox::stepLine(int dir, bool playIt) {
    if (m_selModified && m_autoCommit) commit();
    const auto vis = m_tab->grid()->visibleRows();
    const int act = m_tab->active();
    auto it = std::find(vis.begin(), vis.end(), size_t(std::max(0, act)));
    if (it == vis.end()) return false;
    if (dir < 0 && it == vis.begin()) return false;
    auto next = dir < 0 ? it - 1 : it + 1;
    if (next == vis.end()) return false;
    m_tab->setActive(int(*next));
    if (playIt) playSelection();
    return true;
}

void AudioBox::stepSyllable(int dir) {
    if (m_syl.empty()) return void(stepLine(dir));
    int n = m_activeSyl + dir;
    if (n < 0 || n >= int(m_syl.size())) {
        if (!stepLine(dir, false)) return;
        if (m_syl.empty()) return playSelection();
        // back from the first syllable: the last one of the line before (Aegisub)
        n = dir < 0 ? int(m_syl.size()) - 1 : 0;
    }
    m_activeSyl = n;
    play(m_syl[n].start, m_syl[n].end);
    m_view->overlayChanged();
}

// Aegisub's ModifyStart / ModifyLength: 10 ms steps of the active syllable in karaoke mode, else of the line.
void AudioBox::modifyStart(int steps) {
    if (!m_karaoke || m_syl.empty()) return setSelection(std::min(m_selStart + steps * 10, m_selEnd), m_selEnd, false);
    if (m_activeSyl <= 0) return;
    writeProposedSplit();
    m_tab->s().kara_set_boundary(size_t(m_tab->active()), size_t(m_activeSyl), m_syl[m_activeSyl].start + steps * 10,
                                 false, "");
    m_tab->changed(TabPanel::FromAudio);
    karaokeChanged();
}

void AudioBox::modifyLength(int steps, bool shiftFollowing) {
    if (!m_karaoke || m_syl.empty()) return setSelection(m_selStart, m_selEnd + steps * 10, false);
    const size_t next = size_t(m_activeSyl) + 1;
    if (next >= m_syl.size()) return;
    writeProposedSplit();
    m_tab->s().kara_set_boundary(size_t(m_tab->active()), next, m_syl[next].start + steps * 10, shiftFollowing, "");
    m_tab->changed(TabPanel::FromAudio);
    karaokeChanged();
}

// Kainote's audio hotkeys and defaults: a second key is a command of its own ("alternative")
static const AudioKey kAudioKeys[] = {
    {"audioCommit", QT_TRANSLATE_NOOP("AudioBox", "Commit"), "Return"},
    {"audioCommitAlt", QT_TRANSLATE_NOOP("AudioBox", "Commit (alternative)"), "G"},
    {"audioCommitDefault", QT_TRANSLATE_NOOP("AudioBox", "Commit and use default timing for next line"), "Shift+G"},
    {"audioPrev", QT_TRANSLATE_NOOP("AudioBox", "Previous line (syllable in karaoke)"), "Left"},
    {"audioPrevAlt", QT_TRANSLATE_NOOP("AudioBox", "Previous line"), "Z"},
    {"audioNext", QT_TRANSLATE_NOOP("AudioBox", "Next line (syllable in karaoke)"), "Right"},
    {"audioNextAlt", QT_TRANSLATE_NOOP("AudioBox", "Next line"), "X"},
    {"audioPlay", QT_TRANSLATE_NOOP("AudioBox", "Play (the syllable in karaoke)"), "Down"},
    {"audioPlayAlt", QT_TRANSLATE_NOOP("AudioBox", "Play (alternative)"), "S"},
    {"audioPlayLine", QT_TRANSLATE_NOOP("AudioBox", "Play line"), "Up"},
    {"audioPlayLineAlt", QT_TRANSLATE_NOOP("AudioBox", "Play line (alternative)"), "R"},
    {"audioStop", QT_TRANSLATE_NOOP("AudioBox", "Stop"), "H"},
    {"audioGoTo", QT_TRANSLATE_NOOP("AudioBox", "Go to selection"), "B"},
    {"audioScrollLeft", QT_TRANSLATE_NOOP("AudioBox", "Scroll left"), "A"},
    {"audioScrollRight", QT_TRANSLATE_NOOP("AudioBox", "Scroll right"), "F"},
    {"audioBeforeMark", QT_TRANSLATE_NOOP("AudioBox", "Play before mark"), "0"},
    {"audioAfterMark", QT_TRANSLATE_NOOP("AudioBox", "Play after mark"), "."},
    {"audioFirst500", QT_TRANSLATE_NOOP("AudioBox", "Play first 500 ms"), "E"},
    {"audioLast500", QT_TRANSLATE_NOOP("AudioBox", "Play last 500 ms"), "D"},
    {"audioBefore500", QT_TRANSLATE_NOOP("AudioBox", "Play 500 ms before"), "Q"},
    {"audioAfter500", QT_TRANSLATE_NOOP("AudioBox", "Play 500 ms after"), "W"},
    {"audioToEnd", QT_TRANSLATE_NOOP("AudioBox", "Play to end"), "T"},
    {"audioLeadIn", QT_TRANSLATE_NOOP("AudioBox", "Add lead-in"), "C"},
    {"audioLeadOut", QT_TRANSLATE_NOOP("AudioBox", "Add lead-out"), "V"},
    // Aegisub's, with its keys where Kainote has none
    {"audioPlayToggle", QT_TRANSLATE_NOOP("AudioBox", "Play or stop"), ""},
    {"audioCommitNext", QT_TRANSLATE_NOOP("AudioBox", "Commit and go to the next line"), ""},
    {"audioCommitStay", QT_TRANSLATE_NOOP("AudioBox", "Commit and stay on the line"), ""},
    {"audioGoStart", QT_TRANSLATE_NOOP("AudioBox", "Go to selection start"), ""},
    {"audioGoEnd", QT_TRANSLATE_NOOP("AudioBox", "Go to selection end"), ""},
    {"audioLeadBoth", QT_TRANSLATE_NOOP("AudioBox", "Add lead-in and lead-out"), ""},
    {"audioStartEarlier", QT_TRANSLATE_NOOP("AudioBox", "Start 10 ms earlier (syllable in karaoke)"), "Num+4"},
    {"audioStartLater", QT_TRANSLATE_NOOP("AudioBox", "Start 10 ms later (syllable in karaoke)"), "Num+6"},
    {"audioShorter", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms earlier (syllable in karaoke)"), "Num+-"},
    {"audioShorterAlt", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms earlier (alternative)"), "Num+7"},
    {"audioLonger", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms later (syllable in karaoke)"), "Num++"},
    {"audioLongerAlt", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms later (alternative)"), "Num+9"},
    {"audioShorterShift", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms earlier, the following syllables too"), "Shift+Num+-"},
    {"audioLongerShift", QT_TRANSLATE_NOOP("AudioBox", "End 10 ms later, the following syllables too"), "Shift+Num++"},
};

std::span<const AudioKey> AudioBox::keys() { return kAudioKeys; }

void AudioBox::runCommand(const QString &id) {
    const qint64 span = qint64(m_view->width() * m_msPerPx);
    if (id == "audioCommit" || id == "audioCommitAlt") commitAndNext();
    else if (id == "audioCommitDefault") commitAndNext(true);
    else if (id == "audioPrev") m_karaoke ? stepSyllable(-1) : void(stepLine(-1));
    else if (id == "audioPrevAlt") stepLine(-1);
    else if (id == "audioNext") m_karaoke ? stepSyllable(1) : void(stepLine(1));
    else if (id == "audioNextAlt") stepLine(1);
    else if (id == "audioPlay" || id == "audioPlayAlt") {
        if (m_karaoke && size_t(m_activeSyl) < m_syl.size()) play(m_syl[m_activeSyl].start, m_syl[m_activeSyl].end);
        else playSelection();
    } else if (id == "audioPlayLine" || id == "audioPlayLineAlt") playSelection();
    else if (id == "audioStop") {
        m_tab->video()->pause();
        stop();
    } else if (id == "audioGoTo") ensureVisible(m_selStart, m_selEnd);
    else if (id == "audioScrollLeft") setLeft(m_left - span / 4);
    else if (id == "audioScrollRight") setLeft(m_left + span / 4);
    else if (id == "audioBeforeMark") {
        if (m_mark >= 0) play(std::max<qint64>(0, m_mark - 1000), m_mark);
    } else if (id == "audioAfterMark") {
        if (m_mark >= 0) play(m_mark, m_mark + 1000);
    } else if (id == "audioFirst500") play(m_selStart, std::min(m_selEnd, m_selStart + 500));
    else if (id == "audioLast500") play(std::max(m_selStart, m_selEnd - 500), m_selEnd);
    else if (id == "audioBefore500") play(std::max<qint64>(0, m_selStart - 500), m_selStart);
    else if (id == "audioAfter500") play(m_selEnd, m_selEnd + 500);
    else if (id == "audioToEnd") play(m_selStart, m_duration);
    else if (id == "audioLeadIn")
        setSelection(std::max<qint64>(0, m_selStart - QSettings().value("audio/leadIn", 200).toInt()), m_selEnd, false);
    else if (id == "audioLeadOut")
        setSelection(m_selStart, m_selEnd + QSettings().value("audio/leadOut", 300).toInt(), false);
    else if (id == "audioLeadBoth")
        setSelection(std::max<qint64>(0, m_selStart - QSettings().value("audio/leadIn", 200).toInt()),
                     m_selEnd + QSettings().value("audio/leadOut", 300).toInt(), false);
    else if (id == "audioPlayToggle") {
        if (playing()) stop();
        else playSelection();
    } else if (id == "audioCommitNext") commitAndNext(false, true);
    else if (id == "audioCommitStay") {
        commit();
        ++m_dragId;
    } else if (id == "audioGoStart") setLeft(m_selStart - span / 2);
    else if (id == "audioGoEnd") setLeft(m_selEnd - span / 2);
    else if (id == "audioStartEarlier") modifyStart(-1);
    else if (id == "audioStartLater") modifyStart(1);
    else if (id == "audioShorter" || id == "audioShorterAlt") modifyLength(-1, false);
    else if (id == "audioLonger" || id == "audioLongerAlt") modifyLength(1, false);
    else if (id == "audioShorterShift") modifyLength(-1, true);
    else if (id == "audioLongerShift") modifyLength(1, true);
}

void AudioBox::keyPressEvent(QKeyEvent *e) {
    // the keys the hotkey editor gives the audio commands: a numpad key's own first ("Num+4"), else
    // the same key of the main keyboard; the numpad's Enter is Enter
    const auto key = Qt::Key(e->key() == Qt::Key_Enter ? Qt::Key_Return : e->key());
    for (const auto mods : {e->modifiers(), e->modifiers() & ~Qt::KeypadModifier}) {
        const QKeySequence pressed(QKeyCombination(mods, key));
        for (const auto &k : kAudioKeys)
            if (auto *a = m_tab->main()->action(k.id); a && a->shortcut() == pressed) return runCommand(k.id);
    }
    QWidget::keyPressEvent(e);
}

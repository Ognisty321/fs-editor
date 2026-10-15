#include "videobox.h"

#include "audiobox.h"
#include "icons.h"
#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "visualtools.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <climits>

// ---- worker -------------------------------------------------------------------

namespace {
// The frame's picture, without the subtitles; called with the media locked. Drawing on it detaches
// it from the cached one.
QImage decode(VideoMedia &m, qint64 frame) {
    if (m.video && frame == m.cachedFrame) return m.cached;
    if (!m.video) { // a dummy: one picture for every frame
        if (m.cached.isNull()) {
            m.cached = QImage(m.width, m.height, QImage::Format_RGBA8888);
            m.cached.fill(m.dummyColor);
            if (m.dummyPattern) { // Aegisub's: 8x8 squares, every other one lighter (darker for light colours)
                QColor other = m.dummyColor.toHsl();
                const int l = other.lightness();
                other.setHsl(other.hue(), other.saturation(), l + 24 <= 255 ? l + 24 : l - 24);
                QPainter p(&m.cached);
                for (int y = 0; y < m.height; y += 8)
                    for (int x = (y / 8) % 2 ? 0 : 8; x < m.width; x += 16) p.fillRect(x, y, 8, 8, other);
            }
        }
        return m.cached;
    }
    QImage img(m.width, m.height, QImage::Format_RGBA8888);
    (*m.video)->frame(frame, rust::Slice<uint8_t>(img.bits(), size_t(img.sizeInBytes())));
    m.cachedFrame = frame;
    m.cached = img;
    return img;
}
} // namespace

void FrameWorker::render(quint64 id, qint64 frame, qint64 ms, bool subs) {
    if (id < latest.load()) return; // a newer request is queued
    std::lock_guard g(m_media->m);
    QImage img;
    try {
        img = decode(*m_media, frame);
        if (subs && m_media->renderer) {
            const rust::Slice<uint8_t> px(img.bits(), size_t(img.sizeInBytes()));
            if (m_media->scriptDirty) {
                (*m_media->renderer)->set_script(m_media->script);
                m_media->scriptDirty = false;
            }
            (*m_media->renderer)->blend(ms, size_t(img.width()), size_t(img.height()), size_t(img.width()),
                                        size_t(img.height()), px);
        }
    } catch (const rust::Error &e) {
        emit failed(QString::fromUtf8(e.what()));
    }
    emit ready(id, frame, img);
}

// ---- view ---------------------------------------------------------------------

VideoView::VideoView(VideoBox *box) : QRhiWidget(box), m_box(box) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(160, 90);
}

void VideoView::setFrame(const QImage &img) {
    m_frame = img;
    m_frameDirty = true;
    update();
}

QRectF VideoView::displayRect() const {
    const auto info = m_box->info();
    double vw = info.width > 0 ? info.width : 16, vh = info.height > 0 ? info.height : 9;
    if (info.sar_num > 0 && info.sar_den > 0) vw = vw * info.sar_num / info.sar_den;
    const double s = std::min(width() / vw, height() / vh);
    return QRectF((width() - vw * s) / 2, (height() - vh * s) / 2, vw * s, vh * s);
}

QPointF VideoView::toWidget(QPointF p) const {
    const auto info = m_box->tab()->s().file_info();
    const QRectF d = displayRect();
    const double nx = p.x() / std::max(1, info.play_res_x), ny = p.y() / std::max(1, info.play_res_y);
    return {d.left() + (nx - m_zoom.left()) / m_zoom.width() * d.width(),
            d.top() + (ny - m_zoom.top()) / m_zoom.height() * d.height()};
}

QPointF VideoView::toScript(QPointF p) const {
    const auto info = m_box->tab()->s().file_info();
    const QRectF d = displayRect();
    const double nx = m_zoom.left() + (p.x() - d.left()) / d.width() * m_zoom.width();
    const double ny = m_zoom.top() + (p.y() - d.top()) / d.height() * m_zoom.height();
    return {nx * info.play_res_x, ny * info.play_res_y};
}

double VideoView::scriptScale() const {
    return displayRect().width() / m_zoom.width() / std::max(1, m_box->tab()->s().file_info().play_res_x);
}

void VideoView::setZoom(QRectF uv) {
    m_zoom = uv.normalized().intersected(QRectF(0, 0, 1, 1));
    if (m_zoom.width() < 0.1 || m_zoom.height() < 0.1) m_zoom = {0, 0, 1, 1};
    m_box->tab()->main()->updateStatus();
    updateOverlay();
}

void VideoView::initialize(QRhiCommandBuffer *) {
    if (m_frameQuad.ready()) return;
    auto *u = rhi()->nextResourceUpdateBatch();
    QImage blank(16, 16, QImage::Format_RGBA8888_Premultiplied);
    blank.fill(Qt::transparent);
    uploadImage(rhi(), m_frameTex, m_frame.isNull() ? blank : m_frame, u);
    uploadImage(rhi(), m_overlayTex, blank, u);
    auto *rp = renderTarget()->renderPassDescriptor();
    m_frameQuad.init(rhi(), rp, sampleCount(), ":/shaders/texture.frag.qsb", false, true, m_frameTex.get(), u);
    m_overlayQuad.init(rhi(), rp, sampleCount(), ":/shaders/texture.frag.qsb", true, false, m_overlayTex.get(), u);
    m_pendingInit = u;
    m_frameDirty = m_overlayDirty = true;
}

void VideoView::render(QRhiCommandBuffer *cb) {
    auto *u = m_pendingInit ? m_pendingInit : rhi()->nextResourceUpdateBatch();
    m_pendingInit = nullptr;
    const QSize out = renderTarget()->pixelSize();
    const double dpr = devicePixelRatioF();
    if (m_frameDirty && !m_frame.isNull()) {
        if (uploadImage(rhi(), m_frameTex, m_frame, u)) m_frameQuad.setTexture(m_frameTex.get());
        m_frameDirty = false;
    }
    // after a resize the old overlay would be stretched over the new size
    if (m_overlayDirty || !m_overlayTex || m_overlayTex->pixelSize() != out) {
        QImage ov(out, QImage::Format_RGBA8888_Premultiplied);
        ov.fill(Qt::transparent);
        ov.setDevicePixelRatio(dpr);
        {
            QPainter p(&ov);
            p.setRenderHint(QPainter::Antialiasing);
            if (QSettings().value("video/overscan").toBool()) paintOverscan(p);
            if (m_zoomDragging) {
                p.setPen(QPen(Qt::yellow, 1, Qt::DashLine));
                p.drawRect(QRectF(m_zoomDragStart, mapFromGlobal(QCursor::pos()).toPointF()));
            }
            m_box->m_tools->paint(p);
        }
        if (uploadImage(rhi(), m_overlayTex, ov, u)) m_overlayQuad.setTexture(m_overlayTex.get());
        m_overlayDirty = false;
    }
    const QRectF d = displayRect();
    RhiQuad::Uniforms fu;
    fu.uvRect[0] = float(m_zoom.left());
    fu.uvRect[1] = float(m_zoom.top());
    fu.uvRect[2] = float(m_zoom.right());
    fu.uvRect[3] = float(m_zoom.bottom());
    m_frameQuad.setUniforms(u, fu, QRectF(d.topLeft() * dpr, d.size() * dpr), out);
    m_overlayQuad.setUniforms(u, RhiQuad::Uniforms{}, QRectF(QPointF(0, 0), QSizeF(out)), out);
    cb->beginPass(renderTarget(), QColor(0x10, 0x10, 0x10), {1.0f, 0}, u);
    if (!m_frame.isNull()) m_frameQuad.draw(cb, out);
    m_overlayQuad.draw(cb, out);
    cb->endPass();
}

// Aegisub's overscan mask (BBC guidelines): title safe and action safe areas, rounded corners.
void VideoView::paintOverscan(QPainter &p) const {
    const QRectF d = displayRect();
    const bool wide = d.width() / d.height() > 1.75;
    const QPointF areas[] = {wide ? QPointF(.1, .05) : QPointF(.067, .05), wide ? QPointF(.035, .035) : QPointF(.033, .035)};
    for (const QPointF a : areas) {
        const QPointF m(a.x() * d.width(), a.y() * d.height());
        const QPointF c[] = {d.topLeft() + m, QPointF(d.right() - m.x(), d.top() + m.y()), d.bottomRight() - m,
                             QPointF(d.left() + m.x(), d.bottom() - m.y())};
        QPainterPath inner;
        for (int i = 0; i < 4; ++i) {
            const QPointF prev = c[(i + 3) % 4], next = c[(i + 1) % 4];
            const QPointF from = (prev + c[i] * 4) / 5, to = (next + c[i] * 4) / 5;
            if (i == 0) inner.moveTo(from);
            else inner.lineTo(from);
            inner.quadTo(c[i], to);
        }
        inner.closeSubpath();
        QPainterPath mask;
        mask.addRect(d);
        p.fillPath(mask.subtracted(inner), QColor(30, 70, 200, 128));
    }
}

void VideoView::releaseResources() {
    m_frameQuad.release();
    m_overlayQuad.release();
    m_frameTex.reset();
    m_overlayTex.reset();
    m_pendingInit = nullptr;
}

void VideoView::mousePressEvent(QMouseEvent *e) {
    if (m_box->m_zoomMode && e->button() == Qt::LeftButton) {
        m_zoomDragging = true;
        m_zoomDragStart = e->position();
        return;
    }
    if (m_box->m_tools->tool() == VisualTools::Pointer && e->button() == Qt::LeftButton &&
        !(e->modifiers() & Qt::ControlModifier) && m_zoom != QRectF(0, 0, 1, 1)) {
        m_panning = true;
        m_panFrom = e->position();
        m_panZoom = m_zoom;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_box->m_tools->press(e)) updateOverlay();
}

void VideoView::mouseMoveEvent(QMouseEvent *e) {
    if (m_panning) {
        const QRectF d = displayRect();
        const QPointF by = e->position() - m_panFrom;
        QRectF z = m_panZoom.translated(-by.x() / d.width() * m_panZoom.width(), -by.y() / d.height() * m_panZoom.height());
        z.moveLeft(std::clamp(z.left(), 0.0, 1 - z.width()));
        z.moveTop(std::clamp(z.top(), 0.0, 1 - z.height()));
        m_zoom = z;
        updateOverlay(); // the frame and the handles with it
        return;
    }
    if (m_zoomDragging || m_box->m_tools->move(e)) updateOverlay();
}

void VideoView::leaveEvent(QEvent *) { updateOverlay(); } // the crosshair goes with the mouse

void VideoView::mouseReleaseEvent(QMouseEvent *e) {
    if (m_panning) {
        m_panning = false;
        unsetCursor();
        return;
    }
    if (m_zoomDragging) {
        m_zoomDragging = false;
        m_box->m_zoomMode = false;
        const QRectF d = displayRect();
        auto uv = [&](QPointF p) {
            return QPointF(m_zoom.left() + (p.x() - d.left()) / d.width() * m_zoom.width(),
                           m_zoom.top() + (p.y() - d.top()) / d.height() * m_zoom.height());
        };
        setZoom(QRectF(uv(m_zoomDragStart), uv(e->position())));
        return;
    }
    if (m_box->m_tools->release(e)) updateOverlay();
}

void VideoView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (m_box->m_tools->doubleClick(e)) return updateOverlay();
    if (m_box->m_tools->tool() == VisualTools::Pointer && e->button() == Qt::LeftButton)
        m_box->setFullscreen(!m_box->isFullScreen());
}

void VideoView::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        emit m_box->heightDelta(e->angleDelta().y() / 4);
        return;
    }
    if (m_box->m_tools->tool() != VisualTools::Pointer) return;
    // zoom 100%..1000% around the cursor
    const double f = e->angleDelta().y() > 0 ? 1 / 1.2 : 1.2;
    const QRectF d = displayRect();
    const QPointF c(m_zoom.left() + (e->position().x() - d.left()) / d.width() * m_zoom.width(),
                    m_zoom.top() + (e->position().y() - d.top()) / d.height() * m_zoom.height());
    const double nw = std::clamp(m_zoom.width() * f, 0.1, 1.0), nh = std::clamp(m_zoom.height() * f, 0.1, 1.0);
    QRectF z(c.x() - (c.x() - m_zoom.left()) * nw / m_zoom.width(), c.y() - (c.y() - m_zoom.top()) * nh / m_zoom.height(),
             nw, nh);
    z.moveLeft(std::clamp(z.left(), 0.0, 1 - nw));
    z.moveTop(std::clamp(z.top(), 0.0, 1 - nh));
    setZoom(z);
}

void VideoView::keyPressEvent(QKeyEvent *e) {
    if (m_box->m_tools->key(e)) {
        updateOverlay();
        return;
    }
    if (e->matches(QKeySequence::Copy)) { // Aegisub's "copy coordinates": the script point under the mouse
        const QPointF p = toScript(mapFromGlobal(QCursor::pos()).toPointF());
        QApplication::clipboard()->setText(QString("%1,%2").arg(qRound(p.x())).arg(qRound(p.y())));
        return;
    }
    // Kainote's volume keys: Num . louder, Num 0 quieter
    const int k = e->key();
    if (e->modifiers() & Qt::KeypadModifier && (k == Qt::Key_Period || k == Qt::Key_Comma || k == Qt::Key_0)) {
        m_box->m_volume->setValue(m_box->m_volume->value() + (k == Qt::Key_0 ? -5 : 5));
        return;
    }
    switch (e->key()) {
    case Qt::Key_Space: m_box->togglePlay(); return;
    case Qt::Key_L: m_box->seekMs(m_box->time() + 5000); return;
    case Qt::Key_Semicolon: m_box->seekMs(m_box->time() - 5000); return;
    case Qt::Key_Up: m_box->seekMs(m_box->time() + 60000); return;
    case Qt::Key_Down: m_box->seekMs(m_box->time() - 60000); return;
    case Qt::Key_Left: m_box->stepFrames(-1); return;
    case Qt::Key_Right: m_box->stepFrames(1); return;
    case Qt::Key_Period: m_box->openSibling(1); return;
    case Qt::Key_Comma: m_box->openSibling(-1); return;
    case Qt::Key_N: m_box->stepChapter(-1); return; // Kainote's chapter keys
    case Qt::Key_M: m_box->stepChapter(1); return;
    case Qt::Key_F: m_box->setFullscreen(!m_box->isFullScreen()); return;
    case Qt::Key_Escape: m_box->setFullscreen(false); return;
    case Qt::Key_B:
        m_box->setFullscreen(false);
        window()->showMinimized();
        return;
    }
    QRhiWidget::keyPressEvent(e);
}

void VideoView::contextMenuEvent(QContextMenuEvent *e) {
    if (m_box->m_tools->tool() != VisualTools::Pointer) return; // right button belongs to the tools
    auto *mw = m_box->tab()->main();
    QMenu m(this);
    const QPointF sp = toScript(e->pos());
    m.addAction(tr("Copy video position"), this, [sp] {
        QApplication::clipboard()->setText(QString("%1,%2").arg(qRound(sp.x())).arg(qRound(sp.y())));
    });
    m.addAction(m_box->playing() ? tr("Pause") : tr("Play"), m_box, &VideoBox::togglePlay);
    m.addAction(tr("Stop"), m_box, [this] {
        m_box->pause();
        m_box->seekFrame(0);
    });
    m.addAction(m_box->isFullScreen() ? tr("Exit full screen") : tr("Full screen"), m_box,
                [this] { m_box->setFullscreen(!m_box->isFullScreen()); });
    if (const auto screens = QGuiApplication::screens(); screens.size() > 1)
        for (int i = 0; i < screens.size(); ++i)
            m.addAction(tr("Full screen on monitor %1").arg(i + 1), m_box,
                        [this, s = screens[i]] { m_box->setFullscreen(true, s); })
                ->setEnabled(m_box->hasVideo() && !(m_box->isFullScreen() && m_box->screen() == screens[i]));
    m.addAction(m_box->detached() ? tr("Attach video") : tr("Detach video"), m_box,
                [this] { m_box->setDetached(!m_box->detached()); });
    if (m_box->tab()->playerMode())
        m.addAction(tr("Open editor"), m_box, [this] { m_box->tab()->setPlayerMode(false); });
    m.addMenu(mw->recentMenu("subs"));
    m.addMenu(mw->recentMenu("video"));
    m.addAction(mw->action("openVideo"));
    m.addAction(mw->action("openSubs"));
    m.addAction(m_box->m_seek->isVisible() ? tr("Hide progress bar") : tr("Show progress bar"), m_box,
                [this] { m_box->m_seek->setVisible(!m_box->m_seek->isVisible()); });
    if (const auto &ch = m_box->chapters(); !ch.empty()) {
        auto *chMenu = m.addMenu(tr("Chapters"));
        for (size_t i = 0; i < ch.size(); ++i) {
            const qint64 f = ch[i].first;
            auto *a = chMenu->addAction(QString("%1\t[%2]").arg(ch[i].second, fs::fmtTime(m_box->timeAt(f))), m_box,
                                        [this, f] { m_box->seekFrame(f); });
            a->setCheckable(true); // the one playing
            a->setChecked(f <= m_box->frame() && (i + 1 == ch.size() || ch[i + 1].first > m_box->frame()));
        }
    }
    // Aegisub's aspect ratio modes, kept in the script's Video AR Mode/Value
    auto *arMenu = m.addMenu(tr("Change aspect ratio"));
    const auto setMode = [this](int mode, double custom) {
        m_box->setAspect(VideoBox::aspectOfMode(mode, custom));
        m_box->tab()->s().set_project_value("Video AR Mode", fs::Utf8(mode ? QString::number(mode) : QString()));
        m_box->tab()->s().set_project_value("Video AR Value",
                                            fs::Utf8(mode == 4 ? QString::number(custom) : QString()));
    };
    const int cur = fs::q(m_box->tab()->s().project_value("Video AR Mode")).toInt();
    const char *modes[] = {QT_TR_NOOP("Default"), QT_TR_NOOP("Full screen (4:3)"), QT_TR_NOOP("Widescreen (16:9)"),
                           QT_TR_NOOP("Cinematic (2.35)"), QT_TR_NOOP("Custom...")};
    for (int i = 0; i < 5; ++i) {
        auto *a = arMenu->addAction(tr(modes[i]), m_box, [this, setMode, i] {
            if (i < 4) return setMode(i, 0);
            bool ok;
            const QRectF r = displayRect();
            const double ar =
                QInputDialog::getDouble(this, tr("Aspect ratio"), tr("Display aspect ratio (width / height)"),
                                        r.width() / std::max(1.0, r.height()), 0.5, 5, 4, &ok);
            if (ok) setMode(4, ar);
        });
        a->setCheckable(true);
        a->setChecked(i == cur);
    }
    m.addSeparator();
    for (const char *id : {"saveFrameSubs", "copyFrameSubs", "saveFrame", "copyFrame", "saveFrameSubsOnly",
                           "copyFrameSubsOnly"})
        m.addAction(mw->action(id));
    m.addSeparator();
    m.addAction(mw->action("closeVideo"));
    m.addAction(tr("Remove video"), this, [this, mw] {
        const QString p = m_box->path();
        if (p.isEmpty() || !mw->confirm(tr("Move %1 to the recycle bin?").arg(QFileInfo(p).fileName()))) return;
        m_box->close();
        QFile::moveToTrash(p);
    })->setEnabled(!m_box->path().isEmpty());
    m.exec(e->globalPos());
}

// ---- box ----------------------------------------------------------------------

namespace {
// The seek bar with the keyframes and chapters marked, the chapters named in tooltips (Kainote).
// A click jumps there, Shift+click to the nearest keyframe; the wheel steps a frame, with Shift a
// keyframe (Aegisub)
class SeekBar : public QSlider {
public:
    explicit SeekBar(VideoBox *box) : QSlider(Qt::Horizontal, box), m_box(box) {
        setFocusPolicy(Qt::NoFocus); // its keys would move it without the video; the window's keys seek
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        QSlider::paintEvent(e);
        QPainter p(this);
        const auto t = track();
        const auto &kf = m_box->tab()->keyframes();
        // shortcut: a video's own keyframes (a thousand an episode) would fill the bar, so they show only when
        // a few pixels apart, as a loaded scene-change list is; a zoomed bar if people want them all
        if (!kf.empty() && kf.size() * 4 <= size_t(std::max(0, t.second))) {
            QColor c = palette().color(QPalette::Text);
            c.setAlpha(110);
            p.setPen(c);
            int last = INT_MIN;
            for (const auto f : kf)
                if (const int x = xAt(f, t); x != last) { // one line a pixel
                    p.drawLine(x, height() - height() / 4, x, height());
                    last = x;
                }
        }
        p.setPen(QPen(palette().color(QPalette::Highlight), 2));
        for (const auto &c : m_box->chapters()) {
            const int x = xAt(c.first, t);
            p.drawLine(x, 0, x, height() / 3);
            p.drawLine(x, height() - height() / 3, x, height());
        }
    }
    bool event(QEvent *e) override {
        if (e->type() != QEvent::ToolTip) return QSlider::event(e);
        const auto *h = static_cast<QHelpEvent *>(e);
        const auto t = track();
        for (const auto &[f, title] : m_box->chapters())
            if (std::abs(xAt(f, t) - h->pos().x()) <= 4) {
                QToolTip::showText(h->globalPos(), QString("%1 [%2]").arg(title, fs::fmtTime(m_box->timeAt(f))), this);
                return true;
            }
        QToolTip::hideText();
        return true;
    }
    void mousePressEvent(QMouseEvent *e) override {
        QStyleOptionSlider o;
        initStyleOption(&o);
        const bool onHandle =
            style()->subControlRect(QStyle::CC_Slider, &o, QStyle::SC_SliderHandle, this).contains(e->position().toPoint());
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        if (e->button() != Qt::LeftButton || (onHandle && !shift)) return QSlider::mousePressEvent(e);
        const auto t = track();
        const qint64 f = QStyle::sliderValueFromPosition(minimum(), maximum(), e->position().toPoint().x() - t.first, t.second);
        if (shift) {
            const auto &kf = m_box->tab()->keyframes();
            if (kf.empty()) return;
            auto it = std::lower_bound(kf.begin(), kf.end(), f);
            if (it == kf.end() || (it != kf.begin() && f - *std::prev(it) < *it - f)) --it;
            return m_box->seekFrame(*it);
        }
        m_box->seekFrame(f);
        QSlider::mousePressEvent(e); // the handle is under the mouse now: dragging on goes on seeking
    }
    void wheelEvent(QWheelEvent *e) override {
        const int d = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
        if (!d) return;
        if (e->modifiers() & Qt::ShiftModifier)
            m_box->tab()->main()->action(d > 0 ? "prevKeyframe" : "nextKeyframe")->trigger();
        else m_box->stepFrames(d > 0 ? -1 : 1);
    }

private:
    // where the handle's centre runs: the left end and the length, asked of the style once
    std::pair<int, int> track() const {
        QStyleOptionSlider o;
        initStyleOption(&o);
        const QRect g = style()->subControlRect(QStyle::CC_Slider, &o, QStyle::SC_SliderGroove, this);
        const QRect h = style()->subControlRect(QStyle::CC_Slider, &o, QStyle::SC_SliderHandle, this);
        return {g.x() + h.width() / 2, g.width() - h.width()};
    }
    int xAt(qint64 frame, std::pair<int, int> t) const {
        return t.first + QStyle::sliderPositionFromValue(minimum(), maximum(), int(frame), t.second);
    }
    VideoBox *m_box;
};

QToolButton *button(QWidget *parent, const char *name, const QString &tip) {
    auto *b = new QToolButton(parent);
    b->setIcon(icon(name, QColor(0x4cc38a)));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFixedSize(26, 26);
    return b;
}
} // namespace

VideoBox::VideoBox(TabPanel *tab) : QWidget(tab), m_tab(tab), m_media(std::make_shared<VideoMedia>()) {
    m_worker = new FrameWorker(m_media);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
    connect(m_worker, &FrameWorker::ready, this, [this](quint64, qint64, QImage img) {
        m_view->setFrame(img);
        // a tool's handles come and go with the line's frames (Aegisub redraws them on each frame);
        // the move tool marks where the line is on each, the position tool the other selected lines on it
        const int tool = m_tools->tool();
        if (const bool shown = m_tools->shown();
            shown != m_toolsShown || tool == VisualTools::Pos || (shown && tool == VisualTools::Move)) {
            m_toolsShown = shown;
            m_view->updateOverlay();
        }
        m_busy = false;
        if (m_pending) {
            m_pending = false;
            request();
        }
    });
    connect(m_worker, &FrameWorker::failed, this, [this](const QString &e) { m_tab->main()->status(e); });

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    m_view = new VideoView(this);
    v->addWidget(m_view, 1);
    m_seek = new SeekBar(this);
    v->addWidget(m_seek);
    connect(m_seek, &QSlider::sliderMoved, this, [this](int f) { seekFrame(f); });

    m_controls = new QWidget(this);
    auto *row = new QHBoxLayout(m_controls);
    row->setContentsMargins(2, 0, 2, 0);
    row->setSpacing(2);
    auto *prev = button(m_controls, "player-skip-back", tr("Previous file"));
    auto *playBtn = button(m_controls, "player-play-filled", tr("Play / Pause"));
    auto *playLine = button(m_controls, "player-track-next", tr("Play the current line"));
    auto *stop = button(m_controls, "player-stop-filled", tr("Stop"));
    auto *next = button(m_controls, "player-skip-forward", tr("Next file"));
    for (auto *b : {prev, playBtn, playLine, stop, next}) row->addWidget(b);
    connect(prev, &QToolButton::clicked, this, [this] { openSibling(-1); });
    connect(next, &QToolButton::clicked, this, [this] { openSibling(1); });
    connect(playBtn, &QToolButton::clicked, this, &VideoBox::togglePlay);
    connect(playLine, &QToolButton::clicked, this, [this] { m_tab->playLine(m_tab->active()); });
    connect(stop, &QToolButton::clicked, this, [this] {
        pause();
        seekFrame(0);
    });
    m_timeField = new QLineEdit(m_controls);
    m_timeField->setReadOnly(true);
    m_timeField->setMinimumWidth(fontMetrics().horizontalAdvance("0:00:00,000; 000000; 00; -00000ms, -00000ms") + 12);
    row->addWidget(m_timeField, 1);
    m_volume = new QSlider(Qt::Horizontal, m_controls);
    m_volume->setRange(0, 100);
    m_volume->setValue(QSettings().value("video/volume", 90).toInt());
    m_volume->setMaximumWidth(110);
    m_volume->setToolTip(tr("Volume"));
    row->addWidget(m_volume);
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        QSettings().setValue("video/volume", v);
        m_tab->audio()->setVolume(v / 100.0f);
    });
    v->addWidget(m_controls);

    m_tools = new VisualTools(this);
    m_toolRow = new QWidget(this);
    auto *rows = new QVBoxLayout(m_toolRow);
    rows->setContentsMargins(2, 0, 2, 0);
    rows->setSpacing(0);
    auto *tr_ = new QHBoxLayout;
    rows->addLayout(tr_);
    tr_->addWidget(m_tools->toolRow(m_toolRow), 1);
    m_seekOn = new QComboBox(m_toolRow);
    m_seekOn->setToolTip(tr("Move video to selected line on:"));
    m_seekOn->addItems({tr("Double-clicking a line (always on)"), tr("Every line change"), tr("Clicking a line or editing when paused"),
                        tr("Clicking a line or editing"), tr("Editing line when paused"), tr("Editing")});
    m_playOn = new QComboBox(m_toolRow);
    m_playOn->setToolTip(tr("On moving to another line play:"));
    m_playOn->addItems({tr("Nothing"), tr("Audio to line end"), tr("Video and audio to line end"),
                        tr("Video and audio to next line start")});
    QSettings st;
    m_seekOn->setCurrentIndex(st.value("video/seekOn", SeekClickOrEditPaused).toInt());
    m_playOn->setCurrentIndex(st.value("video/playOn", PlayNothing).toInt());
    connect(m_seekOn, &QComboBox::currentIndexChanged, this, [](int i) { QSettings().setValue("video/seekOn", i); });
    connect(m_playOn, &QComboBox::currentIndexChanged, this, [](int i) { QSettings().setValue("video/playOn", i); });
    // as wide as their texts when there is room, else down to ten characters (the list shows them whole)
    for (auto *c : {m_seekOn, m_playOn}) c->setMinimumWidth(10 * c->fontMetrics().averageCharWidth());
    tr_->addWidget(m_playOn); // Kainote order: play mode, then seek mode
    tr_->addWidget(m_seekOn);
    // the tool's options below, like Aegisub's sub-toolbar: one row next to the modes did not fit
    // a video in the default layout
    rows->addWidget(m_tools->options());
    v->addWidget(m_toolRow);

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(5);
    connect(m_timer, &QTimer::timeout, this, &VideoBox::tick);
    updateTimeField();
}

VideoBox::~VideoBox() {
    *m_openCancel = true;
    m_worker->latest = ~0ull; // drop queued renders
    m_thread.quit();
    m_thread.wait();
}

int VideoBox::seekOn() const { return m_seekOn->currentIndex(); }
int VideoBox::playOn() const { return m_playOn->currentIndex(); }

std::vector<std::int64_t> VideoBox::timecodes() const { return m_dummyFps > 0 ? std::vector<std::int64_t>{} : m_timecodes; }
std::vector<std::int64_t> VideoBox::keyframes() const { return m_keyframes; }

void VideoBox::setTimecodes(std::vector<std::int64_t> tc) {
    if (tc.empty()) {
        std::lock_guard g(m_media->m);
        if (!m_media->video) return; // a dummy keeps its constant rate
        const auto own = (**m_media->video).timecodes();
        tc.assign(own.begin(), own.end());
    }
    m_timecodes = std::move(tc);
    m_scriptAt = -2; // frame times moved
    updateTimeField();
}

void VideoBox::open(const QString &path, qint64 frame, double ar) {
    const QString name = QFileInfo(path).fileName();
    m_tab->main()->status(tr("Indexing %1…").arg(name));
    *m_openCancel = true; // a file still indexing is replaced
    m_openCancel = std::make_shared<std::atomic_bool>(false);
    auto media = m_media;
    auto cancel = m_openCancel;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex";
    QMetaObject::invokeMethod(m_worker, [this, media, path, name, cache, cancel, frame, ar] {
        QString err;
        std::vector<std::pair<qint64, QString>> chapters; // start ms, title
        {
            std::lock_guard g(media->m);
            try {
                const fs::PercentProgress progress([this, name, cancel](int pct) {
                    QMetaObject::invokeMethod(this, [this, name, pct, cancel] {
                        if (!*cancel) m_tab->main()->status(tr("Indexing %1… %2%").arg(name).arg(pct));
                    });
                    return !*cancel;
                });
                auto v = fsb::open_video(fs::Utf8(path), fs::Utf8(cache), progress);
                const auto info = v->info();
                media->width = info.width;
                media->height = info.height;
                media->video.emplace(std::move(v));
                media->cachedFrame = -1;
                if (!media->renderer) media->renderer.emplace(fsb::new_renderer());
                (*media->renderer)->load_attached_fonts(fs::Utf8(path));
                for (const auto &c : fsb::chapters(fs::Utf8(path)))
                    chapters.emplace_back(c.start, c.title.empty() ? tr("Chapter %1").arg(chapters.size() + 1)
                                                                   : fs::q(c.title));
                media->scriptDirty = true;
            } catch (const rust::Error &e) {
                err = QString::fromUtf8(e.what());
            }
        }
        QMetaObject::invokeMethod(this, [this, path, err, cancel, frame, ar, chapters] {
            if (*cancel) { // closed, or replaced by a newer open (which has its own status)
                if (cancel == m_openCancel) m_tab->main()->status(QString());
                return;
            }
            if (!err.isEmpty()) {
                m_tab->main()->error(err);
                return;
            }
            {
                std::lock_guard g(m_media->m); // released before opened(): its slots lock it again
                const auto &v = **m_media->video;
                m_info = v.info();
                const auto tc = v.timecodes(), kf = v.keyframes();
                m_timecodes.assign(tc.begin(), tc.end());
                m_keyframes.assign(kf.begin(), kf.end());
            }
            if (ar > 0) setAspect(ar);
            m_frames = m_info.frames;
            m_dummyFps = 0;
            m_dummySpec.clear();
            m_path = path;
            m_frame = std::clamp<qint64>(frame, 0, std::max<qint64>(0, m_frames - 1));
            m_scriptAt = -2;
            m_seek->setRange(0, int(std::max<qint64>(0, m_frames - 1)));
            m_chapters.clear();
            for (const auto &[ms, title] : chapters) m_chapters.emplace_back(frameAtStart(ms), title);
            m_tab->main()->status(QString());
            emit opened();
        });
    });
}

void VideoBox::setAspect(double ar) {
    if (ar > 0) {
        m_info.sar_num = int(ar * m_info.height * 1000);
        m_info.sar_den = std::max(1, m_info.width * 1000);
    } else {
        std::lock_guard g(m_media->m);
        const auto own = m_media->video ? (*m_media->video)->info() : fsb::VideoInfo{};
        m_info.sar_num = own.sar_num;
        m_info.sar_den = own.sar_den;
    }
    m_view->updateOverlay();
    m_tab->fitVideo();
}

double VideoBox::aspectOfMode(int mode, double custom) {
    const double ar[] = {0, 4.0 / 3, 16.0 / 9, 2.35, std::clamp(custom, 0.0, 5.0)};
    return mode > 0 && mode < 5 ? ar[mode] : 0;
}

QString VideoBox::dummySpec(double fps, qint64 frames, int w, int h, QColor c, bool pattern) {
    return QString("?dummy:%1:%2:%3:%4:%5:%6:%7:%8")
        .arg(QString::number(fps, 'g', 10))
        .arg(frames)
        .arg(w)
        .arg(h)
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(pattern ? "c" : "");
}

bool VideoBox::openDummy(const QString &spec) {
    const QStringList t = spec.mid(7).split(':');
    if (!spec.startsWith("?dummy:") || t.size() != 8) return false;
    bool ok = true;
    const auto num = [&](int k, qint64 lo, qint64 hi) {
        const qint64 v = t[k].toLongLong(&ok);
        if (v < lo || v > hi) ok = false;
        return v;
    };
    double fps = t[0].toDouble();
    if (const QStringList nd = t[0].split('/'); nd.size() == 2 && nd[1].toDouble() > 0) fps = nd[0].toDouble() / nd[1].toDouble();
    const qint64 frames = num(1, 1, 100'000'000);
    if (!ok) return false;
    const int w = int(num(2, 16, 8192));
    if (!ok) return false;
    const int h = int(num(3, 16, 8192));
    if (!ok) return false;
    int rgb[3];
    for (int k = 0; k < 3 && ok; ++k) rgb[k] = int(num(4 + k, 0, 255));
    if (!ok || !(fps > 0 && fps <= 1000)) return false;
    const qint64 ms = qint64(frames * 1000 / fps);
    *m_openCancel = true; // a video still being indexed would replace it
    {
        std::lock_guard g(m_media->m);
        m_media->video.reset();
        m_media->cachedFrame = -1;
        m_media->cached = QImage();
        m_media->dummyColor = QColor(rgb[0], rgb[1], rgb[2]);
        m_media->dummyPattern = t[7] == "c";
        m_media->width = w;
        m_media->height = h;
        if (!m_media->renderer) m_media->renderer.emplace(fsb::new_renderer());
        m_media->scriptDirty = true;
    }
    m_info = {};
    m_info.width = w;
    m_info.height = h;
    m_info.fps = fps;
    m_info.frames = frames;
    m_info.duration_ms = ms;
    m_frames = m_info.frames;
    m_dummyFps = fps;
    m_dummySpec = spec;
    m_timecodes.clear();
    for (qint64 f = 0; f <= m_frames; ++f) m_timecodes.push_back(qint64(f * 1000 / fps));
    m_keyframes.clear();
    m_chapters.clear();
    m_path.clear();
    m_frame = 0;
    m_scriptAt = -2;
    m_seek->setRange(0, int(std::max<qint64>(0, m_frames - 1)));
    emit opened();
    return true;
}

void VideoBox::close() {
    pause();
    *m_openCancel = true; // else the lock below waits for the indexing to finish
    {
        std::lock_guard g(m_media->m);
        m_media->video.reset();
        m_media->cachedFrame = -1;
        m_media->cached = QImage();
    }
    m_frames = 0;
    m_chapters.clear();
    m_path.clear();
    m_dummySpec.clear();
    m_info = {};
    m_view->setFrame(QImage());
    emit opened();
}

void VideoBox::stepChapter(int dir) {
    const auto by = [](const auto &c) { return c.first; };
    if (dir > 0) {
        const auto it = std::ranges::upper_bound(m_chapters, m_frame, {}, by);
        if (it != m_chapters.end()) seekFrame(it->first);
    } else {
        const auto it = std::ranges::lower_bound(m_chapters, m_frame, {}, by);
        if (it != m_chapters.begin()) seekFrame(std::prev(it)->first);
    }
}

qint64 VideoBox::frameAt(qint64 ms) const {
    if (m_timecodes.empty()) return 0;
    auto it = std::upper_bound(m_timecodes.begin(), m_timecodes.end(), ms);
    const qint64 f = qint64(it - m_timecodes.begin()) - 1;
    return std::clamp<qint64>(f, 0, std::max<qint64>(0, m_frames - 1));
}

qint64 VideoBox::frameAtStart(qint64 ms) const {
    if (m_timecodes.empty()) return 0;
    const auto it = std::lower_bound(m_timecodes.begin(), m_timecodes.end(), ms);
    return std::clamp<qint64>(qint64(it - m_timecodes.begin()), 0, std::max<qint64>(0, m_frames - 1));
}

qint64 VideoBox::timeAt(qint64 frame) const {
    if (m_timecodes.empty()) return 0;
    const qint64 n = qint64(m_timecodes.size());
    if (frame < n) return m_timecodes[size_t(std::max<qint64>(0, frame))];
    const qint64 last = n > 1 ? m_timecodes[n - 1] - m_timecodes[n - 2] : 40;
    return m_timecodes[n - 1] + (frame - n + 1) * last;
}

void VideoBox::seekMs(qint64 ms) { seekFrame(frameAt(ms)); }

void VideoBox::seekFrame(qint64 n) {
    if (!hasVideo()) return;
    n = std::clamp<qint64>(n, 0, m_frames - 1);
    m_frame = n;
    if (m_playing) {
        m_playStartMs = time();
        m_clock.restart();
        if (m_tab->audio()->hasAudio()) m_tab->audio()->play(time(), m_playEnd >= 0 ? m_playEnd : m_info.duration_ms, true);
    }
    request();
    m_seek->setValue(int(n));
    updateTimeField();
    emit timeChanged(time(), n);
}

void VideoBox::request() {
    if (!hasVideo()) return;
    ensureScript();
    if (m_busy) {
        m_pending = true;
        return;
    }
    m_busy = true;
    const quint64 id = ++m_reqId;
    m_worker->latest = id;
    QMetaObject::invokeMethod(m_worker, "render", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(qint64, m_frame),
                              Q_ARG(qint64, time()), Q_ARG(bool, true));
}

// Aegisub's export strategy: right after an edit libass gets only the lines of
// the shown frame (cheap for long scripts); the whole script once another frame
// is wanted, since edits and seeks tend to come in runs.
void VideoBox::loadScript(bool shownFrameOnly) {
    auto script = m_tab->s().script_for_render(m_tab->hideOriginal(), shownFrameOnly ? time() : -1);
    std::lock_guard g(m_media->m);
    m_media->script = std::move(script);
    m_media->scriptDirty = true;
    m_scriptAt = shownFrameOnly ? m_frame : -1;
}

void VideoBox::ensureScript() {
    if (m_scriptAt != -1 && m_scriptAt != m_frame) loadScript(false);
}

void VideoBox::subsChanged() {
    if (hasVideo()) loadScript(true);
    else m_scriptAt = -2; // built when a video opens
    m_tools->reload();
    m_view->updateOverlay();
    request();
}

void VideoBox::lineChanged() {
    m_tools->reload();
    m_view->updateOverlay();
    updateTimeField();
}

void VideoBox::play() {
    if (!hasVideo() || m_playing) return;
    m_playing = true;
    m_playStartMs = time();
    m_clock.restart();
    if (m_tab->audio()->hasAudio()) m_tab->audio()->play(time(), m_playEnd >= 0 ? m_playEnd : m_info.duration_ms, true);
    m_timer->start();
}

void VideoBox::pause() {
    if (!m_playing) return;
    m_playing = false;
    m_playEnd = -1;
    m_timer->stop();
    m_tab->audio()->stop();
}

void VideoBox::togglePlay() {
    if (m_playing) pause();
    else play();
}

void VideoBox::playRange(qint64 from, qint64 to) {
    pause();
    seekMs(from);
    m_playEnd = to;
    play();
}

void VideoBox::tick() {
    auto *audio = m_tab->audio();
    qint64 pos = audio->hasAudio() && audio->playing() ? audio->positionMs() : m_playStartMs + m_clock.elapsed();
    if ((m_playEnd >= 0 && pos >= m_playEnd) || pos >= m_info.duration_ms) {
        pause();
        return;
    }
    const qint64 f = frameAt(pos);
    if (f != m_frame) {
        m_frame = f;
        request();
        m_seek->setValue(int(f));
        updateTimeField();
        emit timeChanged(time(), f);
    }
}

void VideoBox::updateTimeField() {
    const qint64 t = time();
    QString s = QString("%1:%2:%3,%4; %5")
                    .arg(t / 3600000)
                    .arg(t / 60000 % 60, 2, 10, QChar('0'))
                    .arg(t / 1000 % 60, 2, 10, QChar('0'))
                    .arg(t % 1000, 3, 10, QChar('0'))
                    .arg(m_frame);
    const int row = m_tab->active();
    if (row >= 0 && size_t(row) < m_tab->s().row_count()) {
        const auto r = m_tab->s().row(size_t(row));
        s += QString("; %1; %2ms, %3ms").arg(m_frame - frameAt(r.start)).arg(t - r.start).arg(t - r.end);
    }
    m_timeField->setText(s);
    const bool kf = std::binary_search(m_keyframes.begin(), m_keyframes.end(), m_frame);
    m_timeField->setStyleSheet(kf ? "background: #7a5a10" : "");
}

void VideoBox::setPlayerMode(bool on) { m_toolRow->setVisible(!on); }

void VideoBox::undock() {
    auto *sp = qobject_cast<QSplitter *>(parentWidget());
    m_fsParent = parentWidget();
    m_fsIndex = sp ? sp->indexOf(this) : -1;
    setParent(nullptr);
    setAttribute(Qt::WA_QuitOnClose, false); // closing the main window still quits
}

void VideoBox::dock() {
    if (auto *sp = qobject_cast<QSplitter *>(m_fsParent)) sp->insertWidget(m_fsIndex, this);
    else setParent(m_fsParent);
    show();
}

void VideoBox::setFullscreen(bool on, QScreen *on_screen) {
    if (on) {
        QScreen *target = on_screen ? on_screen : screen(); // still the main window's while docked
        if (isFullScreen() && target == screen()) return;
        if (isFullScreen()) {
            showNormal(); // to another monitor (Kainote)
        } else {
            if (!m_detached) undock();
            m_toolRow->hide();
        }
        // Qt fills the screen the window is on; an undocked box's old position means nothing
        if (!m_detached || target != screen()) move(target->geometry().topLeft());
        showFullScreen();
    } else {
        if (!isFullScreen()) return;
        showNormal();
        if (!m_detached) dock();
        m_toolRow->setVisible(!m_tab->playerMode());
    }
    m_view->updateOverlay();
}

void VideoBox::setDetached(bool on) {
    if (on == m_detached) return;
    setFullscreen(false);
    QSettings st;
    if (on) {
        undock();
        setWindowTitle(m_tab->title() + " - FS Editor");
        if (!restoreGeometry(st.value("video/detachedGeometry").toByteArray())) resize(800, 520);
        show();
    } else {
        st.setValue("video/detachedGeometry", saveGeometry());
        dock();
    }
    m_detached = on;
    m_view->updateOverlay();
}

void VideoBox::closeEvent(QCloseEvent *e) {
    if (!m_detached) return QWidget::closeEvent(e);
    e->ignore();
    QTimer::singleShot(0, this, [this] { setDetached(false); }); // not while the window is closing
}

void VideoBox::resetZoom() { m_view->setZoom({0, 0, 1, 1}); }

QImage VideoBox::grabFrame(Grab mode) {
    if (!hasVideo()) return {};
    ensureScript();
    std::lock_guard g(m_media->m);
    QImage img;
    try {
        if (mode == SubsOnly) {
            img = QImage(m_media->width, m_media->height, QImage::Format_RGBA8888_Premultiplied);
            img.fill(Qt::transparent);
        } else {
            img = decode(*m_media, m_frame);
        }
        if (mode != Raw && m_media->renderer) {
            const rust::Slice<uint8_t> px(img.bits(), size_t(img.sizeInBytes()));
            const size_t w = size_t(img.width()), h = size_t(img.height());
            (*m_media->renderer)->set_script(m_media->script);
            m_media->scriptDirty = false;
            if (mode == SubsOnly) (*m_media->renderer)->overlay(time(), w, h, w, h, px);
            else (*m_media->renderer)->blend(time(), w, h, w, h, px);
        }
    } catch (const rust::Error &e) {
        m_tab->main()->error(QString::fromUtf8(e.what()));
    }
    return img;
}

void VideoBox::saveFrame(Grab mode) {
    const QImage img = grabFrame(mode);
    if (img.isNull()) return;
    // a dummy has no folder: the script's, else home (Aegisub)
    const QString script = m_tab->path();
    const QString base = !m_path.isEmpty() ? QFileInfo(m_path).absolutePath() + "/" + QFileInfo(m_path).completeBaseName()
                         : !script.isEmpty() ? QFileInfo(script).absolutePath() + "/dummy"
                                             : QDir::homePath() + "/dummy";
    QString f;
    for (int n = 1; QFileInfo::exists(f = QString("%1_%2_%3.png").arg(base).arg(n, 3, 10, QChar('0')).arg(m_frame)); ++n) {
    }
    if (img.save(f, "PNG")) m_tab->main()->status(tr("Saved %1").arg(QDir::toNativeSeparators(f)));
    else m_tab->main()->error(tr("Could not save %1").arg(QDir::toNativeSeparators(f)));
}

void VideoBox::openSibling(int dir) {
    if (m_path.isEmpty()) return;
    const QFileInfo fi(m_path);
    const QStringList exts = {"*.mkv", "*.mp4", "*.avi", "*.webm", "*.mov", "*.ts", "*.m2ts", "*.wmv", "*.flv", "*.ogm"};
    const QStringList files = fi.dir().entryList(exts, QDir::Files, QDir::Name);
    const int i = int(files.indexOf(fi.fileName()));
    if (i < 0) return;
    const int j = i + dir;
    if (j < 0 || j >= files.size()) return;
    m_tab->openVideo(fi.dir().filePath(files[j]));
}

void VideoBox::wheelEvent(QWheelEvent *e) {
    // the wheel over the control panel changes the volume
    m_volume->setValue(m_volume->value() + e->angleDelta().y() / 24);
}

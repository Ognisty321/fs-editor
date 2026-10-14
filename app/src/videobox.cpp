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
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

// ---- worker -------------------------------------------------------------------

void FrameWorker::render(quint64 id, qint64 frame, qint64 ms, bool subs) {
    if (id < latest.load()) return; // a newer request is queued
    std::lock_guard g(m_media->m);
    QImage img(m_media->width, m_media->height, QImage::Format_RGBA8888);
    const rust::Slice<uint8_t> px(img.bits(), size_t(img.sizeInBytes()));
    try {
        if (m_media->video) (*m_media->video)->frame(frame, px);
        else img.fill(QColor(0x30, 0x30, 0x30)); // dummy video
        if (subs && m_media->renderer) {
            if (m_media->scriptDirty) {
                (*m_media->renderer)->set_script(fs::Utf8(m_media->script));
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
    if (m_box->m_tools->press(e)) updateOverlay();
}

void VideoView::mouseMoveEvent(QMouseEvent *e) {
    if (m_zoomDragging || m_box->m_tools->move(e)) updateOverlay();
}

void VideoView::mouseReleaseEvent(QMouseEvent *e) {
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
    if (m_box->tab()->playerMode())
        m.addAction(tr("Open editor"), m_box, [this] { m_box->tab()->setPlayerMode(false); });
    m.addMenu(mw->recentMenu("subs"));
    m.addMenu(mw->recentMenu("video"));
    m.addAction(mw->action("openVideo"));
    m.addAction(mw->action("openSubs"));
    m.addAction(m_box->m_seek->isVisible() ? tr("Hide progress bar") : tr("Show progress bar"), m_box,
                [this] { m_box->m_seek->setVisible(!m_box->m_seek->isVisible()); });
    m.addAction(tr("Change aspect ratio"), m_box, [this] {
        bool ok;
        const double ar = QInputDialog::getDouble(this, tr("Aspect ratio"), tr("Display aspect ratio (width / height)"),
                                                  double(m_box->m_info.width) / std::max(1, m_box->m_info.height),
                                                  0.1, 10, 4, &ok);
        if (!ok) return;
        // express the override as a sample aspect ratio
        m_box->m_info.sar_num = int(ar * m_box->m_info.height * 1000);
        m_box->m_info.sar_den = m_box->m_info.width * 1000;
        updateOverlay();
    });
    m.addSeparator();
    auto saveFrame = [this](bool subs) {
        const QString f = QFileDialog::getSaveFileName(this, tr("Save frame"), {}, "PNG (*.png)");
        if (!f.isEmpty()) m_box->grabFrame(subs).save(f, "PNG");
    };
    m.addAction(tr("Save frame with subtitles as PNG"), this, [saveFrame] { saveFrame(true); });
    m.addAction(tr("Copy frame with subtitles to clipboard"), this,
                [this] { QApplication::clipboard()->setImage(m_box->grabFrame(true)); });
    m.addAction(tr("Save frame as PNG"), this, [saveFrame] { saveFrame(false); });
    m.addAction(tr("Copy frame to clipboard"), this,
                [this] { QApplication::clipboard()->setImage(m_box->grabFrame(false)); });
    m.addSeparator();
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
    m_seek = new QSlider(Qt::Horizontal, this);
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
    auto *tr_ = new QHBoxLayout(m_toolRow);
    tr_->setContentsMargins(2, 0, 2, 0);
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
    tr_->addWidget(m_playOn); // Kainote order: play mode, then seek mode
    tr_->addWidget(m_seekOn);
    v->addWidget(m_toolRow);

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(5);
    connect(m_timer, &QTimer::timeout, this, &VideoBox::tick);
    updateTimeField();
}

VideoBox::~VideoBox() {
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
    updateTimeField();
}

void VideoBox::open(const QString &path) {
    m_tab->main()->status(tr("Indexing %1…").arg(QFileInfo(path).fileName()));
    auto media = m_media;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex";
    QMetaObject::invokeMethod(m_worker, [this, media, path, cache] {
        QString err;
        {
            std::lock_guard g(media->m);
            try {
                auto v = fsb::open_video(fs::Utf8(path), fs::Utf8(cache));
                const auto info = v->info();
                media->width = info.width;
                media->height = info.height;
                media->video.emplace(std::move(v));
                if (!media->renderer) media->renderer.emplace(fsb::new_renderer());
                (*media->renderer)->load_attached_fonts(fs::Utf8(path));
                media->scriptDirty = true;
            } catch (const rust::Error &e) {
                err = QString::fromUtf8(e.what());
            }
        }
        QMetaObject::invokeMethod(this, [this, path, err] {
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
            m_frames = m_info.frames;
            m_dummyFps = 0;
            m_path = path;
            m_frame = 0;
            m_seek->setRange(0, int(std::max<qint64>(0, m_frames - 1)));
            m_tab->main()->status(QString());
            emit opened();
        });
    });
}

void VideoBox::openDummy(int w, int h, double fps, qint64 ms) {
    {
        std::lock_guard g(m_media->m);
        m_media->video.reset();
        m_media->width = w;
        m_media->height = h;
        if (!m_media->renderer) m_media->renderer.emplace(fsb::new_renderer());
        m_media->scriptDirty = true;
    }
    m_info = {};
    m_info.width = w;
    m_info.height = h;
    m_info.fps = fps;
    m_info.frames = qint64(ms * fps / 1000);
    m_info.duration_ms = ms;
    m_frames = m_info.frames;
    m_dummyFps = fps;
    m_timecodes.clear();
    for (qint64 f = 0; f <= m_frames; ++f) m_timecodes.push_back(qint64(f * 1000 / fps));
    m_keyframes.clear();
    m_path.clear();
    m_frame = 0;
    m_seek->setRange(0, int(std::max<qint64>(0, m_frames - 1)));
    emit opened();
}

void VideoBox::close() {
    pause();
    {
        std::lock_guard g(m_media->m);
        m_media->video.reset();
    }
    m_frames = 0;
    m_path.clear();
    m_info = {};
    m_view->setFrame(QImage());
    emit opened();
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
        if (m_tab->audio()->hasAudio()) m_tab->audio()->play(time(), m_playEnd >= 0 ? m_playEnd : m_info.duration_ms);
    }
    request();
    m_seek->setValue(int(n));
    updateTimeField();
    emit timeChanged(time(), n);
}

void VideoBox::request() {
    if (!hasVideo()) return;
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

void VideoBox::subsChanged() {
    {
        std::lock_guard g(m_media->m);
        m_media->script = fs::q(m_tab->s().script_for_render(m_tab->hideOriginal()));
        m_media->scriptDirty = true;
    }
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
    if (m_tab->audio()->hasAudio()) m_tab->audio()->play(time(), m_playEnd >= 0 ? m_playEnd : m_info.duration_ms);
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

void VideoBox::setFullscreen(bool on) {
    if (on == isFullScreen()) return;
    if (on) {
        auto *sp = qobject_cast<QSplitter *>(parentWidget());
        m_fsParent = parentWidget();
        m_fsIndex = sp ? sp->indexOf(this) : -1;
        setParent(nullptr);
        m_toolRow->hide();
        showFullScreen();
    } else {
        showNormal();
        if (auto *sp = qobject_cast<QSplitter *>(m_fsParent)) sp->insertWidget(m_fsIndex, this);
        else setParent(m_fsParent);
        m_toolRow->setVisible(!m_tab->playerMode());
        show();
    }
    m_view->updateOverlay();
}

void VideoBox::resetZoom() { m_view->setZoom({0, 0, 1, 1}); }

QImage VideoBox::grabFrame(bool withSubs) {
    if (!hasVideo()) return {};
    std::lock_guard g(m_media->m);
    QImage img(m_media->width, m_media->height, QImage::Format_RGBA8888);
    const rust::Slice<uint8_t> px(img.bits(), size_t(img.sizeInBytes()));
    try {
        if (m_media->video) (*m_media->video)->frame(m_frame, px);
        else img.fill(QColor(0x30, 0x30, 0x30));
        if (withSubs && m_media->renderer) {
            (*m_media->renderer)->set_script(fs::Utf8(m_media->script));
            (*m_media->renderer)->blend(time(), size_t(img.width()), size_t(img.height()), size_t(img.width()),
                                        size_t(img.height()), px);
        }
    } catch (const rust::Error &e) {
        m_tab->main()->error(QString::fromUtf8(e.what()));
    }
    return img;
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

// Video box (Kainote layout): GPU video view (QRhi) with libass subtitles,
// seek slider, playback buttons, time field, volume and the visual-tools row.
// Frames are decoded and composited on a worker thread.
#pragma once

#include "bridge.h"
#include "rhiquad.h"

#include <QElapsedTimer>
#include <QImage>
#include <QRhiWidget>
#include <QThread>
#include <QWidget>
#include <atomic>
#include <mutex>
#include <optional>

class TabPanel;
class VisualTools;
class QSlider;
class QLineEdit;
class QComboBox;
class QToolButton;
class QTimer;
class QStackedWidget;

// Decoder state shared with the worker thread.
struct VideoMedia {
    std::mutex m;
    std::optional<rust::Box<fsb::Video>> video;
    std::optional<rust::Box<fsb::Renderer>> renderer;
    int width = 640, height = 480;
    rust::String script;
    bool scriptDirty = false;
};

class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(std::shared_ptr<VideoMedia> m) : m_media(std::move(m)) {}
    std::atomic<quint64> latest{0};
public slots:
    void render(quint64 id, qint64 frame, qint64 ms, bool subs);
signals:
    void ready(quint64 id, qint64 frame, QImage image);
    void failed(QString error);
private:
    std::shared_ptr<VideoMedia> m_media;
};

class VideoView : public QRhiWidget {
    Q_OBJECT
public:
    explicit VideoView(class VideoBox *box);
    void setFrame(const QImage &img);
    void updateOverlay() { m_overlayDirty = true; update(); }
    void paintOverscan(QPainter &p) const;
    // video frame area in widget coordinates
    QRectF displayRect() const;
    // script (PlayRes) coordinates <-> widget coordinates, zoom aware
    QPointF toWidget(QPointF script) const;
    QPointF toScript(QPointF widget) const;
    double scriptScale() const; // widget px per script px (x)
    QRectF zoom() const { return m_zoom; }
    void setZoom(QRectF uv);
    const QImage &frame() const { return m_frame; }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    VideoBox *m_box;
    QImage m_frame;
    bool m_frameDirty = false, m_overlayDirty = true;
    QRectF m_zoom{0, 0, 1, 1};
    QPointF m_zoomDragStart;
    bool m_zoomDragging = false;
    RhiQuad m_frameQuad, m_overlayQuad;
    std::unique_ptr<QRhiTexture> m_frameTex, m_overlayTex;
    QRhiResourceUpdateBatch *m_pendingInit = nullptr;
};

class VideoBox : public QWidget {
    Q_OBJECT
    friend class VideoView;
public:
    enum SeekOn { SeekDoubleClick, SeekEveryChange, SeekClickOrEditPaused, SeekClickOrEdit, SeekEditPaused, SeekEdit };
    enum PlayOn { PlayNothing, PlayAudioToEnd, PlayVideoToEnd, PlayVideoToNextStart };

    explicit VideoBox(TabPanel *tab);
    ~VideoBox() override;

    TabPanel *tab() const { return m_tab; }
    void open(const QString &path);
    void openDummy(int w, int h, double fps, qint64 ms);
    void close();
    bool hasVideo() const { return m_frames > 0; }
    QString path() const { return m_path; }
    fsb::VideoInfo info() const { return m_info; }
    std::vector<std::int64_t> timecodes() const; // int64_t = cxx i64 (qint64 is long long, int64_t is long on Linux)
    std::vector<std::int64_t> keyframes() const;
    void setTimecodes(std::vector<std::int64_t> tc); // empty: the video's own

    qint64 frameAt(qint64 ms) const;      // frame shown at ms
    qint64 frameAtStart(qint64 ms) const; // first frame a line starting at ms is visible on (Aegisub START)
    void seekStart(qint64 ms) { seekFrame(frameAtStart(ms)); }
    qint64 timeAt(qint64 frame) const;
    qint64 time() const { return timeAt(m_frame); }
    qint64 frame() const { return m_frame; }
    qint64 nextFrameTime() const { return timeAt(m_frame + 1); }
    void seekMs(qint64 ms);
    void seekFrame(qint64 n);
    void play();
    void pause();
    void togglePlay();
    void playRange(qint64 from, qint64 to);
    bool playing() const { return m_playing; }
    void stepFrames(int n) { seekFrame(m_frame + n); }

    void subsChanged();
    void lineChanged();
    void setPlayerMode(bool on);
    void setFullscreen(bool on);
    void zoomMode() { m_zoomMode = true; }
    void resetZoom();
    int seekOn() const;
    int playOn() const;
    VisualTools *tools() const { return m_tools; }
    VideoView *view() const { return m_view; }
    QImage grabFrame(bool withSubs);

signals:
    void opened();
    void timeChanged(qint64 ms, qint64 frame);
    void heightDelta(int dy);

protected:
    void wheelEvent(QWheelEvent *) override;

private:
    void request();
    void loadScript(bool shownFrameOnly);
    void ensureScript();
    void tick();
    void updateTimeField();
    void openSibling(int dir);

    TabPanel *m_tab;
    std::shared_ptr<VideoMedia> m_media;
    QThread m_thread;
    FrameWorker *m_worker;
    quint64 m_reqId = 0;
    bool m_busy = false, m_pending = false;
    QString m_path;
    fsb::VideoInfo m_info{};
    qint64 m_frames = 0, m_frame = 0;
    qint64 m_scriptAt = -2; // frame of a one-frame script; -1 the whole script; -2 out of date
    std::vector<std::int64_t> m_timecodes, m_keyframes;
    double m_dummyFps = 0;
    bool m_playing = false, m_zoomMode = false;
    qint64 m_playEnd = -1, m_playStartMs = 0;
    QElapsedTimer m_clock;
    QTimer *m_timer;

    VideoView *m_view;
    VisualTools *m_tools;
    QSlider *m_seek, *m_volume;
    QLineEdit *m_timeField;
    QWidget *m_controls, *m_toolRow;
    QComboBox *m_seekOn, *m_playOn;
    QWidget *m_fsParent = nullptr;
    int m_fsIndex = -1;
};

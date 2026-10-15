// Video box (Kainote layout): GPU video view (QRhi) with libass subtitles,
// seek slider, playback buttons, time field, volume and the visual-tools row.
// Frames are decoded and composited on a worker thread.
#pragma once

#include "bridge.h"
#include "mainwindow.h"
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
class QScreen;

// Decoder state shared with the worker thread.
struct VideoMedia {
    std::mutex m;
    std::optional<rust::Box<fsb::Video>> video;
    std::optional<rust::Box<fsb::Renderer>> renderer;
    int width = 640, height = 480;
    rust::String script;
    bool scriptDirty = false;
    // the last frame decoded, without the subtitles: a visual tool's drag or a typed letter renders
    // the same frame again (Aegisub's frame cache); -1 after the video changed
    qint64 cachedFrame = -1;
    QImage cached;
    QColor dummyColor{0x30, 0x30, 0x30}; // the dummy video's picture, plain or a checkerboard
    bool dummyPattern = false;
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
    void leaveEvent(QEvent *) override;
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
    QPointF m_panFrom; // a zoomed frame dragged (Kainote moves its zoom rectangle)
    QRectF m_panZoom;
    bool m_panning = false;
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
    // frame: where to start, ar: display aspect override (a script's "Video Position" / "Video AR Value")
    void open(const QString &path, qint64 frame = 0, double ar = 0);
    // Aegisub's dummy video, "?dummy:fps:frames:width:height:r:g:b:c" ("c": a checkerboard), as
    // scripts save it; false when the text is not one
    bool openDummy(const QString &spec);
    static QString dummySpec(double fps, qint64 frames, int w, int h, QColor c, bool pattern);
    QString dummySpec() const { return m_dummySpec; } // empty unless a dummy is open
    void close();
    bool hasVideo() const { return m_frames > 0; }
    QString path() const { return m_path; }
    fsb::VideoInfo info() const { return m_info; }
    std::vector<std::int64_t> timecodes() const; // int64_t = cxx i64 (qint64 is long long, int64_t is long on Linux)
    std::vector<std::int64_t> keyframes() const;
    // the file's chapters (Kainote): first frame and title, by frame
    const std::vector<std::pair<qint64, QString>> &chapters() const { return m_chapters; }
    void stepChapter(int dir); // to the nearest chapter start before (dir < 0) or after the current frame
    void setTimecodes(std::vector<std::int64_t> tc); // empty: the video's own
    void setAspect(double ar); // display width / height, expressed as the sample aspect ratio; 0: the file's own
    // Aegisub's Video AR Mode: 1 4:3, 2 16:9, 3 2.35, 4 the custom value, else 0 (the file's own)
    static double aspectOfMode(int mode, double custom);

    qint64 frameAt(qint64 ms) const;      // frame shown at ms
    qint64 frameAtStart(qint64 ms) const; // first frame a line starting at ms is visible on (Aegisub START)
    qint64 frameAtEnd(qint64 ms) const { return frameAt(ms - 1); } // last frame a line ending at ms is on (END)
    void seekStart(qint64 ms) { seekFrame(frameAtStart(ms)); }
    qint64 timeAt(qint64 frame) const;
    // Aegisub's TimeAtFrame START/END: halfway to the neighbouring frame, so rounding to centiseconds
    // cannot move the line onto another frame
    qint64 timeAtStart(qint64 frame) const {
        const qint64 prev = timeAt(frame - 1), cur = timeAt(frame);
        return prev + (cur - prev + 1) / 2;
    }
    qint64 timeAtEnd(qint64 frame) const {
        const qint64 cur = timeAt(frame), next = timeAt(frame + 1);
        return cur + (next - cur + 1) / 2;
    }
    qint64 time() const { return timeAt(m_frame); }
    qint64 frame() const { return m_frame; }
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
    void setFullscreen(bool on, QScreen *on_screen = nullptr); // default: the screen the box is on
    // Aegisub's detached video: the box in a window of its own; closing that window docks it back
    void setDetached(bool on);
    bool detached() const { return m_detached; }
    void zoomMode() { m_zoomMode = true; }
    void resetZoom();
    int seekOn() const;
    int playOn() const;
    VisualTools *tools() const { return m_tools; }
    VideoView *view() const { return m_view; }
    static std::span<const PanelKey> keys(); // the video's own keys
    void runCommand(const QString &id);
    enum Grab { Raw, WithSubs, SubsOnly }; // SubsOnly: on a transparent picture (Aegisub)
    QImage grabFrame(Grab mode);
    void saveFrame(Grab mode); // beside the video, <name>_NNN_<frame>.png (Aegisub, Kainote)

signals:
    void opened();
    void timeChanged(qint64 ms, qint64 frame);
    void heightDelta(int dy);

protected:
    void wheelEvent(QWheelEvent *) override;
    void closeEvent(QCloseEvent *) override;

private:
    void undock(); // out of the tab, into a window of its own
    void dock();
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
    // set to stop an open still indexing (close, a newer open, destruction)
    std::shared_ptr<std::atomic_bool> m_openCancel = std::make_shared<std::atomic_bool>(false);
    quint64 m_reqId = 0;
    bool m_busy = false, m_pending = false;
    QString m_path;
    fsb::VideoInfo m_info{};
    qint64 m_frames = 0, m_frame = 0;
    qint64 m_scriptAt = -2; // frame of a one-frame script; -1 the whole script; -2 out of date
    std::vector<std::int64_t> m_timecodes, m_keyframes;
    std::vector<std::pair<qint64, QString>> m_chapters;
    double m_dummyFps = 0;
    QString m_dummySpec;
    bool m_playing = false, m_zoomMode = false;
    qint64 m_playEnd = -1, m_playStartMs = 0;
    QElapsedTimer m_clock;
    QTimer *m_timer;

    VideoView *m_view;
    VisualTools *m_tools;
    bool m_toolsShown = false; // whether the tool's handles were on the last frame
    QSlider *m_seek, *m_volume;
    QLineEdit *m_timeField;
    QWidget *m_controls, *m_toolRow;
    QComboBox *m_seekOn, *m_playOn;
    QWidget *m_fsParent = nullptr;
    int m_fsIndex = -1;
    bool m_detached = false;
};

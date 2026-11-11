// Video box (Kainote layout): GPU video view (QRhi) with libass subtitles,
// seek slider, playback buttons, time field, volume and the visual-tools row.
// Frames are decoded and the subtitles drawn on a worker thread; the view's
// shader makes RGB of the decoder's planes and lays the subtitles over them.
#pragma once

#include "bridge.h"
#include "mainwindow.h"
#include "rhiquad.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QRhiWidget>
#include <QThread>
#include <QWidget>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class TabPanel;
class VisualTools;
class QSlider;
class QLineEdit;
class QComboBox;
class QToolButton;
class QTimer;
class QStackedWidget;
class QScreen;

// A frame's picture as the view draws it: the decoder's planes, which its shader converts (the
// frames of a video while it plays: no conversion on the CPU), or a finished RGBA picture where
// there are none (a dummy video, a pixel format the planes are not given for).
struct VideoPicture {
    quint64 serial = 0; // another picture has another one: the view uploads a picture once
    QImage rgba;
    QByteArray y, u, v; // row after row, as `planes` says
    fsb::FramePlanes planes{};
    bool isNull() const { return rgba.isNull() && y.isEmpty(); }
    bool deep() const { return !y.isEmpty() && planes.bytes == 2; } // 16-bit samples
};

// The subtitles on a frame, drawn over its picture: the part of the frame they cover.
struct SubsPicture {
    quint64 serial = 0;
    QImage image; // premultiplied; null when none show
    QPoint at;    // its top left in the frame
    QSize frame;  // the frame it was drawn for
};

// Decoder state shared with the worker thread.
struct VideoMedia {
    std::mutex m; // the worker holds it while it renders
    std::optional<rust::Box<fsb::Video>> video;
    std::optional<rust::Box<fsb::Renderer>> renderer;
    int width = 640, height = 480;
    // The subtitles the GUI thread hands over, under their own lock so an edit never waits for
    // a frame; the worker loads them into the renderer before the next frame with subtitles.
    std::mutex subsM;
    std::optional<rust::Box<fsb::RenderSubs>> subs;
    std::optional<rust::Box<fsb::RenderSubs>> takeSubs() {
        std::lock_guard g(subsM);
        return std::exchange(subs, std::nullopt);
    }
    // the last frame decoded to RGBA, without the subtitles: what a saved or copied frame is made
    // of, and the view's picture where there are no planes; -1 after the video changed
    qint64 cachedFrame = -1;
    QImage cached;
    // the last frame's picture for the view: a visual tool's drag or a typed letter renders the
    // same frame again (Aegisub's frame cache); -1 after the video changed, -2 a dummy's picture
    qint64 shownFrame = -1;
    VideoPicture shown;
    void forget() { cachedFrame = shownFrame = -1; }
    // The planes handed to the view lately. A set nothing else holds any more is written again:
    // a new one that large is new pages from the system on every frame.
    std::vector<std::array<QByteArray, 3>> planePool;
    // what the renderer drew at its last overlay_box, which it answers "the same" by, and the
    // frame of RGBA it draws into
    SubsPicture subsShown;
    std::unique_ptr<uint8_t[]> subsBuf;
    size_t subsBufSize = 0;
    // Whether the view shows planes at all (video/planes in the settings, on unless turned off to
    // compare with the conversion on the CPU; off where the graphics API has no texture to
    // convert them in), and planes of 16-bit samples (not where it has no such texture). The
    // view's thread sets them, the worker reads them.
    std::atomic_bool planes{true}, deepPlanes{true};
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
    void ready(quint64 id, qint64 frame, VideoPicture picture, SubsPicture subs);
    void failed(QString error);
private:
    std::shared_ptr<VideoMedia> m_media;
};

class VideoView : public QRhiWidget {
    Q_OBJECT
public:
    explicit VideoView(class VideoBox *box);
    void setFrame(const VideoPicture &picture, const SubsPicture &subs);
    void updateOverlay() { m_overlayDirty = true; update(); }
    void paintOverscan(QPainter &p) const;
    // video frame area in widget coordinates
    QRectF displayRect() const;
    // script (PlayRes) coordinates <-> widget coordinates, zoom aware
    QPointF toWidget(QPointF script) const;
    QPointF toScript(QPointF widget) const;
    double scriptScale() const; // widget px per script px (x)
    QRectF zoom() const { return m_zoom; }
    // the part of the frame shown, and how much smaller than fitted the frame is drawn (zoomed out)
    void setZoom(QRectF uv, double shrink = 1);
    // the zoomed part moved back to the frame's centre, the zoom kept (Aegisub's video/reset_pan)
    void resetPan();
    bool zoomed() const { return m_zoom != QRectF(0, 0, 1, 1) || m_shrink != 1; }
    // f < 1 zooms in, keeping the frame's point under the widget point at in place
    void zoomBy(double f, QPointF at);
    // the video's own pixels at that scale (Aegisub's 50/100/200%), around the view's centre
    void zoomScale(double scale);

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
    VideoPicture m_frame;
    SubsPicture m_subs;
    bool m_frameDirty = false, m_subsDirty = false, m_overlayDirty = true;
    // what the graphics API has here: textures to make a frame of its planes in, and textures of
    // 16-bit samples for the planes of a video of more than 8 bits
    bool m_stages = true, m_deep = true;
    bool showable(const VideoPicture &p) const { return p.y.isEmpty() || (m_stages && (m_deep || !p.deep())); }
    QRectF m_zoom{0, 0, 1, 1};
    double m_shrink = 1; // 0.1..1 of the fitted size, below 1 only with the whole frame shown
    QPointF m_zoomDragStart;
    bool m_zoomDragging = false;
    QPointF m_panFrom; // a zoomed frame dragged (Kainote moves its zoom rectangle)
    QRectF m_panZoom;
    bool m_panning = false;
    // The frame with its subtitles, in a texture of the frame's size (m_whole), made when either
    // changes: a frame that came as planes is made RGB there in two steps, the chroma rows as
    // wide as the luma (chroma.frag into m_rows) and then the frame (yuv.frag); one that came as
    // RGBA is put there as it is; the subtitles go over it. The view draws that texture at its
    // zoom, then the tools and marks.
    RhiQuad m_rowsQuad, m_planesQuad, m_rgbaQuad, m_subsQuad, m_frameQuad, m_overlayQuad;
    RhiStage m_rows, m_whole;
    std::unique_ptr<QRhiTexture> m_planeTex[3], m_frameTex, m_subsTex, m_overlayTex;
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
    // the file open, or the one still being indexed that replaces it
    QString wanted() const { return m_opening.isEmpty() ? m_path : m_opening; }
    // Aegisub's dummy video dialog: ten hours at 1000 fps
    static constexpr qint64 kMaxDummyFrames = 36'000'000;
    fsb::VideoInfo info() const { return m_info; }
    // the video's own pixel aspect; info()'s is the one shown, after a ratio set by hand
    std::pair<int, int> videoSar() const { return {m_videoSarNum, m_videoSarDen}; }
    std::vector<std::int64_t> timecodes() const; // int64_t = cxx i64 (qint64 is long long, int64_t is long on Linux)
    std::vector<std::int64_t> keyframes() const;
    // the file's chapters (Kainote): first frame and title, by frame
    const std::vector<std::pair<qint64, QString>> &chapters() const { return m_chapters; }
    void stepChapter(int dir); // to the nearest chapter start before (dir < 0) or after the current frame
    void setTimecodes(std::vector<std::int64_t> tc); // empty: the video's own
    void setAspect(double ar); // display width / height, expressed as the sample aspect ratio; 0: the file's own
    // Aegisub's mode (aspectOfMode), kept in the script's Video AR Mode/Value; 4 asks for the custom value
    void setAspectMode(int mode);
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
    void stepFrames(int n);

    void subsChanged();
    void syncMatrix(); // decode with the script's YCbCr Matrix, when it changed (Aegisub)
    void lineChanged();
    void setPlayerMode(bool on);
    void setFullscreen(bool on, QScreen *on_screen = nullptr); // default: the screen the box is on
    // Aegisub's detached video: the box in a window of its own; closing that window docks it back
    void setDetached(bool on);
    bool detached() const { return m_detached; }
    void zoomMode(); // Kainote's: zooms in on the centre by the percent set, or turns the zoom off
    void resetZoom();
    void focusSeek();
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
    void stepped(qint64 frame); // a step of one frame passed over this one (Aegisub plays its audio)
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
    QString m_opening; // being indexed
    fsb::VideoInfo m_info{};
    qint64 m_frames = 0, m_frame = 0;
    qint64 m_scriptAt = -2; // frame of a one-frame script; -1 the whole script; -2 out of date
    std::optional<QString> m_ycbcr; // the YCbCr Matrix header the video is decoded with
    std::vector<std::int64_t> m_timecodes, m_keyframes;
    // the open video's own timecodes and pixel aspect, kept so going back to them needs no lock
    std::vector<std::int64_t> m_videoTimecodes;
    int m_videoSarNum = 0, m_videoSarDen = 0;
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
    class QCheckBox *m_fsToolbar; // full screen only
    QComboBox *m_seekOn, *m_playOn;
    QWidget *m_fsParent = nullptr;
    int m_fsIndex = -1;
    bool m_detached = false;
};

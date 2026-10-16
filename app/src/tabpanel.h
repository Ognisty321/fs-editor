// One editor tab (Kainote "TabPanel"): a subtitle session with its video,
// audio, edit box, grid and Shift Times panel, plus the shared selection.
#pragma once

#include "bridge.h"

#include <QDateTime>
#include <QFileInfo>
#include <QWidget>
#include <set>

class MainWindow;
class Grid;
class EditBox;
class ShiftPanel;
class VideoBox;
class AudioBox;
class QSplitter;

class TabPanel : public QWidget {
    Q_OBJECT
public:
    // Flags for changed(): who caused the change, so it is not reloaded.
    enum Source { Other = 0, FromEditBox = 1, FromAudio = 2, FromVideo = 4 };
    enum ViewMode { All = 1, VideoSubs, AudioSubs, OnlyVideo, OnlySubs };

    explicit TabPanel(MainWindow *main);
    ~TabPanel() override;

    fsb::Session &s() { return *m_session; }
    MainWindow *main() const { return m_main; }
    Grid *grid() const { return m_grid; }
    EditBox *edit() const { return m_edit; }
    ShiftPanel *shift() const { return m_shift; }
    VideoBox *video() const { return m_video; }
    AudioBox *audio() const { return m_audio; }

    // file
    bool openSubs(const QString &path, const QString &encoding = {}); // empty: detect
    // Aegisub's "Text import options" for a plain text file about to open; false: cancelled.
    static bool askTextImport(QWidget *parent);
    // Subtitle stream `index` of a video, as an unsaved script.
    bool openEmbedded(const QString &video, int index);
    void importStyles(bool plainText);
    bool save(const QString &path = {});
    bool saveTranslation(const QString &path);
    // Drop the subtitles, keep video and audio (Kainote "Remove subtitles from the editor").
    void clearSubs();
    QString path() const;
    QString title() const;
    bool modified() const;
    // the file's time as read or saved: another program's change shows as a different one
    QDateTime diskTime() const { return m_diskTime; }
    void noteDisk() { m_diskTime = QFileInfo(path()).lastModified(); }

    // selection; rows are logical grid rows
    int active() const { return m_active; }
    std::vector<size_t> selection() const { return {m_sel.begin(), m_sel.end()}; }
    bool isSelected(size_t r) const { return m_sel.contains(r); }
    // follow: seek or play the video as its settings say (a line picked on the video does not)
    void setActive(int row, bool select = true, bool follow = true);
    void setSelection(const std::vector<size_t> &rows, int active);
    void toggleSelected(size_t r);

    // call after every document mutation
    void changed(int source = Other);
    // undo/redo keep the selection when possible
    void undo();
    void redo();
    // after a change that moved lines or brought others back (undo, sort): the lines the session
    // holds as selected, selected again
    void reselect();

    // media
    void openVideo(const QString &path, qint64 frame = 0, double ar = 0);
    void openDummyVideo(const QString &spec); // VideoBox::openDummy's
    void openAudio(const QString &path);
    void openBlankAudio(qint64 ms, bool noise = false);
    void closeAudio();
    void closeVideo();
    QString keyframesPath() const { return m_keyframesPath; } // empty: the video's own
    void openKeyframes(const QString &path);
    const std::vector<std::int64_t> &keyframes() const { return m_keyframes; }
    void setKeyframes(std::vector<std::int64_t> kf);
    void closeKeyframes(); // back to the video's own
    void openTimecodes(const QString &path); // empty: the video's own; applied when a video opens
    QString timecodesPath() const { return m_timecodesPath; }
    qint64 videoTime() const;
    // where a line starting (ending) on the video frame goes: Aegisub's TimeAtFrame START (END), safe
    // from rounding to centiseconds; videoTime() without video
    qint64 videoStartTime() const;
    qint64 videoEndTime() const;
    void seekVideo(qint64 ms);
    void playLine(int row);

    void setViewMode(int mode);
    int viewMode() const { return m_viewMode; }
    void setPlayerMode(bool on);
    bool playerMode() const { return m_playerMode; }
    void showShiftPanel(bool on);
    bool shiftPanelVisible() const;
    bool hideOriginal() const { return m_hideOriginal; }
    void setHideOriginal(bool on);
    // the video context the core needs for frame shifting and automation
    void updateVideoContext();
    // Kainote: the video panel takes the picture's shape, with no bars. The width follows the
    // height, or the height the width (after the panel's side is dragged); an axis held at its
    // limit leaves the rest to the other one. Runs from the event loop, once layouts are done.
    void fitVideo(bool byWidth = false);

signals:
    void docChanged(int source);
    void activeChanged(int row);
    void titleChanged();
    void statusChanged();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void applyLayout();
    void syncSelection(); // to the session, for the undo steps
    void matchResolution();
    void matchMatrix();    // Aegisub's update_ycbcr_matrix
    void matchLayoutRes(); // Aegisub's update_layout_res
    bool untouched();      // the starting document, nothing written yet

    MainWindow *m_main;
    rust::Box<fsb::Session> m_session;
    Grid *m_grid;
    EditBox *m_edit;
    ShiftPanel *m_shift;
    VideoBox *m_video;
    AudioBox *m_audio;
    QSplitter *m_vsplit, *m_top, *m_right, *m_bottom;
    std::set<size_t> m_sel;
    int m_active = 0;
    int m_viewMode = All;
    bool m_playerMode = false;
    bool m_hideOriginal = false;
    std::vector<std::int64_t> m_keyframes;
    QString m_keyframesPath, m_timecodesPath; // loaded from files, kept over the video's own
    QTimer *m_saveLater = nullptr;
    QDateTime m_diskTime;
};

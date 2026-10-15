// Audio box (Kainote layout): waveform or spectrogram drawn on the GPU via
// QRhi, timing of the active line, karaoke syllables, the search bar, the
// zoom/volume sliders and the 23-button row.
#pragma once

#include "bridge.h"
#include "rhiquad.h"

#include <QRhiWidget>
#include <QWidget>
#include <atomic>
#include <memory>
#include <optional>

class TabPanel;
class AudioBox;
class QScrollBar;
class QSlider;
class QToolButton;
class QTimer;

class AudioView : public QRhiWidget {
    Q_OBJECT
public:
    explicit AudioView(AudioBox *box);
    void dataChanged() { m_dataDirty = true; m_overlayDirty = true; update(); }
    void overlayChanged() { m_overlayDirty = true; update(); }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    void paintOverlay(QPainter &p);
    AudioBox *m_box;
    RhiQuad m_wave, m_overlay;
    std::unique_ptr<QRhiTexture> m_data, m_overlayTex;
    bool m_dataDirty = true, m_overlayDirty = true, m_spectrumTex = false;
    QRhiResourceUpdateBatch *m_pendingInit = nullptr;
    // drag: 0 none, 1 start, 2 end, 3 ruler scroll, 10+k karaoke boundary k
    int m_drag = 0;
    Qt::MouseButton m_button = Qt::NoButton;
    int m_pressX = 0;
    qint64 m_pressLeft = 0;
    int m_hoverX = -1;
};

class AudioBox : public QWidget {
    Q_OBJECT
    friend class AudioView;
public:
    explicit AudioBox(TabPanel *tab);
    ~AudioBox() override;

    bool hasAudio() const { return m_audio.has_value(); }
    const fsb::Audio *source() const { return m_audio ? &**m_audio : nullptr; }
    void open(const QString &path);
    void openBlank(qint64 ms);
    void close();
    QString path() const { return m_path; }

    void play(qint64 from, qint64 to);
    void stop();
    bool playing() const;
    qint64 positionMs() const;
    void setVolume(float v);
    qint64 markTime() const { return m_mark; }
    void centerOn(qint64 ms);
    void setMark(qint64 ms);

    void lineChanged();   // active row changed
    void docChanged();    // document edited elsewhere
    void commit();

signals:
    void opened();

protected:
    void keyPressEvent(QKeyEvent *) override;

private:
    // coordinates
    double msPerPx() const { return m_msPerPx; }
    int xOf(qint64 ms) const { return int((ms - m_left) / m_msPerPx); }
    qint64 msAt(int x) const { return m_left + qint64(x * m_msPerPx); }
    qint64 snap(qint64 ms, bool toggle) const;
    const std::vector<std::pair<qint64, qint64>> &inactiveLines() const;
    void setLeft(qint64 ms);
    void zoom(double factor, int aroundX);
    void ensureVisible(qint64 a, qint64 b);
    void updateScroll();
    void setSelection(qint64 start, qint64 end, bool fromDrag);
    void loaded();
    void tick();
    void playSelection();
    void stepLine(int dir);
    void stepSyllable(int dir);
    void modifyStart(int steps);
    void modifyLength(int steps, bool shiftFollowing);
    void autoSplit();
    void karaokeChanged();
    QWidget *buttonRow();

    TabPanel *m_tab;
    std::optional<rust::Box<fsb::Audio>> m_audio;
    QString m_path;
    // set to stop a load still decoding (close, a newer open, destruction)
    std::shared_ptr<std::atomic_bool> m_openCancel = std::make_shared<std::atomic_bool>(false);
    qint64 m_duration = 0;
    qint64 m_left = 0;
    double m_msPerPx = 10;
    float m_vzoom = 1;
    qint64 m_selStart = 0, m_selEnd = 0;
    bool m_selModified = false;
    qint64 m_mark = -1;
    qint64 m_playEnd = -1;
    int m_dragId = 0;
    std::vector<fsb::Syllable> m_syl;
    int m_activeSyl = 0;
    mutable std::vector<std::pair<qint64, qint64>> m_inactive;
    mutable std::tuple<quint64, int, int> m_inactiveKey{~quint64(0), -1, -1};

    AudioView *m_view;
    QScrollBar *m_scroll;
    QSlider *m_hzoom, *m_vzoomSlider, *m_volume;
    QToolButton *m_link;
    QTimer *m_timer;
    // toggles
    bool m_karaoke = false, m_autoSplit = false, m_autoCommit = true, m_nextOnCommit = false, m_autoScroll = true,
         m_spectrum = false, m_enhance = false;
};

#include "tabpanel.h"

#include "audiobox.h"
#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "shiftpanel.h"
#include "videobox.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

TabPanel::TabPanel(MainWindow *main) : QWidget(main), m_main(main), m_session(fsb::new_session()) {
    m_grid = new Grid(this);
    m_edit = new EditBox(this);
    m_shift = new ShiftPanel(this);
    m_video = new VideoBox(this);
    m_audio = new AudioBox(this);

    // [video | (audio / edit box)]  over  [grid | shift panel]
    m_vsplit = new QSplitter(Qt::Vertical, this);
    m_top = new QSplitter(Qt::Horizontal, m_vsplit);
    m_right = new QSplitter(Qt::Vertical, m_top);
    m_bottom = new QSplitter(Qt::Horizontal, m_vsplit);
    m_top->addWidget(m_video);
    m_top->addWidget(m_right);
    m_right->addWidget(m_audio);
    m_right->addWidget(m_edit);
    m_bottom->addWidget(m_grid);
    m_bottom->addWidget(m_shift);
    m_vsplit->addWidget(m_top);
    m_vsplit->addWidget(m_bottom);
    for (auto *s : {m_vsplit, m_top, m_right, m_bottom}) s->setChildrenCollapsible(false);
    m_top->setStretchFactor(1, 1);
    m_bottom->setStretchFactor(0, 1);
    m_vsplit->setStretchFactor(1, 1);
    QSettings st;
    m_right->setSizes({st.value("layout/audioHeight", 170).toInt(), 300});
    m_vsplit->setSizes({st.value("layout/topHeight", 380).toInt(), 400});
    m_top->setSizes({st.value("layout/videoWidth", 560).toInt(), 700});
    connect(m_vsplit, &QSplitter::splitterMoved, this, [this] { QSettings().setValue("layout/topHeight", m_vsplit->sizes()[0]); });
    connect(m_right, &QSplitter::splitterMoved, this, [this] { QSettings().setValue("layout/audioHeight", m_right->sizes()[0]); });
    connect(m_top, &QSplitter::splitterMoved, this, [this] { QSettings().setValue("layout/videoWidth", m_top->sizes()[0]); });

    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_vsplit);

    connect(m_video, &VideoBox::opened, this, [this] {
        updateVideoContext();
        setKeyframes(m_video->keyframes());
        if (!m_video->path().isEmpty()) {
            s().set_project_value("Video File", fs::Utf8(m_video->path()));
            if (QSettings().value("video/openAudio", true).toBool() && !m_audio->hasAudio())
                m_audio->open(m_video->path());
        }
        m_video->subsChanged();
        m_video->seekFrame(0);
        applyLayout();
        emit statusChanged();
        emit titleChanged();
    });
    connect(m_video, &VideoBox::timeChanged, this, [this](qint64 ms) {
        m_grid->setVideoTime(ms);
        m_audio->docChanged();
        emit statusChanged();
    });
    connect(m_video, &VideoBox::heightDelta, this, [this](int dy) {
        auto sizes = m_vsplit->sizes();
        sizes[0] = std::max(150, sizes[0] + dy);
        m_vsplit->setSizes(sizes);
    });
    connect(m_audio, &AudioBox::opened, this, [this] {
        if (!m_audio->path().isEmpty()) s().set_project_value("Audio File", fs::Utf8(m_audio->path()));
        applyLayout();
    });

    m_audio->setVisible(false);
    m_video->setVisible(false);
    showShiftPanel(st.value("layout/shiftTimes", true).toBool());
    changed();
    setActive(0);
}

TabPanel::~TabPanel() = default;

QString TabPanel::path() const { return fs::q(m_session->file_info().path); }
bool TabPanel::modified() const { return m_session->file_info().modified; }

QString TabPanel::title() const {
    const QString p = path();
    return (p.isEmpty() ? tr("Untitled") : QFileInfo(p).fileName()) + (modified() ? "*" : "");
}

bool TabPanel::openSubs(const QString &path) {
    try {
        s().open(fs::Utf8(path));
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    m_sel.clear();
    m_active = 0;
    changed();
    setActive(std::clamp(fs::q(s().info_value("Active Line")).toInt(), 0, std::max(0, int(s().row_count()) - 1)));
    // reopen the media remembered in the script, as Kainote does
    const QDir dir = QFileInfo(path).dir();
    auto resolve = [&](const QString &key) {
        const QString v = fs::q(s().info_value(fs::Utf8(key)));
        if (v.isEmpty() || v.startsWith("?dummy")) return QString();
        const QString abs = QFileInfo(v).isAbsolute() ? v : dir.filePath(v);
        return QFileInfo::exists(abs) ? abs : QString();
    };
    if (QSettings().value("video/openWithSubs", true).toBool()) {
        if (const QString v = resolve("Video File"); !v.isEmpty()) openVideo(v);
        if (const QString a = resolve("Audio File"); !a.isEmpty() && a != resolve("Video File")) openAudio(a);
        if (const QString k = resolve("Keyframes File"); !k.isEmpty()) openKeyframes(k);
    }
    emit titleChanged();
    return true;
}

bool TabPanel::openEmbedded(const QString &video, int index) {
    try {
        s().open_embedded(fs::Utf8(video), index);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    m_sel.clear();
    m_active = 0;
    changed();
    setActive(0);
    emit titleChanged();
    return true;
}

bool TabPanel::save(const QString &target) {
    const QString p = target.isEmpty() ? path() : target;
    if (p.isEmpty()) return false;
    try {
        if (m_active >= 0) s().set_project_value("Active Line", fs::Utf8(QString::number(m_active)));
        s().save(fs::Utf8(p));
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    changed();
    emit titleChanged();
    return true;
}

bool TabPanel::saveTranslation(const QString &p) {
    try {
        s().save_translation(fs::Utf8(p));
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

void TabPanel::clearSubs() {
    m_session = fsb::new_session();
    updateVideoContext();
    m_sel.clear();
    changed();
    setActive(0);
}

void TabPanel::setActive(int row, bool select) {
    const int n = int(s().row_count());
    if (n == 0) {
        m_active = -1;
        m_sel.clear();
    } else {
        row = std::clamp(row, 0, n - 1);
        if (select) m_sel = {size_t(row)};
        m_active = row;
    }
    m_grid->refresh();
    m_grid->ensureVisible(m_active);
    m_edit->load(m_active);
    m_audio->lineChanged();
    m_video->lineChanged();
    if (m_active >= 0 && m_video->hasVideo()) {
        const auto r = s().row(size_t(m_active));
        const int seek = m_video->seekOn();
        const bool paused = !m_video->playing();
        if (seek == VideoBox::SeekEveryChange ||
            (seek == VideoBox::SeekClickOrEditPaused && paused) || seek == VideoBox::SeekClickOrEdit)
            m_video->seekStart(r.start);
        switch (m_video->playOn()) {
        case VideoBox::PlayAudioToEnd: m_audio->play(r.start, r.end); break;
        case VideoBox::PlayVideoToEnd: m_video->playRange(r.start, r.end); break;
        case VideoBox::PlayVideoToNextStart:
            m_video->playRange(r.start, size_t(m_active + 1) < s().row_count() ? s().row(size_t(m_active + 1)).start : r.end);
            break;
        }
    }
    emit activeChanged(m_active);
    emit statusChanged();
}

void TabPanel::setSelection(const std::vector<size_t> &rows, int active) {
    m_sel = {rows.begin(), rows.end()};
    setActive(active, false);
}

void TabPanel::toggleSelected(size_t r) {
    if (m_sel.contains(r) && m_sel.size() > 1) m_sel.erase(r);
    else m_sel.insert(r);
    m_grid->refresh();
}

void TabPanel::changed(int source) {
    const size_t n = s().row_count();
    // drop selections past the end (deletions, undo)
    while (!m_sel.empty() && *m_sel.rbegin() >= n) m_sel.erase(std::prev(m_sel.end()));
    if (m_active >= int(n)) m_active = int(n) - 1;
    if (m_sel.empty() && m_active >= 0) m_sel.insert(size_t(m_active));
    m_grid->refresh();
    if (!(source & FromEditBox)) m_edit->reload();
    m_audio->docChanged();
    m_video->subsChanged();
    emit docChanged(source);
    emit titleChanged();
    emit statusChanged();
}

void TabPanel::undo() {
    if (s().undo()) changed();
}

void TabPanel::redo() {
    if (s().redo()) changed();
}

void TabPanel::openVideo(const QString &path) {
    m_video->open(path);
    m_main->addRecent("video", path);
}

void TabPanel::openDummyVideo(int w, int h, double fps, qint64 ms) { m_video->openDummy(w, h, fps, ms); }

void TabPanel::openAudio(const QString &path) {
    m_audio->open(path);
    m_main->addRecent("audio", path);
}

void TabPanel::openBlankAudio(qint64 ms) { m_audio->openBlank(ms); }

void TabPanel::closeAudio() {
    m_audio->close();
    s().set_project_value("Audio File", "");
    applyLayout();
}

void TabPanel::openKeyframes(const QString &path) {
    try {
        const auto kf = fsb::load_keyframes(fs::Utf8(path));
        setKeyframes({kf.begin(), kf.end()});
        s().set_project_value("Keyframes File", fs::Utf8(path));
        m_main->addRecent("keyframes", path);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
    }
}

void TabPanel::setKeyframes(std::vector<std::int64_t> kf) {
    m_keyframes = std::move(kf);
    updateVideoContext();
    m_audio->docChanged();
}

void TabPanel::updateVideoContext() {
    const auto info = m_video->info();
    const auto tc = m_video->timecodes();
    s().set_video_context(info.width, info.height, {tc.data(), tc.size()}, {m_keyframes.data(), m_keyframes.size()});
}

qint64 TabPanel::videoTime() const {
    if (m_video->hasVideo()) return m_video->time();
    return m_audio->hasAudio() ? std::max<qint64>(0, m_audio->markTime()) : 0;
}

void TabPanel::seekVideo(qint64 ms) {
    if (m_video->hasVideo()) m_video->seekMs(ms);
}

void TabPanel::playLine(int row) {
    if (row < 0 || size_t(row) >= s().row_count()) return;
    const auto r = s().row(size_t(row));
    if (m_video->hasVideo()) m_video->playRange(r.start, r.end);
    else m_audio->play(r.start, r.end);
}

void TabPanel::setViewMode(int mode) {
    m_viewMode = mode;
    applyLayout();
}

void TabPanel::setPlayerMode(bool on) {
    m_playerMode = on;
    m_video->setPlayerMode(on);
    applyLayout();
    if (on && m_video->hasVideo()) {
        // fit the window to the video, Kainote "Ctrl+E"
        const auto info = m_video->info();
        window()->resize(window()->size() - size() + QSize(info.width, info.height + 70));
    }
}

void TabPanel::showShiftPanel(bool on) {
    m_shift->setVisible(on);
    QSettings().setValue("layout/shiftTimes", on);
}

bool TabPanel::shiftPanelVisible() const { return m_shift->isVisible(); }

void TabPanel::setHideOriginal(bool on) {
    m_hideOriginal = on;
    m_video->subsChanged();
}

void TabPanel::applyLayout() {
    const bool video = m_video->hasVideo() && m_viewMode != AudioSubs && m_viewMode != OnlySubs;
    const bool audio = m_audio->hasAudio() && m_viewMode != VideoSubs && m_viewMode != OnlyVideo && !m_playerMode;
    const bool editor = !m_playerMode && m_viewMode != OnlyVideo;
    m_video->setVisible(video || m_playerMode);
    m_audio->setVisible(audio);
    m_edit->setVisible(editor);
    m_right->setVisible(editor || audio);
    m_bottom->setVisible(editor);
    m_top->setVisible(video || editor || audio);
    m_shift->setVisible(editor && QSettings().value("layout/shiftTimes", true).toBool());
}

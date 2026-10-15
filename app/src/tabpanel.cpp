#include "tabpanel.h"

#include "audiobox.h"
#include "dialogs.h"
#include "editbox.h"
#include "grid.h"
#include "mainwindow.h"
#include "shiftpanel.h"
#include "videobox.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSettings>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
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
    connect(m_vsplit, &QSplitter::splitterMoved, this, [this] {
        QSettings().setValue("layout/topHeight", m_vsplit->sizes()[0]);
        fitVideo();
    });
    connect(m_right, &QSplitter::splitterMoved, this, [this] { QSettings().setValue("layout/audioHeight", m_right->sizes()[0]); });
    connect(m_top, &QSplitter::splitterMoved, this, [this] {
        QSettings().setValue("layout/videoWidth", m_top->sizes()[0]);
        fitVideo(true);
    });

    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(m_vsplit);

    connect(m_video, &VideoBox::opened, this, [this] {
        updateVideoContext();
        if (m_keyframesPath.isEmpty()) setKeyframes(m_video->keyframes());
        if (!m_timecodesPath.isEmpty()) openTimecodes(m_timecodesPath);
        if (m_video->hasVideo()) matchResolution();
        if (!m_video->path().isEmpty()) {
            s().set_project_value("Video File", fs::Utf8(m_video->path()));
            if (QSettings().value("video/openAudio", true).toBool() && !m_audio->hasAudio())
                m_audio->open(m_video->path());
        }
        m_video->subsChanged();
        m_video->seekFrame(m_video->frame()); // 0, or the position the script was saved at
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
        fitVideo();
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

TabPanel::~TabPanel() {
    // a detached or full screen video is no child of the tab; it goes while the tab is whole
    if (!m_video->parentWidget()) delete m_video;
}

QString TabPanel::path() const { return fs::q(m_session->file_info().path); }
bool TabPanel::modified() const { return m_session->file_info().modified; }

QString TabPanel::title() const {
    const QString p = path();
    return (p.isEmpty() ? tr("Untitled") : QFileInfo(p).fileName()) + (modified() ? "*" : "");
}

namespace {
struct TextImport {
    QString separator, comment;
    bool blank;
};
TextImport textImport() {
    const QSettings st;
    return {st.value("textImport/actorSeparator", ":").toString(), st.value("textImport/commentStarter", "#").toString(),
            st.value("textImport/includeBlank", false).toBool()};
}
} // namespace

bool TabPanel::askTextImport(QWidget *parent) {
    const TextImport t = textImport();
    QDialog d(parent);
    d.setWindowTitle(tr("Text import options"));
    auto *form = new QFormLayout(&d);
    auto *sep = new QLineEdit(t.separator, &d);
    auto *rem = new QLineEdit(t.comment, &d);
    auto *blank = new QCheckBox(tr("Include blank lines"), &d);
    blank->setChecked(t.blank);
    sep->setToolTip(tr("Text before it names the line's actor; empty reads no actors"));
    rem->setToolTip(tr("Lines starting with it become comments; empty makes none"));
    form->addRow(tr("Actor separator:"), sep);
    form->addRow(tr("Comment starter:"), rem);
    form->addRow(blank);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return false;
    QSettings st;
    st.setValue("textImport/actorSeparator", sep->text());
    st.setValue("textImport/commentStarter", rem->text());
    st.setValue("textImport/includeBlank", blank->isChecked());
    return true;
}

bool TabPanel::openSubs(const QString &path, const QString &encoding) {
    try {
        const TextImport t = textImport();
        s().open_as(fs::Utf8(path), fs::Utf8(encoding), fs::Utf8(t.separator), fs::Utf8(t.comment), t.blank);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    noteDisk();
    // Aegisub's backup of the file as opened, kept in the autosave folder (File > Open autosave)
    if (const QFileInfo fi(path); fi.absolutePath() != QFileInfo(autosaveDir()).absoluteFilePath()) {
        const QString copy = autosaveDir() + "/" + fi.completeBaseName() + ".ORIGINAL." + fi.suffix();
        QDir().mkpath(autosaveDir());
        QFile::remove(copy);
        QFile::copy(path, copy);
    }
    m_sel.clear();
    m_active = 0;
    m_edit->load(-1); // a new file: forget the revert text of the old one
    m_grid->resetFilter();
    changed();
    setActive(std::clamp(fs::q(s().project_value("Active Line")).toInt(), 0, std::max(0, int(s().row_count()) - 1)));
    if (const int top = fs::q(s().project_value("Scroll Position")).toInt(); top > 0) // after setActive, as Aegisub
        m_grid->verticalScrollBar()->setValue(top);
    // reopen the media remembered in the script, as Kainote does
    const QDir dir = QFileInfo(path).dir();
    auto resolve = [&](const QString &key) {
        const QString v = fs::q(s().project_value(fs::Utf8(key)));
        if (v.isEmpty() || v.startsWith("?dummy")) return QString();
        const QString abs = QFileInfo(v).isAbsolute() ? v : dir.filePath(v);
        return QFileInfo::exists(abs) ? abs : QString();
    };
    if (QSettings().value("video/openWithSubs", true).toBool()) {
        if (const QString v = resolve("Video File"); !v.isEmpty()) {
            const double ar = VideoBox::aspectOfMode(fs::q(s().project_value("Video AR Mode")).toInt(),
                                                     fs::q(s().project_value("Video AR Value")).toDouble());
            openVideo(v, fs::q(s().project_value("Video Position")).toLongLong(), ar);
        }
        if (const QString a = resolve("Audio File"); !a.isEmpty() && a != resolve("Video File")) openAudio(a);
        if (const QString k = resolve("Keyframes File"); !k.isEmpty()) openKeyframes(k);
        if (const QString t = resolve("Timecodes File"); !t.isEmpty()) openTimecodes(t);
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
    m_grid->resetFilter();
    changed();
    setActive(0);
    emit titleChanged();
    return true;
}

bool TabPanel::save(const QString &target) {
    const QString p = target.isEmpty() ? path() : target;
    if (p.isEmpty()) return false;
    try {
        // written only when non-zero, as Aegisub does; a video that did not open keeps its old position
        auto setNum = [this](rust::Str key, qint64 v) { s().set_project_value(key, fs::Utf8(v ? QString::number(v) : QString())); };
        setNum("Active Line", std::max(0, m_active));
        setNum("Scroll Position", m_grid->verticalScrollBar()->value());
        if (m_video->hasVideo()) setNum("Video Position", m_video->frame());
        s().save(fs::Utf8(p));
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
        return false;
    }
    noteDisk();
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
        if (select) {
            m_sel = {size_t(row)};
            m_grid->setAnchor(size_t(row));
        }
        m_active = row;
    }
    syncSelection();
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
    syncSelection();
    m_grid->refresh();
}

void TabPanel::changed(int source) {
    const size_t n = s().row_count();
    // drop selections past the end (deletions, undo)
    while (!m_sel.empty() && *m_sel.rbegin() >= n) m_sel.erase(std::prev(m_sel.end()));
    if (m_active >= int(n)) m_active = int(n) - 1;
    if (m_sel.empty() && m_active >= 0) m_sel.insert(size_t(m_active));
    syncSelection();
    m_grid->refresh();
    if (!(source & FromEditBox)) m_edit->reload();
    m_audio->docChanged();
    m_video->subsChanged();
    emit docChanged(source);
    emit titleChanged();
    emit statusChanged();
}

// the lines the step was made on come back selected (Aegisub)
void TabPanel::undo() {
    if (s().undo()) reselect();
}

void TabPanel::redo() {
    if (s().redo()) reselect();
}

void TabPanel::reselect() {
    const auto rows = s().selected_rows();
    const int active = int(s().selected_active());
    changed();
    if (!rows.empty()) setSelection({rows.begin(), rows.end()}, active >= 0 ? active : int(rows.front()));
}

void TabPanel::syncSelection() {
    s().set_selection(fs::slice(selection()), m_active);
}

void TabPanel::openVideo(const QString &path, qint64 frame, double ar) {
    if (ar <= 0) { // another video: its own aspect, as Aegisub resets it
        s().set_project_value("Video AR Mode", "");
        s().set_project_value("Video AR Value", "");
    }
    m_video->open(path, frame, ar);
    m_main->addRecent("video", path);
}

// Aegisub's update_play_res: a script without a resolution takes the video's; another one may be
// resampled to it (asked by default, as Kainote does)
void TabPanel::matchResolution() {
    auto &ss = s();
    const auto fi = ss.file_info();
    if (const QString f = fs::q(fi.format); f != "ASS" && f != "SSA") return;
    const auto info = m_video->info();
    const double par = info.sar_num > 0 && info.sar_den > 0 ? double(info.sar_num) / info.sar_den : 1.0;
    const int vx = qRound(info.width * std::max(1.0, par)), vy = qRound(info.height * std::max(1.0, 1 / par));
    if (vx <= 0 || vy <= 0 || (fi.play_res_x == vx && fi.play_res_y == vy)) return;
    const bool unset = fs::q(ss.info_value("PlayResX")).toInt() <= 0 && fs::q(ss.info_value("PlayResY")).toInt() <= 0;
    // the starting document, nothing written yet
    const bool fresh = fi.path.empty() && ss.row_count() <= 1 && (ss.row_count() == 0 || ss.row(0).text.empty());
    if (fresh) { // watching a video in a new tab does not ask to save it
        ss.set_info_quietly("PlayResX", fs::Utf8(QString::number(vx)));
        ss.set_info_quietly("PlayResY", fs::Utf8(QString::number(vy)));
        changed();
        return;
    }
    if (unset) {
        static int edits = 0;
        const QString amend = QString("video-resolution-%1").arg(++edits); // one undo step
        ss.set_info_value("PlayResX", fs::Utf8(QString::number(vx)), fs::Utf8(amend));
        ss.set_info_value("PlayResY", fs::Utf8(QString::number(vy)), fs::Utf8(amend));
        changed();
        return;
    }
    const int sx = fi.play_res_x, sy = fi.play_res_y;
    // a dummy video only fills in a missing resolution; exact multiples are kept for subpixel precision
    if (m_video->path().isEmpty() || (sx % vx == 0 && sy % vy == 0)) return;
    QSettings st;
    if (!st.value("video/askResolution", true).toBool()) return;
    const bool arChanged = std::abs(double(sx) / sy - double(vx) / vy) / (double(vx) / vy) > .01;
    QDialog d(this);
    d.setWindowTitle(tr("Resolution mismatch"));
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(tr("The resolution of the loaded video and the resolution specified for the subtitles "
                               "don't match.\n\nVideo resolution: %1 × %2\nScript resolution: %3 × %4\n\n"
                               "Change subtitles resolution to match video?")
                                .arg(vx).arg(vy).arg(sx).arg(sy), &d));
    QStringList choices{tr("Leave resolution as it is")};
    if (arChanged)
        choices << tr("Resample script (stretch to new aspect ratio)") << tr("Resample script (add borders)")
                << tr("Resample script (remove borders)");
    else
        choices << tr("Resample script");
    auto *group = new QButtonGroup(&d);
    for (int i = 0; i < choices.size(); ++i) {
        auto *r = new QRadioButton(choices[i], &d);
        group->addButton(r, i);
        v->addWidget(r);
    }
    group->button(std::clamp(st.value("video/resolutionChoice", 0).toInt(), 0, int(choices.size()) - 1))->setChecked(true);
    auto *dontAsk = new QCheckBox(tr("Don't ask again (can be turned back on in the settings)"), &d);
    v->addWidget(dontAsk);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    const bool ok = d.exec() == QDialog::Accepted;
    if (dontAsk->isChecked()) st.setValue("video/askResolution", false);
    if (!ok) return;
    const int choice = group->checkedId();
    st.setValue("video/resolutionChoice", choice);
    if (choice <= 0) return;
    fsb::ResampleSpec r{};
    r.from_x = sx;
    r.from_y = sy;
    r.to_x = vx;
    r.to_y = vy;
    r.ar_mode = uint8_t(choice - 1); // stretch, add borders, remove borders
    ss.resample(r);
    changed();
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
        m_keyframesPath = path;
        s().set_project_value("Keyframes File", fs::Utf8(path));
        m_main->addRecent("keyframes", path);
    } catch (const rust::Error &e) {
        m_main->error(QString::fromUtf8(e.what()));
    }
}

void TabPanel::closeKeyframes() {
    m_keyframesPath.clear();
    s().set_project_value("Keyframes File", "");
    setKeyframes(m_video->keyframes());
}

void TabPanel::openTimecodes(const QString &path) {
    std::vector<std::int64_t> tc;
    if (!path.isEmpty() && m_video->hasVideo()) {
        try {
            const auto v = fsb::load_timecodes(fs::Utf8(path), m_video->info().frames);
            tc.assign(v.begin(), v.end());
        } catch (const rust::Error &e) {
            m_main->error(QString::fromUtf8(e.what()));
            return;
        }
    }
    m_timecodesPath = path;
    s().set_project_value("Timecodes File", fs::Utf8(path));
    if (!m_video->hasVideo()) return;
    m_video->setTimecodes(std::move(tc));
    updateVideoContext();
    m_audio->docChanged();
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

qint64 TabPanel::videoStartTime() const {
    return m_video->hasVideo() ? m_video->timeAtStart(m_video->frame()) : videoTime();
}

qint64 TabPanel::videoEndTime() const {
    return m_video->hasVideo() ? m_video->timeAtEnd(m_video->frame()) : videoTime();
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
    fitVideo();
}

void TabPanel::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    fitVideo();
}

void TabPanel::fitVideo(bool byWidth) {
    QTimer::singleShot(0, this, [this, byWidth] {
        auto *view = m_video->view();
        const QRectF d = view->displayRect();
        // a detached or full screen video, or one alone in its row, has no neighbour to give way
        if (!m_video->hasVideo() || !m_video->isVisible() || m_video->parentWidget() != m_top ||
            !m_right->isVisible() || d.isEmpty() || view->height() <= 0)
            return;
        const double ar = d.width() / d.height();
        QList<int> top = m_top->sizes();
        int viewW = view->width();
        if (!byWidth) {
            const int lo = m_video->minimumSizeHint().width();
            const int hi = std::max(lo, top[0] + top[1] - m_right->minimumSizeHint().width());
            const int w = std::clamp(top[0] + qRound(view->height() * ar) - viewW, lo, hi);
            viewW += w - top[0];
            top = {w, top[0] + top[1] - w};
            m_top->setSizes(top);
        }
        // the height this width needs: all of it after a drag of the side, else what a limit left over
        const int dh = qRound(viewW / ar) - view->height();
        if (std::abs(dh) > 1) {
            QList<int> v = m_vsplit->sizes();
            v = {v[0] + dh, v[1] - dh};
            m_vsplit->setSizes(v);
        }
    });
}

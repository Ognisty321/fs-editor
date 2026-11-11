// MainWindow's Video menu.
#include "mainwindow_p.h"

#include "audiobox.h"
#include "colorpicker.h"
#include "editbox.h"
#include "tabpanel.h"
#include "videobox.h"
#include "visualtools.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>
#include <tuple>
#include <vector>

void MainWindow::buildVideoMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *video = menus.video = barMenu(tr("&Video"));
    add("openVideo", tr("Open video"), QKeySequence("Ctrl+Shift+O"), video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Open video"), QFileInfo(tab()->path()).absolutePath(),
            tr("Video (%1);;All files (*)").arg("*." + kVideoExt.join(" *.")));
        if (!f.isEmpty()) openFile(f); // as a dropped video: the subtitles in it or named like it offered
    });
    video->addMenu(recentMenu("video"));
    // Aegisub's video/close: the file stays where it is (Remove video recycles it)
    add("closeVideo", tr("Close video"), {}, video, [=] { tab()->closeVideo(); });
    add("playVideoLine", tr("Play the current line"), {}, video, [=] { tab()->playLine(tab()->active()); });
    add("openKeyframes", tr("Open keyframes"), {}, video, [=, this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open keyframes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Keyframes (*.txt *.log);;All files (*)"));
        if (!f.isEmpty()) tab()->openKeyframes(f);
    });
    video->addMenu(recentMenu("keyframes"));
    add("saveKeyframes", tr("Save keyframes..."), {}, video, [=, this] {
        if (tab()->keyframes().empty()) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save keyframes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Keyframes (*.txt);;All files (*)"));
        if (f.isEmpty()) return;
        const auto kf = tab()->keyframes();
        try {
            fsb::save_keyframes(fs::Utf8(f), {kf.data(), kf.size()});
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Save keyframes"), QString::fromUtf8(e.what()));
        }
    });
    add("closeKeyframes", tr("Close keyframes"), {}, video, [=, this] {
        tab()->closeKeyframes(); // back to the video's own, like Aegisub
        updateActions();
    });
    // Aegisub's timecodes: override the video's frame times (VFR typesetting on a CFR encode)
    add("openTimecodes", tr("Open timecodes..."), {}, video, [=, this] {
        if (!tab()->video()->hasVideo()) return status(tr("Open a video first"));
        const QString f = QFileDialog::getOpenFileName(this, tr("Open timecodes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Timecodes (*.txt);;All files (*)"));
        if (!f.isEmpty()) tab()->openTimecodes(f);
    });
    video->addMenu(recentMenu("timecodes"));
    add("saveTimecodes", tr("Save timecodes..."), {}, video, [=, this] {
        const auto tc = tab()->video()->timecodes();
        if (tc.size() < 2) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Save timecodes"), QFileInfo(tab()->path()).absolutePath(),
                                                       tr("Timecodes (*.txt);;All files (*)"));
        if (f.isEmpty()) return;
        try {
            fsb::save_timecodes(fs::Utf8(f), {tc.data(), tc.size()});
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Save timecodes"), QString::fromUtf8(e.what()));
        }
    });
    add("closeTimecodes", tr("Close timecodes"), {}, video, [=, this] {
        tab()->openTimecodes({});
        updateActions();
    });
    add("openDummy", tr("Open dummy video"), {}, video, [=, this] {
        QDialog d(this);
        d.setWindowTitle(tr("Dummy video"));
        auto *f = new QFormLayout(&d);
        // the last ones used, as Aegisub keeps them
        QSettings st;
        auto *w = new QSpinBox(&d), *h = new QSpinBox(&d);
        w->setRange(16, 8192);
        h->setRange(16, 8192);
        w->setValue(st.value("video/dummyWidth", 1920).toInt());
        h->setValue(st.value("video/dummyHeight", 1080).toInt());
        // Aegisub's resolution shortcuts
        static const std::tuple<int, int, const char *> sizes[] = {
            {640, 480, QT_TR_NOOP("SD fullscreen")},         {704, 480, QT_TR_NOOP("SD anamorphic")},
            {640, 360, QT_TR_NOOP("SD widescreen")},         {704, 396, QT_TR_NOOP("SD widescreen")},
            {640, 352, QT_TR_NOOP("SD widescreen MOD16")},   {704, 400, QT_TR_NOOP("SD widescreen MOD16")},
            {1024, 576, QT_TR_NOOP("SuperPAL widescreen")},  {1280, 720, QT_TR_NOOP("HD 720p")},
            {1920, 1080, QT_TR_NOOP("FHD 1080p")},           {2560, 1440, QT_TR_NOOP("QHD 1440p")},
            {3840, 2160, QT_TR_NOOP("4K UHD 2160p")},        {1080, 1920, QT_TR_NOOP("FHD vertical")}};
        auto *preset = new QComboBox(&d);
        for (auto [pw, ph, name] : sizes) preset->addItem(QString("%1×%2 (%3)").arg(pw).arg(ph).arg(tr(name)), QSize(pw, ph));
        const auto matchPreset = [=] { preset->setCurrentIndex(preset->findData(QSize(w->value(), h->value()))); };
        matchPreset();
        connect(preset, &QComboBox::activated, &d, [=] {
            const QSize s = preset->currentData().toSize();
            w->setValue(s.width());
            h->setValue(s.height());
        });
        for (auto *s : {w, h}) connect(s, &QSpinBox::valueChanged, &d, matchPreset);
        auto *fps = new QDoubleSpinBox(&d);
        fps->setRange(1, 240);
        fps->setDecimals(3);
        fps->setValue(st.value("video/dummyFps", 23.976).toDouble());
        auto *len = new QLineEdit(st.value("video/dummyLength", "0:30:00.00").toString(), &d);
        QColor colour = st.value("video/dummyColor", QColor(0x30, 0x30, 0x30)).value<QColor>();
        auto *pick = new QPushButton(&d);
        const auto show = [pick, &colour] { pick->setStyleSheet(QString("background:%1").arg(colour.name())); };
        show();
        connect(pick, &QPushButton::clicked, &d, [&, show] {
            if (const QColor c = ColorPicker::get(&d, colour, false); c.isValid()) colour = c, show();
        });
        auto *pattern = new QCheckBox(tr("Checkerboard pattern"), &d);
        pattern->setChecked(st.value("video/dummyPattern").toBool());
        f->addRow(tr("Resolution"), preset);
        f->addRow(tr("Width"), w);
        f->addRow(tr("Height"), h);
        f->addRow(tr("FPS"), fps);
        f->addRow(tr("Duration"), len);
        f->addRow(tr("Colour"), pick);
        f->addRow(pattern);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        const qint64 ms = fs::parseTime(len->text());
        st.setValue("video/dummyColor", colour);
        st.setValue("video/dummyPattern", pattern->isChecked());
        st.setValue("video/dummyWidth", w->value());
        st.setValue("video/dummyHeight", h->value());
        st.setValue("video/dummyFps", fps->value());
        st.setValue("video/dummyLength", len->text());
        tab()->openDummyVideo(VideoBox::dummySpec(fps->value(),
                                                  std::clamp<qint64>(qint64((ms > 0 ? ms : 1800000) * fps->value() / 1000), 1,
                                                                     VideoBox::kMaxDummyFrames),
                                                  w->value(), h->value(), colour, pattern->isChecked()));
    });
    video->addSeparator();
    // Aegisub's: every selected line, and the other end follows when it would be passed
    auto toVideo = [this](bool start) {
        auto *t = current();
        if (t->active() < 0) return;
        const auto sel = t->selection();
        const QSettings st; // Kainote's offsets, never before 0
        const qint64 from = std::max<qint64>(0, t->videoStartTime() + st.value("video/insertStartOffset", 0).toInt());
        const qint64 to = std::max<qint64>(0, t->videoEndTime() + st.value("video/insertEndOffset", 0).toInt());
        t->s().snap_to_video(fs::slice(sel), from, to, start);
        t->changed();
    };
    add("startFromVideo", tr("Insert start time from video"), QKeySequence("Ctrl+Left"), video,
        [toVideo] { toVideo(true); });
    add("endFromVideo", tr("Insert end time from video"), QKeySequence("Ctrl+Right"), video,
        [toVideo] { toVideo(false); });
    // Kainote's snap: the active line's start (end) to the nearest keyframe or start/end of the line
    // before or after it, within 5 s and never past the line's other end
    auto snap = [this](bool start) {
        withRow([start](TabPanel *t, fsb::Row &r) {
            const qint64 at = start ? r.start : r.end, other = start ? r.end : r.start;
            std::vector<qint64> near;
            if (t->video()->hasVideo())
                for (auto f : t->keyframes()) near.push_back(t->video()->timeAtStart(f));
            for (int n : {t->active() - 1, t->active() + 1})
                if (n >= 0 && size_t(n) < t->s().row_count()) {
                    const auto o = t->s().row(size_t(n));
                    near.insert(near.end(), {o.start, o.end});
                }
            qint64 best = at;
            for (qint64 c : near)
                if (c != at && std::abs(c - at) < 5000 && (start ? c < other : c > other) &&
                    (best == at || std::abs(c - at) < std::abs(best - at)))
                    best = c;
            (start ? r.start : r.end) = best;
        });
    };
    add("snapStart", tr("Snap start to keyframe"), QKeySequence("Shift+Left"), video, [=] { snap(true); });
    add("snapEnd", tr("Snap end to keyframe"), QKeySequence("Shift+Right"), video, [=] { snap(false); });
    add("prevFrame", tr("Previous frame"), QKeySequence("Left"), video, [=] { tab()->video()->stepFrames(-1); });
    add("nextFrame", tr("Next frame"), QKeySequence("Right"), video, [=] { tab()->video()->stepFrames(1); });
    // Aegisub's video/frame/*/large, by its Fast Jump Step setting
    const auto jump = [] { return std::max(1, QSettings().value("video/fastJump", 10).toInt()); };
    add("prevFrames", tr("Back several frames"), QKeySequence("Alt+Left"), video, [=] { tab()->video()->stepFrames(-jump()); });
    add("nextFrames", tr("Forward several frames"), QKeySequence("Alt+Right"), video, [=] { tab()->video()->stepFrames(jump()); });
    // Aegisub's keys
    add("goStart", tr("Go to start time of line"), QKeySequence("Ctrl+1"), video, [=] {
        if (tab()->active() >= 0) tab()->video()->seekStart(tab()->s().row(size_t(tab()->active())).start);
    });
    add("goEnd", tr("Go to end time of line"), QKeySequence("Ctrl+2"), video, [=] {
        if (tab()->active() >= 0) tab()->seekVideo(tab()->s().row(size_t(tab()->active())).end - 1);
    });
    add("playPause", tr("Play / Pause"), QKeySequence("Alt+Space"), video, [=] { tab()->video()->togglePlay(); });
    auto stepKeyframe = [=](int dir) {
        const auto kf = tab()->keyframes();
        const qint64 f = tab()->video()->frame();
        if (dir > 0) {
            auto it = std::upper_bound(kf.begin(), kf.end(), f);
            if (it != kf.end()) tab()->video()->seekFrame(*it);
        } else {
            auto it = std::lower_bound(kf.begin(), kf.end(), f);
            if (it != kf.begin()) tab()->video()->seekFrame(*std::prev(it));
        }
    };
    add("prevKeyframe", tr("Go to previous keyframe"), {}, video, [=] { stepKeyframe(-1); });
    add("nextKeyframe", tr("Go to next keyframe"), {}, video, [=] { stepKeyframe(1); });
    // the frame shown as a picture: saved beside the video, or copied
    const std::tuple<const char *, const char *, const char *, VideoBox::Grab> grabs[] = {
        {"saveFrameSubs", QT_TR_NOOP("Save frame with subtitles as PNG"), QT_TR_NOOP("Copy frame with subtitles to clipboard"), VideoBox::WithSubs},
        {"saveFrame", QT_TR_NOOP("Save frame as PNG"), QT_TR_NOOP("Copy frame to clipboard"), VideoBox::Raw},
        {"saveFrameSubsOnly", QT_TR_NOOP("Save subtitles only as PNG"), QT_TR_NOOP("Copy subtitles only to clipboard"), VideoBox::SubsOnly}};
    for (auto [id, save, copy, mode] : grabs) {
        add(id, tr(save), {}, video, [=] { tab()->video()->saveFrame(mode); });
        add(QString(id).replace("save", "copy"), tr(copy), {}, video,
            [=] { QApplication::clipboard()->setImage(tab()->video()->grabFrame(mode)); });
    }
    add("prevChapter", tr("Go to previous chapter"), {}, video, [=] { tab()->video()->stepChapter(-1); });
    add("nextChapter", tr("Go to next chapter"), {}, video, [=] { tab()->video()->stepChapter(1); });
    // Aegisub: video/frame/prev|next/boundary - the active line's start or end, then the neighbour line
    add("prevBoundary", tr("Previous boundary"), {}, video, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        auto *v = t->video();
        const fsb::Row r = t->s().row(size_t(t->active()));
        for (qint64 f : {v->frameAt(r.end - 1), v->frameAtStart(r.start)})
            if (f < v->frame()) return v->seekFrame(f);
        if (t->active() > 0) {
            t->setActive(t->active() - 1);
            v->seekFrame(v->frameAt(t->s().row(size_t(t->active())).end - 1));
        }
    });
    add("nextBoundary", tr("Next boundary"), {}, video, [=] {
        auto *t = tab();
        if (t->active() < 0 || !t->video()->hasVideo()) return;
        auto *v = t->video();
        const fsb::Row r = t->s().row(size_t(t->active()));
        for (qint64 f : {v->frameAtStart(r.start), v->frameAt(r.end - 1)})
            if (f > v->frame()) return v->seekFrame(f);
        if (size_t(t->active()) + 1 < t->s().row_count()) {
            t->setActive(t->active() + 1);
            v->seekStart(t->s().row(size_t(t->active())).start);
        }
    });
    add("jumpTo", tr("Jump to..."), QKeySequence("Ctrl+G"), video, [=, this] {
        auto *v = tab()->video();
        if (!v->hasVideo()) return;
        QDialog d(this);
        d.setWindowTitle(tr("Jump to"));
        auto *f = new QFormLayout(&d);
        auto *time = new TimeEdit(&d);
        auto *frame = new QSpinBox(&d);
        frame->setRange(0, int(std::max<qint64>(0, v->info().frames - 1)));
        time->setMs(v->time(), v->frame());
        frame->setValue(int(v->frame()));
        connect(time, &TimeEdit::edited, frame, [=] {
            if (time->ms() >= 0) frame->setValue(int(v->frameAt(time->ms())));
        });
        connect(frame, &QSpinBox::valueChanged, time, [=](int n) { time->setMs(v->timeAt(n), n); });
        f->addRow(tr("Frame:"), frame);
        f->addRow(tr("Time:"), time);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() == QDialog::Accepted) v->seekFrame(frame->value());
    });
    // Aegisub's Video details
    auto *overscan = add("showOverscan", tr("Show overscan mask"), {}, video, {});
    overscan->setCheckable(true);
    overscan->setChecked(QSettings().value("video/overscan").toBool());
    connect(overscan, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("video/overscan", on);
        for (auto *t : tabs()) t->video()->view()->updateOverlay();
    });
    add("videoDetails", tr("Video details..."), {}, video, [=, this] {
        auto *v = tab()->video();
        if (!v->hasVideo()) return;
        const fsb::VideoInfo i = v->info();
        const int g = std::gcd(i.width, i.height);
        QMessageBox::information(
            this, tr("Video details"),
            tr("File name: %1\nFPS: %2%3\nResolution: %4x%5 (%6:%7)\nPixel aspect: %8:%9\nLength: %10 frames (%11)\nDecoder: BestSource\nColor matrix: %12")
                .arg(QFileInfo(v->path()).fileName())
                .arg(i.fps, 0, 'f', 3)
                .arg(i.vfr ? tr(" (variable)") : QString())
                .arg(i.width).arg(i.height).arg(i.width / std::max(1, g)).arg(i.height / std::max(1, g))
                .arg(i.sar_num).arg(i.sar_den)
                .arg(i.frames).arg(fs::fmtTime(i.duration_ms))
                .arg((i.untagged ? tr("untagged (guessed %1)").arg(fs::q(i.best_matrix)) : fs::q(i.matrix).isEmpty() ? tr("unknown") : fs::q(i.matrix)) +
                     (i.hdr ? tr(", HDR/WCG") : QString())));
    });
    add("audioToVideo", tr("Set audio position to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->centerOn(tab()->videoTime()); });
    add("markToVideo", tr("Set audio marker to video time"), {}, video,
        [=] { if (tab()->audio()->hasAudio()) tab()->audio()->setMark(tab()->videoTime()); });
    video->addSeparator();
    add("zoomVideo", tr("Zoom video"), {}, video, [=] { tab()->video()->zoomMode(); });
    // the tool row's tools, for keys of one's own (Aegisub has them on A..J, taken here by WASD)
    auto *visualTools = subMenu(video, tr("Visual tools"));
    for (int t = 0; t < VisualTools::ToolCount; ++t)
        add(VisualTools::tools()[t].id, QCoreApplication::translate("VisualTools", VisualTools::tools()[t].name), {},
            visualTools, [=] { tab()->video()->tools()->setTool(t); });
    // Aegisub's video/tool/vclip/*: each mode of the vector clip and drawing tools
    auto *vecModes = subMenu(visualTools, tr("Vector clip modes"));
    for (int k = 0; k < VisualTools::VecModeCount; ++k)
        add(QString("vecMode%1").arg(k), QCoreApplication::translate("VisualTools", VisualTools::kVecModes[k]), {},
            vecModes, [=] { tab()->video()->tools()->setVecMode(k); });
    add("resetZoom", tr("Turn off video zoom"), QKeySequence("Ctrl+Shift+Z"), video, [=] { tab()->video()->resetZoom(); });
    add("resetPan", tr("Center the zoomed video"), {}, video, [=] { tab()->video()->view()->resetPan(); });
    // Aegisub's video/zoom/* (its keys for in and out) and video/focus_seek
    add("zoomIn", tr("Zoom in"), QKeySequence("Ctrl+Num++"), video, [=] {
        auto *v = tab()->video()->view();
        v->zoomBy(1 / 1.2, v->displayRect().center());
    });
    add("zoomOut", tr("Zoom out"), QKeySequence("Ctrl+Num+-"), video, [=] {
        auto *v = tab()->video()->view();
        v->zoomBy(1.2, v->displayRect().center());
    });
    for (const int p : {50, 100, 200})
        add(QString("zoom%1").arg(p), tr("Zoom %1%").arg(p), {}, video, [=] { tab()->video()->view()->zoomScale(p / 100.0); });
    add("focusSeek", tr("Focus the video slider"), QKeySequence("Ctrl+Space"), video,
        [=] { tab()->video()->focusSeek(); });
    addPanelKeys(subMenu(video, tr("Video keys")), VideoBox::keys(), "VideoBox",
              [=](const char *id) { tab()->video()->runCommand(id); });
    auto *detach = add("detachVideo", tr("Detach video"), {}, video, [=] {
        tab()->video()->setDetached(!tab()->video()->detached());
    });
    detach->setCheckable(true);
    connect(video, &QMenu::aboutToShow, this, [=] { detach->setChecked(tab()->video()->detached()); });
}

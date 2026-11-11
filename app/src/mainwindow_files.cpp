// MainWindow's files: opening by kind and pairing what is dropped together, saving and
// conversion, sessions, the recent files, files changed on disk and the autosave.
#include "mainwindow_p.h"

#include "dialogutil.h"
#include "icons.h"
#include "settingsdialog.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
#include <QCollator>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrentRun>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace {
const QStringList kSubsExt = {"ass", "ssa", "srt", "sub", "mpl", "tmp", "txt", "ttxt", "vtt"};
bool isKeyframesFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(64).toLower();
    return head.contains("keyframe format") || head.startsWith("# xvid") || head.startsWith("# ffindex");
}
} // namespace

bool MainWindow::isVideoFile(const QString &path) { return kVideoExt.contains(QFileInfo(path).suffix().toLower()); }

// An untitled tab holding only the empty starting line can take a file (it keeps its video/audio).
bool untouched(TabPanel *tab) {
    return tab && tab->path().isEmpty() && !tab->modified() &&
           (tab->s().row_count() == 0 || (tab->s().row_count() == 1 && tab->s().row(0).text.empty()));
}

QString MainWindow::trackName(const fsb::SubTrack &t) {
    return QString("#%1  %2%3%4")
        .arg(t.index)
        .arg(fs::q(t.codec))
        .arg(t.language.empty() ? QString() : " [" + fs::q(t.language) + "]")
        .arg(t.title.empty() ? QString() : "  " + fs::q(t.title));
}

int MainWindow::pickTrack(const rust::Vec<fsb::SubTrack> &tracks, const QString &question, const QString &title) {
    QStringList items;
    for (const auto &t : tracks) items << trackName(t);
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, title.isEmpty() ? tr("Subtitles in the video") : title, question,
                                               items, 0, false, &ok);
    return ok ? tracks[size_t(std::max(0, int(items.indexOf(pick))))].index : -1;
}

// Kainote's Remove temporary files: the autosaves, the index cache and the backups of replacing in
// files, picked by hand, all of them, or those last changed before a date; with Remove all, of every
// kind at once
void MainWindow::removeTemporary() {
    QDialog d(this);
    d.setWindowTitle(tr("Remove temporary files"));
    auto *v = new QVBoxLayout(&d);
    auto *dateRow = new QHBoxLayout;
    v->addLayout(dateRow);
    dateRow->addWidget(new QLabel(tr("Files older than"), &d));
    auto *before = new QDateEdit(QDate::currentDate().addMonths(-1), &d);
    before->setCalendarPopup(true);
    dateRow->addWidget(before);
    dateRow->addStretch();
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const std::vector<std::pair<QString, QString>> kinds = {
        {tr("Autosave files"), autosaveDir()},
        {tr("Video and audio index files"), QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex"},
        {tr("Backups of files replaced in"), appData + "/ReplaceBackup"}};
    auto *done = new QLabel(&d);
    // the files in dir and below, all or those last changed before the date; how many went
    const auto removeIn = [before](const QString &dir, bool older) {
        int n = 0;
        for (QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QFileInfo f(it.next());
            if (!older || f.lastModified().date() < before->date()) n += QFile::remove(f.filePath());
        }
        return n;
    };
    const auto report = [done](int n) { done->setText(tr("Files removed: %1").arg(n)); };
    const auto sure = [&d](const QString &what) {
        return QMessageBox::question(&d, tr("Remove temporary files"), what) == QMessageBox::Yes;
    };
    auto *everything = new QHBoxLayout;
    v->addLayout(everything);
    auto *allButton = new QPushButton(tr("Remove all files"), &d);
    auto *allOlder = new QPushButton(tr("Remove all older files"), &d);
    everything->addWidget(allButton);
    everything->addWidget(allOlder);
    connect(allButton, &QPushButton::clicked, &d, [=] {
        if (!sure(tr("Remove every autosave, index and backup file?"))) return;
        int n = 0;
        for (const auto &[name, dir] : kinds) n += removeIn(dir, false);
        report(n);
    });
    connect(allOlder, &QPushButton::clicked, &d, [=] {
        int n = 0;
        for (const auto &[name, dir] : kinds) n += removeIn(dir, true);
        report(n);
    });
    for (const auto &[name, dir] : kinds) {
        auto *box = new QGroupBox(name, &d);
        box->setToolTip(QDir::toNativeSeparators(dir));
        auto *h = new QHBoxLayout(box);
        auto *picked = new QPushButton(tr("Remove chosen..."), box);
        auto *all = new QPushButton(tr("Remove all"), box);
        auto *older = new QPushButton(tr("Remove older"), box);
        for (auto *b : {picked, all, older}) h->addWidget(b);
        connect(picked, &QPushButton::clicked, &d, [&d, dir, report] {
            int n = 0;
            for (const QString &f : QFileDialog::getOpenFileNames(&d, tr("Choose files to remove"), dir)) n += QFile::remove(f);
            report(n);
        });
        connect(all, &QPushButton::clicked, &d, [=] {
            if (sure(tr("Remove every file in\n%1?").arg(QDir::toNativeSeparators(dir)))) report(removeIn(dir, false));
        });
        connect(older, &QPushButton::clicked, &d, [=] { report(removeIn(dir, true)); });
        v->addWidget(box);
    }
    v->addWidget(done);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    bb->button(QDialogButtonBox::Close)->setDefault(true); // Enter never removes files
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    d.exec();
}

// Kainote's: a file next to it with the same name (up to the last dot) and one of these extensions
static QString sameNamed(const QString &path, const QStringList &exts) {
    const QFileInfo fi(path);
    for (const QFileInfo &f : fi.dir().entryInfoList(QDir::Files, QDir::Name))
        if (f.completeBaseName().compare(fi.completeBaseName(), Qt::CaseInsensitive) == 0 &&
            exts.contains(f.suffix().toLower()) && f != fi && !(f.suffix().toLower() == "txt" && isKeyframesFile(f.filePath())))
            return f.filePath();
    return {};
}

void MainWindow::openFile(const QString &path, const QString &encoding) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "fses") {
        openSessionFile(path);
    } else if (kVideoExt.contains(ext)) {
        // Kainote: the subtitles named like the video, offered unless they are the tab's already
        if (const QString subs = m_openingPairs ? QString() : sameNamed(path, kSubsExt);
            !subs.isEmpty() && QFileInfo(subs) != QFileInfo(current()->path()) &&
            confirm(tr("Load subtitles named \"%1\"?").arg(QFileInfo(subs).fileName())) &&
            (untouched(current()) || maybeSave(current())) && current()->openSubs(subs))
            addRecent("subs", subs);
        // like Kainote: an empty tab offers the subtitles stored in the video
        if (const auto tracks = fsb::subtitle_tracks(fs::Utf8(path)); !tracks.empty() && untouched(current()))
            if (const int i = pickTrack(tracks, tr("The video contains subtitles. Load them?")); i >= 0)
                current()->openEmbedded(path, i);
        // unless the subtitles loaded above opened it, at the frame and aspect they were saved with
        if (QFileInfo(current()->video()->wanted()) != QFileInfo(path)) current()->openVideo(path);
        updateActions();
    } else if (kAudioExt.contains(ext)) {
        current()->openAudio(path);
    } else if (ext == "txt" && isKeyframesFile(path)) {
        current()->openKeyframes(path);
    } else {
        // plain text, as Aegisub imports it, asks how its actors and comments are marked
        if (ext == "txt" && fsb::is_plain_text(fs::Utf8(path)) && !TabPanel::askTextImport(this)) return;
        // a file open already: its tab, read again if unchanged there (two tabs would save over each other)
        for (auto *t : tabs())
            if (!t->path().isEmpty() && QFileInfo(t->path()) == QFileInfo(path)) {
                showPanel(t);
                if (t->modified()) status(tr("%1 is open already").arg(QFileInfo(path).fileName()));
                else t->openSubs(path, encoding);
                return updateActions();
            }
        // reuse an empty, untouched tab, otherwise open a new one; Kainote's option (its default) replaces
        // the current tab's subtitles instead, keeping its video and audio
        TabPanel *tab = current();
        if (QSettings().value("main/subsInCurrentTab", false).toBool() && tab) {
            if (!maybeSave(tab)) return;
        } else if (!untouched(tab)) {
            tab = newTab();
        }
        if (!tab->openSubs(path, encoding)) return updateActions();
        addRecent("subs", path);
        // Kainote: a tab with no video offers the video named like the subtitles, when they name none
        // (a named one may be loading in the background)
        if (const QString video = m_openingPairs ? QString() : sameNamed(path, kVideoExt);
            !video.isEmpty() && tab->video()->wanted().isEmpty() && tab->s().project_value("Video File").empty() &&
            confirm(tr("Load the video from the folder: \"%1\"?").arg(QFileInfo(video).fileName())))
            tab->openVideo(video);
        updateActions();
    }
}

// Kainote's: files opened together pair subtitles with videos in name order, each pair in its own
// tab; audio and keyframes go to the last one.
void MainWindow::openFiles(QStringList files) {
    if (files.size() == 1) return openFile(files.front());
    const QScopedValueRollback pairing(m_openingPairs, true);
    QCollator order; // "ep2" before "ep10"
    order.setNumericMode(true);
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(files, order);
    QStringList subs, videos, other;
    for (const QString &f : std::as_const(files)) {
        const QString ext = QFileInfo(f).suffix().toLower();
        if (kVideoExt.contains(ext)) videos << f;
        else if (kAudioExt.contains(ext) || (ext == "txt" && isKeyframesFile(f))) other << f;
        else subs << f;
    }
    TabPanel *last = nullptr; // the tab filled last: its video may still be loading
    for (qsizetype i = 0; i < std::max(subs.size(), videos.size()); ++i) {
        if (i < subs.size()) { // takes an empty tab or a new one
            if (i > 0 && !untouched(current())) newTab();
            openFile(subs[i]);
        }
        else if (current() == last || !untouched(current()) || !current()->video()->path().isEmpty()) newTab();
        if (i < videos.size()) openFile(videos[i]);
        last = current();
    }
    for (const QString &f : std::as_const(other)) openFile(f);
}

QMenu *MainWindow::recentMenu(const QString &kind) {
    if (auto *m = m_recent.value(kind)) return m;
    static const QMap<QString, QString> titles = {{"subs", tr("Recently opened subtitles")},
                                                  {"video", tr("Recently opened videos")},
                                                  {"audio", tr("Recently opened audio")},
                                                  {"keyframes", tr("Recently opened keyframes")},
                                                  {"timecodes", tr("Recently opened timecodes")}};
    auto *m = new QMenu(titles.value(kind, kind), this);
    m_recent.insert(kind, m);
    connect(m, &QMenu::aboutToShow, this, [this, m, kind] {
        m->clear();
        QSettings st;
        for (const QString &f : st.value("recent/" + kind).toStringList().mid(0, st.value("main/recentCount", 20).toInt())) {
            m->addAction(fs::menuText(QFileInfo(f).fileName()), this, [this, f, kind] {
                if (kind == "subs") openFile(f);
                else if (kind == "video") current()->openVideo(f);
                else if (kind == "audio") current()->openAudio(f);
                else if (kind == "timecodes") current()->openTimecodes(f);
                else current()->openKeyframes(f);
            })->setToolTip(f);
        }
        if (m->isEmpty()) m->addAction(tr("(empty)"))->setEnabled(false);
    });
    // a toolbar button shows the submenu
    auto *a = new QAction(m->title(), this);
    a->setMenu(m);
    a->setIcon(kind == "subs" ? icon("file-time", kAmber) : kind == "video" ? icon("clock-play", kBlue) : icon("history", kTeal));
    m_actions.insert("recent" + kind.left(1).toUpper() + kind.mid(1), a);
    return m;
}

void MainWindow::checkDisk(TabPanel *t) {
    if (!t || t->path().isEmpty() || m_checkingDisk) return;
    const QFileInfo fi(t->path());
    if (!fi.exists() || fi.lastModified() == t->diskTime()) return;
    // Not under a dialog or a running script: they hold rows and styles of the file as it is,
    // and a macro's result would go over the lines read again. Asked once they are done.
    if (QApplication::activeModalWidget() || m_scriptsRunning) {
        if (!m_diskRetry) {
            m_diskRetry = true;
            QTimer::singleShot(500, this, [this] {
                m_diskRetry = false;
                checkDisk(current());
            });
        }
        return;
    }
    t->noteDisk(); // asked once for each change
    m_checkingDisk = true;
    const auto r = QMessageBox::question(
        this, tr("File changed"),
        (t->modified() ? tr("%1 was changed by another program. Read it again, losing the changes made here?")
                       : tr("%1 was changed by another program. Read it again?"))
            .arg(fi.fileName()));
    m_checkingDisk = false;
    if (r == QMessageBox::Yes) t->openSubs(t->path());
    updateActions();
}

void MainWindow::rereadChanged() {
    for (auto *t : tabs()) {
        if (t->path().isEmpty()) continue;
        const QFileInfo fi(t->path());
        if (!fi.exists() || fi.lastModified() == t->diskTime()) continue;
        if (t->modified()) checkDisk(t); // its changes or the file's: the user's choice
        else t->openSubs(t->path());
    }
    updateActions();
}

bool MainWindow::autosaveTabs(bool wait) {
    QDir().mkpath(autosaveDir());
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss");
    QSet<QString> used; // two untitled tabs in the same second must not share a file
    bool any = false;
    // the current tab last: after a panic in it, the others are written before it can fail again
    auto all = tabs();
    std::ranges::stable_partition(all, [this](TabPanel *t) { return t != current(); });
    for (auto *t : all) {
        const quint64 gen = t->s().generation();
        if (!t->modified() || t->property("autosaved").toULongLong() == gen) continue;
        const QString base = t->path().isEmpty() ? tr("Untitled") : QFileInfo(t->path()).completeBaseName();
        QString name = base;
        for (int k = 2; used.contains(name); ++k) name = QString("%1 (%2)").arg(base).arg(k);
        used.insert(name);
        const QString path = autosaveDir() + "/" + name + "." + stamp + ".AUTOSAVE.ass";
        // the file it is a copy of, for Save once it is recovered
        if (QFile from(path + ".from"); !t->path().isEmpty() && from.open(QIODevice::WriteOnly))
            from.write(t->path().toUtf8());
        if (!wait) {
            // the timer's: serialised and written on a worker so a big file does not stall typing;
            // marked now so the next tick skips it, unmarked if the write fails
            const QVariant was = t->property("autosaved");
            t->setProperty("autosaved", gen);
            auto backup = std::make_shared<rust::Box<fsb::Backup>>(t->s().backup());
            auto *watch = new QFutureWatcher<QString>(this);
            connect(watch, &QFutureWatcher<QString>::finished, this, [this, watch, tab = QPointer(t), gen, was] {
                watch->deleteLater();
                const QString err = watch->result();
                if (err.isEmpty()) return;
                if (tab && tab->property("autosaved").toULongLong() == gen) tab->setProperty("autosaved", was);
                status(err);
            });
            watch->setFuture(QtConcurrent::run([backup, path]() -> QString {
                try {
                    (*backup)->write(fs::Utf8(path));
                    return {};
                } catch (const rust::Error &e) {
                    return QString::fromUtf8(e.what());
                }
            }));
            any = true;
            continue;
        }
        try {
            t->s().autosave(fs::Utf8(path));
            t->setProperty("autosaved", gen);
            any = true;
        } catch (const rust::Error &e) {
            status(QString::fromUtf8(e.what()));
        }
    }
    return any;
}

void MainWindow::addRecent(const QString &kind, const QString &path) {
    QSettings st;
    QStringList l = st.value("recent/" + kind).toStringList();
    l.removeAll(path);
    l.prepend(path);
    st.setValue("recent/" + kind, l.mid(0, st.value("main/recentCount", 20).toInt()));
}

bool MainWindow::saveAs(TabPanel *tab) {
    QString start = tab->path().isEmpty() ? tab->property("savePath").toString() : tab->path();
    if (!start.isEmpty() && !tab->s().lossless(fs::Utf8(start))) {
        // Aegisub's: to an ASS name when the file's format cannot hold it all
        status(tr("Its format cannot hold everything in these subtitles: save them as ASS"));
        const QFileInfo fi(start);
        start = fi.dir().filePath(fi.completeBaseName() + ".ass");
    }
    if (QSettings().value("main/saveWithVideoName").toBool() && !tab->video()->path().isEmpty()) {
        const QFileInfo vf(tab->video()->path());
        start = vf.dir().filePath(vf.completeBaseName() + "." +
                                  (start.isEmpty() ? QString("ass") : QFileInfo(start).suffix()));
    }
    if (start.isEmpty()) start = QSettings().value("main/lastDir").toString() + "/" + tr("Untitled") + ".ass";
    const QString p = QFileDialog::getSaveFileName(this, tr("Save subtitles as"), start, kSubsFilter);
    if (p.isEmpty()) return false;
    if (!tab->s().lossless(fs::Utf8(p)) &&
        QMessageBox::warning(this, tr("Save subtitles as"),
                             tr("%1 cannot hold everything in these subtitles: styles, tags, comments or "
                                "attachments will be missing from the file. Save anyway?")
                                 .arg(QFileInfo(p).suffix().toUpper()),
                             QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
        return false;
    if (!SettingsDialog::applyScriptDefaults(this, tab->s(), p) || !tab->save(p)) return false;
    addRecent("subs", p);
    return true;
}

// a file in a format that cannot hold the subtitles goes to Save as (Aegisub), and so does a
// converted one, not to overwrite a file of the new extension (Kainote)
bool MainWindow::saveTab(TabPanel *tab) {
    if (tab->path().isEmpty() || tab->s().converted() || !tab->s().lossless(fs::Utf8(tab->path())))
        return saveAs(tab);
    if (!SettingsDialog::applyScriptDefaults(this, tab->s(), tab->path())) return false;
    if (!tab->save()) return saveAs(tab);
    addRecent("subs", tab->path());
    return true;
}

bool MainWindow::maybeSave(TabPanel *tab) {
    if (!tab->modified()) return true;
    m_tabs->setCurrentIndex(indexOfPanel(tab));
    const auto r = QMessageBox::question(this, tr("Save changes"),
                                         tr("Save changes to %1?").arg(tab->title()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Discard) return true;
    // no answer (the program quit with the question open) keeps the file as it is, like Cancel
    return r == QMessageBox::Save && saveTab(tab);
}

void MainWindow::convert(const QString &format) {
    auto *tab = current();
    // Kainote's: a conversion that loses something asks first
    if (!tab->s().lossless(fs::Utf8("x." + format.toLower())) &&
        QMessageBox::warning(this, tr("Conversion"),
                             tr("%1 cannot hold everything in these subtitles: styles, tags, comments or "
                                "attachments will be lost. Convert anyway?")
                                 .arg(format),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    // Kainote's conversion settings, shown first when asked for
    if (QSettings().value("convert/showSettings", false).toBool()) {
        QDialog d(this);
        d.setWindowTitle(tr("Conversion"));
        auto *form = new QFormLayout(&d);
        const auto save = SettingsDialog::conversionPage(form, &d);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        form->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        save();
    }
    const auto info = tab->video()->info();
    try {
        tab->s().set_default_fps(QSettings().value("convert/fps", 23.976).toDouble());
        tab->s().convert_with(fs::Utf8(format), SettingsDialog::conversionSpec(info.width, info.height));
    } catch (const rust::Error &e) {
        return error(QString::fromUtf8(e.what()));
    }
    tab->changed();
    updateActions();
}

QStringList MainWindow::sessionFiles(int *front) const {
    QStringList files;
    *front = 0;
    for (auto *t : tabs()) {
        // a tab with no script file comes back with its video, as in Kainote's session
        const QString f = t->path().isEmpty() ? t->video()->path() : t->path();
        if (t == current()) *front = int(files.size()); // counted among the files kept
        if (!f.isEmpty()) files << f;
    }
    return files;
}

void MainWindow::openSession(const QStringList &files, int front) {
    TabPanel *pick = nullptr;
    for (int k = 0; k < files.size(); ++k) {
        if (!QFileInfo::exists(files[k])) continue;
        // each file its own tab: a video alone would otherwise take the next script in
        // (one still being indexed too)
        if (!untouched(current()) || current()->video()->hasVideo() || !current()->video()->wanted().isEmpty()) newTab();
        openFile(files[k]);
        if (k == front || !pick) pick = current();
    }
    if (pick) showPanel(pick);
}

void MainWindow::openSessionFile(const QString &path) {
    QFile f(path);
    const QJsonObject o = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    if (!o["files"].isArray()) return error(tr("%1 is not a session file").arg(QDir::toNativeSeparators(path)));
    openSession(o["files"].toVariant().toStringList(), o["current"].toInt());
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    e->acceptProposedAction();
    // after the drop returns: openFile may ask a question, which must not block the drag source
    QTimer::singleShot(0, this, [this, files] {
        if (!files.isEmpty()) openFiles(files);
    });
}

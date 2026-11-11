// MainWindow's File menu: opening and saving, Aegisub's export, sessions, the log and the
// settings.
#include "mainwindow_p.h"

#include "audiobox.h"
#include "automationhost.h"
#include "autosavedialog.h"
#include "editbox.h"
#include "grid.h"
#include "settingsdialog.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace {
// Export also writes Aegisub's write-only formats, which the end of the name picks
struct ExportType {
    const char *filter, *suffix; // suffix: what a name chosen with the filter must end with
};
const ExportType kExportTypes[] = {
    {QT_TRANSLATE_NOOP("MainWindow", "Subtitles (*.ass *.ssa *.srt *.sub *.mpl *.txt *.tmp *.ttxt *.vtt)"), ""},
    {QT_TRANSLATE_NOOP("MainWindow", "WebVTT (*.vtt)"), ".vtt"},
    {QT_TRANSLATE_NOOP("MainWindow", "MPEG-4 Streaming Text (*.ttxt)"), ".ttxt"},
    {QT_TRANSLATE_NOOP("MainWindow", "Adobe Encore (*.encore.txt)"), ".encore.txt"},
    {QT_TRANSLATE_NOOP("MainWindow", "TranStation (*.transtation.txt)"), ".transtation.txt"},
    {QT_TRANSLATE_NOOP("MainWindow", "EBU subtitling data exchange format, EBU tech 3264 (*.stl)"), ".stl"},
    {QT_TRANSLATE_NOOP("MainWindow", "All files (*)"), ""},
};
} // namespace

void MainWindow::buildFileMenu(Menus &menus) {
    auto tab = [this] { return current(); };

    auto *file = menus.file = barMenu(tr("&File"));
    add("openSubs", tr("Open subtitles"), QKeySequence("Ctrl+O"), file, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open subtitles"),
                                                                QSettings().value("main/lastDir").toString(), kSubsFilter);
        if (!files.isEmpty()) openFiles(files);
        if (!files.isEmpty()) QSettings().setValue("main/lastDir", QFileInfo(files.first()).absolutePath());
    });
    // Aegisub's "Open Subtitles with Charset": for files whose encoding detection guesses wrong
    add("openSubsCharset", tr("Open subtitles with encoding..."), {}, file, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open subtitles"),
                                                       QSettings().value("main/lastDir").toString(), kSubsFilter);
        if (f.isEmpty()) return;
        bool ok = false;
        const QString enc = QInputDialog::getItem(this, tr("Choose encoding"), tr("Text encoding:"),
                                                  {"UTF-8", "UTF-16LE", "UTF-16BE", "windows-1250", "windows-1251",
                                                   "windows-1252", "ISO-8859-2", "Shift_JIS", "GBK", "Big5", "EUC-KR"},
                                                  0, true, &ok);
        if (ok) openFile(f, enc);
    });
    add("save", tr("Save"), QKeySequence("Ctrl+S"), file, [=, this] { saveTab(tab()); });
    add("saveAll", tr("Save all"), {}, file, [this] {
        for (auto *t : tabs())
            if (t->modified()) saveTab(t);
    });
    add("saveAs", tr("Save as..."), QKeySequence("Ctrl+Shift+S"), file, [=, this] { saveAs(tab()); });
    // Kainote's "Load subtitles from MKV": a track of the open video replaces the subtitles
    add("subsFromVideo", tr("Load subtitles from video..."), {}, file, [=, this] {
        const QString video = tab()->video()->path();
        const auto tracks = fsb::subtitle_tracks(fs::Utf8(video));
        if (tracks.empty()) return status(tr("The video contains no subtitles"));
        if (!maybeSave(tab())) return;
        if (const int i = pickTrack(tracks, tr("Subtitles to load:")); i >= 0) tab()->openEmbedded(video, i);
    });
    add("saveTranslation", tr("Save translation"), {}, file, [=, this] {
        // a name of its own: Enter in the dialog must not replace the file with both texts
        const QFileInfo fi(tab()->path());
        const QString start = fi.fileName().isEmpty() ? QString() : fi.dir().filePath(fi.completeBaseName() + ".translation.ass");
        const QString p = QFileDialog::getSaveFileName(this, tr("Save translation"), start, kSubsFilter);
        if (!p.isEmpty()) tab()->saveTranslation(p);
    });
    // Aegisub's Export: a filtered copy in another format/encoding; the open file stays as it is
    add("exportAs", tr("Export as..."), {}, file, [=, this] {
        auto *t = tab();
        const bool video = t->video()->hasVideo();
        QSettings st;
        // Aegisub's DialogExport: every filter (the built-in ones among the scripts' by priority), checked
        // as the file last exported, the settings panel of each checked one beside the list. The scripts'
        // config functions run as it opens, as there.
        ensureAutoload();
        const auto filters = (*m_automation)->export_filters(t->s());
        rust::Vec<fsb::DialogData> panels;
        try {
            auto job = (*m_automation)->start_filter_configs(t->s());
            runScript(*job);
            panels = (*m_automation)->finish_filter_configs(std::move(job));
        } catch (const rust::Error &) {
            return; // cancelled while the config functions ran
        }
        QDialog d(this);
        d.setWindowTitle(tr("Export"));
        auto *filterBox = new QGroupBox(tr("Filters"), &d);
        auto *left = new QVBoxLayout(filterBox);
        auto *list = new QListWidget(filterBox);
        list->setMinimumSize(220, 120);
        const QStringList checked = fs::q(t->s().project_value("Export Filters")).split('|');
        auto description = [&](size_t k) {
            switch (filters[k].kind) {
            case 1:
                return tr("Transform subtitle times, including those in override tags, from an input framerate to an "
                          "output framerate.\n\nThis is useful for converting regular time subtitles to VFRaC time "
                          "subtitles for hardsubbing.\nIt can also be used to convert subtitles to a different speed "
                          "video, such as NTSC to PAL speedup.");
            case 2: return tr("Fixes styles by replacing any style that isn't available on file with Default.");
            default: return fs::q(filters[k].description);
            }
        };
        for (size_t k = 0; k < filters.size(); ++k) {
            const auto &fi = filters[k];
            const QString name = fs::q(fi.name);
            auto *it = new QListWidgetItem(fi.kind == 1   ? tr("Transform Framerate")
                                           : fi.kind == 2 ? tr("Fix Styles")
                                                          : name,
                                           list);
            it->setData(Qt::UserRole, qulonglong(k));
            it->setCheckState(checked.contains(name) ? Qt::Checked : Qt::Unchecked);
        }
        left->addWidget(list, 1);
        auto *moves = new QHBoxLayout;
        auto *up = new QPushButton(tr("Move &Up"), filterBox), *down = new QPushButton(tr("Move &Down"), filterBox);
        auto *all = new QPushButton(tr("Select &All"), filterBox), *none = new QPushButton(tr("Select &None"), filterBox);
        for (auto *b : {up, down, all, none}) moves->addWidget(b);
        left->addLayout(moves);
        auto *about = new QPlainTextEdit(filterBox);
        about->setReadOnly(true);
        about->setFixedHeight(64);
        left->addWidget(about);
        auto rate = [&](const QString &def) {
            auto *c = new QComboBox(&d);
            c->setEditable(true);
            if (video) c->addItem(tr("From video"), "video");
            c->addItems({"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"});
            c->setCurrentText(def);
            return c;
        };
        auto *from = rate(video ? tr("From video") : "23.976"), *to = rate(st.value("export/fpsTo", "25").toString());
        auto *enc = new QComboBox(filterBox);
        enc->addItems({"UTF-8", "UTF-16LE", "windows-1250", "windows-1252", "ISO-8859-2", "windows-1251"});
        {
            // the file's own, as Aegisub keeps it; Aegisub's names ("Unicode (UTF-8)") by what is in brackets
            QString e = fs::q(t->s().project_value("Export Encoding"));
            if (const qsizetype a = e.lastIndexOf('('), b = e.lastIndexOf(')'); a >= 0 && b > a) e = e.mid(a + 1, b - a - 1);
            const int k = enc->findText(e, Qt::MatchFixedString);
            enc->setCurrentIndex(std::max(0, k));
        }
        auto *encRow = new QHBoxLayout;
        encRow->addWidget(new QLabel(tr("Text encoding:"), filterBox));
        encRow->addWidget(enc, 1);
        left->addLayout(encRow);
        // the settings panels, shown for the checked filters
        auto *right = new QVBoxLayout;
        std::map<size_t, QGroupBox *> boxes;
        std::map<size_t, std::vector<QWidget *>> widgets;
        for (size_t k = 0; k < filters.size(); ++k) {
            if (filters[k].kind == 1) {
                auto *box = new QGroupBox(tr("Transform Framerate"), &d);
                auto *form = new QFormLayout(box);
                form->addRow(tr("Subtitles timed to:"), from);
                form->addRow(tr("Convert to:"), to);
                boxes[k] = box;
            } else if (k < panels.size() && !panels[k].controls.empty()) {
                auto *box = new QGroupBox(fs::q(filters[k].name), &d);
                widgets[k] = scriptControls(panels[k].controls, new QGridLayout(box), box);
                boxes[k] = box;
            }
            if (boxes.count(k)) right->addWidget(boxes[k]);
        }
        right->addStretch();
        auto refresh = [&] {
            for (int r = 0; r < list->count(); ++r)
                if (auto b = boxes.find(list->item(r)->data(Qt::UserRole).toULongLong()); b != boxes.end())
                    b->second->setVisible(list->item(r)->checkState() == Qt::Checked);
            d.adjustSize();
        };
        connect(list, &QListWidget::itemChanged, &d, refresh);
        connect(list, &QListWidget::currentRowChanged, &d, [&](int r) {
            about->setPlainText(r < 0 ? QString() : description(list->item(r)->data(Qt::UserRole).toULongLong()));
        });
        auto move = [&](int by) {
            const int r = list->currentRow();
            if (r < 0 || r + by < 0 || r + by >= list->count()) return;
            list->insertItem(r + by, list->takeItem(r));
            list->setCurrentRow(r + by);
        };
        connect(up, &QPushButton::clicked, &d, [&] { move(-1); });
        connect(down, &QPushButton::clicked, &d, [&] { move(1); });
        auto setAll = [&](bool on) {
            for (int r = 0; r < list->count(); ++r) list->item(r)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        };
        connect(all, &QPushButton::clicked, &d, [&] { setAll(true); });
        connect(none, &QPushButton::clicked, &d, [&] { setAll(false); });
        auto *columns = new QHBoxLayout;
        columns->addWidget(filterBox);
        columns->addLayout(right, 1);
        auto *f = new QVBoxLayout(&d);
        f->addLayout(columns, 1);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addWidget(bb);
        list->setCurrentRow(0);
        refresh();
        const bool accepted = d.exec() == QDialog::Accepted;
        // in list order, as they run; the file remembers which, closed or not (Aegisub)
        std::vector<size_t> ids;
        QStringList on;
        for (int r = 0; r < list->count(); ++r)
            if (list->item(r)->checkState() == Qt::Checked) {
                const size_t k = list->item(r)->data(Qt::UserRole).toULongLong();
                ids.push_back(k);
                on << fs::q(filters[k].name);
            }
        t->s().set_project_value("Export Filters", fs::Utf8(on.join('|')));
        if (!accepted) return;
        for (auto &[k, w] : widgets) panels[k].controls = scriptValues(panels[k].controls, w);
        t->s().set_project_value("Export Encoding", fs::Utf8(enc->currentText()));
        st.setValue("export/fpsTo", to->currentText());
        QStringList types;
        for (const auto &e : kExportTypes) types << tr(e.filter);
        QString type = st.value("export/type", types[0]).toString();
        QString p = QFileDialog::getSaveFileName(this, tr("Export subtitles"), QFileInfo(t->path()).absolutePath(),
                                                 types.join(";;"), &type);
        if (p.isEmpty()) return;
        st.setValue("export/type", type);
        // a name typed without the type's double extension still gets it
        if (const qsizetype k = types.indexOf(type); k >= 0) {
            const QString suffix = kExportTypes[k].suffix;
            if (!suffix.isEmpty() && !p.endsWith(suffix, Qt::CaseInsensitive)) {
                if (suffix.count('.') > 1 && p.endsWith(".txt", Qt::CaseInsensitive)) p.chop(4);
                p += suffix;
            }
        }
        auto value = [](QComboBox *c) {
            return c->currentText() == c->itemText(0) && c->itemData(0).isValid() ? QString("video") : c->currentText();
        };
        fsb::ExportSpec spec;
        spec.fps_from = fs::rs(value(from));
        spec.fps_to = fs::rs(value(to)); // read by the chain's Transform Framerate only
        spec.encoding = fs::rs(enc->currentText());
        const QString lower = p.toLower();
        if (lower.endsWith(".encore.txt") || lower.endsWith(".transtation.txt")) {
            // Aegisub's AskForFPS: SMPTE times need a constant rate, so the video's only when it has one
            struct Rate {
                QString name;
                const char *fps;
                bool drop;
            };
            std::vector<Rate> rates;
            const auto vi = t->video()->info();
            if (video && !vi.vfr) rates.push_back({tr("From video (%1)").arg(vi.fps, 0, 'g', 6), "video", false});
            rates.insert(rates.end(), {{tr("15.000 FPS"), "15", false},
                                       {tr("23.976 FPS (Decimated NTSC)"), "24000/1001", false},
                                       {tr("24.000 FPS (FILM)"), "24", false},
                                       {tr("25.000 FPS (PAL)"), "25", false},
                                       {tr("29.970 FPS (NTSC)"), "30000/1001", false},
                                       {tr("29.970 FPS (NTSC with SMPTE dropframe)"), "30000/1001", true},
                                       {tr("30.000 FPS"), "30", false},
                                       {tr("50.000 FPS (PAL x2)"), "50", false},
                                       {tr("59.940 FPS (NTSC x2)"), "60000/1001", false},
                                       {tr("60.000 FPS"), "60", false},
                                       {tr("119.880 FPS (NTSC x4)"), "120000/1001", false},
                                       {tr("120.000 FPS"), "120", false}});
            QStringList names;
            for (const auto &r : rates) names << r.name;
            bool ok = false;
            const QString pick = QInputDialog::getItem(this, tr("FPS"), tr("Please choose the appropriate FPS for the subtitles:"),
                                                       names, 0, false, &ok);
            if (!ok) return;
            const auto &r = rates[size_t(std::max<qsizetype>(0, names.indexOf(pick)))];
            spec.smpte_fps = rust::String(r.fps);
            spec.smpte_drop = r.drop;
        }
        if (lower.endsWith(".stl") && !ebuSettings(spec)) return;
        const auto tc = t->video()->timecodes();
        try {
            bool done = true;
            if (ids.empty()) {
                spec.fps_to = rust::String();
                t->s().export_copy(fs::Utf8(p), spec, {tc.data(), tc.size()});
            } else {
                auto job = (*m_automation)->start_export(fs::slice(ids), {panels.data(), panels.size()}, spec,
                                                         {tc.data(), tc.size()}, t->s());
                runScript(*job);
                done = (*m_automation)->finish_export(std::move(job), t->s(), fs::Utf8(p), spec, {tc.data(), tc.size()});
            }
            if (done) status(tr("Exported %1").arg(QFileInfo(p).fileName()));
        } catch (const std::exception &e) {
            QMessageBox::warning(this, tr("Export"), QString::fromUtf8(e.what()));
        }
    });
    file->addMenu(recentMenu("subs"));
    add("removeSubs", tr("Remove subtitles from the editor"), {}, file, [=, this] {
        if (maybeSave(tab())) tab()->clearSubs();
    });
    auto *videoName = add("saveWithVideoName", tr("Save subtitles using the video name"), {}, file, {});
    videoName->setCheckable(true);
    videoName->setChecked(QSettings().value("main/saveWithVideoName").toBool());
    connect(videoName, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/saveWithVideoName", on); });
    add("openAutosave", tr("Open autosave"), {}, file, [this] {
        AutosaveDialog d(this);
        if (d.exec() != QDialog::Accepted || d.chosen().isEmpty()) return;
        // as an unsaved script: Save asks where, not into the autosave folder that gets cleaned, and
        // starts at the file it was autosaved from
        TabPanel *tab = untouched(current()) ? current() : newTab();
        if (tab->openSubs(d.chosen())) {
            tab->s().detach();
            QFile from(d.chosen() + ".from");
            tab->setProperty("savePath", from.open(QIODevice::ReadOnly) ? QString::fromUtf8(from.readAll()) : QString());
            tab->changed();
            updateActions();
        }
    });
    add("removeTemp", tr("Remove temporary files..."), {}, file, [this] { removeTemporary(); });
    // Log window: automation output and errors.
    auto *logDlg = new QDialog(this);
    logDlg->setWindowTitle(tr("Log"));
    auto *logText = new QPlainTextEdit(logDlg);
    logText->setObjectName("log");
    logText->setReadOnly(true);
    (new QVBoxLayout(logDlg))->addWidget(logText);
    logDlg->resize(560, 360);
    add("showLog", tr("Show / Hide log window"), {}, file, [logDlg] { logDlg->setVisible(!logDlg->isVisible()); });
    auto *session = subMenu(file, tr("Last session"));
    add("loadSession", tr("Load last session"), {}, session, [this] {
        openSession(QSettings().value("session/files").toStringList(), QSettings().value("session/current").toInt());
    });
    // Kainote's session files, in a format of our own (JSON): the tabs' files and the one in front
    const QString sessionFilter = tr("FS Editor session (*.fses)");
    add("loadSessionFile", tr("Load session from file..."), {}, session, [this, sessionFilter] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Load session"),
                                                          QSettings().value("main/sessionDir").toString(), sessionFilter);
        if (path.isEmpty()) return;
        QSettings().setValue("main/sessionDir", QFileInfo(path).absolutePath());
        openSessionFile(path);
    });
    add("saveSessionFile", tr("Save session to file..."), {}, session, [this, sessionFilter] {
        // the files as saved come back: unsaved changes are saved first, or the saving is cancelled
        for (auto *t : tabs())
            if (!maybeSave(t)) return;
        const QString path = QFileDialog::getSaveFileName(this, tr("Save session"),
                                                          QSettings().value("main/sessionDir").toString(), sessionFilter);
        if (path.isEmpty()) return;
        int front = 0;
        const QStringList files = sessionFiles(&front);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) ||
            f.write(QJsonDocument(QJsonObject{{"files", QJsonArray::fromStringList(files)}, {"current", front}})
                        .toJson()) < 0)
            return error(f.errorString());
        QSettings().setValue("main/sessionDir", QFileInfo(path).absolutePath());
        status(tr("Session saved: %1").arg(QFileInfo(path).fileName()));
    });
    auto *ask = add("sessionAtStart", tr("Load last session after program start"), {}, session, {});
    ask->setCheckable(true);
    ask->setChecked(QSettings().value("main/askSession", true).toBool());
    connect(ask, &QAction::toggled, this, [](bool on) { QSettings().setValue("main/askSession", on); });
    file->addSeparator();
    add("settings", tr("Settings"), {}, file, [this] {
        const auto apply = [this] {
            (*m_automation)->set_trace_level(QSettings().value("automation/traceLevel", 3).toInt());
            (*m_automation)->set_search_paths(fs::Utf8(QSettings().value("automation/autoloadPath").toString()),
                                              fs::Utf8(QSettings().value("automation/includePath").toString()));
            watchScripts();
            if (m_toolbar) m_toolbar->setIconSize(QSize(22, 22) * QSettings().value("main/toolbarIconSize", 22).toInt() / 22);
            action("sessionAtStart")->setChecked(QSettings().value("main/askSession", true).toBool()); // the menu's tick
            // another autosave interval counts from now, not from the end of the one running
            if (auto *timer = findChild<QTimer *>("autosave", Qt::FindDirectChildrenOnly)) {
                const int ms = std::max(QSettings().value("main/autosaveSeconds", 60).toInt(), 10) * 1000;
                if (timer->interval() != ms) timer->start(ms);
            }
            for (auto *t : tabs()) { // the limits colour the grid and the edit box
                t->grid()->viewport()->update();
                t->grid()->applySettings();
                t->edit()->reload();
                t->audio()->docChanged(); // inactive lines
                t->audio()->settingsChanged();
                t->s().set_undo_limit(size_t(QSettings().value("main/undoLevels", 500).toInt()));
                emit t->titleChanged(); // the tab title's length
            }
        };
        for (;;) {
            SettingsDialog d(this);
            connect(&d, &SettingsDialog::applied, this, apply);
            if (d.exec() != SettingsDialog::Restored) break;
        }
    });
    add("hotkeys", tr("Hotkeys..."), {}, file, [this] { editHotkeys(); });
    add("exit", tr("Exit"), QKeySequence("Alt+F4"), file, [this] { close(); });
}

bool MainWindow::ebuSettings(fsb::ExportSpec &spec) {
    QSettings st;
    st.beginGroup("ebuStl");
    QDialog d(this);
    d.setWindowTitle(tr("Export to EBU STL format"));
    auto *f = new QFormLayout(&d);
    auto combo = [&](const QStringList &items, const char *key, int def) {
        auto *c = new QComboBox(&d);
        c->addItems(items);
        c->setCurrentIndex(std::clamp(st.value(key, def).toInt(), 0, int(items.size()) - 1));
        return c;
    };
    auto *tv = combo({tr("23.976 fps (non-standard, STL24.01)"), tr("24 fps (non-standard, STL24.01)"),
                      tr("25 fps (STL25.01)"), tr("29.97 fps (non-dropframe, STL30.01)"),
                      tr("29.97 fps (dropframe, STL30.01)"), tr("30 fps (STL30.01)")},
                     "tvStandard", 0);
    auto *enc = combo({tr("ISO 6937-2 (Latin/Western Europe)"), tr("ISO 8859-5 (Cyrillic)"), tr("ISO 8859-6 (Arabic)"),
                       tr("ISO 8859-7 (Greek)"), tr("ISO 8859-8 (Hebrew)"), tr("UTF-8 Unicode (non-standard)")},
                      "textEncoding", 0);
    auto *wrap = combo({tr("Automatically wrap long lines (ASS)"), tr("Automatically wrap long lines (Balanced)"),
                        tr("Abort if any lines are too long"), tr("Skip lines that are too long")},
                       "lineWrapping", 1);
    auto *len = new QSpinBox(&d);
    len->setRange(10, 99);
    len->setValue(st.value("maxLineLength", 42).toInt());
    auto *align = new QCheckBox(tr("Translate alignments"), &d);
    align->setChecked(st.value("translateAlignments", true).toBool());
    auto *offset = new QLineEdit(st.value("timecodeOffset", "00:00:00:00").toString(), &d);
    offset->setInputMask("99:99:99:99");
    auto *inclusive = new QCheckBox(tr("Out-times are inclusive"), &d);
    inclusive->setChecked(st.value("inclusiveEndTimes", true).toBool());
    auto *display = combo({tr("Open captions"), tr("Level-1 teletext"), tr("Level-2 teletext")}, "displayStandard", 0);
    f->addRow(tr("TV standard:"), tv);
    f->addRow(tr("Text encoding:"), enc);
    f->addRow(tr("Line wrapping:"), wrap);
    f->addRow(tr("Max. line length:"), len);
    f->addRow(align);
    f->addRow(tr("Time code offset:"), offset);
    f->addRow(inclusive);
    f->addRow(tr("Display standard:"), display);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    f->addRow(bb);
    if (d.exec() != QDialog::Accepted) return false;
    st.setValue("tvStandard", tv->currentIndex());
    st.setValue("textEncoding", enc->currentIndex());
    st.setValue("lineWrapping", wrap->currentIndex());
    st.setValue("maxLineLength", len->value());
    st.setValue("translateAlignments", align->isChecked());
    st.setValue("timecodeOffset", offset->text());
    st.setValue("inclusiveEndTimes", inclusive->isChecked());
    st.setValue("displayStandard", display->currentIndex());
    spec.ebu_tv_standard = std::uint8_t(tv->currentIndex());
    spec.ebu_text_encoding = std::uint8_t(enc->currentIndex());
    spec.ebu_line_wrapping = std::uint8_t(wrap->currentIndex());
    spec.ebu_max_line_length = len->value();
    spec.ebu_translate_alignments = align->isChecked();
    spec.ebu_timecode_offset = fs::rs(offset->text());
    spec.ebu_inclusive_end_times = inclusive->isChecked();
    spec.ebu_display_standard = std::uint8_t(display->currentIndex());
    return true;
}

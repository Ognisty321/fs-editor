#include "settingsdialog.h"

#include "associations.h"
#include "colorpicker.h"
#include "dialogutil.h"
#include "editbox.h"
#include "grid.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

std::function<void()> SettingsDialog::conversionPage(QFormLayout *form, QWidget *owner) {
    const QSettings st;
    auto *style = new QComboBox(owner);
    style->addItem(tr("The file's own"), QString());
    for (const QString &cat : styleCatalogs())
        for (const auto &s : catalogStyles(cat)) {
            const QString name = fs::q(s.name);
            style->addItem(cat + " / " + name, cat + "\n" + name);
        }
    style->setCurrentIndex(std::max(0, style->findData(st.value("convert/style"))));
    form->addRow(tr("Style of subtitles converted to ASS"), style);
    auto *tags = new QLineEdit(st.value("convert/tags").toString(), owner);
    tags->setPlaceholderText("{\\be1}");
    form->addRow(tr("Tags put before each line converted to ASS"), tags);
    auto *ends = new QCheckBox(tr("New end times by the length of the text"), owner);
    ends->setChecked(st.value("convert/newEnds", false).toBool());
    form->addRow(ends);
    auto *perChar = new QSpinBox(owner);
    perChar->setRange(30, 1000);
    perChar->setSuffix(" ms");
    perChar->setValue(st.value("convert/msPerChar", 110).toInt());
    form->addRow(tr("Time per character (TMP always gets new end times)"), perChar);
    auto *fps = new QComboBox(owner);
    fps->setEditable(true);
    fps->addItems({"23.976", "24", "25", "29.97", "30", "50", "59.94", "60"});
    fps->setValidator(new QDoubleValidator(1, 1000, 3, fps));
    fps->setCurrentText(st.value("convert/fps", "23.976").toString());
    form->addRow(tr("MicroDVD frame rate without a video"), fps);
    auto *show = new QCheckBox(tr("Show these settings before a conversion"), owner);
    show->setChecked(st.value("convert/showSettings", false).toBool());
    form->addRow(show);
    return [=] {
        QSettings st;
        st.setValue("convert/style", style->currentData());
        st.setValue("convert/tags", tags->text());
        st.setValue("convert/newEnds", ends->isChecked());
        st.setValue("convert/msPerChar", perChar->value());
        st.setValue("convert/fps", QLocale::c().toDouble(fps->currentText().replace(',', '.')) > 0
                                       ? fps->currentText().replace(',', '.')
                                       : QString("23.976"));
        st.setValue("convert/showSettings", show->isChecked());
    };
}

// Kainote's default script properties: these [Script Info] keys take the values set when ASS is saved
static const std::pair<const char *, const char *> kScriptDefaults[] = {
    {QT_TRANSLATE_NOOP("SettingsDialog", "Title"), "Title"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Author"), "Original Script"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Translator"), "Original Translation"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Proofreading"), "Original Editing"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Timer"), "Original Timing"},
    {QT_TRANSLATE_NOOP("SettingsDialog", "Editing"), "Script Updated By"}};

static QString scriptDefaultKey(const char *key, const char *what = "") {
    return QStringLiteral("scriptDefaults/%1%2").arg(QLatin1String(key), QLatin1String(what));
}

std::function<void()> SettingsDialog::scriptDefaultsPage(QFormLayout *form, QWidget *owner) {
    const QSettings st;
    auto *note = new QLabel(tr("The properties ticked are set to these values when subtitles are saved as ASS."), owner);
    note->setWordWrap(true);
    form->addRow(note);
    std::vector<std::tuple<const char *, QCheckBox *, QLineEdit *>> fields;
    for (auto [label, key] : kScriptDefaults) {
        auto *on = new QCheckBox(tr(label), owner);
        on->setChecked(st.value(scriptDefaultKey(key, "On"), false).toBool());
        auto *value = new QLineEdit(st.value(scriptDefaultKey(key)).toString(), owner);
        form->addRow(on, value);
        fields.emplace_back(key, on, value);
    }
    auto *ask = new QCheckBox(tr("Ask before changing the properties"), owner);
    ask->setChecked(st.value("scriptDefaults/ask", true).toBool());
    form->addRow(ask);
    return [=] {
        QSettings st;
        for (auto [key, on, value] : fields) {
            st.setValue(scriptDefaultKey(key, "On"), on->isChecked());
            st.setValue(scriptDefaultKey(key), value->text());
        }
        st.setValue("scriptDefaults/ask", ask->isChecked());
    };
}

QStringList SettingsDialog::scriptDefaultKeys() {
    QStringList keys{"scriptDefaults/ask"};
    for (auto [label, key] : kScriptDefaults) keys << scriptDefaultKey(key) << scriptDefaultKey(key, "On");
    return keys;
}

bool SettingsDialog::applyScriptDefaults(QWidget *parent, fsb::Session &s, const QString &path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext != "ass" && ext != "ssa") return true;
    QSettings st;
    std::vector<std::pair<const char *, QString>> changes; // key, new value; label by index below
    std::vector<const char *> labels;
    for (auto [label, key] : kScriptDefaults) {
        const QString v = st.value(scriptDefaultKey(key)).toString();
        if (st.value(scriptDefaultKey(key, "On"), false).toBool() && fs::q(s.info_value(key)) != v) {
            changes.emplace_back(key, v);
            labels.push_back(label);
        }
    }
    if (changes.empty()) return true;
    std::vector<bool> take(changes.size(), true);
    if (st.value("scriptDefaults/ask", true).toBool()) {
        QDialog d(parent);
        d.setWindowTitle(tr("Script properties"));
        auto *lay = new QVBoxLayout(&d);
        lay->addWidget(new QLabel(tr("Change these properties of the file to the default ones?"), &d));
        std::vector<QCheckBox *> boxes;
        for (size_t i = 0; i < changes.size(); ++i) {
            const QString old = fs::q(s.info_value(changes[i].first));
            auto *b = new QCheckBox(tr("%1: %2 → %3")
                                        .arg(tr(labels[i]), old.isEmpty() ? tr("(none)") : old,
                                             changes[i].second.isEmpty() ? tr("(none)") : changes[i].second),
                                    &d);
            b->setChecked(true);
            lay->addWidget(b);
            boxes.push_back(b);
        }
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Change"));
        auto *keep = bb->addButton(tr("Keep the file's"), QDialogButtonBox::ActionRole);
        auto *stop = bb->addButton(tr("Change and turn off confirmation"), QDialogButtonBox::ActionRole);
        lay->addWidget(bb);
        // Cancel cancels the saving, as in Kainote; keeping the file's saves it unchanged
        QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        QObject::connect(keep, &QPushButton::clicked, &d, [&d] { d.done(2); });
        QObject::connect(stop, &QPushButton::clicked, &d, [&d] { d.done(3); });
        const int r = d.exec();
        if (r == QDialog::Rejected) return false;
        if (r == 2) return true;
        if (r == 3) st.setValue("scriptDefaults/ask", false);
        for (size_t i = 0; i < boxes.size(); ++i) take[i] = boxes[i]->isChecked();
    }
    for (size_t i = 0; i < changes.size(); ++i)
        if (take[i]) s.set_info_value(changes[i].first, fs::Utf8(changes[i].second), "scriptDefaults");
    return true;
}

fsb::ConvertSpec SettingsDialog::conversionSpec(int w, int h) {
    const QSettings st;
    fsb::ConvertSpec spec;
    spec.res_x = w > 0 && h > 0 ? w : st.value("subs/defaultResWidth", 1920).toInt();
    spec.res_y = w > 0 && h > 0 ? h : st.value("subs/defaultResHeight", 1080).toInt();
    if (const QStringList cs = st.value("convert/style").toString().split('\n'); cs.size() == 2)
        for (const auto &s : catalogStyles(cs[0]))
            if (fs::q(s.name) == cs[1]) spec.style = fsb::style_lines({&s, 1});
    spec.tags = fs::rs(st.value("convert/tags").toString());
    spec.new_ends = st.value("convert/newEnds", false).toBool();
    spec.ms_per_char = st.value("convert/msPerChar", 110).toInt();
    return spec;
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    QSettings st;
    // Kainote's layout: the pages listed on the left
    auto *list = new QListWidget(this);
    auto *pages = new QStackedWidget(this);
    const auto page = [list, pages](const QString &name) {
        list->addItem(name);
        auto *w = new QWidget(pages);
        pages->addWidget(w);
        auto *form = new QFormLayout(w);
        form->setContentsMargins(0, 0, 0, 0);
        return form;
    };
    QFormLayout *editor = page(tr("Editor")), *subs = page(tr("Subtitles")), *video = page(tr("Video")),
                *audio = page(tr("Audio")), *automation = page(tr("Automation")), *colors = page(tr("Colors")),
                *conversion = page(tr("Conversion")), *scriptDefaults = page(tr("Script properties"));
    const auto saveConversion = conversionPage(conversion, this);
    const auto saveScriptDefaults = scriptDefaultsPage(scriptDefaults, this);
    buildColorsPage(colors);
    // Kainote's file associations: the types ticked open here from Explorer
    std::vector<std::pair<QString, QCheckBox *>> types;
    if (assoc::supported()) {
        QFormLayout *files = page(tr("File associations"));
        QSettings classes(assoc::kClasses, QSettings::NativeFormat);
        auto *note = new QLabel(tr("Subtitle files of the types ticked open in this program. A program picked for a "
                                   "type in Explorer stays its default; this one is then in its \"Open with\" list."),
                                this);
        note->setWordWrap(true);
        files->addRow(note);
        for (const QString &ext : assoc::types()) {
            auto *b = new QCheckBox("." + ext, this);
            b->setChecked(assoc::isOn(classes, ext));
            files->addRow(b);
            types.emplace_back(ext, b);
        }
    }
    struct Check { const char *key; const char *label; bool def; QFormLayout *page; };
    const Check checks[] = {
        {"main/askSession", QT_TR_NOOP("Load the last session at startup"), true, editor},
        {"main/saveUiState", QT_TR_NOOP("Save UI state in subtitles files"), true, editor},
        {"main/backupOriginal", QT_TR_NOOP("Keep a copy of each opened file (.ORIGINAL) in the autosave folder"), true,
         editor},
        {"main/saveOnChange", QT_TR_NOOP("Save the file after every change"), false, editor},
        {"main/checkUpdates", QT_TR_NOOP("Check for a newer version at startup (once a day, asks github.com)"), false,
         editor},
        {"edit/syntax", QT_TR_NOOP("Syntax highlighting in the edit box"), true, editor},
        {"edit/callTips", QT_TR_NOOP("Call tips: the parameters of the tag at the cursor"), false, editor},
        {"video/sliderKeyframes", QT_TR_NOOP("Show keyframes in the seek bar"), true, video},
        {"video/autohideTools", QT_TR_NOOP("Only show visual tools when the mouse is over the video"), false, video},
        {"audio/drawKeyframesKaraoke", QT_TR_NOOP("Show keyframes in the audio in karaoke mode"), true, audio},
        {"grid/focusOnClick", QT_TR_NOOP("A click in the grid moves the keyboard focus there"), true, subs},
        // Kainote's behaviours
        {"main/subsInCurrentTab", QT_TR_NOOP("Open subtitles in the current tab, in place of its subtitles"), false,
         editor},
        {"edit/timesStayOnLine", QT_TR_NOOP("Enter in the times stays on the line"), false, editor},
        {"edit/spellDoubleClick", QT_TR_NOOP("Spelling suggestions on a double click on a misspelled word"), false,
         editor},
        {"edit/samplerOnLeftClick",
         QT_TR_NOOP("A left click on a colour button opens the colour sampler, a right click the colour picker"), false,
         editor},
        {"grid/centerActive", QT_TR_NOOP("Center the active line in the grid when it changes"), false, subs},
        {"grid/duplicateKeepsSelection", QT_TR_NOOP("Duplicated lines leave the originals selected"), false, subs},
        {"video/clickPauses", QT_TR_NOOP("A left click pauses the video"), false, video},
        {"video/openAtActive", QT_TR_NOOP("Open a video at the active line when the subtitles saved no position"),
         false, video},
        {"video/fullscreenOnStart", QT_TR_NOOP("Full screen for a video opened alone with the program"), false, video},
        {"video/visualWarningsOff", QT_TR_NOOP("No warnings in the visual tools"), false, video},
        {"grid/visibleAfterFullscreen", QT_TR_NOOP("Leaving full screen makes the line seen active"), false, subs},
        {"grid/selectFromLastTab", QT_TR_NOOP("A tab picked makes active the line at the time of the last tab's line"),
         false, subs},
        {"grid/loadSorted", QT_TR_NOOP("Sort the lines by start time when opening"), false, subs},
        {"shift/followTab", QT_TR_NOOP("The shift times values go along to the tab picked"), false, editor},
        {"edit/softBreak", QT_TR_NOOP("Shift+Enter adds \\n"), false, editor},
        {"subs/defaultResAuto", QT_TR_NOOP("New files use the resolution of the first video opened"), true, subs},
        {"subs/cpsIgnoreWhitespace", QT_TR_NOOP("Ignore whitespace in characters per second"), true, subs},
        {"subs/cpsIgnorePunctuation", QT_TR_NOOP("Ignore punctuation in characters per second"), true, subs},
        {"subs/wrapsCountAll", QT_TR_NOOP("Count spaces and punctuation in the lengths of wraps"), true, subs},
        {"grid/ctrlClickActive", QT_TR_NOOP("A line added to the selection with Ctrl becomes active"), true, subs},
        {"audio/mergeN", QT_TR_NOOP("Automatic karaoke splitting merges every \"n\" with the syllable before"), true,
         audio},
        {"audio/karaokeMoveOnClick", QT_TR_NOOP("A click on a syllable next to the one picked moves their boundary"),
         false, audio},
        {"video/openAudio", QT_TR_NOOP("Open audio together with video"), true, video},
        {"video/askResolution", QT_TR_NOOP("Ask to resample the subtitles to a video of another resolution"), true,
         video},
        {"video/untaggedWarning", QT_TR_NOOP("Warn on untagged video color matrix"), true, video},
        {"video/hdrWarning", QT_TR_NOOP("Warn on HDR/WCG video"), true, video},
        {"audio/snapKeyframes", QT_TR_NOOP("Snap audio boundaries to keyframes"), false, audio},
        {"audio/snapLines", QT_TR_NOOP("Snap audio boundaries to other lines"), false, audio},
        {"audio/inactiveComments", QT_TR_NOOP("Show inactive comments in the audio"), false, audio},
        {"audio/wheelZooms", QT_TR_NOOP("Mouse wheel zooms the audio (Shift scrolls)"), false, audio},
        {"audio/autoFocus", QT_TR_NOOP("Focus the audio when the mouse is over it"), true, audio},
        {"audio/playOnStep", QT_TR_NOOP("Play audio when stepping in video"), false, audio},
        {"audio/drawKeyframes", QT_TR_NOOP("Show keyframes in the audio"), true, audio},
        {"audio/drawCursorTime", QT_TR_NOOP("Show the time under the mouse in the audio"), true, audio},
        {"audio/drawVideoPosition", QT_TR_NOOP("Show the video position in the audio"), true, audio},
        {"audio/drawSelection", QT_TR_NOOP("Show the selection background in the audio"), true, audio},
        {"audio/drawSeconds", QT_TR_NOOP("Show seconds boundaries in the audio"), true, audio},
    };
    std::vector<std::pair<QString, QCheckBox *>> boxes;
    for (const auto &c : checks) {
        auto *b = new QCheckBox(tr(c.label), this);
        b->setChecked(st.value(c.key, c.def).toBool());
        c.page->addRow(b);
        boxes.emplace_back(c.key, b);
    }
    auto *inactive = new QComboBox(this);
    inactive->addItems({tr("None"), tr("Previous line"), tr("Previous and next line"), tr("All lines")});
    inactive->setCurrentIndex(std::clamp(st.value("audio/inactiveLines", 3).toInt(), 0, 3));
    audio->addRow(tr("Inactive lines in the audio"), inactive);
    auto *leadIn = new QSpinBox(this), *leadOut = new QSpinBox(this);
    for (auto *s : {leadIn, leadOut}) {
        s->setRange(0, 5000);
        s->setSuffix(" ms");
    }
    leadIn->setValue(st.value("audio/leadIn", 200).toInt());
    leadOut->setValue(st.value("audio/leadOut", 300).toInt());
    // Kainote's preferred audio tracks of a file with several
    auto *tracks = new QLineEdit(st.value("audio/preferredTracks").toString(), this);
    tracks->setPlaceholderText(tr("e.g. jpn;Japanese"));
    tracks->setToolTip(tr("Languages or parts of track names, in order of preference, separated by \";\". A file "
                          "with several audio tracks loads the first one that matches, or asks."));
    audio->addRow(tr("Preferred audio tracks"), tracks);
    // Kainote's ASS tags replacement character, shown in the grid's simplified tags mode
    auto *tagMark = new QLineEdit(st.value("grid/tagMark", QString(QChar(0x2600))).toString(), this);
    tagMark->setMaxLength(8);
    subs->addRow(tr("Simplified tags shown as"), tagMark);
    // Kainote's font filter text of the style editor
    auto *fontFilter = new QLineEdit(st.value("style/fontFilterText", kFontFilterChars).toString(), this);
    subs->addRow(tr("Characters the font filter requires"), fontFilter);
    auto *device = new QComboBox(this);
    device->addItem(tr("Default"), "");
    for (const auto &n : fsb::audio_devices()) device->addItem(fs::q(n), fs::q(n));
    device->setCurrentIndex(std::max(0, device->findData(st.value("audio/device", ""))));
    device->setToolTip(tr("For audio opened after it is changed"));
    audio->addRow(tr("Audio output device"), device);
    audio->addRow(tr("Audio lead-in"), leadIn);
    audio->addRow(tr("Audio lead-out"), leadOut);
    auto *autosave = new QSpinBox(this);
    autosave->setRange(0, 3600);
    autosave->setSuffix(" s");
    autosave->setSpecialValueText(tr("off"));
    autosave->setValue(st.value("main/autosaveSeconds", 60).toInt());
    editor->addRow(tr("Autosave every"), autosave);
    auto *autosaveFolder = new QLineEdit(QDir::toNativeSeparators(st.value("main/autosaveDir").toString()), this);
    autosaveFolder->setPlaceholderText(QDir::toNativeSeparators(autosaveDir()));
    editor->addRow(tr("Autosave folder"), autosaveFolder);
    // the character counter's limits, red in the grid and the edit box (Aegisub's, Kainote's); the
    // characters per second redden from the first to fully at the second (Aegisub's warning and error)
    const std::pair<const char *, int> limitKeys[] = {
        {"subs/maxCps", 15}, {"subs/cpsError", 30}, {"subs/maxLineLength", 43}, {"subs/maxLines", 2}};
    const char *limitLabels[] = {QT_TR_NOOP("Maximum characters per second"),
                                 QT_TR_NOOP("Characters per second shown fully red"), QT_TR_NOOP("Maximum line length"),
                                 QT_TR_NOOP("Maximum lines")};
    std::vector<std::pair<QString, QSpinBox *>> spins;
    for (int k = 0; k < 4; ++k) {
        auto *s = new QSpinBox(this);
        s->setRange(1, k == 3 ? 20 : 1000);
        s->setValue(st.value(limitKeys[k].first, limitKeys[k].second).toInt());
        subs->addRow(tr(limitLabels[k]), s);
        spins.emplace_back(limitKeys[k].first, s);
    }
    auto *duration = new QSpinBox(this); // of an inserted line (Aegisub's Timing/Default Duration)
    duration->setRange(0, 60000);
    duration->setSingleStep(500);
    duration->setSuffix(" ms");
    duration->setValue(st.value("subs/defaultDuration", 3000).toInt());
    subs->addRow(tr("Default line duration"), duration);
    spins.emplace_back("subs/defaultDuration", duration);
    auto *fastJump = new QSpinBox(this); // Aegisub's Video/Slider/Fast Jump Step
    fastJump->setRange(1, 1000);
    fastJump->setSuffix(tr(" frames"));
    fastJump->setValue(st.value("video/fastJump", 10).toInt());
    video->addRow(tr("Frames skipped by a fast jump"), fastJump);
    spins.emplace_back("video/fastJump", fastJump);
    // Aegisub's Subtitle/Default Resolution, when the first video does not set it, and App/Toolbar Icon Size
    const std::tuple<const char *, int, int, int, QFormLayout *, const char *> moreSpins[] = {
        {"subs/defaultResWidth", 1920, 1, 16384, subs, QT_TR_NOOP("Default width of new files")},
        {"subs/defaultResHeight", 1080, 1, 16384, subs, QT_TR_NOOP("Default height of new files")},
        {"main/toolbarIconSize", 22, 12, 64, editor, QT_TR_NOOP("Toolbar icon size")},
        {"audio/spectrumCacheMb", 128, 2, 1024, audio, QT_TR_NOOP("Spectrum cache memory (MB)")},
        // Aegisub's FFmpegSource Cache/Files and Size for the index files, Kainote's cache files limit
        {"video/indexCacheFiles", 20, 0, 100000, video, QT_TR_NOOP("Index cache files kept (0: any number)")},
        {"video/indexCacheMb", 42, 1, 100000, video, QT_TR_NOOP("Index cache size (MB)")},
        // Kainote's offsets of the times Insert start (end) time from video puts in
        {"video/insertStartOffset", 0, -100000, 100000, video, QT_TR_NOOP("Offset of a start time from video (ms)")},
        {"video/insertEndOffset", 0, -100000, 100000, video, QT_TR_NOOP("Offset of an end time from video (ms)")},
        // Aegisub's Limits/Undo Levels and Find Replace
        {"main/undoLevels", 500, 2, 10000, editor, QT_TR_NOOP("Undo levels")},
        {"find/recentCount", 20, 0, 99, editor, QT_TR_NOOP("Find and replace history entries")},
        // Kainote's max autosave files (also those of the files as opened), counted at startup
        {"main/autosaveMax", 1000, 2, 1000000, editor, QT_TR_NOOP("Autosave files kept")},
        {"main/tabTitleChars", 40, 20, 150, editor, QT_TR_NOOP("Characters of a tab's title")},
        // Kainote's start video zoom in percent
        {"video/zoomPercent", 200, 100, 1000, video, QT_TR_NOOP("Video zoom starts at (%)")}};
    for (auto [key, def, lo, hi, form, label] : moreSpins) {
        auto *s = new QSpinBox(this);
        s->setRange(lo, hi);
        s->setValue(st.value(key, def).toInt());
        form->addRow(tr(label), s);
        spins.emplace_back(key, s);
    }
    // Aegisub's line boundaries thickness, snap distance and start drag sensitivity; Kainote's mark play time
    const std::tuple<const char *, int, int, int, const char *, const char *> audioSpins[] = {
        {"audio/boundaryWidth", 2, 1, 10, QT_TR_NOOP("Line boundaries thickness"), " px"},
        {"audio/snapDistance", 8, 0, 50, QT_TR_NOOP("Snapping distance"), " px"},
        {"audio/dragSensitivity", 3, 0, 50, QT_TR_NOOP("Distance before a drag times the line"), " px"},
        {"audio/markPlayMs", 1000, 100, 10000, QT_TR_NOOP("Time played before and after the mark"), " ms"},
        {"audio/delay", 0, -60000, 60000, QT_TR_NOOP("Audio delay (for audio opened after)"), " ms"}};
    for (auto [key, def, lo, hi, label, suffix] : audioSpins) {
        auto *s = new QSpinBox(this);
        s->setRange(lo, hi);
        s->setSuffix(suffix);
        s->setValue(st.value(key, def).toInt());
        audio->addRow(tr(label), s);
        spins.emplace_back(key, s);
    }
    auto *recent = new QSpinBox(this); // Aegisub's Limits/MRU
    recent->setRange(1, 99);
    recent->setValue(st.value("main/recentCount", 20).toInt());
    editor->addRow(tr("Recently opened files listed"), recent);
    spins.emplace_back("main/recentCount", recent);
    // the fonts picked, written with the rest by OK or Apply (Cancel leaves the ones set)
    const auto fonts = std::make_shared<std::map<QString, QFont>>();
    const auto fontButton = [this, editor, fonts](const QString &key, const QFont &def, const QString &label,
                                                  QFormLayout *form = nullptr) {
        const auto name = [](const QFont &f) { return QString("%1, %2 pt").arg(f.family()).arg(f.pointSize()); };
        auto *b = new QPushButton(name(QSettings().value(key, def).value<QFont>()), this);
        connect(b, &QPushButton::clicked, this, [=, this] {
            bool ok;
            const auto picked = fonts->find(key);
            const QFont f = QFontDialog::getFont(
                &ok, picked != fonts->end() ? picked->second : QSettings().value(key, def).value<QFont>(), this);
            if (ok) {
                (*fonts)[key] = f;
                b->setText(name(f));
            }
        });
        (form ? form : editor)->addRow(label, b);
    };
    fontButton("grid/font", QFont("Tahoma", 10), tr("Grid font (applies to new tabs)"));
    // Kainote's program font
    fontButton("main/programFont", QApplication::font(), tr("Program font (applies from the next start)"));
    // Aegisub's Subtitle/Edit Box/Font Face and Font Size
    fontButton("edit/font", QApplication::font(), tr("Edit box font (applies to new tabs)"));
    // Aegisub's Audio/Karaoke font
    fontButton("audio/karaokeFont", QFont(QApplication::font().family(), 8), tr("Karaoke syllables font"), audio);
    auto *dictDir = new QLineEdit(QDir::toNativeSeparators(spell::dir()), this);
    dictDir->setMinimumWidth(dictDir->fontMetrics().averageCharWidth() * 48);
    editor->addRow(tr("Dictionary folder (Hunspell .aff/.dic)"), dictDir);
    auto *dictLang = new QComboBox(this);
    spell::fillLanguages(dictLang, st.value("spell/lang").toString());
    connect(dictDir, &QLineEdit::editingFinished, this, [dictDir, dictLang] {
        // the languages of the folder typed; the folder is set by OK or Apply
        const QString keep = dictLang->currentData().toString();
        spell::fillLanguages(dictLang, keep, QDir::fromNativeSeparators(dictDir->text().trimmed()));
    });
    editor->addRow(tr("Spell check language"), dictLang);
    auto *fontsDir = new QLineEdit(QDir::toNativeSeparators(st.value("fonts/externalDir").toString()), this);
    fontsDir->setToolTip(tr("Fonts in this folder show in the video and the font lists and are collected, without being "
                            "installed; for videos opened after it is set"));
    editor->addRow(tr("External fonts folder"), fontsDir);
    auto *language = new QComboBox(this);
    language->addItem(tr("System"), "");
    language->addItem("English", "en");
    language->addItem("Polski", "pl");
    language->setCurrentIndex(std::max(0, language->findData(st.value("main/language", ""))));
    editor->addRow(tr("Language (after restart)"), language);
    // Aegisub's default style catalogs: of new files and of each imported format
    std::vector<std::pair<QString, QComboBox *>> catalogs;
    const std::pair<const char *, const char *> catalogKeys[] = {
        {"styles/newFileCatalog", QT_TR_NOOP("Style catalog of new files")},
        {"styles/importCatalog/SRT", QT_TR_NOOP("Style catalog of imported SRT")},
        {"styles/importCatalog/MICRODVD", QT_TR_NOOP("Style catalog of imported MicroDVD")},
        {"styles/importCatalog/MPL2", QT_TR_NOOP("Style catalog of imported MPL2")},
        {"styles/importCatalog/TMP", QT_TR_NOOP("Style catalog of imported TMPlayer")},
        {"styles/importCatalog/TTXT", QT_TR_NOOP("Style catalog of imported TTXT")},
        {"styles/importCatalog/WEBVTT", QT_TR_NOOP("Style catalog of imported WebVTT")},
        {"styles/importCatalog/TXT", QT_TR_NOOP("Style catalog of imported plain text")}};
    for (auto [key, label] : catalogKeys) {
        auto *c = new QComboBox(this);
        c->addItem(tr("None"), "");
        for (const QString &n : styleCatalogs()) c->addItem(n, n);
        c->setCurrentIndex(std::max(0, c->findData(st.value(key, "Default"))));
        c->setToolTip(tr("The subtitles start with the styles of this catalog of the style manager"));
        subs->addRow(tr(label), c);
        catalogs.emplace_back(key, c);
    }
    // Aegisub's Automation/Trace Level: aegisub.debug.out messages above it are not shown
    auto *trace = new QComboBox(this);
    trace->addItems({tr("0: Fatal"), tr("1: Error"), tr("2: Warning"), tr("3: Hint"), tr("4: Debug"), tr("5: Trace")});
    trace->setCurrentIndex(std::clamp(st.value("automation/traceLevel", 3).toInt(), 0, 5));
    automation->addRow(tr("Trace level"), trace);
    // Aegisub's Path/Automation/Autoload and Include
    auto *autoloadPath = new QLineEdit(st.value("automation/autoloadPath").toString(), this);
    autoloadPath->setPlaceholderText("?user/automation/autoload/|?data/automation/autoload/");
    automation->addRow(tr("Autoload path"), autoloadPath);
    auto *includePath = new QLineEdit(st.value("automation/includePath").toString(), this);
    includePath->setPlaceholderText("?user/automation/include/|?data/automation/include/");
    automation->addRow(tr("Include path"), includePath);
    // Kainote's script editor
    auto *scriptEditor = new QLineEdit(QDir::toNativeSeparators(st.value("automation/editor").toString()), this);
    scriptEditor->setPlaceholderText(tr("The system's program for .lua files"));
    automation->addRow(tr("Script editor"), scriptEditor);
    // Aegisub's video properties on open: each Never, Ask or Always
    std::vector<std::pair<QString, QComboBox *>> combos;
    const auto combo = [&](const char *key, int def, const QString &label, const QStringList &items,
                           QFormLayout *form = nullptr) {
        auto *c = new QComboBox(this);
        c->addItems(items);
        c->setCurrentIndex(std::clamp(st.value(key, def).toInt(), 0, int(items.size()) - 1));
        (form ? form : video)->addRow(label, c);
        combos.emplace_back(key, c);
    };
    // Aegisub's App/Auto/Load Linked Files; it was a tick box before
    combo("video/linkedFiles", st.value("video/openWithSubs", true).toBool() ? 1 : 0,
          tr("Open video and audio named in the subtitles"), {tr("Never"), tr("Always"), tr("Ask")});
    combo("video/noMatrix", 1, tr("Use video's YCbCr Matrix when script has no matrix set"),
          {tr("Never"), tr("Ask"), tr("Always set")});
    combo("video/matrixMismatch", 1, tr("Match video's YCbCr Matrix on open"), {tr("Never"), tr("Ask"), tr("Always set")});
    combo("video/noLayoutRes", 1, tr("Set layout resolution from video on open"),
          {tr("Never"), tr("Ask"), tr("Always set"), tr("Always set from script resolution")});
    combo("video/layoutResMismatch", 1, tr("Prompt on layout resolution mismatch"),
          {tr("Never"), tr("When aspect ratio changes"), tr("Always")});
    // Aegisub's Audio/Renderer/Spectrum and Colour/Audio Display/Spectrum
    combo("audio/spectrumQuality", 1, tr("Spectrum quality"),
          {tr("Regular quality"), tr("Better quality"), tr("High quality"), tr("Insane quality")}, audio);
    combo("audio/freqCurve", 0, tr("Spectrum frequency mapping"),
          {tr("Linear"), tr("Extended"), tr("Medium"), tr("Compressed"), tr("Logarithmic")}, audio);
    combo("audio/spectrumScheme", 0, tr("Spectrum colours"), {tr("Default"), tr("Icy Blue"), tr("Green")}, audio);
    combo("audio/waveformStyle", 1, tr("Waveform style"), {tr("Maximum"), tr("Maximum + Average")}, audio);
    const QStringList scroll = {tr("Resizes the video box"), tr("Resizes the video box (reversed)"), tr("Zooms the video"),
                                tr("Zooms the video (reversed)"), tr("Pans the video"), tr("Pans the video (X/Y swapped)"),
                                tr("Does nothing")};
    combo("video/scrollAction", 2, tr("Scrolling on the video display"), scroll);
    combo("video/ctrlScrollAction", 0, tr("Ctrl+Scrolling on the video display"), scroll);
    combo("video/shiftScrollAction", 4, tr("Shift+Scrolling on the video display"), scroll);
    combo("video/screenshotPath", 0, tr("Save frames"),
          {tr("Next to the video"), tr("Next to the subtitles"), tr("In a folder asked for")});
    combo("automation/autoreload", 1, tr("Automatically reload changed scripts"),
          {tr("No scripts"), tr("Subtitle-local scripts"), tr("Global autoload scripts"), tr("All scripts")}, automation);
    // Kainote's loading method of the autoload scripts; the change counts from the next start
    combo("automation/loadMethod", 1, tr("Load the autoload scripts"),
          {tr("At program start, in the background"), tr("At program start"),
           tr("When the menu first opens, in the background"), tr("When the menu first opens")},
          automation);
    // Avisynth (.avs) and VapourSynth (.vpy) scripts as video and audio: where VapourSynth is, and Aegisub's
    // Avisynth options (memory limit and pre-2.56a Avisynth of its advanced video page, down-mixer and forced
    // sample rate of its advanced audio page). All of them count for scripts opened afterwards.
    auto *vsscript = new QLineEdit(QDir::toNativeSeparators(st.value("scripts/vsscript").toString()), this);
    vsscript->setPlaceholderText(tr("found automatically"));
    vsscript->setToolTip(tr("The VSScript library (vsscript.dll, libvapoursynth-script) of the VapourSynth that opens .vpy "
                            "scripts. Empty: the one VSSCRIPT_PATH or the registry names, or the one Python has. It is "
                            "loaded with the first script opened and kept until the program closes."));
    video->addRow(tr("VapourSynth library (VSScript)"), vsscript);
    const auto scriptSpin = [&](const char *key, int max, int step, const QString &label, const QString &suffix,
                                const QString &zero, QFormLayout *form) {
        auto *s = new QSpinBox(this);
        s->setRange(0, max);
        s->setSingleStep(step);
        s->setSuffix(suffix);
        s->setSpecialValueText(zero);
        s->setValue(st.value(key, 0).toInt());
        s->setToolTip(tr("For scripts opened after it is changed"));
        form->addRow(label, s);
        spins.emplace_back(key, s);
    };
    scriptSpin("scripts/avsMemoryMb", 65536, 64, tr("Avisynth memory limit"), " MB", tr("Avisynth's own"), video);
    auto *avsAncient = new QCheckBox(tr("Allow Avisynth older than 2.6"), this);
    avsAncient->setChecked(st.value("scripts/avsAllowAncient", false).toBool());
    avsAncient->setToolTip(tr("Such a version converts the frames to RGB itself, with the matrix of the subtitles"));
    video->addRow(avsAncient);
    boxes.emplace_back("scripts/avsAllowAncient", avsAncient);
    combo("scripts/avsDownmixer", 0, tr("Avisynth down-mixer"), {"ConvertToMono", "GetLeftChannel", "GetRightChannel"},
          audio);
    scriptSpin("scripts/avsSampleRate", 384000, 1000, tr("Avisynth audio sample rate"), " Hz", tr("the script's"), audio);
    // Aegisub's OK, Cancel, Apply and Restore Defaults
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply |
                                        QDialogButtonBox::RestoreDefaults,
                                    this);
    const auto save = [this, vsscript, boxes, spins, combos, inactive, leadIn, leadOut, autosave, dictDir, dictLang, language, catalogs, fonts,
                       trace, tracks, tagMark, fontFilter, saveConversion, saveScriptDefaults, fontsDir, device, autosaveFolder, autoloadPath, includePath, scriptEditor, types] {
        QSettings st;
        QSettings classes(assoc::kClasses, QSettings::NativeFormat);
        bool associated = false;
        for (const auto &[ext, b] : types)
            if (b->isChecked() != assoc::isOn(classes, ext)) {
                assoc::set(classes, ext, b->isChecked(), QCoreApplication::applicationFilePath());
                associated = true;
            }
        if (associated) assoc::notifyShell();
        st.setValue("automation/traceLevel", trace->currentIndex());
        st.setValue("automation/autoloadPath", autoloadPath->text().trimmed());
        st.setValue("automation/includePath", includePath->text().trimmed());
        st.setValue("automation/editor", QDir::fromNativeSeparators(scriptEditor->text().trimmed()));
        for (auto &[k, c] : catalogs) st.setValue(k, c->currentData());
        st.setValue("main/language", language->currentData());
        // none typed: the folder next to the program again
        if (const QString dd = QDir::fromNativeSeparators(dictDir->text().trimmed()); dd.isEmpty()) st.remove("spell/dir");
        else st.setValue("spell/dir", dd);
        st.setValue("spell/lang", dictLang->currentData().toString());
        for (const auto &[k, f] : *fonts) st.setValue(k, f);
        spell::reload();
        for (auto &[k, b] : boxes) st.setValue(k, b->isChecked());
        st.setValue("audio/inactiveLines", inactive->currentIndex());
        st.setValue("audio/leadIn", leadIn->value());
        st.setValue("audio/preferredTracks", tracks->text().trimmed());
        st.setValue("grid/tagMark", tagMark->text());
        st.setValue("style/fontFilterText", fontFilter->text());
        saveConversion();
        saveScriptDefaults();
        st.setValue("fonts/externalDir", QDir::fromNativeSeparators(fontsDir->text().trimmed()));
        st.setValue("scripts/vsscript", QDir::fromNativeSeparators(vsscript->text().trimmed()));
        st.setValue("audio/device", device->currentData());
        st.setValue("main/autosaveDir", QDir::fromNativeSeparators(autosaveFolder->text().trimmed()));
        st.setValue("audio/leadOut", leadOut->value());
        st.setValue("main/autosaveSeconds", autosave->value());
        for (auto &[k, s] : spins) st.setValue(k, s->value());
        for (auto &[k, c] : combos) st.setValue(k, c->currentIndex());
        limits::reload();
        applyMediaSettings();
        emit applied();
    };
    connect(bb, &QDialogButtonBox::accepted, this, [this, save] {
        save();
        accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, save);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // the options of these pages go back to their defaults; the colour themes, hotkeys and file
    // associations have their own
    QStringList owned = {"automation/traceLevel", "main/language",    "spell/dir",        "spell/lang",
                         "audio/inactiveLines",   "audio/leadIn",     "audio/leadOut",    "audio/preferredTracks",
                         "fonts/externalDir",     "audio/device",     "main/autosaveDir", "main/autosaveSeconds",
                         "video/openWithSubs",    "grid/font",        "edit/font",        "automation/autoloadPath",
                         "main/programFont",      "audio/karaokeFont",
                         "automation/includePath", "automation/editor", "grid/tagMark", "style/fontFilterText", "convert/style",
                         "convert/tags", "convert/newEnds", "convert/msPerChar", "convert/fps", "convert/showSettings",
                         "scripts/vsscript"};
    owned << scriptDefaultKeys();
    for (const auto &[k, w] : boxes) owned << k;
    for (const auto &[k, w] : spins) owned << k;
    for (const auto &[k, w] : catalogs) owned << k;
    for (const auto &[k, w] : combos) owned << k;
    connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this, owned] {
        if (!qEnvironmentVariableIsSet("FSED_SMOKE") && // asks nothing in a smoke test
            QMessageBox::question(this, tr("Restore defaults?"),
                                  tr("Are you sure that you want to restore the defaults? All your settings will be "
                                     "overridden.")) != QMessageBox::Yes)
            return;
        QSettings st;
        for (const QString &k : owned) st.remove(k);
        spell::reload();
        limits::reload();
        applyMediaSettings();
        emit applied();
        done(Restored); // reopened, to show the defaults
    });
    connect(list, &QListWidget::currentRowChanged, pages, [pages](int row) {
        pages->setCurrentIndex(row);
        QSettings().setValue("settings/page", row); // opens where it was left
    });
    list->setCurrentRow(std::clamp(st.value("settings/page").toInt(), 0, list->count() - 1));
    list->setFixedWidth(list->sizeHintForColumn(0) + 2 * list->frameWidth() + 24);
    auto *g = new QGridLayout(this);
    g->addWidget(list, 0, 0);
    g->addWidget(pages, 0, 1, Qt::AlignTop);
    g->addWidget(bb, 1, 0, 1, 2);
    g->setColumnStretch(1, 1);
}

// Kainote's colours page: a theme (built-in or a user's file), the window's palette, and every colour
// by where it is used, each change shown at once; Cancel puts the theme back as it was.
void SettingsDialog::buildColorsPage(QFormLayout *form) {
    auto work = std::make_shared<theme::Theme>(theme::current());
    auto *themes = new QComboBox(this);
    const auto fillThemes = [themes](const QString &select) {
        const QSignalBlocker quiet(themes);
        themes->clear();
        themes->addItem(tr("Dark"), "Dark");
        themes->addItem(tr("Light"), "Light");
        for (const QString &n : theme::userThemes()) themes->addItem(n, n);
        themes->setCurrentIndex(std::max(0, themes->findData(select)));
    };
    fillThemes(work->name);
    auto *saveAs = new QPushButton(tr("Save as..."), this), *remove = new QPushButton(tr("Delete"), this),
         *restore = new QPushButton(tr("Restore"), this);
    restore->setToolTip(tr("Discard the changes made to the colours of this theme"));
    auto *row = new QHBoxLayout;
    row->addWidget(themes, 1);
    for (auto *b : {saveAs, remove, restore}) row->addWidget(b);
    form->addRow(tr("Theme"), row);
    auto *palette = new QComboBox(this);
    palette->addItems({tr("System"), tr("Light"), tr("Dark")});
    form->addRow(tr("Window colors"), palette);
    auto *tree = new QTreeWidget(this);
    tree->setHeaderLabels({tr("Element"), tr("Color"), tr("Bold")});
    tree->setRootIsDecorated(false);
    tree->setMinimumHeight(320);
    form->addRow(tree);

    const auto swatch = [](const QColor &c) {
        QPixmap px(28, 14);
        px.fill(Qt::transparent);
        QPainter p(&px);
        for (int y = 0; y < 14; y += 7) // a checkerboard under what is see-through
            for (int x = 0; x < 28; x += 7) p.fillRect(x, y, 7, 7, (x + y) / 7 % 2 ? Qt::lightGray : Qt::white);
        p.fillRect(px.rect(), c);
        p.setPen(Qt::gray);
        p.drawRect(px.rect().adjusted(0, 0, -1, -1));
        return QIcon(px);
    };
    const auto refresh = [=] {
        const QSignalBlocker quiet(tree);
        tree->clear();
        QTreeWidgetItem *groupItem = nullptr;
        for (int r = 0; r < theme::RoleCount; ++r) {
            const auto role = theme::Role(r);
            if (!groupItem || groupItem->text(0) != theme::group(role)) {
                groupItem = new QTreeWidgetItem(tree, {theme::group(role)});
                groupItem->setFirstColumnSpanned(true);
                QFont f = groupItem->font(0);
                f.setBold(true);
                groupItem->setFont(0, f);
                groupItem->setExpanded(true);
            }
            auto *it = new QTreeWidgetItem(groupItem, {theme::label(role), work->colors[r].name(QColor::HexArgb)});
            it->setIcon(1, swatch(work->colors[r]));
            it->setData(0, Qt::UserRole, r);
            if (theme::hasBold(role)) it->setCheckState(2, work->bold[r] ? Qt::Checked : Qt::Unchecked);
        }
        tree->expandAll();
        tree->resizeColumnToContents(0);
        palette->setCurrentIndex(work->palette);
        remove->setEnabled(work->name != "Dark" && work->name != "Light");
    };
    refresh();
    const auto show = [work] { theme::apply(*work, false); };
    connect(themes, &QComboBox::currentIndexChanged, this, [=] {
        if (auto t = theme::load(themes->currentData().toString())) *work = *t;
        refresh();
        show();
    });
    connect(restore, &QPushButton::clicked, this, [=] {
        if (auto t = theme::load(work->name)) *work = *t;
        refresh();
        show();
    });
    connect(palette, &QComboBox::currentIndexChanged, this, [=](int i) {
        work->palette = i;
        show();
    });
    connect(tree, &QTreeWidget::itemChanged, this, [=](QTreeWidgetItem *it, int column) {
        if (column != 2 || !it->data(0, Qt::UserRole).isValid()) return;
        work->bold[it->data(0, Qt::UserRole).toInt()] = it->checkState(2) == Qt::Checked;
        show();
    });
    connect(tree, &QTreeWidget::itemActivated, this, [=, this](QTreeWidgetItem *it) {
        if (!it->data(0, Qt::UserRole).isValid()) return;
        const int r = it->data(0, Qt::UserRole).toInt();
        const QColor before = work->colors[r];
        const auto set = [=](const QColor &c) {
            work->colors[r] = c;
            const QSignalBlocker quiet(tree);
            it->setText(1, c.name(QColor::HexArgb));
            it->setIcon(1, swatch(c));
            show();
        };
        const QColor c = ColorPicker::get(this, before, true, set);
        set(c.isValid() ? c : before);
    });
    connect(saveAs, &QPushButton::clicked, this, [=, this] {
        const QString name =
            QInputDialog::getText(this, tr("Save theme"), tr("Theme name:"), QLineEdit::Normal,
                                  work->name == "Dark" || work->name == "Light" ? QString() : work->name)
                .trimmed();
        if (name.isEmpty()) return;
        static const QRegularExpression bad(R"([\\/:*?"<>|])");
        if (name == "Dark" || name == "Light" || name.contains(bad)) {
            QMessageBox::warning(this, tr("Save theme"), tr("A theme cannot be named \"%1\".").arg(name));
            return;
        }
        if (theme::userThemes().contains(name) &&
            QMessageBox::question(this, tr("Save theme"), tr("Replace the theme \"%1\"?").arg(name)) != QMessageBox::Yes)
            return;
        work->name = name;
        if (!theme::save(*work)) {
            QMessageBox::warning(this, tr("Save theme"), tr("Could not write the theme to %1.").arg(theme::userDir()));
            return;
        }
        fillThemes(name);
        refresh();
    });
    connect(remove, &QPushButton::clicked, this, [=, this] {
        if (QMessageBox::question(this, tr("Delete theme"), tr("Delete the theme \"%1\"?").arg(work->name)) !=
            QMessageBox::Yes)
            return;
        theme::remove(work->name);
        *work = theme::builtIn(true);
        fillThemes(work->name);
        refresh();
        show();
    });
    // OK keeps it (a user theme's file too); Cancel shows the theme of before again
    connect(this, &QDialog::accepted, this, [work] {
        theme::apply(*work);
        if (work->name != "Dark" && work->name != "Light") theme::save(*work);
    });
    connect(this, &QDialog::rejected, this, [before = theme::current()] { theme::apply(before, false); });
}

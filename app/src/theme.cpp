#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QWidget>

namespace theme {
namespace {

struct Entry {
    Role role;
    const char *key; // in the settings and in theme files, under "theme/"
    const char *label;
    int group;
    QRgb dark, light; // 0xAARRGGBB
    bool lightBold = false; // Aegisub's Subtitle/Syntax/Bold defaults; the dark theme has none
};

// Light: Aegisub's default colours where it has them; dark: FS Editor's own
const Entry kEntries[RoleCount] = {
    {GridBackground, "grid/background", QT_TRANSLATE_NOOP("theme", "Background"), 0, 0xff1f2227, 0xffffffff},
    {GridDialogue, "grid/dialogue", QT_TRANSLATE_NOOP("theme", "Line background"), 0, 0xff272b31, 0xffffffff},
    {GridComment, "grid/comment", QT_TRANSLATE_NOOP("theme", "Comment background"), 0, 0xff3b332e, 0xffd8def5},
    {GridSelection, "grid/selection", QT_TRANSLATE_NOOP("theme", "Selection"), 0, 0x5a508cdc, 0x3200dc6e},
    {GridOnVideo, "grid/onVideo", QT_TRANSLATE_NOOP("theme", "Lines visible on the video"), 0, 0x3c468c46,
     0x3cfff078},
    {GridCollision, "grid/collision", QT_TRANSLATE_NOOP("theme", "Collisions"), 0, 0xffe06c6c, 0xffff0000},
    {GridCompare, "grid/compare", QT_TRANSLATE_NOOP("theme", "Compared lines"), 0, 0x509a5ad0, 0x3c965ad2},
    {GridCompareText, "grid/compareText", QT_TRANSLATE_NOOP("theme", "Differences of compared lines"), 0, 0x96b070f0,
     0x6eb070f0},
    {GridText, "grid/text", QT_TRANSLATE_NOOP("theme", "Text"), 0, 0xffdcdcdc, 0xff000000},
    {GridLines, "grid/lines", QT_TRANSLATE_NOOP("theme", "Lines"), 0, 0xff3a3f46, 0xffbebebe},
    {GridHeader, "grid/header", QT_TRANSLATE_NOOP("theme", "Header"), 0, 0xff17191d, 0xffa5cfe7},
    {GridHeaderText, "grid/headerText", QT_TRANSLATE_NOOP("theme", "Header text"), 0, 0xffb0b6be, 0xff000000},
    {GridLabelNormal, "grid/labelNormal", QT_TRANSLATE_NOOP("theme", "Line number"), 0, 0xff2d3e57, 0xffc4ecc9},
    {GridLabelModified, "grid/labelModified", QT_TRANSLATE_NOOP("theme", "Line number: modified"), 0, 0xff55702c,
     0xffe8d678},
    {GridLabelSaved, "grid/labelSaved", QT_TRANSLATE_NOOP("theme", "Line number: saved"), 0, 0xff4a4e56, 0xffd2d2d2},
    {GridLabelDoubtful, "grid/labelDoubtful", QT_TRANSLATE_NOOP("theme", "Line number: doubtful"), 0, 0xffb8641a,
     0xffffaa5a},
    {GridActive, "grid/active", QT_TRANSLATE_NOOP("theme", "Active line border"), 0, 0xfff0b030, 0xffff5bef},
    {GridError, "grid/error", QT_TRANSLATE_NOOP("theme", "Over the limits"), 0, 0xff8a2525, 0xffffaaaa},
    {GridMisspelled, "grid/misspelled", QT_TRANSLATE_NOOP("theme", "Misspelled words"), 0, 0xffe04040, 0xffff0000},
    {SyntaxNormal, "syntax/normal", QT_TRANSLATE_NOOP("theme", "Normal"), 1, 0xffdcdcdc, 0xff000000},
    {SyntaxComment, "syntax/comment", QT_TRANSLATE_NOOP("theme", "Comments"), 1, 0xff808890, 0xff000000, true},
    {SyntaxDrawingCmd, "syntax/drawingCommand", QT_TRANSLATE_NOOP("theme", "Drawing commands"), 1, 0xffe06c75,
     0xff000000, true},
    {SyntaxDrawingX, "syntax/drawingX", QT_TRANSLATE_NOOP("theme", "Drawing X"), 1, 0xffe8a87c, 0xff5a2828},
    {SyntaxDrawingY, "syntax/drawingY", QT_TRANSLATE_NOOP("theme", "Drawing Y"), 1, 0xff98c379, 0xff285a28},
    {SyntaxBrackets, "syntax/brackets", QT_TRANSLATE_NOOP("theme", "Brackets"), 1, 0xffc678dd, 0xff1432ff},
    {SyntaxSlashes, "syntax/slashes", QT_TRANSLATE_NOOP("theme", "Slashes and parentheses"), 1, 0xff56b6c2,
     0xffff00c8},
    {SyntaxTags, "syntax/tags", QT_TRANSLATE_NOOP("theme", "Tags"), 1, 0xff6cb4ee, 0xff5a5a5a, true},
    {SyntaxError, "syntax/error", QT_TRANSLATE_NOOP("theme", "Errors"), 1, 0xffff6060, 0xffc80000},
    {SyntaxParameters, "syntax/parameters", QT_TRANSLATE_NOOP("theme", "Parameters"), 1, 0xffd19a66, 0xff285a28},
    {SyntaxLineBreak, "syntax/lineBreak", QT_TRANSLATE_NOOP("theme", "Line breaks"), 1, 0xffe5c07b, 0xffa0a0a0, true},
    {SyntaxKaraokeTemplate, "syntax/karaokeTemplate", QT_TRANSLATE_NOOP("theme", "Karaoke templater code"), 1,
     0xffc39bff, 0xff8000c0, true},
    {SyntaxKaraokeVariable, "syntax/karaokeVariable", QT_TRANSLATE_NOOP("theme", "Karaoke templater variables"), 1,
     0xffc39bff, 0xff8000c0, true},
    {SyntaxBackground, "syntax/background", QT_TRANSLATE_NOOP("theme", "Background"), 1, 0xff1f2227, 0xffffffff},
    {SyntaxErrorBackground, "syntax/errorBackground", QT_TRANSLATE_NOOP("theme", "Error background"), 1, 0xff5a2020,
     0xffffc8c8},
    {SyntaxMisspelled, "syntax/misspelled", QT_TRANSLATE_NOOP("theme", "Misspelled words"), 1, 0xffff4040,
     0xffff0000},
    {AudioBackground, "audio/background", QT_TRANSLATE_NOOP("theme", "Background"), 2, 0xff101418, 0xff000000},
    {AudioSelection, "audio/selection", QT_TRANSLATE_NOOP("theme", "Selection"), 2, 0xff26344e, 0xff005000},
    {AudioSelectionModified, "audio/selectionModified", QT_TRANSLATE_NOOP("theme", "Modified selection"), 2,
     0xff453048, 0xff502800},
    {AudioWaveform, "audio/waveform", QT_TRANSLATE_NOOP("theme", "Waveform"), 2, 0xff4cb060, 0xff00c800},
    {AudioWaveformAverage, "audio/waveformAverage", QT_TRANSLATE_NOOP("theme", "Waveform average"), 2, 0xff9ce8a8,
     0xff32ff32},
    {AudioStart, "audio/start", QT_TRANSLATE_NOOP("theme", "Line start"), 2, 0xffe03030, 0xffd80000},
    {AudioEnd, "audio/end", QT_TRANSLATE_NOOP("theme", "Line end"), 2, 0xfff09020, 0xff0000d8},
    {AudioInactive, "audio/inactive", QT_TRANSLATE_NOOP("theme", "Inactive line boundaries"), 2, 0xff808090,
     0xffbebebe},
    {AudioMark, "audio/mark", QT_TRANSLATE_NOOP("theme", "Marker"), 2, 0xffe040e0, 0xff0064ff},
    {AudioKeyframe, "audio/keyframe", QT_TRANSLATE_NOOP("theme", "Keyframes"), 2, 0xff40d040, 0xffff00ff},
    {AudioCursor, "audio/cursor", QT_TRANSLATE_NOOP("theme", "Play cursor"), 2, 0xffffffff, 0xffffffff},
    {AudioSyllable, "audio/syllable", QT_TRANSLATE_NOOP("theme", "Syllable boundaries"), 2, 0xfff0e040, 0xffffff00},
    {VisualHandle, "visual/handle", QT_TRANSLATE_NOOP("theme", "Handles"), 3, 0xffffffff, 0xfffffdb9},
    {VisualLine, "visual/line", QT_TRANSLATE_NOOP("theme", "Lines"), 3, 0xfff0c83c, 0xffbb0000},
    {VisualAccent, "visual/accent", QT_TRANSLATE_NOOP("theme", "Highlighted handle"), 3, 0xffe64646, 0xffffa928},
    {VisualShade, "visual/shade", QT_TRANSLATE_NOOP("theme", "Shaded area"), 3, 0x5a000000, 0x80000000},
};

std::optional<Theme> g_current;

QString pathOf(const QString &name) { return userDir() + "/" + name + ".ini"; }

void write(QSettings &s, const Theme &t) {
    s.setValue("theme/name", t.name);
    s.setValue("theme/palette", t.palette);
    for (const Entry &e : kEntries) {
        s.setValue(QString("theme/") + e.key, t.colors[e.role].name(QColor::HexArgb));
        if (hasBold(e.role)) s.setValue(QString("theme/bold/") + e.key, t.bold[e.role]);
    }
}

// what the settings or the file leave out comes from the built-in theme of that name, else the dark one
Theme read(QSettings &s) {
    Theme t = builtIn(s.value("theme/name").toString() != "Light");
    t.name = s.value("theme/name", t.name).toString();
    t.palette = std::clamp(s.value("theme/palette", t.palette).toInt(), 0, 2);
    for (const Entry &e : kEntries) {
        if (const QColor c = QColor::fromString(s.value(QString("theme/") + e.key).toString()); c.isValid())
            t.colors[e.role] = c;
        if (hasBold(e.role)) t.bold[e.role] = s.value(QString("theme/bold/") + e.key, t.bold[e.role]).toBool();
    }
    return t;
}

} // namespace

Theme builtIn(bool dark) {
    Theme t;
    t.name = dark ? "Dark" : "Light";
    t.palette = dark ? 2 : 1;
    for (const Entry &e : kEntries) {
        Q_ASSERT(&e - kEntries == e.role); // the table is in Role order
        t.colors[e.role] = QColor::fromRgba(dark ? e.dark : e.light);
        t.bold[e.role] = !dark && e.lightBold;
    }
    return t;
}

const Theme &current() {
    if (!g_current) {
        QSettings st;
        g_current = st.contains("theme/name") ? read(st) : builtIn(true);
    }
    return *g_current;
}

QColor color(Role r) { return current().colors[r]; }
bool bold(Role r) { return current().bold[r]; }

void apply(const Theme &t, bool store) {
    g_current = t;
    if (store) {
        QSettings st;
        write(st, t);
    }
    QGuiApplication::styleHints()->setColorScheme(t.palette == 1   ? Qt::ColorScheme::Light
                                                  : t.palette == 2 ? Qt::ColorScheme::Dark
                                                                   : Qt::ColorScheme::Unknown);
    emit notifier()->changed();
    for (QWidget *w : QApplication::allWidgets()) w->update(); // the grid, the audio, the video's tools
}

QString label(Role r) { return QCoreApplication::translate("theme", kEntries[r].label); }

QString group(Role r) {
    static const char *const names[] = {QT_TRANSLATE_NOOP("theme", "Grid"), QT_TRANSLATE_NOOP("theme", "Text field"),
                                        QT_TRANSLATE_NOOP("theme", "Audio"),
                                        QT_TRANSLATE_NOOP("theme", "Visual tools")};
    return QCoreApplication::translate("theme", names[kEntries[r].group]);
}

bool hasBold(Role r) { return r >= SyntaxNormal && r <= SyntaxKaraokeVariable; }

QString userDir() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/themes"; }

QStringList userThemes() {
    QStringList out;
    for (const QFileInfo &fi : QDir(userDir()).entryInfoList({"*.ini"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        if (fi.completeBaseName() != "Dark" && fi.completeBaseName() != "Light") out << fi.completeBaseName();
    return out;
}

std::optional<Theme> load(const QString &name) {
    if (name == "Dark" || name == "Light") return builtIn(name == "Dark");
    if (!QFileInfo::exists(pathOf(name))) return std::nullopt;
    QSettings f(pathOf(name), QSettings::IniFormat);
    Theme t = read(f);
    t.name = name; // the file's name, whatever it says inside
    return t;
}

bool save(const Theme &t) {
    QDir().mkpath(userDir());
    QSettings f(pathOf(t.name), QSettings::IniFormat);
    f.clear();
    write(f, t);
    f.sync();
    return f.status() == QSettings::NoError;
}

bool remove(const QString &name) { return QFile::remove(pathOf(name)); }

Notifier *notifier() {
    static Notifier n;
    return &n;
}

} // namespace theme

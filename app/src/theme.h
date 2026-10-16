// Colour themes (Kainote's colours page with its theme files, Aegisub's colour options): every colour
// the editor paints itself, the text field's syntax styles with Aegisub's bold flags, and the
// window's light or dark palette. Built-in Dark and Light themes; user themes are .ini files.
#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <array>
#include <optional>

namespace theme {

enum Role : int {
    GridBackground,
    GridDialogue,
    GridComment,
    GridSelection,
    GridOnVideo,
    GridCollision,
    GridCompare,
    GridCompareText,
    GridText,
    GridLines,
    GridHeader,
    GridHeaderText,
    GridLabelNormal,
    GridLabelModified,
    GridLabelSaved,
    GridLabelDoubtful,
    GridActive,
    GridError,
    GridMisspelled,
    // the text field's styles, in fs_core::syntax::Style's order up to SyntaxKaraokeVariable;
    // a curve's endpoints take the X and Y colours, underlined
    SyntaxNormal,
    SyntaxComment,
    SyntaxDrawingCmd,
    SyntaxDrawingX,
    SyntaxDrawingY,
    SyntaxBrackets,
    SyntaxSlashes,
    SyntaxTags,
    SyntaxError,
    SyntaxParameters,
    SyntaxLineBreak,
    SyntaxKaraokeTemplate,
    SyntaxKaraokeVariable,
    SyntaxBackground,
    SyntaxErrorBackground,
    SyntaxMisspelled,
    SyntaxBraces, // Kainote's matching brackets' background
    AudioBackground,
    AudioSelection,
    AudioSelectionModified,
    AudioWaveform,
    AudioWaveformAverage,
    AudioStart,
    AudioEnd,
    AudioInactive,
    AudioMark,
    AudioKeyframe,
    AudioCursor,
    AudioSyllable,
    VisualHandle,
    VisualLine,
    VisualAccent,
    VisualShade,
    RoleCount
};

struct Theme {
    QString name; // "Dark", "Light" or a user theme's file name
    int palette = 0; // the window's colours: 0 the system's, 1 light, 2 dark
    std::array<QColor, RoleCount> colors;
    std::array<bool, RoleCount> bold{}; // the syntax styles only
};

const Theme &current();
QColor color(Role r);
bool bold(Role r);
Theme builtIn(bool dark);
// The theme in use from now on: kept in the settings, the window's palette set, changed() told.
void apply(const Theme &t, bool store = true);

QString label(Role r); // translated
QString group(Role r); // translated: Grid, Text field, Audio, Visual tools
bool hasBold(Role r);

// user themes: <app data>/themes/<name>.ini
QString userDir();
QStringList userThemes();
std::optional<Theme> load(const QString &name); // a built-in one too
bool save(const Theme &t); // under t.name
bool remove(const QString &name);

class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier *notifier();

} // namespace theme

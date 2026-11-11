// What the dialogs share with each other and with the rest of the window: where the
// autosaves and the style catalogs are kept, an ASS colour as a QColor, and the settings
// that are applied outside their dialog.
#pragma once

#include "bridge.h"

#include <QColor>
#include <vector>

// "&HAABBGGRR" (a style's) or "&HBBGGRR&" (a tag's) as a colour; ASS alpha 0 is opaque
QColor assColor(const QString &s);
// The characters Kainote's font filter of the style editor asks a font for, until the settings name others
extern const QString kFontFilterChars;

// Autosave folder; files are "<name>.<yyyy-MM-dd-HH-mm-ss>.AUTOSAVE.ass" (Aegisub naming).
QString autosaveDir();

// Style catalogs (Aegisub's .sty files): the file of one, their names, and the styles of one (none
// when it is missing)
QString catalogFile(const QString &name);
QStringList styleCatalogs();
std::vector<fsb::StyleData> catalogStyles(const QString &name);

void applyMediaSettings();
// The index files of the videos and audio used longest ago go over the limits (Aegisub's
// CleanCache); `used` was just opened
void cleanIndexCache(const QString &used);

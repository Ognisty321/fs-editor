// What the translation units of MainWindow share among themselves; mainwindow.h is the window's
// header for everything else.
#pragma once

#include "mainwindow.h"

#include <QColor>
#include <QStringList>

class QAction;

// The bar's menus the layout arranges, from their builders
struct MainWindow::Menus {
    QMenu *file = nullptr, *edit = nullptr, *lines = nullptr, *video = nullptr, *audio = nullptr, *view = nullptr,
          *filter = nullptr, *subs = nullptr;
};

inline const QStringList kVideoExt = {"mkv", "mp4", "avi", "webm", "mov", "m2ts", "ts", "wmv", "flv", "m4v", "mpg", "mpeg", "ogv"};
inline const QStringList kAudioExt = {"wav", "mp3", "flac", "aac", "ac3", "eac3", "dts", "ogg", "opus", "m4a", "mka", "wma"};
inline const QString kSubsFilter =
    QObject::tr("Subtitles (*.ass *.ssa *.srt *.sub *.mpl *.txt *.tmp *.ttxt *.vtt);;All files (*)");

// Default toolbar content (Kainote TOOLBAR_IDS order, our ids).
inline const QStringList kToolbarDefault = {"openSubs", "recentSubs", "openVideo", "recentVideo", "save", "saveAs",
                                            "saveAll", "removeSubs", "editor", "findReplace", "styleManager",
                                            "properties", "shiftTimes", "goStart", "goEnd", "startFromVideo",
                                            "endFromVideo", "toASS", "toSRT", "zoomVideo", "resample", "hideTags",
                                            "settings"};

inline const QColor kAmber(0xf0b429), kBlue(0x4aa3ff), kGreen(0x4cc38a), kRed(0xf2665c), kPurple(0xb48cff),
    kTeal(0x36c5d0), kOrange(0xff8c42);

// An untitled tab holding only the empty starting line can take a file (it keeps its video/audio).
bool untouched(TabPanel *tab);
// the command's own name: a menu text may say more (Undo's step); a macro's is kept apart
QString commandName(const QAction *a);
// the command's keys in its tooltip
void showHotkey(QAction *a);

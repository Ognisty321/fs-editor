#pragma once

#include <QString>

class QWidget;

// The update check of Aegisub and Kainote: the newest release is asked from the project's page
// and offered when it is newer than this program.
namespace updates {
// -1, 0 or 1 as version `a` is older than, the same as or newer than `b`: the numbers of each
// are compared in turn ("v0.10" is newer than "0.9.3"), a missing one counting as zero.
int compare(const QString &a, const QString &b);
// Asks now. `manual` (the Help menu's entry) tells the answer whatever it is; without it only a
// newer version that was not skipped is told, and a failed request stays silent.
void check(QWidget *parent, bool manual);
// At the program's start: asks when main/checkUpdates is on and a day has passed since the last.
void checkAtStart(QWidget *parent);
} // namespace updates

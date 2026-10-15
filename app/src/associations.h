// Kainote's file associations: the subtitle types open in this program from Explorer. Written for
// the user alone (HKCU\Software\Classes on Windows), so no administrator is needed; elsewhere the
// desktop's own settings do it and there is nothing to set.
#pragma once

#include <QSettings>
#include <QStringList>

namespace assoc {

bool supported();          // Windows
const QStringList &types(); // the extensions offered, without the dot
// `classes`: the user's registry classes (kClasses), or any settings for a check
bool isOn(QSettings &classes, const QString &ext);
// On: this program's ProgID for the type, in its "Open with" list and its default unless the user
// picked one in Explorer (Windows keeps that choice above the classes). Off: the ProgID gone and
// the type's default back to what it was before.
void set(QSettings &classes, const QString &ext, bool on, const QString &exe);
inline constexpr const char *kClasses = "HKEY_CURRENT_USER\\Software\\Classes"; // QSettings::NativeFormat
void notifyShell(); // Explorer re-reads the types' icons and programs

} // namespace assoc

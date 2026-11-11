#include "dialogutil.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>
#include <QStandardPaths>

#include <vector>

QColor assColor(const QString &s) {
    QString h = s;
    h.remove("&H", Qt::CaseInsensitive).remove('&');
    const uint v = h.toUInt(nullptr, 16);
    return QColor(int(v & 0xff), int(v >> 8 & 0xff), int(v >> 16 & 0xff), 255 - int(v >> 24 & 0xff));
}

// Style catalogs are files of Style: lines, as Aegisub and Kainote keep them: theirs can be copied in
QString catalogFile(const QString &name) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/catalog/" + name + ".sty";
}

const QString kFontFilterChars = QString::fromUtf8("ĄĆĘŁŃÓŚŹŻąćęłńóśźż");

QStringList styleCatalogs() {
    QStringList cats;
    for (const QFileInfo &fi : QFileInfo(catalogFile("x")).dir().entryInfoList({"*.sty"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        cats << fi.completeBaseName();
    return cats;
}

// Kainote's external fonts folder (libass renders from it and the font lists show its fonts) and
// the audio output device
void cleanIndexCache(const QString &used) {
    const QSettings st;
    fsb::clean_index_cache(fs::Utf8(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bsindex"),
                           fs::Utf8(used),
                           st.value("video/indexCacheMb", 42).toULongLong(),
                           st.value("video/indexCacheFiles", 20).toULongLong());
}

void applyMediaSettings() {
    fsb::set_audio_device(fs::Utf8(QSettings().value("audio/device").toString()));
    fsb::set_audio_delay(QSettings().value("audio/delay", 0).toLongLong());
    const QString dir = QSettings().value("fonts/externalDir").toString();
    fsb::set_fonts_dir(fs::Utf8(dir));
    static QSet<QString> added; // Qt cannot take an application font back; one left is harmless
    if (dir.isEmpty()) return;
    QDirIterator it(dir, {"*.ttf", "*.otf", "*.ttc", "*.otc"}, QDir::Files); // libass reads only the folder itself
    while (it.hasNext())
        if (const QString f = it.next(); !added.contains(f)) {
            QFontDatabase::addApplicationFont(f);
            added << f;
        }
}

std::vector<fsb::StyleData> catalogStyles(const QString &name) {
    QFile f(catalogFile(name));
    if (!f.open(QIODevice::ReadOnly)) return {}; // a catalog with nothing saved yet
    const auto parsed = fsb::parse_style_lines(fs::Utf8(QString::fromUtf8(f.readAll())));
    return {parsed.begin(), parsed.end()};
}

// Aegisub's Path/Auto/Save
QString autosaveDir() {
    const QString dir = QSettings().value("main/autosaveDir").toString();
    return dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave" : dir;
}

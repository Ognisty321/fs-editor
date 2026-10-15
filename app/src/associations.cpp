#include "associations.h"

#include <QCoreApplication>
#include <QDir>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace assoc {

namespace {
QString progId(const QString &ext) { return "FSEditor." + ext; }
// the type's default program before this one took it, kept beside it to be put back
constexpr const char *kPrevious = "FSEditor.Previous";
} // namespace

bool supported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

const QStringList &types() {
    static const QStringList t{"ass", "ssa", "srt", "sub", "mpl", "txt", "tmp", "ttxt"};
    return t;
}

bool isOn(QSettings &classes, const QString &ext) {
    return classes.contains(progId(ext) + "/shell/open/command/Default");
}

void set(QSettings &classes, const QString &ext, bool on, const QString &exe) {
    const QString id = progId(ext), type = "." + ext;
    const QString native = QDir::toNativeSeparators(exe);
    if (on) {
        classes.setValue(id + "/Default", QCoreApplication::translate("assoc", "%1 subtitles").arg(ext.toUpper()));
        classes.setValue(id + "/DefaultIcon/Default", native + ",0");
        classes.setValue(id + "/shell/open/command/Default", "\"" + native + "\" \"%1\"");
        classes.setValue(type + "/OpenWithProgids/" + id, QString());
        const QString was = classes.value(type + "/Default").toString();
        if (was != id) {
            classes.setValue(type + "/" + kPrevious, was);
            classes.setValue(type + "/Default", id);
        }
    } else {
        classes.remove(id);
        classes.remove(type + "/OpenWithProgids/" + id);
        if (classes.value(type + "/Default").toString() == id) {
            const QString was = classes.value(type + "/" + kPrevious).toString();
            if (was.isEmpty()) classes.remove(type + "/Default");
            else classes.setValue(type + "/Default", was);
        }
        classes.remove(type + "/" + kPrevious);
    }
    classes.sync();
}

void notifyShell() {
#ifdef _WIN32
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

} // namespace assoc

#include "audiobox.h"
#include "dialogs.h"
#include "editbox.h"
#include "associations.h"
#include "grid.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "theme.h"
#include "videobox.h"
#include "visualtools.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QEventLoop>
#include <QLibraryInfo>
#include <QListWidget>
#include <QTreeWidget>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QFile>
#include <QSettings>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
#include <QPainter>
#include <QTranslator>
#include <QUrl>
#include <QWheelEvent>

#include <atomic>
#include <csignal>
#include <numeric>
#include <cstdio>
#include <ranges>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

static MainWindow *g_main = nullptr;

// Last chance for unsaved work (Aegisub saves on a fatal exception too): modified tabs go to
// the autosave folder, where File > Open autosave file lists them.
static void recoverTabs() {
    static std::atomic_flag once;
    if (once.test_and_set() || !g_main || !g_main->autosaveTabs()) return;
    const QString msg = QObject::tr("FS Editor has crashed. Unsaved subtitles were written to %1 "
                                    "and can be opened with File > Open autosave file.")
                            .arg(QDir::toNativeSeparators(autosaveDir()));
#ifdef _WIN32
    // a smoke run is unattended: a modal box would only hang it
    if (!qEnvironmentVariableIsSet("FSED_SMOKE"))
        MessageBoxW(nullptr, reinterpret_cast<const wchar_t *>(msg.utf16()), L"FS Editor", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", qPrintable(msg));
#endif
}

#ifdef _WIN32
// Print a symbolised stack of the current thread to stderr; release builds
// carry PDBs (RelWithDebInfo), so crash reports from users are readable.
static void printStack() {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    void *frames[64];
    const USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    for (USHORT i = 0; i < n; ++i) {
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 off = 0;
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD loff = 0;
        const auto addr = DWORD64(frames[i]);
        const bool hasSym = SymFromAddr(proc, addr, &off, sym);
        const bool hasLine = SymGetLineFromAddr64(proc, addr, &loff, &line);
        std::fprintf(stderr, "  #%u %s %s:%lu\n", i, hasSym ? sym->Name : "?", hasLine ? line.FileName : "",
                     hasLine ? line.LineNumber : 0);
    }
    std::fflush(stderr);
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS *ep) {
    std::fprintf(stderr, "fsed crashed: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress);
    printStack();
    recoverTabs();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// A panic on the GUI thread: the bridge aborts the process right after.
// Aegisub's OnExceptionInMainLoop: an exception out of an event handler (a bridge error a slot
// missed) writes the autosaves and says so, and the app goes on instead of ending
class App final : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject *o, QEvent *e) override {
        try {
            return QApplication::notify(o, e);
        } catch (const std::exception &x) {
            failed(QString::fromUtf8(x.what()));
        } catch (...) {
            failed(QStringLiteral("unknown exception"));
        }
        return false;
    }

private:
    static void failed(const QString &what) {
        std::fprintf(stderr, "fsed: %s\n", qPrintable(what));
        if (g_main) g_main->autosaveTabs();
        if (!qEnvironmentVariableIsSet("FSED_SMOKE"))
            QMessageBox::critical(g_main, "FS Editor",
                                  QObject::tr("An error occurred: %1\n\nUnsaved subtitles were written to the "
                                              "autosave folder. Save your work and restart FS Editor.")
                                      .arg(what));
    }
};

struct PanicRecovery final : fsb::PanicHandler {
    void fatal(rust::Str) const override { recoverTabs(); } // Rust already printed the message
};

// abort() (qFatal, the bridge after a panic) and, outside Windows, faults.
static void fatalSignal(int sig) {
    std::signal(sig, SIG_DFL);
    std::fprintf(stderr, "fsed: fatal signal %d\n", sig);
#ifdef _WIN32
    printStack();
#endif
    recoverTabs();
    std::raise(sig);
}

// The dialog a smoke action opened: a modal one, else the newest visible modeless one, else
// another window (a detached video).
static QWidget *smokeDialog(QWidget &w) {
    QWidget *top = QApplication::activeModalWidget();
    if (!top)
        for (auto *d : w.findChildren<QDialog *>())
            if (d->isVisible()) top = d;
    if (!top)
        for (auto *t : QApplication::topLevelWidgets())
            if (t != &w && t->isVisible() && t->windowType() == Qt::Window) top = t;
    return top;
}

// FSED_CHECK_ASSOC=<file>: the file associations' registry writes, on a settings file instead
static bool checkAssoc(const QString &file) {
    QFile::remove(file);
    QSettings c(file, QSettings::IniFormat);
    c.setValue(".srt/Default", "Other.srt");
    assoc::set(c, "srt", true, "C:/fsed.exe");
    assoc::set(c, "ass", true, "C:/fsed.exe");
    const bool on = assoc::isOn(c, "srt") && !assoc::isOn(c, "ssa") && c.value(".srt/Default") == "FSEditor.srt" &&
                    c.value("FSEditor.srt/shell/open/command/Default") == "\"C:\\fsed.exe\" \"%1\"" &&
                    c.contains(".ass/OpenWithProgids/FSEditor.ass");
    assoc::set(c, "srt", false, {});
    assoc::set(c, "ass", false, {});
    const bool off = !assoc::isOn(c, "srt") && c.value(".srt/Default") == "Other.srt" && !c.contains(".ass/Default") &&
                     !c.contains(".srt/OpenWithProgids/FSEditor.srt") && !c.contains(".srt/FSEditor.Previous");
    std::fprintf(stderr, "assoc %s\n", on && off ? "ok" : "FAILED");
    return on && off;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, fatalSignal);
#endif
    std::signal(SIGABRT, fatalSignal);
    fsb::install_panic_hook(std::make_unique<PanicRecovery>());
    App app(argc, argv);
    if (const QString f = qEnvironmentVariable("FSED_CHECK_ASSOC"); !f.isEmpty()) return checkAssoc(f) ? 0 : 1;
    QApplication::setOrganizationName("FS Editor");
    QApplication::setApplicationName("FS Editor");
    QApplication::setApplicationDisplayName("FS Editor");
    const bool smoke = qEnvironmentVariableIsSet("FSED_SMOKE");
    if (smoke) {
        // separate, fresh settings so smoke runs never touch the user's own
        QApplication::setApplicationName("FS Editor smoke test");
        QSettings st;
        st.clear();
        st.setValue("main/askSession", false);
        // FSED_SMOKE_SET="audio/spectrum=true;audio/karaoke=true"
        for (const QString &kv : qEnvironmentVariable("FSED_SMOKE_SET").split(';', Qt::SkipEmptyParts))
            if (const QString v = kv.section('=', 1); v.contains('|')) st.setValue(kv.section('=', 0, 0), v.split('|')); // a list
            else st.setValue(kv.section('=', 0, 0), v);
    }
    // UI language: main/language ("" follows the system), applied at startup
    const QString lang = QSettings().value("main/language").toString();
    const QLocale locale = lang.isEmpty() ? QLocale() : QLocale(lang);
    QTranslator own, qt;
    if (own.load(locale, "fsed", "_", ":/i18n")) app.installTranslator(&own);
    if (qt.load(locale, "qtbase", "_", ":/i18n") ||
        qt.load(locale, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qt);
    theme::apply(theme::current(), false); // its window palette
    // Kainote's program font, when one is set
    if (const QVariant f = QSettings().value("main/programFont"); f.isValid()) QApplication::setFont(f.value<QFont>());
    MainWindow w;
    g_main = &w;
    w.show();
    // a run that did not close (power loss, a killed process, a crash) leaves its lock behind, and
    // the next start offers the autosaves (Kainote's session restore); a second instance has none
    QDir().mkpath(autosaveDir());
    QLockFile running(autosaveDir() + "/running.lock");
    const bool left = QFileInfo::exists(running.fileName());
    const bool crashed = running.tryLock(0) && left && !smoke &&
                         !QDir(autosaveDir()).entryList({"*.AUTOSAVE.ass"}, QDir::Files).isEmpty();
    const QStringList files = app.arguments().mid(1);
    // from the event loop: opening may ask a question (subtitles inside a video)
    QTimer::singleShot(0, &w, [&w, files, crashed] {
        if (crashed && QMessageBox::question(&w, "FS Editor",
                                             QObject::tr("FS Editor was not closed properly last time. Open one "
                                                         "of the autosaved subtitles?")) == QMessageBox::Yes)
            w.action("openAutosave")->trigger();
        if (!files.isEmpty()) w.openFiles(files);
        // Kainote's full screen for a video opened alone with the program
        if (files.size() == 1 && MainWindow::isVideoFile(files[0]) &&
            QSettings().value("video/fullscreenOnStart", false).toBool())
            w.current()->video()->setFullscreen(true);
        if (files.isEmpty() && QSettings().value("main/askSession", true).toBool()) w.action("loadSession")->trigger();
    });
    // Smoke test: FSED_SMOKE=out.png grabs the window after loading and quits;
    // FSED_SMOKE_ROW selects a line first (seeks the video like a click).
    if (qEnvironmentVariableIsSet("FSED_SMOKE_ROW"))
        QTimer::singleShot(std::max(500, qEnvironmentVariableIntValue("FSED_SMOKE_MS") / 2), &w,
                           [&w] {
                w.current()->setActive(qEnvironmentVariableIntValue("FSED_SMOKE_ROW"));
                // FSED_SMOKE_TOOL=n picks a visual tool (VisualTools::Tool order)
                if (qEnvironmentVariableIsSet("FSED_SMOKE_TOOL"))
                    w.current()->video()->tools()->setTool(qEnvironmentVariableIntValue("FSED_SMOKE_TOOL"));
                // FSED_SMOKE_TYPE="ab" types at the end of the line's text; FSED_SMOKE_WHEEL="n,steps"
                // turns the wheel with Shift over its character n
                SubsTextEdit *text = nullptr;
                for (auto *te : w.findChildren<SubsTextEdit *>())
                    if (te->isVisible()) text = te;
                // FSED_SMOKE_SELECT=n selects n lines from the row on
                if (const int n = qEnvironmentVariableIntValue("FSED_SMOKE_SELECT"); n > 1) {
                    const int row = qEnvironmentVariableIntValue("FSED_SMOKE_ROW");
                    std::vector<size_t> rows(static_cast<size_t>(n));
                    std::iota(rows.begin(), rows.end(), size_t(row));
                    w.current()->setSelection(rows, row);
                }
                if (text && qEnvironmentVariableIsSet("FSED_SMOKE_TYPE")) {
                    text->setFocus();
                    text->moveCursor(QTextCursor::End);
                    for (const QChar ch : qEnvironmentVariable("FSED_SMOKE_TYPE")) {
                        QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
                        QApplication::sendEvent(text, &e);
                    }
                }
                if (const QStringList wh = qEnvironmentVariable("FSED_SMOKE_WHEEL").split(','); text && wh.size() == 2) {
                    QTextCursor c(text->document());
                    c.setPosition(wh[0].toInt());
                    const QPointF at = text->cursorRect(c).center() + QPointF(1, 0);
                    QWheelEvent e(at, text->viewport()->mapToGlobal(at), {}, QPoint(0, 120 * wh[1].toInt()),
                                  Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(text->viewport(), &e);
                }
                // FSED_SMOKE_AUDIO="x1,x2,b[,m]": a press at x1 dragged to x2 in the audio, in view
                // fractions; b: l, r or m button; m: a (Alt) or c (Ctrl)
                if (const QStringList a = qEnvironmentVariable("FSED_SMOKE_AUDIO").split(','); a.size() >= 3) {
                    auto *view = w.current()->audio()->findChild<AudioView *>();
                    const auto button = a[2] == "r" ? Qt::RightButton : a[2] == "m" ? Qt::MiddleButton : Qt::LeftButton;
                    const auto mods = a.value(3) == "a" ? Qt::AltModifier : a.value(3) == "c" ? Qt::ControlModifier : Qt::NoModifier;
                    const auto at = [&](double f) { return QPointF(f * view->width(), view->height() * 0.6); };
                    auto send = [&](QEvent::Type t, QPointF p, Qt::MouseButtons held) {
                        QMouseEvent e(t, p, view->mapToGlobal(p), button, held, mods);
                        QApplication::sendEvent(view, &e);
                    };
                    const double x1 = a[0].toDouble(), x2 = a[1].toDouble();
                    send(QEvent::MouseButtonPress, at(x1), button);
                    for (int k = 1; k <= 4; ++k) send(QEvent::MouseMove, at(x1 + (x2 - x1) * k / 4), button);
                    send(QEvent::MouseButtonRelease, at(x2), Qt::NoButton);
                }
                // FSED_SMOKE_AUDIOKEY="Shift+G", _VIDEOKEY="L", _EDITKEY="Ctrl+B": a key in the audio, video, line text
                const std::pair<const char *, QWidget *> keyTargets[] = {
                    {"FSED_SMOKE_AUDIOKEY", w.current()->audio()->findChild<AudioView *>()},
                    {"FSED_SMOKE_VIDEOKEY", w.current()->video()->view()},
                    {"FSED_SMOKE_EDITKEY",
                     w.current()->edit()->findChild<SubsTextEdit *>({}, Qt::FindDirectChildrenOnly)}};
                for (const auto &[var, view] : keyTargets)
                    for (const QKeySequence k(qEnvironmentVariable(var)); int i : std::views::iota(0, k.count())) {
                        QKeyEvent e(QEvent::KeyPress, k[i].key(), k[i].keyboardModifiers()); // "F, Esc": in turn
                        QApplication::sendEvent(view, &e);
                    }
                // FSED_SMOKE_DBLCLICK="x,y": a left double-click on the video, in view fractions
                if (const QStringList d = qEnvironmentVariable("FSED_SMOKE_DBLCLICK").split(','); d.size() == 2) {
                    QWidget *view = w.current()->video()->view();
                    const QPointF p(d[0].toDouble() * view->width(), d[1].toDouble() * view->height());
                    QMouseEvent e(QEvent::MouseButtonDblClick, p, view->mapToGlobal(p), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(view, &e);
                }
                // FSED_SMOKE_GRIDCLICK="3,c": a left click on the third line shown in the grid ("c": with Ctrl)
                if (const QStringList g = qEnvironmentVariable("FSED_SMOKE_GRIDCLICK").split(','); g[0].toInt() > 0) {
                    QWidget *vp = w.current()->grid()->viewport();
                    const int h = w.current()->grid()->rowHeight();
                    const QPointF p(40, h * g[0].toInt() + h / 2);
                    const auto mods = g.value(1) == "c" ? Qt::ControlModifier : Qt::NoModifier;
                    QMouseEvent press(QEvent::MouseButtonPress, p, vp->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, mods);
                    QApplication::sendEvent(vp, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease, p, vp->mapToGlobal(p), Qt::LeftButton, Qt::NoButton, mods);
                    QApplication::sendEvent(vp, &release);
                }
                // FSED_SMOKE_DRAG="x1,y1,x2,y2": a left-button drag on the video, in view fractions 0..1;
                // "x1,y1,x2,y2,z" zooms in a step first; "...;x1,y1,x2,y2" drags again; more pairs drag
                // through each point
                for (const QString &drag : qEnvironmentVariable("FSED_SMOKE_DRAG").split(';')) {
                    const QStringList d = drag.split(',');
                    if (d.size() < 4) return;
                    QWidget *view = w.current()->video()->view();
                    if (d.size() % 2) {
                        const QPointF c(view->width() / 2.0, view->height() / 2.0);
                        QWheelEvent e(c, view->mapToGlobal(c), {}, {0, 120}, Qt::NoButton, Qt::NoModifier,
                                      Qt::NoScrollPhase, false);
                        QApplication::sendEvent(view, &e);
                    }
                    const auto at = [&](int k) {
                        return QPointF(d[k].toDouble() * view->width(), d[k + 1].toDouble() * view->height());
                    };
                    auto send = [view](QEvent::Type t, QPointF p, Qt::MouseButtons held) {
                        QMouseEvent e(t, p, view->mapToGlobal(p), Qt::LeftButton, held, Qt::NoModifier);
                        QApplication::sendEvent(view, &e);
                    };
                    send(QEvent::MouseButtonPress, at(0), Qt::LeftButton);
                    const int last = (d.size() / 2 - 1) * 2;
                    for (int p = 2; p <= last; p += 2)
                        for (int k = 1; k <= 4; ++k) send(QEvent::MouseMove, at(p - 2) + (at(p) - at(p - 2)) * k / 4, Qt::LeftButton);
                    send(QEvent::MouseButtonRelease, at(last), Qt::NoButton);
                }
            });
    // FSED_SMOKE_ACTION runs a menu command by id ("a;b" one after the other); a modal dialog
    // it opens is grabbed instead.
    if (const QString ids = qEnvironmentVariable("FSED_SMOKE_ACTION"); !ids.isEmpty())
        QTimer::singleShot(std::max(700, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 2 / 3), &w, [&w, ids] {
            for (const QString &id : ids.split(';'))
                if (auto *a = w.action(id)) a->trigger();
        });
    // FSED_SMOKE_MENU="Automation" opens that menu of the window, or a submenu of it, by its title;
    // "video>Audio streams" opens the video's context menu with that submenu shown ("tabs>..." the
    // tab bar's, "edit:7>..." the line text's at its character 7), more ">" for submenus in it; an
    // item at the end is picked, then the menu is shown again FSED_SMOKE_REOPEN ms later (1500)
    static std::function<void(QString)> contextMenu;
    static std::function<void(QString, int, int)> walk;
    // the menu shows once the main thread is free (a load reports progress): tried till then
    walk = [](const QString &path, int level, int tries) {
        const QStringList parts = path.split('>');
        auto *m = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QAction *a = nullptr;
        for (auto *x : m ? m->actions() : QList<QAction *>())
            if (fs::plainText(x->text()) == parts[level]) a = x;
        if (!a) {
            if (tries > 0) QTimer::singleShot(300, [path, level, tries] { walk(path, level, tries - 1); });
            return;
        }
        if (!a->menu()) {
            for (int k = 0; k < 10 && QApplication::activePopupWidget(); ++k) QApplication::activePopupWidget()->close();
            a->trigger();
            const int wait = qEnvironmentVariableIsSet("FSED_SMOKE_REOPEN") ? qEnvironmentVariableIntValue("FSED_SMOKE_REOPEN") : 1500;
            return QTimer::singleShot(wait, [back = parts.mid(0, level).join('>')] { contextMenu(back); });
        }
        m->setActiveAction(a);
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(m, &right);
        if (level + 1 < parts.size()) QTimer::singleShot(300, [path, level] { walk(path, level + 1, 40); });
    };
    contextMenu = [&w](const QString &path) {
        const QString target = path.section('>', 0, 0);
        QWidget *on = nullptr;
        QPoint at;
        if (target == "video") {
            on = w.current()->video()->view();
            at = on->rect().center();
        } else if (target == "tabs") {
            for (auto *bar : w.findChildren<QTabBar *>())
                if (!on && bar->isVisible() && bar->contextMenuPolicy() == Qt::CustomContextMenu &&
                    qobject_cast<QTabWidget *>(bar->parentWidget()))
                    on = bar, at = bar->tabRect(0).center();
        } else if (target.startsWith("edit:")) {
            for (auto *te : w.findChildren<SubsTextEdit *>())
                if (te->isVisible()) {
                    QTextCursor c = te->textCursor();
                    c.setPosition(target.mid(5).toInt());
                    on = te->viewport(), at = te->cursorRect(c).center();
                }
        }
        if (!on) return;
        if (path.contains('>')) QTimer::singleShot(300, &w, [path] { walk(path, 1, 40); });
        QContextMenuEvent e(QContextMenuEvent::Mouse, at, on->mapToGlobal(at));
        QApplication::sendEvent(on, &e); // the menu runs in here till the shot
    };
    if (const QString title = qEnvironmentVariable("FSED_SMOKE_MENU"); !title.isEmpty())
        QTimer::singleShot(std::max(700, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 2 / 3), &w, [&w, title] {
            if (title.startsWith("video") || title.startsWith("tabs") || title.startsWith("edit:")) return contextMenu(title);
            for (auto *m : w.findChildren<QMenu *>())
                if (fs::plainText(m->title()) == title) return m->popup(w.mapToGlobal(QPoint(0, 30)));
        });
    // FSED_SMOKE_SEEK="0.5" clicks the video's seek bar halfway ("0.5,s": with Shift); "w,-1" turns
    // the wheel over it a notch down ("w,-1,s": with Shift)
    if (const QStringList a = qEnvironmentVariable("FSED_SMOKE_SEEK").split(','); !a[0].isEmpty())
        QTimer::singleShot(std::max(700, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 2 / 3), &w, [&w, a] {
            auto *bar = w.current()->video()->findChild<QSlider *>(QString(), Qt::FindDirectChildrenOnly);
            const auto mods = a.back() == "s" ? Qt::ShiftModifier : Qt::NoModifier;
            const QPointF p(a[0] == "w" ? bar->width() / 2.0 : a[0].toDouble() * bar->width(), bar->height() / 2.0);
            if (a[0] == "w" && a.size() > 1) {
                QWheelEvent e(p, bar->mapToGlobal(p), {}, {0, 120 * a[1].toInt()}, Qt::NoButton, mods, Qt::NoScrollPhase,
                              false);
                QApplication::sendEvent(bar, &e);
                return;
            }
            QMouseEvent press(QEvent::MouseButtonPress, p, bar->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, mods);
            QApplication::sendEvent(bar, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, p, bar->mapToGlobal(p), Qt::LeftButton, Qt::NoButton, mods);
            QApplication::sendEvent(bar, &release);
        });
    // FSED_SMOKE_DROP=file drops it onto the line edit box, like a file dragged from the desktop.
    if (const QString f = qEnvironmentVariable("FSED_SMOKE_DROP"); !f.isEmpty())
        QTimer::singleShot(std::max(600, qEnvironmentVariableIntValue("FSED_SMOKE_MS") / 3), &w, [&w, f] {
            QMimeData md;
            md.setUrls({QUrl::fromLocalFile(f)});
            QWidget *target = &w;
            for (auto *te : w.findChildren<QTextEdit *>())
                if (te->isVisible()) target = te->viewport();
            // Qt only delivers a drop to the widget that accepted the drag enter
            QDragEnterEvent enter(QPointF(5, 5), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter);
            QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &drop);
        });
    // FSED_SMOKE_CLICK="Check" presses that button in the open dialog, else in the main window;
    // "Replace;Close" presses one after the other, a button goes by its text or its tooltip;
    // "^" closes the dialog (to shoot the main window after a non-modal one); "Sort#2" presses the
    // second such button; a text no button has selects the list and tree items and the tabs with it; "~500"
    // waits half a second (for a search on a worker); "Edit@" presses Edit with the rest pressed half
    // a second later, in the modal dialog it opens. "+Bold" presses it with Shift (by its text or tooltip),
    // "?Bold" writes the tooltip it shows to <shot>.txt.
    static std::function<void(QStringList)> clicks;
    clicks = [&w](QStringList texts) {
            while (!texts.isEmpty()) {
                QString text = texts.takeFirst();
                if (text.endsWith('@')) {
                    text.chop(1);
                    QTimer::singleShot(500, &w, [rest = texts] { clicks(rest); });
                    texts.clear();
                }
                if (text.startsWith('~')) {
                    QEventLoop wait;
                    QTimer::singleShot(text.mid(1).toInt(), &wait, &QEventLoop::quit);
                    wait.exec();
                    continue;
                }
                const QChar how = text.startsWith('+') || text.startsWith('?') ? text[0] : QChar();
                if (!how.isNull()) text.remove(0, 1);
                QWidget *top = smokeDialog(w);
                if (text == "^" && top) top->close();
                int nth = 1;
                if (const int h = int(text.lastIndexOf('#')); h > 0 && text.mid(h + 1).toInt() > 0)
                    nth = text.mid(h + 1).toInt(), text.truncate(h);
                bool clicked = false;
                for (auto *b : (top ? top : &w)->findChildren<QAbstractButton *>())
                    if ((b->text() == text || b->toolTip() == text) &&
                        b->isVisible() && --nth == 0) {
                        const QPoint c = b->rect().center();
                        if (how == '+') {
                            QMouseEvent press(QEvent::MouseButtonPress, c, b->mapToGlobal(c), Qt::LeftButton,
                                              Qt::LeftButton, Qt::ShiftModifier);
                            QApplication::sendEvent(b, &press);
                        } else if (how == '?') {
                            QHelpEvent help(QEvent::ToolTip, c, b->mapToGlobal(c));
                            QApplication::sendEvent(b, &help);
                            QFile f(qEnvironmentVariable("FSED_SMOKE") + ".txt");
                            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) f.write((QToolTip::text() + QChar(10)).toUtf8());
                        } else {
                            b->click();
                        }
                        clicked = true;
                        break;
                    }
                if (!clicked) {
                    for (auto *l : (top ? top : &w)->findChildren<QListWidget *>())
                        for (auto *it : l->findItems(text, Qt::MatchExactly)) l->setCurrentItem(it, QItemSelectionModel::Select);
                    for (auto *t : (top ? top : &w)->findChildren<QTreeWidget *>())
                        for (auto *it : t->findItems(text, Qt::MatchExactly | Qt::MatchRecursive)) {
                            t->setCurrentItem(it);
                            t->scrollToItem(it);
                        }
                    for (auto *tb : (top ? top : &w)->findChildren<QTabBar *>())
                        for (int i = 0; i < tb->count(); ++i)
                            if (tb->tabText(i) == text) tb->setCurrentIndex(i);
                }
            }
        };
    if (const QString texts = qEnvironmentVariable("FSED_SMOKE_CLICK"); !texts.isEmpty())
        QTimer::singleShot(std::max(800, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 5 / 6), &w,
                           [texts] { clicks(texts.split(';')); });
    if (const QString out = qEnvironmentVariable("FSED_SMOKE"); !out.isEmpty())
        QTimer::singleShot(std::max(1000, qEnvironmentVariableIntValue("FSED_SMOKE_MS")), &w, [&w, out] {
            QWidget *shot = smokeDialog(w);
            if (!shot) shot = &w;
            // the audio open then, which a shot cannot tell (a track still loading looks like silence)
            if (auto *au = w.current()->audio(); !au->path().isEmpty()) {
                QFile f(out + ".txt");
                if (f.open(QIODevice::WriteOnly | QIODevice::Append))
                    f.write(QString("audio %1 track %2" + QString(QChar(10))).arg(au->path()).arg(au->track()).toUtf8());
            }
            // tool tips and popups over it are windows of their own: drawn where they are, the shot
            // grown to take in those reaching past it
            QList<QWidget *> pops;
            const QRect own(shot->mapToGlobal(QPoint()), shot->size());
            QRect all = own;
            for (QWidget *pop : QApplication::topLevelWidgets())
                if (const QRect r(pop->mapToGlobal(QPoint()), pop->size());
                    pop->isVisible() && (pop->windowType() == Qt::ToolTip || pop->windowType() == Qt::Popup) && r.intersects(own))
                    pops << pop, all |= r;
            QPixmap px(all.size());
            px.fill(Qt::black);
            {
                QPainter p(&px);
                p.drawPixmap(shot->mapToGlobal(QPoint()) - all.topLeft(), shot->grab());
                for (QWidget *pop : pops) p.drawPixmap(pop->mapToGlobal(QPoint()) - all.topLeft(), pop->grab());
            }
            px.save(out);
            QApplication::exit(0);
        });
    const int rc = app.exec();
    g_main = nullptr; // the window is being destroyed: nothing left to recover
    return rc;
}

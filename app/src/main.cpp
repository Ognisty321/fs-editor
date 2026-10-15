#include "audiobox.h"
#include "dialogs.h"
#include "editbox.h"
#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"
#include "visualtools.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QLibraryInfo>
#include <QListWidget>
#include <QLockFile>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QSlider>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QWheelEvent>

#include <atomic>
#include <csignal>
#include <numeric>
#include <cstdio>

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

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#else
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL}) std::signal(sig, fatalSignal);
#endif
    std::signal(SIGABRT, fatalSignal);
    fsb::install_panic_hook(std::make_unique<PanicRecovery>());
    App app(argc, argv);
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
            st.setValue(kv.section('=', 0, 0), kv.section('=', 1));
    }
    // UI language: main/language ("" follows the system), applied at startup
    const QString lang = QSettings().value("main/language").toString();
    const QLocale locale = lang.isEmpty() ? QLocale() : QLocale(lang);
    QTranslator own, qt;
    if (own.load(locale, "fsed", "_", ":/i18n")) app.installTranslator(&own);
    if (qt.load(locale, "qtbase", "_", ":/i18n") ||
        qt.load(locale, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qt);
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
        for (const QString &f : files) w.openFile(f);
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
                // FSED_SMOKE_AUDIOKEY="Shift+G": that key pressed in the audio
                if (const QKeySequence k(qEnvironmentVariable("FSED_SMOKE_AUDIOKEY")); !k.isEmpty()) {
                    auto *view = w.current()->audio()->findChild<AudioView *>();
                    QKeyEvent e(QEvent::KeyPress, k[0].key(), k[0].keyboardModifiers());
                    QApplication::sendEvent(view, &e);
                }
                // FSED_SMOKE_DRAG="x1,y1,x2,y2": a left-button drag on the video, in view fractions 0..1;
                // "x1,y1,x2,y2,z" zooms in a step first; "...;x1,y1,x2,y2" drags again
                for (const QString &drag : qEnvironmentVariable("FSED_SMOKE_DRAG").split(';')) {
                    const QStringList d = drag.split(',');
                    if (d.size() < 4) return;
                    QWidget *view = w.current()->video()->view();
                    if (d.size() > 4) {
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
                    for (int k = 1; k <= 4; ++k) send(QEvent::MouseMove, at(0) + (at(2) - at(0)) * k / 4, Qt::LeftButton);
                    send(QEvent::MouseButtonRelease, at(2), Qt::NoButton);
                }
            });
    // FSED_SMOKE_ACTION runs a menu command by id ("a;b" one after the other); a modal dialog
    // it opens is grabbed instead.
    if (const QString ids = qEnvironmentVariable("FSED_SMOKE_ACTION"); !ids.isEmpty())
        QTimer::singleShot(std::max(700, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 2 / 3), &w, [&w, ids] {
            for (const QString &id : ids.split(';'))
                if (auto *a = w.action(id)) a->trigger();
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
    // "Replace;Close" presses one after the other, a button with no text goes by its tooltip;
    // "^" closes the dialog (to shoot the main window after a non-modal one); "Sort#2" presses the
    // second such button; a text no button has selects the list items and the tabs with it.
    if (const QString texts = qEnvironmentVariable("FSED_SMOKE_CLICK"); !texts.isEmpty())
        QTimer::singleShot(std::max(800, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 5 / 6), &w, [&w, texts] {
            for (QString text : texts.split(';')) {
                QWidget *top = smokeDialog(w);
                if (text == "^" && top) top->close();
                int nth = 1;
                if (const int h = int(text.lastIndexOf('#')); h > 0 && text.mid(h + 1).toInt() > 0)
                    nth = text.mid(h + 1).toInt(), text.truncate(h);
                bool clicked = false;
                for (auto *b : (top ? top : &w)->findChildren<QAbstractButton *>())
                    if ((b->text() == text || (b->text().isEmpty() && b->toolTip() == text)) && b->isVisible() &&
                        --nth == 0) {
                        b->click();
                        clicked = true;
                        break;
                    }
                if (!clicked) {
                    for (auto *l : (top ? top : &w)->findChildren<QListWidget *>())
                        for (auto *it : l->findItems(text, Qt::MatchExactly)) l->setCurrentItem(it, QItemSelectionModel::Select);
                    for (auto *tb : (top ? top : &w)->findChildren<QTabBar *>())
                        for (int i = 0; i < tb->count(); ++i)
                            if (tb->tabText(i) == text) tb->setCurrentIndex(i);
                }
            }
        });
    if (const QString out = qEnvironmentVariable("FSED_SMOKE"); !out.isEmpty())
        QTimer::singleShot(std::max(1000, qEnvironmentVariableIntValue("FSED_SMOKE_MS")), &w, [&w, out] {
            QWidget *top = smokeDialog(w);
            (top ? top : &w)->grab().save(out);
            QApplication::exit(0);
        });
    const int rc = app.exec();
    g_main = nullptr; // the window is being destroyed: nothing left to recover
    return rc;
}

#include "mainwindow.h"
#include "tabpanel.h"
#include "videobox.h"
#include "visualtools.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

// Print a symbolised stack of the faulting thread to stderr; release builds
// carry PDBs (RelWithDebInfo), so crash reports from users are readable.
static LONG WINAPI crashHandler(EXCEPTION_POINTERS *ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    std::fprintf(stderr, "fsed crashed: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress);
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
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// The dialog a smoke action opened: a modal one, else the newest visible modeless one.
static QWidget *smokeDialog(QWidget &w) {
    QWidget *top = QApplication::activeModalWidget();
    if (!top)
        for (auto *d : w.findChildren<QDialog *>())
            if (d->isVisible()) top = d;
    return top;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashHandler);
#endif
    QApplication app(argc, argv);
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
    MainWindow w;
    w.show();
    const QStringList files = app.arguments().mid(1);
    // from the event loop: opening may ask a question (subtitles inside a video)
    QTimer::singleShot(0, &w, [&w, files] {
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
                // FSED_SMOKE_DRAG="x1,y1,x2,y2": a left-button drag on the video, in view fractions 0..1
                const QStringList d = qEnvironmentVariable("FSED_SMOKE_DRAG").split(',');
                if (d.size() != 4) return;
                QWidget *view = w.current()->video()->view();
                const auto at = [&](int k) { return QPointF(d[k].toDouble() * view->width(), d[k + 1].toDouble() * view->height()); };
                auto send = [view](QEvent::Type t, QPointF p, Qt::MouseButtons held) {
                    QMouseEvent e(t, p, view->mapToGlobal(p), Qt::LeftButton, held, Qt::NoModifier);
                    QApplication::sendEvent(view, &e);
                };
                send(QEvent::MouseButtonPress, at(0), Qt::LeftButton);
                for (int k = 1; k <= 4; ++k) send(QEvent::MouseMove, at(0) + (at(2) - at(0)) * k / 4, Qt::LeftButton);
                send(QEvent::MouseButtonRelease, at(2), Qt::NoButton);
            });
    // FSED_SMOKE_ACTION runs a menu command by id; a modal dialog it opens is grabbed instead.
    if (const QString id = qEnvironmentVariable("FSED_SMOKE_ACTION"); !id.isEmpty())
        QTimer::singleShot(std::max(700, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 2 / 3), &w, [&w, id] {
            if (auto *a = w.action(id)) a->trigger();
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
    // FSED_SMOKE_CLICK="Check" presses that button in the open dialog, else in the main window.
    if (const QString text = qEnvironmentVariable("FSED_SMOKE_CLICK"); !text.isEmpty())
        QTimer::singleShot(std::max(800, qEnvironmentVariableIntValue("FSED_SMOKE_MS") * 5 / 6), &w, [&w, text] {
            QWidget *top = smokeDialog(w);
            for (auto *b : (top ? top : &w)->findChildren<QAbstractButton *>())
                if (b->text() == text && b->isVisible()) return b->click();
        });
    if (const QString out = qEnvironmentVariable("FSED_SMOKE"); !out.isEmpty())
        QTimer::singleShot(std::max(1000, qEnvironmentVariableIntValue("FSED_SMOKE_MS")), &w, [&w, out] {
            QWidget *top = smokeDialog(w);
            (top ? top : &w)->grab().save(out);
            QApplication::exit(0);
        });
    return app.exec();
}

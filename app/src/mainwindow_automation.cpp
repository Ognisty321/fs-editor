// MainWindow's automation: the scripts loaded at start and by hand, the Automation menu and
// its manager, and a macro run with its progress window.
#include "mainwindow_p.h"

#include "audiobox.h"
#include "automationhost.h"
#include "editbox.h"
#include "grid.h"
#include "tabpanel.h"
#include "videobox.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QtConcurrentRun>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>
#include <utility>

// What a script is told of the window as it is now: the audio selection, the edit box's cursor,
// and the scroll position, video frame and zoom of aegisub.project_properties.
static fsb::MacroContext macroContext(TabPanel *tab) {
    fsb::MacroContext ctx;
    if (tab->audio()->hasAudio()) {
        ctx.has_audio = true;
        std::tie(ctx.audio_start, ctx.audio_end) = tab->audio()->selection();
    }
    tab->edit()->scriptCursor(ctx.cursor, ctx.sel_start, ctx.sel_end);
    ctx.scroll_position = tab->grid()->verticalScrollBar()->value();
    auto *v = tab->video();
    if (const auto info = v->info(); v->hasVideo() && info.width > 0) {
        ctx.video_position = v->frame();
        ctx.video_zoom = v->view()->displayRect().width() * v->view()->zoom().width() / info.width;
    }
    return ctx;
}

void MainWindow::buildAutomationCommands() {
    m_automationMenu = barMenu(tr("Au&tomation"));
    add("automationManager", tr("Automation manager..."), {}, nullptr, [this] { automationManager(); });
    add("loadScript", tr("Load script"), {}, nullptr, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Load script"), {},
                                                       tr("Automation scripts (*.lua *.moon);;All files (*)"));
        if (f.isEmpty()) return;
        const size_t before = (*m_automation)->macros().size();
        try {
            (*m_automation)->load_script(fs::Utf8(f));
            if ((*m_automation)->macros().size() > before) m_lastMacro = qint64(before);
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        (*m_automation)->add_local_script(current()->s(), fs::Utf8(f)); // the file asks for it next time (Aegisub)
        buildAutomationMenu();
    });
    add("reloadScripts", tr("Reload all scripts"), {}, nullptr, [this] {
        if (m_autoloadJob) ensureAutoload(); // its scripts in first, not added over the reloaded ones
        m_autoloaded = true; // the reload loads the autoload dirs
        for (const auto &e : (*m_automation)->reload()) log(fs::q(e));
        buildAutomationMenu();
    });
    add("runLastScript", tr("Run the last loaded script"), {}, nullptr, [this] {
        if (m_lastMacro >= 0 && size_t(m_lastMacro) < (*m_automation)->macros().size()) runMacro(size_t(m_lastMacro));
    });
    // Aegisub's validate (enabled, and a help text for the status bar in place of the
    // description) and is_active (a toggle macro's check mark), as the menu opens
    connect(m_automationMenu, &QMenu::aboutToShow, this, [this] {
        if (!m_autoloaded) {
            if (QSettings().value("automation/loadMethod", 1).toInt() % 2) {
                ensureAutoload();
            } else if (!m_autoloadJob) {
                startAutoload();
                buildAutomationMenu(); // with the loading line
            }
        }
        auto *tab = current();
        const auto rows = tab->selection();
        const auto states =
            (*m_automation)->macro_states(tab->s(), fs::slice(rows), tab->active(), macroContext(tab));
        for (auto *a : m_actions) {
            const QVariant id = a->property("macroId");
            if (!id.isValid() || id.toULongLong() >= states.size()) continue;
            const auto &st = states[size_t(id.toULongLong())];
            a->setVisible(st.visible); // another file's script (Aegisub's other window)
            a->setEnabled(st.enabled);
            a->setCheckable(st.toggle);
            a->setChecked(st.checked);
            a->setStatusTip(st.help.empty() ? a->toolTip() : fs::q(st.help));
        }
        // a submenu shows while something in it does; the inner ones first
        const auto subs = m_automationMenu->findChildren<QMenu *>();
        for (auto it = subs.rbegin(); it != subs.rend(); ++it) {
            const auto acts = (*it)->actions();
            (*it)->menuAction()->setVisible(std::ranges::any_of(acts, &QAction::isVisible));
        }
    });
}

void MainWindow::buildAutomationMenu() {
    auto *m = m_automationMenu;
    m->clear();
    for (auto it = m_actions.begin(); it != m_actions.end();) {
        if (it.key().startsWith("macro:")) {
            delete it.value();
            it = m_actions.erase(it);
        } else {
            ++it;
        }
    }
    for (const char *id : {"loadScript", "reloadScripts", "automationManager", "runLastScript"})
        m->addAction(m_actions.value(id));
    m->addSeparator();
    QMap<QString, QMenu *> submenus;
    for (const auto &mi : (*m_automation)->macros()) {
        // Aegisub convention: "Group/Name" makes a submenu
        QString name = fs::q(mi.name);
        QMenu *parent = m;
        QStringList parts = name.split('/');
        name = parts.takeLast();
        QString key;
        for (const QString &p : parts) {
            key += p + "/";
            if (!submenus.contains(key)) {
                submenus[key] = parent->addMenu(fs::menuText(p));
                submenus[key]->installEventFilter(this);
            }
            parent = submenus[key];
        }
        const size_t id = mi.id;
        auto *a = add("macro:" + fs::q(mi.script) + "/" + fs::q(mi.name), fs::menuText(name), {}, parent,
                      [this, id] { runMacro(id); });
        a->setProperty("name", name); // its own, as fs::plainText would read it back
        showHotkey(a);
        a->setToolTip(fs::q(mi.description));
        a->setProperty("macroId", qulonglong(id));
    }
    if (m_autoloadJob) m_automationMenu->addAction(tr("Loading the autoload scripts..."))->setEnabled(false);
    if (!m_autoloaded) { // the hotkeys of the scripts not loaded yet load them first
        QSettings st;
        st.beginGroup("hotkeys");
        for (const QString &k : st.allKeys())
            if (k.startsWith("macro:") && !m_actions.contains(k))
                add(k, k, {}, nullptr, [this, k] {
                    // later: the loading deletes this action
                    QTimer::singleShot(0, this, [this, k] {
                        ensureAutoload();
                        if (auto *a = m_actions.value(k); a && a->property("macroId").isValid()) a->trigger();
                    });
                });
    }
    watchScripts();
}

void MainWindow::startAutoload() {
    m_autoloadJob.emplace((*m_automation)->start_autoload());
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this, poll] {
        if (m_autoloadJob && !(*m_automation)->autoload_done(**m_autoloadJob)) return;
        poll->deleteLater();
        ensureAutoload();
    });
    poll->start(30);
}

void MainWindow::ensureAutoload() {
    if (m_autoloaded) return;
    m_autoloaded = true;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto errors = m_autoloadJob ? (*m_automation)->finish_autoload(std::move(*m_autoloadJob))
                                      : (*m_automation)->load_autoload();
    m_autoloadJob.reset();
    QApplication::restoreOverrideCursor();
    for (const auto &e : errors) status(fs::q(e));
    buildAutomationMenu();
    if (m_automationMenu->isVisible()) emit m_automationMenu->aboutToShow(); // the macros' states, loaded while open
}

// Aegisub's Automation/Autoreload Mode: a script file changed loads again (1 the local scripts,
// 2 the autoloaded ones, 3 both)
void MainWindow::watchScripts() {
    if (!current()) return; // starting up: once the first tab is there
    if (!m_scriptWatch) {
        m_scriptWatch = new QFileSystemWatcher(this);
        connect(m_scriptWatch, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
            // an editor saving may replace the file: read it a moment later
            QTimer::singleShot(300, this, [this, path] {
                if (!current()) return;
                const auto list = (*m_automation)->scripts(current()->s());
                for (size_t i = 0; i < list.size(); ++i)
                    if (QFileInfo(fs::q(list[i].path)) == QFileInfo(path)) {
                        try {
                            (*m_automation)->reload_script(i);
                            status(tr("Reloaded %1").arg(QFileInfo(path).fileName()));
                        } catch (const rust::Error &e) {
                            log(QString::fromUtf8(e.what()));
                        }
                        buildAutomationMenu();
                        break;
                    }
            });
        });
    }
    const int mode = QSettings().value("automation/autoreload", 1).toInt();
    if (const QStringList old = m_scriptWatch->files(); !old.isEmpty()) m_scriptWatch->removePaths(old);
    QStringList files;
    for (const auto &s : (*m_automation)->scripts(current()->s()))
        if (mode & (s.autoload ? 2 : 1) && QFileInfo::exists(fs::q(s.path))) files << fs::q(s.path);
    if (!files.isEmpty()) m_scriptWatch->addPaths(files);
}

void MainWindow::automationManager() {
    ensureAutoload();
    QDialog d(this);
    d.setWindowTitle(tr("Automation manager"));
    d.resize(720, 380);
    auto *v = new QVBoxLayout(&d);
    auto *list = new QTreeWidget(&d);
    list->setHeaderLabels({"", tr("Name"), tr("File name"), tr("Description")});
    list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    v->addWidget(list);
    auto *h = new QHBoxLayout;
    v->addLayout(h);
    rust::Vec<fsb::ScriptItem> scripts;
    QPushButton *remove = nullptr, *reload = nullptr, *info = nullptr, *edit = nullptr;
    const auto row = [list] { return list->currentIndex().row(); };
    const auto enable = [&] {
        const int r = row();
        const bool any = r >= 0 && size_t(r) < scripts.size();
        remove->setEnabled(any && !scripts[size_t(r)].autoload);
        reload->setEnabled(any);
        info->setEnabled(any);
        edit->setEnabled(any);
    };
    auto refresh = [&, this](int select) {
        buildAutomationMenu();
        scripts = (*m_automation)->scripts(current()->s());
        list->clear();
        for (const auto &s : scripts) {
            const bool ok = s.error.empty();
            auto *it = new QTreeWidgetItem(list, {s.autoload ? "G" : "L", fs::q(s.name),
                                                  QFileInfo(fs::q(s.path)).fileName(),
                                                  fs::q(ok ? s.description : s.error)});
            it->setHidden(!s.shown); // another file's (Aegisub: another window's manager)
            if (!ok)
                for (int c = 0; c < 4; ++c) it->setForeground(c, QColor(255, 128, 128));
        }
        for (int c = 0; c < 3; ++c) list->resizeColumnToContents(c);
        list->setCurrentItem(list->topLevelItem(std::max(0, std::min(select, list->topLevelItemCount() - 1))));
        enable();
    };
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, &d);
        connect(b, &QPushButton::clicked, &d, fn);
        h->addWidget(b);
        return b;
    };
    button(tr("Add"), [&, this] {
        const QString f = QFileDialog::getOpenFileName(&d, tr("Load script"), {},
                                                       tr("Automation scripts (*.lua *.moon);;All files (*)"));
        if (f.isEmpty()) return;
        try {
            (*m_automation)->load_script(fs::Utf8(f));
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        // Aegisub's local scripts: the file lists those added here
        (*m_automation)->add_local_script(current()->s(), fs::Utf8(f));
        refresh(int(scripts.size()));
    });
    remove = button(tr("Remove"), [&, this] {
        (*m_automation)->remove_local_script(current()->s(), scripts[size_t(row())].path);
        (*m_automation)->remove_script(size_t(row()));
        refresh(row());
    });
    reload = button(tr("Reload"), [&, this] {
        try {
            (*m_automation)->reload_script(size_t(row()));
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
        refresh(row());
    });
    info = button(tr("Show info"), [&] {
        const auto &s = scripts[size_t(row())];
        QMessageBox::information(
            &d, tr("Automation script info"),
            tr("Name: %1\nDescription: %2\nAuthor: %3\nVersion: %4\nFull path: %5\nState: %6\nMacros: %7\n"
               "Export filters: %8")
                .arg(fs::q(s.name), fs::q(s.description), fs::q(s.author), fs::q(s.version), fs::q(s.path),
                     s.error.empty() ? tr("Correctly loaded") : tr("Failed to load: %1").arg(fs::q(s.error)),
                     QString::number(s.macros), QString::number(s.filters)));
    });
    // Kainote's script editing: the editor from the settings, else the system's for .lua; a script
    // saved there loads again when Automatically reload covers it
    edit = button(tr("Edit"), [&, this] {
        const QString file = fs::q(scripts[size_t(row())].path);
        const QString editor = QSettings().value("automation/editor").toString();
        if (!(editor.isEmpty() ? QDesktopServices::openUrl(QUrl::fromLocalFile(file))
                               : QProcess::startDetached(editor, {QDir::toNativeSeparators(file)})))
            error(tr("Could not open %1 in an editor").arg(QDir::toNativeSeparators(file)));
    });
    button(tr("Reload all"), [&, this] {
        for (const auto &e : (*m_automation)->reload()) log(fs::q(e));
        refresh(row());
    });
    h->addStretch();
    button(tr("Close"), [&] { d.accept(); });
    connect(list, &QTreeWidget::currentItemChanged, &d, enable);
    refresh(0);
    d.exec();
}

void MainWindow::loadLocalScripts(TabPanel *t) {
    // Aegisub's LocalScriptManager: the scripts the file lists load only once the user trusts them
    const auto scripts = (*m_automation)->local_scripts(t->s());
    QStringList listed;
    for (const auto &s : scripts) {
        if (s.path.empty()) {
            const QString tok = fs::q(s.token);
            log(tr("Automation Script referenced with unknown location specifier character.\nLocation specifier "
                   "found: %1\nFilename specified: %2")
                    .arg(tok.left(1), tok.mid(1)));
            continue;
        }
        listed << tr("    %1 (%2)").arg(fs::q(s.token), QDir::toNativeSeparators(fs::q(s.path)));
    }
    if (listed.isEmpty()) return;
    QMessageBox box(QMessageBox::Warning, tr("Load associated automation scripts?"),
                    tr("The subtitle file you have opened contains local automation scripts. Do you want to load "
                       "the associated automation scripts?\n\nList of scripts:\n\n%1\n\nYou should only load "
                       "associated automation scripts if you trust their authors! They are not required to view or "
                       "edit the subtitle file.")
                        .arg(listed.join('\n')),
                    QMessageBox::Ok | QMessageBox::Cancel, this);
    box.button(QMessageBox::Ok)->setText(tr("Trust the authors && load scripts"));
    box.button(QMessageBox::Cancel)->setText(tr("No"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Ok) return;
    for (const auto &s : scripts) {
        if (s.path.empty()) continue;
        if (!s.exists) {
            log(tr("Automation Script referenced could not be found.\nFilename specified: %1\nSearched relative to: "
                   "%2\nResolved filename: %3")
                    .arg(fs::q(s.token).mid(1), QDir::toNativeSeparators(fs::q(s.base)),
                         QDir::toNativeSeparators(fs::q(s.path))));
            continue;
        }
        try {
            (*m_automation)->load_script(s.path);
        } catch (const rust::Error &e) {
            log(QString::fromUtf8(e.what()));
        }
    }
    buildAutomationMenu();
}

void MainWindow::runScript(fsb::ScriptJob &job) {
    // Aegisub's BackgroundScriptRunner: the script runs on a worker and m_host brings its dialogs
    // and progress back here. Input waits for the first 300 ms, then the progress window holds it.
    m_host->begin();
    ++m_scriptsRunning;
    const auto done = qScopeGuard([this] { --m_scriptsRunning; });
    QFutureWatcher<void> watch;
    QEventLoop loop;
    connect(&watch, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    watch.setFuture(QtConcurrent::run([&job] { fsb::run_script_job(job); }));
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    if (!watch.isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!watch.isFinished()) {
        m_host->showProgress();
        loop.exec();
    }
    // quitting the application ends the loops early: stop the script, the worker holds the job
    if (!watch.isFinished()) (*m_automation)->stop_script();
    while (!watch.isFinished()) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    m_host->finish();
}

void MainWindow::runMacro(size_t id) {
    auto *tab = current();
    const auto rows = tab->selection();
    const fsb::MacroContext ctx = macroContext(tab);
    fsb::MacroResult r;
    QString err;
    try {
        auto job = (*m_automation)->start_macro(id, tab->s(), fs::slice(rows), tab->active(), ctx);
        runScript(*job);
        r = (*m_automation)->finish_macro(std::move(job), tab->s());
    } catch (const rust::Error &e) {
        err = QString::fromUtf8(e.what());
    }
    log(err); // what the script wrote was in its progress window
    if (r.changed) tab->changed();
    // a macro may only return a new selection (e.g. Select overlaps)
    const auto sel = fs::vec(r.selection);
    if (!sel.empty()) tab->setSelection(sel, r.active >= 0 ? int(r.active) : int(sel.front()));
    if (r.edit_changed) tab->edit()->setScriptCursor(r.cursor, r.sel_start, r.sel_end);
}

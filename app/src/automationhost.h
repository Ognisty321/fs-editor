// Qt side of Automation 4: aegisub.dialog.display/open/save, the clipboard, the status bar and the
// progress window. A script calls it from its worker thread; every Qt call is taken over to the GUI
// thread.
#pragma once

#include "bridge.h"

#include <QPointer>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

class QGridLayout;
class QWidget;
class ScriptProgress;

// A script dialog's controls in `grid` (Aegisub's LuaDialog), for a dialog of its own or an export
// filter's settings panel; the widgets are parallel to `controls`.
std::vector<QWidget *> scriptControls(const rust::Vec<fsb::DialogControlData> &controls, QGridLayout *grid,
                                      QWidget *parent);
// `controls` with the values the widgets hold now.
rust::Vec<fsb::DialogControlData> scriptValues(const rust::Vec<fsb::DialogControlData> &controls,
                                               const std::vector<QWidget *> &widgets);

class GuiAutomationHost : public fsb::AutomationHost {
public:
    explicit GuiAutomationHost(QWidget *parent) : m_parent(parent) {}
    ~GuiAutomationHost() override;

    fsb::DialogReply dialog(const fsb::DialogData &d) const override;
    rust::Vec<rust::String> open_files(rust::Str title, rust::Str dir, rust::Str file, rust::Str wildcard,
                                       bool multiple, bool must_exist) const override;
    rust::String save_file(rust::Str title, rust::Str dir, rust::Str file, rust::Str wildcard,
                           bool dont_prompt_overwrite) const override;
    void progress(double percent) const override;
    void task(rust::Str text) const override;
    void title(rust::Str text) const override;
    bool is_cancelled() const override;
    void status_text(rust::Str text) const override;
    void log(rust::Str text) const override;
    rust::String clipboard_get() const override;
    bool clipboard_set(rust::Str text) const override;

    // Around a script run: begin() clears the cancel flag, showProgress() puts the window up for a
    // run that takes a while, finish() closes it, after the user has read what the script wrote.
    void begin() const;
    void showProgress() const;
    void finish() const;

    // aegisub.set_status_text, on the GUI thread.
    std::function<void(const QString &)> onStatus;
    // The progress window's second Cancel: stop a script that never checks for it.
    std::function<void()> onStop;
    // Script text outside a run (a validate function's error), on the GUI thread.
    std::function<void(const QString &)> onLog;

private:
    // What the script last set, shown by one queued update however often it is set.
    struct Pending {
        int value = -1;
        std::optional<QString> task, title;
        QString log; // all of it, in order
        bool queued = false;
    };
    template <class F> void queue(F set) const;
    void flush() const;
    ScriptProgress *bar() const;

    QWidget *m_parent;
    mutable QPointer<ScriptProgress> m_progress;
    mutable std::atomic<bool> m_cancelled{false};
    mutable bool m_running = false; // GUI thread: between begin() and finish()
    mutable std::mutex m_mx;
    mutable Pending m_pending;
};

// Qt side of Automation 4: aegisub.dialog.display/open/save and the progress window. A macro
// calls it from its worker thread; every Qt call is taken over to the GUI thread.
#pragma once

#include "bridge.h"

#include <QPointer>

#include <atomic>

class QWidget;
class QProgressDialog;

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

    // Around a macro run: begin() clears the cancel flag, showProgress() puts the window up for a
    // run that takes a while, finish() closes it.
    void begin() const;
    void showProgress() const;
    void finish() const;

private:
    QProgressDialog *bar() const;
    QWidget *m_parent;
    mutable QPointer<QProgressDialog> m_progress;
    mutable std::atomic<bool> m_cancelled{false};
};

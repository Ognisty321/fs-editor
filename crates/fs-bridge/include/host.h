// Callbacks the Rust side makes into the Qt app while an automation script
// runs (dialogs, file pickers, progress). The app subclasses AutomationHost.
#pragma once
#include "rust/cxx.h"

#include <cstdint>

namespace fsb {
struct DialogData;
struct DialogReply;

class AutomationHost {
public:
    virtual ~AutomationHost() = default;
    virtual DialogReply dialog(const DialogData &d) const = 0;
    // Empty result = cancelled.
    virtual rust::Vec<rust::String> open_files(rust::Str title, rust::Str dir, rust::Str file,
                                               rust::Str wildcard, bool multiple, bool must_exist) const = 0;
    virtual rust::String save_file(rust::Str title, rust::Str dir, rust::Str file,
                                   rust::Str wildcard, bool dont_prompt_overwrite) const = 0;
    virtual void progress(double percent) const = 0;
    virtual void task(rust::Str text) const = 0;
    virtual void title(rust::Str text) const = 0;
    virtual bool is_cancelled() const = 0;
    virtual void status_text(rust::Str text) const = 0;
    // Empty = no text on the clipboard.
    virtual rust::String clipboard_get() const = 0;
    virtual bool clipboard_set(rust::Str text) const = 0;
};

// Progress of a long open (indexing, decoding), called on the opening thread; false cancels.
class Progress {
public:
    virtual ~Progress() = default;
    virtual bool report(int64_t done, int64_t total) const = 0;
};

// Told about a Rust panic on the GUI thread, just before the bridge aborts the process.
class PanicHandler {
public:
    virtual ~PanicHandler() = default;
    virtual void fatal(rust::Str message) const = 0;
};
} // namespace fsb
